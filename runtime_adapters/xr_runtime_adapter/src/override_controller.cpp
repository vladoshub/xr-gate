#include "override_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace xr_runtime_adapter::override_controller {
namespace {

float clamp01(float v) {
  if (!std::isfinite(v)) return 0.0f;
  return std::max(0.0f, std::min(1.0f, v));
}

float clamp_axis(float v) {
  if (!std::isfinite(v)) return 0.0f;
  return std::max(-1.0f, std::min(1.0f, v));
}

uint64_t normalize_controller_dpad_buttons(uint64_t buttons) {
  // Treat the dedicated D-pad center bit as a thumbstick/trackpad click too.
  // This lets existing OpenVR/Monado consumers keep using thumbstick-click as
  // select/center while preserving the explicit dpad_center bit for future bindings.
  if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_CENTER) != 0ull) {
    buttons |= xr_runtime::CONTROLLER_BUTTON_THUMBSTICK;
  }
  return buttons;
}

float controller_axis_or_button(float axis, uint64_t buttons, uint64_t button_bit) {
  return std::max(clamp01(axis), (buttons & button_bit) != 0ull ? 1.0f : 0.0f);
}

bool controller_side_is_present(const xr_runtime::ControllerDeviceStateV2& controller) {
  return controller.status == xr_runtime::CONTROLLER_INPUT_ACTIVE ||
         controller.status == xr_runtime::CONTROLLER_INPUT_CONNECTED;
}

bool controller_side_has_input(const xr_runtime::ControllerDeviceStateV2& controller) {
  if (!controller_side_is_present(controller)) return false;
  if ((controller.flags & (xr_runtime::CONTROLLER_DEVICE_BUTTONS_VALID |
                           xr_runtime::CONTROLLER_DEVICE_ANALOG_VALID)) != 0u) {
    return true;
  }
  return controller.buttons != 0 ||
         std::abs(controller.trigger) > 0.0001f ||
         std::abs(controller.grip) > 0.0001f ||
         std::abs(controller.thumbstick_x) > 0.0001f ||
         std::abs(controller.thumbstick_y) > 0.0001f;
}

bool controller_side_has_nonzero_input(const xr_runtime::ControllerDeviceStateV2& controller) {
  if (!controller_side_is_present(controller)) return false;
  const uint64_t buttons = normalize_controller_dpad_buttons(controller.buttons);
  return buttons != 0 ||
         std::abs(controller.trigger) > 0.0001f ||
         std::abs(controller.grip) > 0.0001f ||
         std::abs(controller.thumbstick_x) > 0.0001f ||
         std::abs(controller.thumbstick_y) > 0.0001f;
}

void copy_debug_source(xr_runtime::RuntimeControllerSideStateV1& out, const char* source) {
  std::memset(out.debug_source, 0, sizeof(out.debug_source));
  if (!source) return;
  std::strncpy(out.debug_source, source, sizeof(out.debug_source) - 1);
}

