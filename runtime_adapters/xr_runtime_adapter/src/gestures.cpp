#include "gestures.hpp"

#include "override_controller.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xr_runtime_adapter::gestures {
namespace {

float clamp01(float v) {
  if (!std::isfinite(v)) return 0.0f;
  return std::max(0.0f, std::min(1.0f, v));
}

float dist3(float ax, float ay, float az, float bx, float by, float bz) {
  const float dx = ax - bx;
  const float dy = ay - by;
  const float dz = az - bz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool update_gesture_hysteresis(bool was_active,
                               float strength,
                               float activate_threshold,
                               float deactivate_threshold) {
  const float v = clamp01(strength);
  const float on = clamp01(activate_threshold);
  const float off = std::min(on, clamp01(deactivate_threshold));
  return was_active ? (v >= off) : (v >= on);
}

bool skeleton_joint_position_valid(const xr_tracking::HandSkeleton26JointF32V1& j) {
  if (!std::isfinite(j.px) || !std::isfinite(j.py) || !std::isfinite(j.pz)) {
    return false;
  }
  const double mag1 =
      std::abs(static_cast<double>(j.px)) +
      std::abs(static_cast<double>(j.py)) +
      std::abs(static_cast<double>(j.pz));
  if (mag1 <= 1e-6) {
    return false;
  }
  return (j.flags & xr_tracking::HAND_SKELETON26_JOINT_POSITION_VALID) != 0u ||
         (j.flags & xr_tracking::HAND_SKELETON26_JOINT_TRACKED) != 0u;
}

bool skeleton_joint_orientation_valid(const xr_tracking::HandSkeleton26JointF32V1& j) {
  if (!std::isfinite(j.qw) || !std::isfinite(j.qx) ||
      !std::isfinite(j.qy) || !std::isfinite(j.qz)) {
    return false;
  }
  const double n2 =
      static_cast<double>(j.qw) * static_cast<double>(j.qw) +
      static_cast<double>(j.qx) * static_cast<double>(j.qx) +
      static_cast<double>(j.qy) * static_cast<double>(j.qy) +
      static_cast<double>(j.qz) * static_cast<double>(j.qz);
  if (n2 <= 1e-8) {
    return false;
  }
  return (j.flags & xr_tracking::HAND_SKELETON26_JOINT_ORIENTATION_VALID) != 0u ||
         (j.flags & xr_tracking::HAND_SKELETON26_JOINT_TRACKED) != 0u;
}

xr_runtime::HandJointF32V2 runtime_joint_from_skeleton26(
    const xr_tracking::HandSkeleton26JointF32V1& src,
    uint32_t out_joint_id) {
  xr_runtime::HandJointF32V2 out{};
  out.joint_id = out_joint_id;
  out.flags = src.flags;
  out.px = src.px;
  out.py = src.py;
  out.pz = src.pz;
  out.qw = src.qw;
  out.qx = src.qx;
  out.qy = src.qy;
  out.qz = src.qz;
  out.radius_m = src.radius_m;
  out.confidence = src.confidence;
  return out;
}

bool runtime_hand_joint_position_valid(const xr_runtime::HandJointF32V2& j) {
  if (!std::isfinite(j.px) || !std::isfinite(j.py) || !std::isfinite(j.pz)) {
    return false;
  }

  const double mag1 =
      std::abs(static_cast<double>(j.px)) +
      std::abs(static_cast<double>(j.py)) +
      std::abs(static_cast<double>(j.pz));

  return mag1 > 1e-6;
}

bool runtime_hand_side_pose_position_valid(const xr_runtime::HandSideF32V2& side) {
  if (!std::isfinite(side.palm_px) ||
      !std::isfinite(side.palm_py) ||
      !std::isfinite(side.palm_pz)) {
    return false;
  }

  const double mag1 =
      std::abs(static_cast<double>(side.palm_px)) +
      std::abs(static_cast<double>(side.palm_py)) +
      std::abs(static_cast<double>(side.palm_pz));

  return mag1 > 1e-6;
}

float runtime_joint_palm_distance(const xr_runtime::HandSideF32V2& side,
                                  uint32_t joint_id) {
  if (joint_id >= side.joint_count || joint_id >= xr_runtime::HAND_JOINT_COUNT_V2) {
    return -1.0f;
  }
  const auto& tip = side.joints[joint_id];
  if (!runtime_hand_joint_position_valid(tip)) return -1.0f;
  if (!std::isfinite(side.palm_px) || !std::isfinite(side.palm_py) || !std::isfinite(side.palm_pz)) {
    return -1.0f;
  }
  return dist3(tip.px, tip.py, tip.pz, side.palm_px, side.palm_py, side.palm_pz);
}

float runtime_finger_extended_strength(const xr_runtime::HandSideF32V2& side,
                                       uint32_t tip_joint_id,
                                       float curled_distance_m = 0.075f,
                                       float open_distance_m = 0.125f) {
  const float d = runtime_joint_palm_distance(side, tip_joint_id);
  if (d < 0.0f) return -1.0f;
  return clamp01((d - curled_distance_m) / (open_distance_m - curled_distance_m));
}

float runtime_finger_curled_strength(const xr_runtime::HandSideF32V2& side,
                                     uint32_t tip_joint_id,
                                     float curled_distance_m = 0.075f,
                                     float open_distance_m = 0.125f) {
  const float extended = runtime_finger_extended_strength(side, tip_joint_id,
                                                          curled_distance_m,
                                                          open_distance_m);
  if (extended < 0.0f) return -1.0f;
  return clamp01(1.0f - extended);
}

float derive_thumbs_up_strength_from_runtime_hand_v2(const xr_runtime::HandSideF32V2& side) {
  // Runtime 21-joint layout:
  // 4=thumb tip, 8=index tip, 12=middle tip, 16=ring tip, 20=little tip.
  if (side.joint_count <= 20) return -1.0f;

  // Thumbs-up is treated as a fist with the thumb extended. This intentionally
  // ignores world "up" first: shape-only classification is more stable while
  // the camera/hand convention is still being tuned.
  const float thumb_extended = runtime_finger_extended_strength(side, 4u, 0.060f, 0.105f);
  const float index_curled = runtime_finger_curled_strength(side, 8u, 0.075f, 0.125f);
  const float middle_curled = runtime_finger_curled_strength(side, 12u, 0.075f, 0.125f);
  const float ring_curled = runtime_finger_curled_strength(side, 16u, 0.075f, 0.125f);
  const float little_curled = runtime_finger_curled_strength(side, 20u, 0.075f, 0.125f);

  if (thumb_extended < 0.0f || index_curled < 0.0f || middle_curled < 0.0f ||
      ring_curled < 0.0f || little_curled < 0.0f) {
    return -1.0f;
  }

  const float curled_non_thumb =
      (index_curled + middle_curled + ring_curled + little_curled) * 0.25f;
  return clamp01(std::min(thumb_extended, curled_non_thumb));
}

float derive_index_point_strength_from_runtime_hand_v2(const xr_runtime::HandSideF32V2& side) {
  // One-finger index-point: index extended, middle/ring/little curled.
  // Thumb is intentionally ignored so natural pointing poses still work.
  if (side.joint_count <= 20) return -1.0f;

  const float index_extended = runtime_finger_extended_strength(side, 8u, 0.075f, 0.125f);
  const float middle_curled = runtime_finger_curled_strength(side, 12u, 0.075f, 0.125f);
  const float ring_curled = runtime_finger_curled_strength(side, 16u, 0.075f, 0.125f);
  const float little_curled = runtime_finger_curled_strength(side, 20u, 0.075f, 0.125f);

  if (index_extended < 0.0f || middle_curled < 0.0f ||
      ring_curled < 0.0f || little_curled < 0.0f) {
    return -1.0f;
  }

  const float curled_non_index = (middle_curled + ring_curled + little_curled) / 3.0f;
  return clamp01(std::min(index_extended, curled_non_index));
}

float derive_pinch_strength_from_runtime_hand_v2(const xr_runtime::HandSideF32V2& side) {
  // Runtime HandTrackingFrameF32V2 uses the compact 21-joint layout:
  // 0 wrist, 1..4 thumb, 5..8 index, 9..12 middle, 13..16 ring, 17..20 little.
  if (side.joint_count <= 8) return -1.0f;
  const auto& thumb_tip = side.joints[4];
  const auto& index_tip = side.joints[8];
  if (!runtime_hand_joint_position_valid(thumb_tip) ||
      !runtime_hand_joint_position_valid(index_tip)) {
    return -1.0f;
  }
  const float d = dist3(thumb_tip.px, thumb_tip.py, thumb_tip.pz,
                        index_tip.px, index_tip.py, index_tip.pz);
  return clamp01((0.070f - d) / (0.070f - 0.025f));
}

struct RuntimeFinger21 {
  uint32_t proximal = 0;
  uint32_t intermediate = 0;
  uint32_t distal = 0;
  uint32_t tip = 0;
};

float runtime_finger_grab_curl_strength(const xr_runtime::HandSideF32V2& side,
                                        RuntimeFinger21 finger) {
  if (side.joint_count <= finger.tip || !runtime_hand_side_pose_position_valid(side)) {
    return -1.0f;
  }

  const auto& proximal = side.joints[finger.proximal];
  const auto& intermediate = side.joints[finger.intermediate];
  const auto& distal = side.joints[finger.distal];
  const auto& tip = side.joints[finger.tip];

  if (!runtime_hand_joint_position_valid(proximal) ||
      !runtime_hand_joint_position_valid(intermediate) ||
      !runtime_hand_joint_position_valid(distal) ||
      !runtime_hand_joint_position_valid(tip)) {
    return -1.0f;
  }

  const float chain =
      dist3(proximal.px, proximal.py, proximal.pz,
            intermediate.px, intermediate.py, intermediate.pz) +
      dist3(intermediate.px, intermediate.py, intermediate.pz,
            distal.px, distal.py, distal.pz) +
      dist3(distal.px, distal.py, distal.pz, tip.px, tip.py, tip.pz);
  if (!std::isfinite(chain) || chain <= 1e-4f) {
    return -1.0f;
  }

  const float chord = dist3(proximal.px, proximal.py, proximal.pz,
                            tip.px, tip.py, tip.pz);
  const float chord_ratio = chord / chain;

  // Old grab detection used only tip-to-palm distance. That fires on relaxed/open
  // hands whose fingers are extended but bent toward the palm. Require actual
  // finger folding as well: a fist has a short MCP/proximal-to-tip chord compared
  // with the finger bone chain, while a relaxed open hand keeps this ratio high.
  // ratio <= 0.70: strongly folded; ratio >= 0.92: extended/not a fist.
  const float folded_by_chain = clamp01((0.92f - chord_ratio) / (0.92f - 0.70f));

  const float tip_palm = dist3(tip.px, tip.py, tip.pz,
                              side.palm_px, side.palm_py, side.palm_pz);
  // Keep the previous palm-distance idea, but make it only one required clause:
  // <=8.5cm is fist-like, >=14.5cm is open.
  const float close_to_palm = clamp01((0.145f - tip_palm) / (0.145f - 0.085f));

  return clamp01(std::min(folded_by_chain, close_to_palm));
}

float derive_grab_strength_from_runtime_hand_v2(const xr_runtime::HandSideF32V2& side) {
  if (side.joint_count <= 20 || !runtime_hand_side_pose_position_valid(side)) return -1.0f;

  // Runtime 21-joint layout:
  // middle: 9..12, ring: 13..16, little: 17..20.
  // Do not include index here: index is used for pinch/trigger and otherwise
  // makes pinch falsely activate grip/grab.
  const RuntimeFinger21 fingers[] = {
      {9u, 10u, 11u, 12u},
      {13u, 14u, 15u, 16u},
      {17u, 18u, 19u, 20u},
  };

  float sum = 0.0f;
  uint32_t count = 0;
  uint32_t strong_count = 0;
  for (const auto& finger : fingers) {
    const float curl = runtime_finger_grab_curl_strength(side, finger);
    if (curl < 0.0f) continue;
    sum += curl;
    ++count;
    if (curl >= 0.70f) ++strong_count;
  }

  if (count < 2) return -1.0f;
  if (strong_count < 2) return 0.0f;
  return clamp01(sum / static_cast<float>(count));
}

bool update_extra_gesture_button_state(bool& active_state,
                                       int64_t& hold_until_ns,
                                       float strength,
                                       float active_threshold,
                                       float deactive_threshold,
                                       double hold_ms,
                                       int64_t now_ns) {
  const bool candidate_active = update_gesture_hysteresis(active_state,
                                                         strength,
                                                         active_threshold,
                                                         deactive_threshold);
  const int64_t hold_ns = hold_ms > 0.0
      ? static_cast<int64_t>(hold_ms * 1000000.0)
      : 0;
  if (candidate_active) {
    active_state = true;
    hold_until_ns = hold_ns > 0 ? now_ns + hold_ns : now_ns;
    return true;
  }
  if (hold_ns > 0 && now_ns <= hold_until_ns) {
    active_state = true;
    return true;
  }
  active_state = false;
  hold_until_ns = 0;
  return false;
}

void derive_runtime_hand_side_extra_gesture_buttons(
    xr_runtime::HandSideF32V2& side,
    uint32_t thumbs_up_button,
    uint32_t index_point_button,
    float thumbs_up_active_threshold,
    float index_point_active_threshold,
    float thumbs_up_deactive_threshold,
    float index_point_deactive_threshold,
    float response_start,
    double extra_gesture_hold_ms,
    int64_t now_ns,
    RuntimeGestureHysteresisSideState& state) {
  const bool active = side.status == 1u || (side.flags & xr_runtime::HAND_POSE_VALID) != 0u;
  if (!active || (side.flags & xr_runtime::HAND_JOINTS_VALID) == 0u) {
    state.reset_extra_gestures();
    return;
  }

  uint32_t buttons = side.reserved0 & override_controller::runtime_controller_button_mask();

  if (thumbs_up_button != 0u) {
    const float raw = derive_thumbs_up_strength_from_runtime_hand_v2(side);
    if (raw >= 0.0f && std::isfinite(raw)) {
      const float strength = apply_derived_gesture_response_curve(raw, response_start);
      if (update_extra_gesture_button_state(state.thumbs_up_active,
                                            state.thumbs_up_hold_until_ns,
                                            strength,
                                            thumbs_up_active_threshold,
                                            thumbs_up_deactive_threshold,
                                            extra_gesture_hold_ms,
                                            now_ns)) {
        buttons |= thumbs_up_button;
      }
    } else {
      state.thumbs_up_active = false;
      state.thumbs_up_hold_until_ns = 0;
    }
  } else {
    state.thumbs_up_active = false;
    state.thumbs_up_hold_until_ns = 0;
  }

  if (index_point_button != 0u) {
    const float raw = derive_index_point_strength_from_runtime_hand_v2(side);
    if (raw >= 0.0f && std::isfinite(raw)) {
      const float strength = apply_derived_gesture_response_curve(raw, response_start);
      if (update_extra_gesture_button_state(state.index_point_active,
                                            state.index_point_hold_until_ns,
                                            strength,
                                            index_point_active_threshold,
                                            index_point_deactive_threshold,
                                            extra_gesture_hold_ms,
                                            now_ns)) {
        buttons |= index_point_button;
      }
    } else {
      state.index_point_active = false;
      state.index_point_hold_until_ns = 0;
    }
  } else {
    state.index_point_active = false;
    state.index_point_hold_until_ns = 0;
  }

  side.reserved0 = buttons;
}

void derive_missing_runtime_hand_side_gestures(
    xr_runtime::HandSideF32V2& side,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_deactive_threshold,
    float derived_grab_deactive_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start,
    RuntimeGestureHysteresisSideState& state) {
  const bool active = side.status == 1u || (side.flags & xr_runtime::HAND_POSE_VALID) != 0u;
  if (!active || (side.flags & xr_runtime::HAND_JOINTS_VALID) == 0u) {
    state.reset_hand_gestures();
    return;
  }

  bool derived_pinch = false;
  bool derived_grab = false;

  if ((side.flags & xr_runtime::HAND_PINCH_VALID) == 0u) {
    const float raw_pinch = derive_pinch_strength_from_runtime_hand_v2(side);
    if (raw_pinch >= 0.0f && std::isfinite(raw_pinch)) {
      const float pinch = apply_derived_gesture_response_curve(raw_pinch, derived_pinch_response_start);
      const bool pinch_active = update_gesture_hysteresis(state.pinch_active,
                                                          pinch,
                                                          derived_pinch_active_threshold,
                                                          derived_pinch_deactive_threshold);
      state.pinch_active = pinch_active;
      side.pinch_strength = pinch;
      side.pinch_active = pinch_active ? 1u : 0u;
      side.flags |= xr_runtime::HAND_PINCH_VALID;
      derived_pinch = true;
    }
  }

  if ((side.flags & xr_runtime::HAND_GRAB_VALID) == 0u) {
    const float raw_grab = derive_grab_strength_from_runtime_hand_v2(side);
    if (raw_grab >= 0.0f && std::isfinite(raw_grab)) {
      const float grab = apply_derived_gesture_response_curve(raw_grab, derived_grab_response_start);
      const bool pinch_suppresses_grab =
          ((side.flags & xr_runtime::HAND_PINCH_VALID) != 0u) &&
          side.pinch_strength >= derived_pinch_active_threshold &&
          grab < derived_grab_active_threshold;

      const float effective_grab = pinch_suppresses_grab ? std::min(grab, 0.45f) : grab;
      const bool grab_active = !pinch_suppresses_grab &&
          update_gesture_hysteresis(state.grab_active,
                                    effective_grab,
                                    derived_grab_active_threshold,
                                    derived_grab_deactive_threshold);
      state.grab_active = grab_active;
      side.grab_strength = effective_grab;
      side.grab_active = grab_active ? 1u : 0u;
      side.flags |= xr_runtime::HAND_GRAB_VALID;
      derived_grab = true;
    }
  }

  // Gesture conflict resolver for visually-derived gestures only.
  //
  // A real fist often puts thumb/index close together, so raw geometry may report
  // both pinch and grab. For VR controller semantics this is wrong: fist/grab
  // should map to grip, not to trigger+grip. Do not rewrite backend/controller
  // supplied gestures when this call did not derive either field.
  if ((derived_pinch || derived_grab) &&
      (side.flags & xr_runtime::HAND_PINCH_VALID) != 0u &&
      (side.flags & xr_runtime::HAND_GRAB_VALID) != 0u) {
    const bool grab_is_strong = side.grab_strength >= derived_grab_active_threshold || side.grab_active != 0u;
    const bool pinch_is_active = side.pinch_strength >= derived_pinch_active_threshold || side.pinch_active != 0u;

    if (grab_is_strong && pinch_is_active) {
      side.pinch_strength = 0.0f;
      side.pinch_active = 0u;
      state.pinch_active = false;
    }
  }
}


}  // namespace

void RuntimeGestureHysteresisSideState::reset_hand_gestures() {
  pinch_active = false;
  grab_active = false;
}

void RuntimeGestureHysteresisSideState::reset_extra_gestures() {
  thumbs_up_active = false;
  index_point_active = false;
  thumbs_up_hold_until_ns = 0;
  index_point_hold_until_ns = 0;
}

void RuntimeGestureHysteresisSideState::reset_all() {
  reset_hand_gestures();
  reset_extra_gestures();
}

void RuntimeGestureHysteresisState::reset_all() {
  left.reset_all();
  right.reset_all();
}

void RuntimeHandGestureSnapshot::reset() {
  valid = false;
  timestamp_ns = 0;
  left = RuntimeHandGestureSideSnapshot{};
  right = RuntimeHandGestureSideSnapshot{};
}

float apply_derived_gesture_response_curve(float raw_strength, float response_start) {
  const float raw = clamp01(raw_strength);
  const float start = std::max(0.0f, std::min(0.99f, response_start));
  if (raw <= start) return 0.0f;
  return clamp01((raw - start) / (1.0f - start));
}

float derive_pinch_strength_from_skeleton26(const xr_tracking::HandSkeleton26SideF32V1& src) {
  const auto& thumb = src.joints[xr_tracking::HAND26_THUMB_TIP];
  const auto& index = src.joints[xr_tracking::HAND26_INDEX_TIP];
  if (!skeleton_joint_position_valid(thumb) || !skeleton_joint_position_valid(index)) return 0.0f;
  const float d = dist3(thumb.px, thumb.py, thumb.pz, index.px, index.py, index.pz);
  // Strong pinch around <=2.5cm, fully open around >=7cm.  This is only a
  // fallback; controller override can replace it when available.
  return clamp01((0.070f - d) / (0.070f - 0.025f));
}

struct SkeletonFinger26 {
  uint32_t proximal = 0;
  uint32_t intermediate = 0;
  uint32_t distal = 0;
  uint32_t tip = 0;
};

float skeleton_finger_grab_curl_strength(const xr_tracking::HandSkeleton26SideF32V1& src,
                                         SkeletonFinger26 finger) {
  const auto& palm = src.joints[xr_tracking::HAND26_PALM];
  const auto& proximal = src.joints[finger.proximal];
  const auto& intermediate = src.joints[finger.intermediate];
  const auto& distal = src.joints[finger.distal];
  const auto& tip = src.joints[finger.tip];

  if (!skeleton_joint_position_valid(palm) ||
      !skeleton_joint_position_valid(proximal) ||
      !skeleton_joint_position_valid(intermediate) ||
      !skeleton_joint_position_valid(distal) ||
      !skeleton_joint_position_valid(tip)) {
    return -1.0f;
  }

  const float chain =
      dist3(proximal.px, proximal.py, proximal.pz,
            intermediate.px, intermediate.py, intermediate.pz) +
      dist3(intermediate.px, intermediate.py, intermediate.pz,
            distal.px, distal.py, distal.pz) +
      dist3(distal.px, distal.py, distal.pz, tip.px, tip.py, tip.pz);
  if (!std::isfinite(chain) || chain <= 1e-4f) {
    return -1.0f;
  }

  const float chord = dist3(proximal.px, proximal.py, proximal.pz,
                            tip.px, tip.py, tip.pz);
  const float chord_ratio = chord / chain;
  const float folded_by_chain = clamp01((0.92f - chord_ratio) / (0.92f - 0.70f));

  const float tip_palm = dist3(tip.px, tip.py, tip.pz, palm.px, palm.py, palm.pz);
  const float close_to_palm = clamp01((0.145f - tip_palm) / (0.145f - 0.085f));

  return clamp01(std::min(folded_by_chain, close_to_palm));
}

float derive_strict_grab_strength_from_skeleton26(const xr_tracking::HandSkeleton26SideF32V1& src) {
  const auto& palm = src.joints[xr_tracking::HAND26_PALM];
  if (!skeleton_joint_position_valid(palm)) return -1.0f;

  // Grip/grab should be based on the non-index fingers. Index participates in
  // pinch/trigger, so including index makes pinch look like grab.
  const SkeletonFinger26 fingers[] = {
      {xr_tracking::HAND26_MIDDLE_PROXIMAL,
       xr_tracking::HAND26_MIDDLE_INTERMEDIATE,
       xr_tracking::HAND26_MIDDLE_DISTAL,
       xr_tracking::HAND26_MIDDLE_TIP},
      {xr_tracking::HAND26_RING_PROXIMAL,
       xr_tracking::HAND26_RING_INTERMEDIATE,
       xr_tracking::HAND26_RING_DISTAL,
       xr_tracking::HAND26_RING_TIP},
      {xr_tracking::HAND26_LITTLE_PROXIMAL,
       xr_tracking::HAND26_LITTLE_INTERMEDIATE,
       xr_tracking::HAND26_LITTLE_DISTAL,
       xr_tracking::HAND26_LITTLE_TIP},
  };

  float sum = 0.0f;
  uint32_t count = 0;
  uint32_t strong_count = 0;
  for (const auto& finger : fingers) {
    const float curl = skeleton_finger_grab_curl_strength(src, finger);
    if (curl < 0.0f) continue;
    sum += curl;
    ++count;
    if (curl >= 0.70f) ++strong_count;
  }

  if (count < 2) return -1.0f;
  if (strong_count < 2) return 0.0f;
  return clamp01(sum / static_cast<float>(count));
}

float derive_grab_strength_from_skeleton26(const xr_tracking::HandSkeleton26SideF32V1& src) {
  const float grab = derive_strict_grab_strength_from_skeleton26(src);
  return grab >= 0.0f ? grab : 0.0f;
}


xr_runtime::HandSideF32V2 runtime_hand_side_v2_from_skeleton26(
    const xr_tracking::HandSkeleton26SideF32V1& src,
    bool left,
    bool derive_gestures,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start) {
  xr_runtime::HandSideF32V2 out{};
  out.handedness = left ? 1u : 2u;
  out.status = src.status;
  out.flags = 0;
  out.confidence = src.confidence;

  const bool active = src.status == xr_tracking::HAND_SKELETON26_STATUS_TRACKING ||
                      src.status == xr_tracking::HAND_SKELETON26_STATUS_DEGRADED;
  if (!active) {
    out.status = xr_tracking::HAND_SKELETON26_STATUS_NO_HAND;
    out.joint_count = 0;
    return out;
  }

  const auto& palm = src.joints[xr_tracking::HAND26_PALM];
  const auto& wrist = src.joints[xr_tracking::HAND26_WRIST];

  const auto* pose_position = skeleton_joint_position_valid(palm) ? &palm
                             : (skeleton_joint_position_valid(wrist) ? &wrist : nullptr);
  const auto* pose_orientation = skeleton_joint_orientation_valid(palm) ? &palm
                                : (skeleton_joint_orientation_valid(wrist) ? &wrist : nullptr);

  // Treat the runtime hand pose as valid only when both position and
  // orientation are present.  During Mercury reacquire we can temporarily get
  // tracked/position-valid joints with zero quaternions; marking that as
  // HAND_POSE_VALID makes the downstream coordinate transform turn zero
  // orientation into identity+offset, which looks like a crooked hand.
  if (pose_position && pose_orientation) {
    out.palm_px = pose_position->px;
    out.palm_py = pose_position->py;
    out.palm_pz = pose_position->pz;
    out.controller_px = pose_position->px;
    out.controller_py = pose_position->py;
    out.controller_pz = pose_position->pz;

    out.palm_qw = pose_orientation->qw;
    out.palm_qx = pose_orientation->qx;
    out.palm_qy = pose_orientation->qy;
    out.palm_qz = pose_orientation->qz;
    out.controller_qw = pose_orientation->qw;
    out.controller_qx = pose_orientation->qx;
    out.controller_qy = pose_orientation->qy;
    out.controller_qz = pose_orientation->qz;

    out.flags |= xr_runtime::HAND_POSE_VALID;
  }

  if (skeleton_joint_position_valid(wrist)) {
    out.wrist_px = wrist.px;
    out.wrist_py = wrist.py;
    out.wrist_pz = wrist.pz;
  }
  if (skeleton_joint_orientation_valid(wrist)) {
    out.wrist_qw = wrist.qw;
    out.wrist_qx = wrist.qx;
    out.wrist_qy = wrist.qy;
    out.wrist_qz = wrist.qz;
  }

  static constexpr uint32_t map26_to_21[xr_runtime::HAND_JOINT_COUNT_V2] = {
      xr_tracking::HAND26_WRIST,
      xr_tracking::HAND26_THUMB_METACARPAL,
      xr_tracking::HAND26_THUMB_PROXIMAL,
      xr_tracking::HAND26_THUMB_DISTAL,
      xr_tracking::HAND26_THUMB_TIP,
      xr_tracking::HAND26_INDEX_PROXIMAL,
      xr_tracking::HAND26_INDEX_INTERMEDIATE,
      xr_tracking::HAND26_INDEX_DISTAL,
      xr_tracking::HAND26_INDEX_TIP,
      xr_tracking::HAND26_MIDDLE_PROXIMAL,
      xr_tracking::HAND26_MIDDLE_INTERMEDIATE,
      xr_tracking::HAND26_MIDDLE_DISTAL,
      xr_tracking::HAND26_MIDDLE_TIP,
      xr_tracking::HAND26_RING_PROXIMAL,
      xr_tracking::HAND26_RING_INTERMEDIATE,
      xr_tracking::HAND26_RING_DISTAL,
      xr_tracking::HAND26_RING_TIP,
      xr_tracking::HAND26_LITTLE_PROXIMAL,
      xr_tracking::HAND26_LITTLE_INTERMEDIATE,
      xr_tracking::HAND26_LITTLE_DISTAL,
      xr_tracking::HAND26_LITTLE_TIP,
  };

  out.joint_count = xr_runtime::HAND_JOINT_COUNT_V2;
  for (uint32_t i = 0; i < xr_runtime::HAND_JOINT_COUNT_V2; ++i) {
    out.joints[i] = runtime_joint_from_skeleton26(src.joints[map26_to_21[i]], i);
  }
  out.flags |= xr_runtime::HAND_JOINTS_VALID;

  apply_skeleton26_gestures_to_runtime_side(out,
                                            src,
                                            derive_gestures,
                                            derived_pinch_active_threshold,
                                            derived_grab_active_threshold,
                                            derived_pinch_response_start,
                                            derived_grab_response_start);
  return out;
}

xr_runtime::HandTrackingFrameF32V2 runtime_hand_v2_from_skeleton26(
    const xr_tracking::HandSkeleton26FrameF32V1& src,
    bool derive_gestures,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start) {
  xr_runtime::HandTrackingFrameF32V2 out{};
  out.version = xr_runtime::HAND_TRACKING_FORMAT_VERSION_V2;
  out.size_bytes = sizeof(xr_runtime::HandTrackingFrameF32V2);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.hand_count > 0
      ? xr_tracking::HAND_SKELETON26_STATUS_TRACKING
      : xr_tracking::HAND_SKELETON26_STATUS_NO_HAND;
  out.flags = 0;
  out.confidence = 0.0f;
  out.hand_count = src.hand_count;

  if ((src.flags & xr_tracking::HAND_SKELETON26_FRAME_LEFT_VALID) != 0u) {
    out.left = runtime_hand_side_v2_from_skeleton26(src.left,
                                                    true,
                                                    derive_gestures,
                                                    derived_pinch_active_threshold,
                                                    derived_grab_active_threshold,
                                                    derived_pinch_response_start,
                                                    derived_grab_response_start);
    out.flags |= xr_runtime::HAND_FLAG_LEFT_VALID;
    out.confidence = std::max(out.confidence, out.left.confidence);
  } else {
    out.left.handedness = 1;
  }
  if ((src.flags & xr_tracking::HAND_SKELETON26_FRAME_RIGHT_VALID) != 0u) {
    out.right = runtime_hand_side_v2_from_skeleton26(src.right,
                                                     false,
                                                     derive_gestures,
                                                     derived_pinch_active_threshold,
                                                     derived_grab_active_threshold,
                                                     derived_pinch_response_start,
                                                     derived_grab_response_start);
    out.flags |= xr_runtime::HAND_FLAG_RIGHT_VALID;
    out.confidence = std::max(out.confidence, out.right.confidence);
  } else {
    out.right.handedness = 2;
  }
  if ((out.left.flags & xr_runtime::HAND_JOINTS_VALID) != 0u ||
      (out.right.flags & xr_runtime::HAND_JOINTS_VALID) != 0u) {
    out.flags |= xr_runtime::HAND_FLAG_JOINTS_VALID;
  }
  if ((out.left.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) != 0u ||
      (out.right.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) != 0u) {
    out.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
  }
  return out;
}

xr_runtime::HandSideF32V2 runtime_hand_side_v2_from_runtime_v1(
    const xr_runtime::HandSideF64V1& src,
    bool left) {
  xr_runtime::HandSideF32V2 out{};
  out.handedness = left ? 1u : 2u;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.controller_px = static_cast<float>(src.palm_px);
  out.controller_py = static_cast<float>(src.palm_py);
  out.controller_pz = static_cast<float>(src.palm_pz);
  out.controller_qw = static_cast<float>(src.palm_qw);
  out.controller_qx = static_cast<float>(src.palm_qx);
  out.controller_qy = static_cast<float>(src.palm_qy);
  out.controller_qz = static_cast<float>(src.palm_qz);
  out.palm_px = static_cast<float>(src.palm_px);
  out.palm_py = static_cast<float>(src.palm_py);
  out.palm_pz = static_cast<float>(src.palm_pz);
  out.palm_qw = static_cast<float>(src.palm_qw);
  out.palm_qx = static_cast<float>(src.palm_qx);
  out.palm_qy = static_cast<float>(src.palm_qy);
  out.palm_qz = static_cast<float>(src.palm_qz);
  out.wrist_px = static_cast<float>(src.wrist_px);
  out.wrist_py = static_cast<float>(src.wrist_py);
  out.wrist_pz = static_cast<float>(src.wrist_pz);
  out.wrist_qw = static_cast<float>(src.wrist_qw);
  out.wrist_qx = static_cast<float>(src.wrist_qx);
  out.wrist_qy = static_cast<float>(src.wrist_qy);
  out.wrist_qz = static_cast<float>(src.wrist_qz);
  out.vx = static_cast<float>(src.vx);
  out.vy = static_cast<float>(src.vy);
  out.vz = static_cast<float>(src.vz);
  out.wx = static_cast<float>(src.wx);
  out.wy = static_cast<float>(src.wy);
  out.wz = static_cast<float>(src.wz);
  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;
  out.joint_count = 0;
  return out;
}

xr_runtime::HandTrackingFrameF32V2 runtime_hand_v2_from_runtime_v1(
    const xr_runtime::HandTrackingFrameF64V1& src) {
  xr_runtime::HandTrackingFrameF32V2 out{};
  out.version = xr_runtime::HAND_TRACKING_FORMAT_VERSION_V2;
  out.size_bytes = sizeof(xr_runtime::HandTrackingFrameF32V2);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = runtime_hand_side_v2_from_runtime_v1(src.left, true);
  out.right = runtime_hand_side_v2_from_runtime_v1(src.right, false);
  return out;
}

void apply_skeleton26_gestures_to_runtime_side(
    xr_runtime::HandSideF32V2& out,
    const xr_tracking::HandSkeleton26SideF32V1& src,
    bool derive_gestures,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start) {
  if ((src.flags & xr_tracking::HAND_SKELETON26_SIDE_PINCH_VALID) != 0u) {
    out.pinch_strength = clamp01(src.pinch_strength);
    out.pinch_active = src.pinch_active;
    out.flags |= xr_runtime::HAND_PINCH_VALID;
  } else if (derive_gestures) {
    const float raw_pinch = derive_pinch_strength_from_skeleton26(src);
    out.pinch_strength = apply_derived_gesture_response_curve(raw_pinch, derived_pinch_response_start);
    out.pinch_active = out.pinch_strength >= derived_pinch_active_threshold ? 1u : 0u;
    out.flags |= xr_runtime::HAND_PINCH_VALID;
  }

  if ((src.flags & xr_tracking::HAND_SKELETON26_SIDE_GRAB_VALID) != 0u) {
    const float source_grab = clamp01(src.grab_strength);
    const float geometry_grab = derive_strict_grab_strength_from_skeleton26(src);

    // Mercury/backend grab is useful, but it can mark relaxed/open curved fingers
    // as grab. If the 26-joint geometry is available and does not show at least
    // two truly folded non-index fingers, suppress the backend grab before it
    // reaches SteamVR grip.
    if (geometry_grab >= 0.0f && geometry_grab < 0.70f) {
      out.grab_strength = std::min(source_grab, 0.45f);
      out.grab_active = 0u;
    } else {
      out.grab_strength = geometry_grab >= 0.0f ? std::min(source_grab, geometry_grab) : source_grab;
      out.grab_active = src.grab_active;
    }
    out.flags |= xr_runtime::HAND_GRAB_VALID;
  } else if (derive_gestures) {
    const float raw_grab = derive_grab_strength_from_skeleton26(src);
    out.grab_strength = apply_derived_gesture_response_curve(raw_grab, derived_grab_response_start);

    const bool pinch_suppresses_grab =
        ((out.flags & xr_runtime::HAND_PINCH_VALID) != 0u) &&
        out.pinch_strength >= derived_pinch_active_threshold &&
        out.grab_strength < derived_grab_active_threshold;

    if (pinch_suppresses_grab) {
      out.grab_strength = std::min(out.grab_strength, 0.45f);
      out.grab_active = 0u;
    } else {
      out.grab_active = out.grab_strength >= derived_grab_active_threshold ? 1u : 0u;
    }

    out.flags |= xr_runtime::HAND_GRAB_VALID;
  }
}

void derive_missing_runtime_hand_v2_gestures(
    xr_runtime::HandTrackingFrameF32V2& hand,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_deactive_threshold,
    float derived_grab_deactive_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start,
    RuntimeGestureHysteresisState& state,
    bool left_gestures_enabled,
    bool right_gestures_enabled) {
  if (hand.sequence == 0) {
    state.left.reset_hand_gestures();
    state.right.reset_hand_gestures();
    return;
  }
  if (state.reset_counter != hand.reset_counter) {
    state.reset_all();
    state.reset_counter = hand.reset_counter;
  }

  if (left_gestures_enabled &&
      ((hand.flags & xr_runtime::HAND_FLAG_LEFT_VALID) != 0u || hand.left.status != 0 || hand.left.handedness == 1u)) {
    derive_missing_runtime_hand_side_gestures(hand.left,
                                             derived_pinch_active_threshold,
                                             derived_grab_active_threshold,
                                             derived_pinch_deactive_threshold,
                                             derived_grab_deactive_threshold,
                                             derived_pinch_response_start,
                                             derived_grab_response_start,
                                             state.left);
  } else {
    state.left.reset_hand_gestures();
  }
  if (right_gestures_enabled &&
      ((hand.flags & xr_runtime::HAND_FLAG_RIGHT_VALID) != 0u || hand.right.status != 0 || hand.right.handedness == 2u)) {
    derive_missing_runtime_hand_side_gestures(hand.right,
                                              derived_pinch_active_threshold,
                                              derived_grab_active_threshold,
                                              derived_pinch_deactive_threshold,
                                              derived_grab_deactive_threshold,
                                              derived_pinch_response_start,
                                              derived_grab_response_start,
                                              state.right);
  } else {
    state.right.reset_hand_gestures();
  }

  if ((hand.left.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) != 0u ||
      (hand.right.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) != 0u) {
    hand.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
  }
}

void derive_runtime_hand_v2_extra_gesture_buttons(
    xr_runtime::HandTrackingFrameF32V2& hand,
    uint32_t thumbs_up_button,
    uint32_t index_point_button,
    float thumbs_up_active_threshold,
    float index_point_active_threshold,
    float thumbs_up_deactive_threshold,
    float index_point_deactive_threshold,
    float response_start,
    double extra_gesture_hold_ms,
    int64_t now_ns,
    RuntimeGestureHysteresisState& state,
    bool left_gestures_enabled,
    bool right_gestures_enabled) {
  if (hand.sequence == 0) {
    state.left.reset_extra_gestures();
    state.right.reset_extra_gestures();
    return;
  }

  if (left_gestures_enabled &&
      ((hand.flags & xr_runtime::HAND_FLAG_LEFT_VALID) != 0u || hand.left.status != 0 || hand.left.handedness == 1u)) {
    derive_runtime_hand_side_extra_gesture_buttons(hand.left,
                                                   thumbs_up_button,
                                                   index_point_button,
                                                   thumbs_up_active_threshold,
                                                   index_point_active_threshold,
                                                   thumbs_up_deactive_threshold,
                                                   index_point_deactive_threshold,
                                                   response_start,
                                                   extra_gesture_hold_ms,
                                                   now_ns,
                                                   state.left);
  } else {
    state.left.reset_extra_gestures();
  }
  if (right_gestures_enabled &&
      ((hand.flags & xr_runtime::HAND_FLAG_RIGHT_VALID) != 0u || hand.right.status != 0 || hand.right.handedness == 2u)) {
    derive_runtime_hand_side_extra_gesture_buttons(hand.right,
                                                   thumbs_up_button,
                                                   index_point_button,
                                                   thumbs_up_active_threshold,
                                                   index_point_active_threshold,
                                                   thumbs_up_deactive_threshold,
                                                   index_point_deactive_threshold,
                                                   response_start,
                                                   extra_gesture_hold_ms,
                                                   now_ns,
                                                   state.right);
  } else {
    state.right.reset_extra_gestures();
  }
}


namespace {

RuntimeHandGestureSideSnapshot capture_side_gesture_snapshot(
    const xr_runtime::HandSideF32V2& side) {
  RuntimeHandGestureSideSnapshot out{};
  out.valid_flags = side.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID);
  out.pinch_strength = side.pinch_strength;
  out.grab_strength = side.grab_strength;
  out.pinch_active = side.pinch_active;
  out.grab_active = side.grab_active;
  out.valid = out.valid_flags != 0u || out.pinch_active != 0u || out.grab_active != 0u ||
              out.pinch_strength > 0.0f || out.grab_strength > 0.0f;
  return out;
}

void clear_side_gesture_fields(xr_runtime::HandSideF32V2& side) {
  side.pinch_strength = 0.0f;
  side.grab_strength = 0.0f;
  side.pinch_active = 0u;
  side.grab_active = 0u;
  side.flags &= ~(xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID);
}

void apply_side_gesture_snapshot(xr_runtime::HandSideF32V2& side,
                                 const RuntimeHandGestureSideSnapshot& snapshot) {
  clear_side_gesture_fields(side);
  if (!snapshot.valid) return;

  side.pinch_strength = snapshot.pinch_strength;
  side.grab_strength = snapshot.grab_strength;
  side.pinch_active = snapshot.pinch_active;
  side.grab_active = snapshot.grab_active;
  side.flags |= snapshot.valid_flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID);
}

