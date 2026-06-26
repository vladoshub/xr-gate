#pragma once

#include <cstdint>
#include <string>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_tracking/contracts/hand_skeleton26_contract.hpp>

namespace xr_runtime_adapter::gestures {

struct RuntimeGestureHysteresisSideState {
  bool pinch_active = false;
  bool grab_active = false;
  bool thumbs_up_active = false;
  bool index_point_active = false;
  int64_t thumbs_up_hold_until_ns = 0;
  int64_t index_point_hold_until_ns = 0;

  void reset_hand_gestures();
  void reset_extra_gestures();
  void reset_all();
};

struct RuntimeGestureHysteresisState {
  RuntimeGestureHysteresisSideState left;
  RuntimeGestureHysteresisSideState right;
  uint64_t reset_counter = 0;

  void reset_all();
};

struct RuntimeHandGestureSideSnapshot {
  bool valid = false;
  float pinch_strength = 0.0f;
  float grab_strength = 0.0f;
  uint32_t pinch_active = 0;
  uint32_t grab_active = 0;
  uint32_t valid_flags = 0;
};

struct RuntimeHandGestureSnapshot {
  bool valid = false;
  int64_t timestamp_ns = 0;
  RuntimeHandGestureSideSnapshot left;
  RuntimeHandGestureSideSnapshot right;

  void reset();
};

float apply_derived_gesture_response_curve(float raw_strength, float response_start);

float derive_pinch_strength_from_skeleton26(const xr_tracking::HandSkeleton26SideF32V1& src);
float derive_grab_strength_from_skeleton26(const xr_tracking::HandSkeleton26SideF32V1& src);


xr_runtime::HandTrackingFrameF32V2 runtime_hand_v2_from_skeleton26(
    const xr_tracking::HandSkeleton26FrameF32V1& src,
    bool derive_gestures,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start);

xr_runtime::HandTrackingFrameF32V2 runtime_hand_v2_from_runtime_v1(
    const xr_runtime::HandTrackingFrameF64V1& src);

void apply_skeleton26_gestures_to_runtime_side(
    xr_runtime::HandSideF32V2& out,
    const xr_tracking::HandSkeleton26SideF32V1& src,
    bool derive_gestures,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start);

void derive_missing_runtime_hand_v2_gestures(
    xr_runtime::HandTrackingFrameF32V2& hand,
    float derived_pinch_active_threshold,
    float derived_grab_active_threshold,
    float derived_pinch_deactive_threshold,
    float derived_grab_deactive_threshold,
    float derived_pinch_response_start,
    float derived_grab_response_start,
    RuntimeGestureHysteresisState& state,
    bool left_gestures_enabled = true,
    bool right_gestures_enabled = true);

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
    bool left_gestures_enabled = true,
    bool right_gestures_enabled = true);

void clear_runtime_hand_v2_backend_gestures(xr_runtime::HandTrackingFrameF32V2& hand);

void clear_runtime_hand_v2_backend_gestures_by_side(
    xr_runtime::HandTrackingFrameF32V2& hand,
    bool clear_left,
    bool clear_right);

void capture_runtime_hand_v2_gesture_snapshot(
    RuntimeHandGestureSnapshot& snapshot,
    const xr_runtime::HandTrackingFrameF32V2& hand,
    int64_t now_ns,
    bool left_gestures_enabled = true,
    bool right_gestures_enabled = true);

void apply_runtime_hand_v2_gesture_latch_or_clear(
    xr_runtime::HandTrackingFrameF32V2& hand,
    const RuntimeHandGestureSnapshot& snapshot,
    int64_t now_ns,
    double latch_ms,
    bool left_gestures_enabled = true,
    bool right_gestures_enabled = true);

}  // namespace xr_runtime_adapter::gestures