struct Qf {
  float w = 1.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

Qf normalize_q(Qf q) {
  const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (!std::isfinite(n) || n <= 0.0f) return {};
  q.w /= n;
  q.x /= n;
  q.y /= n;
  q.z /= n;
  return q;
}

Qf q_conj(Qf q) { return {q.w, -q.x, -q.y, -q.z}; }

Qf q_mul_raw(Qf a, Qf b) {
  return {
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

Qf q_mul(Qf a, Qf b) {
  return normalize_q(q_mul_raw(a, b));
}

void q_rotate(Qf q, const float in[3], float out[3]) {
  q = normalize_q(q);
  const Qf p{0.0f, in[0], in[1], in[2]};
  const Qf r = q_mul_raw(q_mul_raw(q, p), q_conj(q));
  out[0] = r.x;
  out[1] = r.y;
  out[2] = r.z;
}

Qf q_from_xyzw(const float q_xyzw[4]) {
  return normalize_q({q_xyzw[3], q_xyzw[0], q_xyzw[1], q_xyzw[2]});
}

void set_orientation_xyzw(xr_runtime::RuntimeControllerSideStateV1& out, Qf q) {
  q = normalize_q(q);
  out.orientation_xyzw[0] = q.x;
  out.orientation_xyzw[1] = q.y;
  out.orientation_xyzw[2] = q.z;
  out.orientation_xyzw[3] = q.w;
}

bool hand_side_is_valid(const xr_runtime::HandTrackingFrameF32V2& hand,
                        const xr_runtime::HandSideF32V2& side,
                        bool left) {
  const uint32_t expected_handedness = left ? 1u : 2u;
  const uint32_t frame_flag = left ? xr_runtime::HAND_FLAG_LEFT_VALID
                                   : xr_runtime::HAND_FLAG_RIGHT_VALID;
  if (side.handedness != expected_handedness) return false;
  if ((hand.flags & frame_flag) == 0u) return false;
  if ((side.flags & xr_runtime::HAND_POSE_VALID) == 0u) return false;
  if (side.status != 1u && side.status != 3u) return false;
  return side.confidence > 0.0f;
}

void fill_pose_from_hand(xr_runtime::RuntimeControllerSideStateV1& out,
                         const xr_runtime::HandSideF32V2& side,
                         bool static_orientation,
                         const float static_q_xyzw[4]) {
  out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED |
               xr_runtime::RUNTIME_CONTROLLER_POSE_VALID |
               xr_runtime::RUNTIME_CONTROLLER_TRACKED;
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_POSITION;
  out.last_pose_ns = 0;
  out.position[0] = side.controller_px;
  out.position[1] = side.controller_py;
  out.position[2] = side.controller_pz;

  if (static_orientation) {
    set_orientation_xyzw(out, q_from_xyzw(static_q_xyzw));
    out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_STATIC_ORIENTATION;
  } else {
    set_orientation_xyzw(out, {side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz});
    out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_ORIENTATION;
  }

  if ((side.flags & xr_runtime::HAND_LINEAR_VELOCITY_VALID) != 0u) {
    out.linear_velocity[0] = side.vx;
    out.linear_velocity[1] = side.vy;
    out.linear_velocity[2] = side.vz;
  }
  if ((side.flags & xr_runtime::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
    out.angular_velocity[0] = side.wx;
    out.angular_velocity[1] = side.wy;
    out.angular_velocity[2] = side.wz;
  }
}

void fill_pose_from_hmd_relative(xr_runtime::RuntimeControllerSideStateV1& out,
                                 const xr_runtime::HmdPoseF64V1& hmd,
                                 const float offset[3],
                                 const float static_q_xyzw[4]) {
  if ((hmd.flags & xr_runtime::HMD_FLAG_POSE_VALID) == 0u || hmd.tracking_status != 2u) {
    out.flags |= xr_runtime::RUNTIME_CONTROLLER_POSE_INVALID;
    out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_POSE_INVALID;
    return;
  }

  const Qf hmd_q = normalize_q({static_cast<float>(hmd.qw),
                                static_cast<float>(hmd.qx),
                                static_cast<float>(hmd.qy),
                                static_cast<float>(hmd.qz)});
  float rotated_offset[3]{};
  q_rotate(hmd_q, offset, rotated_offset);

  out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED |
               xr_runtime::RUNTIME_CONTROLLER_POSE_VALID |
               xr_runtime::RUNTIME_CONTROLLER_TRACKED |
               xr_runtime::RUNTIME_CONTROLLER_SYNTHETIC_POSE |
               xr_runtime::RUNTIME_CONTROLLER_HMD_RELATIVE;
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_HMD_RELATIVE_POSE |
                     xr_runtime::RUNTIME_CONTROLLER_SOURCE_SYNTHETIC_POSE |
                     xr_runtime::RUNTIME_CONTROLLER_SOURCE_STATIC_ORIENTATION;
  out.position[0] = static_cast<float>(hmd.px) + rotated_offset[0];
  out.position[1] = static_cast<float>(hmd.py) + rotated_offset[1];
  out.position[2] = static_cast<float>(hmd.pz) + rotated_offset[2];

  set_orientation_xyzw(out, q_mul(hmd_q, q_from_xyzw(static_q_xyzw)));
}

void mark_pose_invalid(xr_runtime::RuntimeControllerSideStateV1& out) {
  out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED |
               xr_runtime::RUNTIME_CONTROLLER_POSE_INVALID;
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_POSE_INVALID;
}

bool should_use_lost_hand_hmd_relative_fallback(
    LostHandPoseFallbackMode mode,
    const xr_runtime::ControllerDeviceStateV2* controller_side) {
  switch (mode) {
    case LostHandPoseFallbackMode::PoseInvalid:
      return false;
    case LostHandPoseFallbackMode::HmdRelativeWithControllerInput:
      return controller_side != nullptr && controller_side_is_present(*controller_side);
    case LostHandPoseFallbackMode::HmdRelative:
      return true;
  }
  return false;
}

void apply_dpad_to_thumbstick(uint64_t buttons, float& x, float& y) {
  if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT) != 0ull) x = -1.0f;
  if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT) != 0ull) x = 1.0f;
  if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_UP) != 0ull) y = 1.0f;
  if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN) != 0ull) y = -1.0f;
}