bool hand_has_side_gestures(const xr_runtime::HandSideF32V2& side) {
  return (side.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) != 0u ||
         side.pinch_active != 0u ||
         side.grab_active != 0u ||
         side.pinch_strength > 0.0f ||
         side.grab_strength > 0.0f;
}

}  // namespace

void capture_runtime_hand_v2_gesture_snapshot(
    RuntimeHandGestureSnapshot& snapshot,
    const xr_runtime::HandTrackingFrameF32V2& hand,
    int64_t now_ns,
    bool left_gestures_enabled,
    bool right_gestures_enabled) {
  snapshot.left = left_gestures_enabled ? capture_side_gesture_snapshot(hand.left)
                                        : RuntimeHandGestureSideSnapshot{};
  snapshot.right = right_gestures_enabled ? capture_side_gesture_snapshot(hand.right)
                                          : RuntimeHandGestureSideSnapshot{};
  snapshot.timestamp_ns = now_ns;
  snapshot.valid = snapshot.left.valid || snapshot.right.valid;
}

void apply_runtime_hand_v2_gesture_latch_or_clear(
    xr_runtime::HandTrackingFrameF32V2& hand,
    const RuntimeHandGestureSnapshot& snapshot,
    int64_t now_ns,
    double latch_ms,
    bool left_gestures_enabled,
    bool right_gestures_enabled) {
  bool use_latch = false;
  if (snapshot.valid && snapshot.timestamp_ns > 0 && latch_ms > 0.0) {
    const int64_t latch_ns = static_cast<int64_t>(latch_ms * 1000000.0);
    use_latch = now_ns >= snapshot.timestamp_ns &&
                now_ns - snapshot.timestamp_ns <= latch_ns;
  }

  if (use_latch) {
    if (left_gestures_enabled) apply_side_gesture_snapshot(hand.left, snapshot.left);
    if (right_gestures_enabled) apply_side_gesture_snapshot(hand.right, snapshot.right);
  } else {
    if (left_gestures_enabled) clear_side_gesture_fields(hand.left);
    if (right_gestures_enabled) clear_side_gesture_fields(hand.right);
  }

  if (hand_has_side_gestures(hand.left) || hand_has_side_gestures(hand.right)) {
    hand.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
  } else {
    hand.flags &= ~xr_runtime::HAND_FLAG_GESTURES_VALID;
  }
}

void clear_runtime_hand_v2_backend_gestures_by_side(
    xr_runtime::HandTrackingFrameF32V2& hand,
    bool clear_left,
    bool clear_right) {
  auto clear_side = [](xr_runtime::HandSideF32V2& side) {
    side.pinch_strength = 0.0f;
    side.grab_strength = 0.0f;
    side.pinch_active = 0u;
    side.grab_active = 0u;
    side.flags &= ~(xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID);
    side.reserved0 &= ~override_controller::runtime_controller_button_mask();
  };

  if (clear_left) clear_side(hand.left);
  if (clear_right) clear_side(hand.right);
  if ((hand.left.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) == 0u &&
      (hand.right.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) == 0u &&
      (hand.left.reserved0 & override_controller::runtime_controller_button_mask()) == 0u &&
      (hand.right.reserved0 & override_controller::runtime_controller_button_mask()) == 0u) {
    hand.flags &= ~xr_runtime::HAND_FLAG_GESTURES_VALID;
  }
}

void clear_runtime_hand_v2_backend_gestures(xr_runtime::HandTrackingFrameF32V2& hand) {
  clear_runtime_hand_v2_backend_gestures_by_side(hand, true, true);
}

}  // namespace xr_runtime_adapter::gestures