void fill_inputs_from_controller(xr_runtime::RuntimeControllerSideStateV1& out,
                                 const xr_runtime::ControllerDeviceStateV2& controller,
                                 const RuntimeControllerSynthesisConfig& cfg) {
  if (!controller_side_is_present(controller)) return;
  const uint64_t buttons = normalize_controller_dpad_buttons(controller.buttons);
  out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED;
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
  out.source_device_hash = controller.stable_device_hash != 0 ? controller.stable_device_hash
                                                              : controller.physical_device_hash;
  out.last_input_ns = 0;
  out.buttons = buttons & xr_runtime::CONTROLLER_BUTTON_KNOWN_MASK;
  out.touches = controller.touches;
  out.changed_buttons = controller.changed_buttons;
  out.trigger = controller_axis_or_button(controller.trigger, buttons, xr_runtime::CONTROLLER_BUTTON_TRIGGER);
  out.grip = controller_axis_or_button(controller.grip, buttons, xr_runtime::CONTROLLER_BUTTON_GRIP);
  out.thumbstick_x = clamp_axis(controller.thumbstick_x);
  out.thumbstick_y = clamp_axis(controller.thumbstick_y);
  if (cfg.dpad_to_thumbstick_axes) {
    apply_dpad_to_thumbstick(buttons, out.thumbstick_x, out.thumbstick_y);
  }
  std::memcpy(out.press_counters, controller.press_counters, sizeof(out.press_counters));
  std::memcpy(out.release_counters, controller.release_counters, sizeof(out.release_counters));
  if (controller_side_has_input(controller)) {
    out.flags |= xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  }
}

void merge_hand_gestures(xr_runtime::RuntimeControllerSideStateV1& out,
                         const xr_runtime::HandSideF32V2& side) {
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_GESTURES;
  if ((side.flags & xr_runtime::HAND_PINCH_VALID) != 0u) {
    out.trigger = std::max(out.trigger, clamp01(side.pinch_strength));
    if (side.pinch_active != 0u) {
      out.buttons |= xr_runtime::CONTROLLER_BUTTON_TRIGGER;
    }
  }
  if ((side.flags & xr_runtime::HAND_GRAB_VALID) != 0u) {
    out.grip = std::max(out.grip, clamp01(side.grab_strength));
    if (side.grab_active != 0u) {
      out.buttons |= xr_runtime::CONTROLLER_BUTTON_GRIP;
    }
  }
  out.buttons |= static_cast<uint64_t>(side.reserved0) & xr_runtime::CONTROLLER_BUTTON_KNOWN_MASK;
  if (out.trigger > 0.0f || out.grip > 0.0f || out.buttons != 0ull) {
    out.flags |= xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  }
}

void compose_side(xr_runtime::RuntimeControllerSideStateV1& out,
                  bool left,
                  const RuntimeControllerSynthesisConfig& cfg,
                  const xr_runtime::HandTrackingFrameF32V2* hand,
                  const xr_runtime::ControllerInputV2* controller_input,
                  const xr_runtime::HmdPoseF64V1* hmd) {
  out.role = left ? xr_runtime::CONTROLLER_SIDE_LEFT : xr_runtime::CONTROLLER_SIDE_RIGHT;
  copy_debug_source(out, xr_runtime::runtime_controller_mode_name(cfg.mode));

  const xr_runtime::HandSideF32V2* hand_side = nullptr;
  bool valid_hand_side = false;
  if (hand != nullptr) {
    hand_side = left ? &hand->left : &hand->right;
    valid_hand_side = hand_side_is_valid(*hand, *hand_side, left);
  }

  const xr_runtime::ControllerDeviceStateV2* controller_side = nullptr;
  if (controller_input != nullptr) {
    controller_side = left ? &controller_input->left : &controller_input->right;
  }

  const float* static_q = left ? cfg.left_static_orientation_xyzw : cfg.right_static_orientation_xyzw;
  const float* hmd_offset = left ? cfg.left_hmd_relative_offset_m : cfg.right_hmd_relative_offset_m;

  const bool use_hmd_relative_lost_hand_fallback =
      hmd != nullptr &&
      should_use_lost_hand_hmd_relative_fallback(cfg.lost_hand_pose_fallback, controller_side);

  switch (cfg.mode) {
    case xr_runtime::RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY:
    case xr_runtime::RuntimeControllerMode::HAND_TRACKING_CONTROLLER_BUTTONS_ONLY:
      if (valid_hand_side) fill_pose_from_hand(out, *hand_side, false, static_q);
      else if (use_hmd_relative_lost_hand_fallback) fill_pose_from_hmd_relative(out, *hmd, hmd_offset, static_q);
      else mark_pose_invalid(out);
      break;
    case xr_runtime::RuntimeControllerMode::HAND_POSITION_CONTROLLER_BUTTONS_STATIC_ORIENTATION:
      if (valid_hand_side) fill_pose_from_hand(out, *hand_side, true, static_q);
      else if (use_hmd_relative_lost_hand_fallback) fill_pose_from_hmd_relative(out, *hmd, hmd_offset, static_q);
      else mark_pose_invalid(out);
      break;
    case xr_runtime::RuntimeControllerMode::CONTROLLER_ONLY_HMD_RELATIVE_POSE:
      if (hmd != nullptr) fill_pose_from_hmd_relative(out, *hmd, hmd_offset, static_q);
      else mark_pose_invalid(out);
      break;
    case xr_runtime::RuntimeControllerMode::CONTROLLER_ONLY_POSE_INVALID:
      mark_pose_invalid(out);
      break;
  }

  if (controller_side != nullptr) {
    fill_inputs_from_controller(out, *controller_side, cfg);
  }

  const bool hand_gestures_enabled = left ? cfg.left_hand_gestures_enabled
                                          : cfg.right_hand_gestures_enabled;
  if (cfg.mode == xr_runtime::RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY &&
      valid_hand_side && hand_gestures_enabled &&
      !(controller_side != nullptr && controller_side_is_present(*controller_side))) {
    // Hand gestures are allowed only when no external controller side is present.
    // With override_controller active, external ControllerInputV2 is authoritative;
    // otherwise palm/pinch/grab gestures can leak as trigger/grip/buttons and cause
    // phantom clicks or discrete movement pulses in SteamVR.
    merge_hand_gestures(out, *hand_side);
  }

  if ((out.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) == 0u &&
      controller_side != nullptr && controller_side_is_present(*controller_side)) {
    out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED;
  }
}

}  // namespace

LostHandPoseFallbackMode parse_lost_hand_pose_fallback_mode(const std::string& value,
                                                                   const char* option_name) {
  if (value == "pose_invalid" || value == "invalid" || value == "off" || value == "none") {
    return LostHandPoseFallbackMode::PoseInvalid;
  }
  if (value == "hmd_relative_with_controller_input" ||
      value == "hmd_relative_with_input" ||
      value == "hmd_relative_on_input" ||
      value == "hmd_relative_when_input") {
    return LostHandPoseFallbackMode::HmdRelativeWithControllerInput;
  }
  if (value == "hmd_relative" || value == "body_locked" || value == "body") {
    return LostHandPoseFallbackMode::HmdRelative;
  }
  throw std::runtime_error(std::string(option_name) +
                           " must be one of: pose_invalid, hmd_relative_with_input, hmd_relative");
}

const char* lost_hand_pose_fallback_mode_name(LostHandPoseFallbackMode mode) {
  switch (mode) {
    case LostHandPoseFallbackMode::PoseInvalid:
      return "pose_invalid";
    case LostHandPoseFallbackMode::HmdRelativeWithControllerInput:
      return "hmd_relative_with_input";
    case LostHandPoseFallbackMode::HmdRelative:
      return "hmd_relative";
  }
  return "unknown";
}

uint32_t runtime_controller_button_mask() {
  return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_TRIGGER |
                               xr_runtime::CONTROLLER_BUTTON_GRIP |
                               xr_runtime::CONTROLLER_BUTTON_MENU |
                               xr_runtime::CONTROLLER_BUTTON_A |
                               xr_runtime::CONTROLLER_BUTTON_B |
                               xr_runtime::CONTROLLER_BUTTON_THUMBSTICK |
                               xr_runtime::CONTROLLER_BUTTON_DPAD_UP |
                               xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN |
                               xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT |
                               xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT |
                               xr_runtime::CONTROLLER_BUTTON_DPAD_CENTER |
                               xr_runtime::CONTROLLER_BUTTON_X |
                               xr_runtime::CONTROLLER_BUTTON_Y |
                               xr_runtime::CONTROLLER_BUTTON_SYSTEM);
}

uint32_t parse_runtime_button_target(const std::string& value,
                                     const char* option_name) {
  if (value == "none" || value == "off" || value == "disabled") return 0u;
  if (value == "trigger") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_TRIGGER);
  if (value == "grip" || value == "grab") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_GRIP);
  if (value == "menu") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_MENU);
  if (value == "a" || value == "button_a") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_A);
  if (value == "b" || value == "button_b") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_B);
  if (value == "x" || value == "button_x") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_X);
  if (value == "y" || value == "button_y") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_Y);
  if (value == "system") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_SYSTEM);
  if (value == "thumbstick" || value == "thumbstick_click") return static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_THUMBSTICK);
  if (value == "dpad_up" || value == "dpad-up" || value == "up") return RUNTIME_BUTTON_DPAD_UP;
  if (value == "dpad_down" || value == "dpad-down" || value == "down") return RUNTIME_BUTTON_DPAD_DOWN;
  if (value == "dpad_left" || value == "dpad-left" || value == "left") return RUNTIME_BUTTON_DPAD_LEFT;
  if (value == "dpad_right" || value == "dpad-right" || value == "right") return RUNTIME_BUTTON_DPAD_RIGHT;
  if (value == "dpad_center" || value == "dpad-center" ||
      value == "dpad_press" || value == "dpad-press" ||
      value == "center") {
    return RUNTIME_BUTTON_DPAD_CENTER;
  }
  throw std::runtime_error(std::string(option_name) +
                           " must be one of: none, trigger, grip, menu, a, b, x, y, system, thumbstick, "
                           "dpad_up, dpad_down, dpad_left, dpad_right, dpad_center");
}

uint32_t controller_buttons_to_runtime_mask(uint64_t buttons) {
  return static_cast<uint32_t>(normalize_controller_dpad_buttons(buttons)) & runtime_controller_button_mask();
}

bool controller_device_is_present(const xr_runtime::ControllerDeviceStateV2& controller) {
  return controller_side_is_present(controller);
}

bool controller_device_has_nonzero_input(const xr_runtime::ControllerDeviceStateV2& controller) {
  return controller_side_has_nonzero_input(controller);
}

bool controller_input_has_present_controller(const xr_runtime::ControllerInputV2& controller) {
  return controller_device_is_present(controller.left) ||
         controller_device_is_present(controller.right);
}

bool controller_input_has_nonzero_input(const xr_runtime::ControllerInputV2& controller) {
  return controller_device_has_nonzero_input(controller.left) ||
         controller_device_has_nonzero_input(controller.right);
}

void apply_controller_gesture_override(
    xr_runtime::HandTrackingFrameF32V2& hand,
    const xr_runtime::ControllerInputV2& controller,
    xr_runtime::ControllerInputConflictPolicy policy,
    float trigger_pinch_threshold,
    float grip_grab_threshold) {
  auto apply_side = [&](xr_runtime::HandSideF32V2& side, const xr_runtime::ControllerDeviceStateV2& controller_side) {
    if (!controller_side_has_nonzero_input(controller_side)) return;

    const uint64_t buttons = normalize_controller_dpad_buttons(controller_side.buttons);
    const float controller_pinch = controller_axis_or_button(
        controller_side.trigger, buttons, xr_runtime::CONTROLLER_BUTTON_TRIGGER);
    const float controller_grab = controller_axis_or_button(
        controller_side.grip, buttons, xr_runtime::CONTROLLER_BUTTON_GRIP);

    const auto apply_value = [&](float current, float controller_value, uint32_t valid_flag) -> float {
      switch (policy) {
        case xr_runtime::ControllerInputConflictPolicy::CONTROLLER_OVERRIDE:
          return controller_value;
        case xr_runtime::ControllerInputConflictPolicy::ADDITIVE:
          return std::max(current, controller_value);
        case xr_runtime::ControllerInputConflictPolicy::HAND_OVERRIDE:
          return (side.flags & valid_flag) != 0u ? current : controller_value;
      }
      return controller_value;
    };

    side.pinch_strength = apply_value(side.pinch_strength, controller_pinch, xr_runtime::HAND_PINCH_VALID);
    side.grab_strength = apply_value(side.grab_strength, controller_grab, xr_runtime::HAND_GRAB_VALID);
    side.pinch_active = side.pinch_strength >= trigger_pinch_threshold ? 1u : 0u;
    side.grab_active = side.grab_strength >= grip_grab_threshold ? 1u : 0u;

    const uint32_t controller_buttons = controller_buttons_to_runtime_mask(buttons);
    switch (policy) {
      case xr_runtime::ControllerInputConflictPolicy::CONTROLLER_OVERRIDE:
        side.reserved0 = controller_buttons;
        break;
      case xr_runtime::ControllerInputConflictPolicy::ADDITIVE:
        side.reserved0 = (side.reserved0 & runtime_controller_button_mask()) | controller_buttons;
        break;
      case xr_runtime::ControllerInputConflictPolicy::HAND_OVERRIDE:
        if ((side.reserved0 & runtime_controller_button_mask()) == 0u) {
          side.reserved0 = controller_buttons;
        } else {
          side.reserved0 &= runtime_controller_button_mask();
        }
        break;
    }

    side.flags |= xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID;
  };

  apply_side(hand.left, controller.left);
  apply_side(hand.right, controller.right);
  hand.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
}

xr_runtime::RuntimeControllerStateFrameV1 compose_runtime_controller_state(
    uint64_t sequence,
    uint64_t timestamp_ns,
    const RuntimeControllerSynthesisConfig& cfg,
    const std::optional<xr_runtime::HandTrackingFrameF32V2>& filtered_hand,
    const std::optional<xr_runtime::ControllerInputV2>& controller_input,
    const std::optional<xr_runtime::HmdPoseF64V1>& runtime_hmd_pose) {
  xr_runtime::RuntimeControllerStateFrameV1 frame{};
  frame.sequence = sequence;
  frame.timestamp_ns = timestamp_ns;

  const xr_runtime::HandTrackingFrameF32V2* hand = filtered_hand ? &(*filtered_hand) : nullptr;
  const xr_runtime::ControllerInputV2* controller = controller_input ? &(*controller_input) : nullptr;
  const xr_runtime::HmdPoseF64V1* hmd = runtime_hmd_pose ? &(*runtime_hmd_pose) : nullptr;

  compose_side(frame.left, true, cfg, hand, controller, hmd);
  compose_side(frame.right, false, cfg, hand, controller, hmd);

  if ((frame.left.flags & xr_runtime::RUNTIME_CONTROLLER_CONNECTED) != 0u) {
    frame.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_LEFT_CONNECTED;
  }
  if ((frame.right.flags & xr_runtime::RUNTIME_CONTROLLER_CONNECTED) != 0u) {
    frame.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_RIGHT_CONNECTED;
  }
  if ((frame.left.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) != 0u) {
    frame.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_LEFT_POSE_VALID;
  }
  if ((frame.right.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) != 0u) {
    frame.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_RIGHT_POSE_VALID;
  }
  if ((frame.left.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) != 0u) {
    frame.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_LEFT_INPUT_VALID;
  }
  if ((frame.right.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) != 0u) {
    frame.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_RIGHT_INPUT_VALID;
  }

  return frame;
}

}  // namespace xr_runtime_adapter::override_controller
