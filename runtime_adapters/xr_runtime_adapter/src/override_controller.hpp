#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <xr_runtime/contracts/controller_input_contract.hpp>
#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>

namespace xr_runtime_adapter::override_controller {

constexpr uint32_t RUNTIME_BUTTON_DPAD_UP = static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_DPAD_UP);
constexpr uint32_t RUNTIME_BUTTON_DPAD_DOWN = static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN);
constexpr uint32_t RUNTIME_BUTTON_DPAD_LEFT = static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT);
constexpr uint32_t RUNTIME_BUTTON_DPAD_RIGHT = static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT);
constexpr uint32_t RUNTIME_BUTTON_DPAD_CENTER = static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_DPAD_CENTER);

enum class LostHandPoseFallbackMode {
  PoseInvalid,
  HmdRelativeWithControllerInput,
  HmdRelative,
};

struct RuntimeControllerSynthesisConfig {
  xr_runtime::RuntimeControllerMode mode = xr_runtime::RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY;

  float controller_trigger_threshold = 0.55f;
  float controller_grip_threshold = 0.55f;

  float left_hmd_relative_offset_m[3] = {-0.22f, -0.22f, -0.35f};
  float right_hmd_relative_offset_m[3] = {0.22f, -0.22f, -0.35f};

  // xyzw, identity by default.  RuntimeControllerStateV1 stores xyzw while
  // HMD/hand input structs use wxyz; keep this explicit to avoid accidental
  // cross-contract quaternion swaps.
  float left_static_orientation_xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float right_static_orientation_xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  bool dpad_to_thumbstick_axes = true;

  LostHandPoseFallbackMode lost_hand_pose_fallback = LostHandPoseFallbackMode::PoseInvalid;

  // Controls only visual hand-derived gestures when building RuntimeControllerStateV1.
  // External ControllerInputV2 buttons/axes remain authoritative and are not affected.
  bool left_hand_gestures_enabled = true;
  bool right_hand_gestures_enabled = true;
};

LostHandPoseFallbackMode parse_lost_hand_pose_fallback_mode(const std::string& value, const char* option_name);
const char* lost_hand_pose_fallback_mode_name(LostHandPoseFallbackMode mode);

uint32_t runtime_controller_button_mask();
uint32_t parse_runtime_button_target(const std::string& value, const char* option_name);
uint32_t controller_buttons_to_runtime_mask(uint64_t buttons);

bool controller_device_is_present(const xr_runtime::ControllerDeviceStateV2& controller);
bool controller_device_has_nonzero_input(const xr_runtime::ControllerDeviceStateV2& controller);
bool controller_input_has_present_controller(const xr_runtime::ControllerInputV2& controller);
bool controller_input_has_nonzero_input(const xr_runtime::ControllerInputV2& controller);

void apply_controller_gesture_override(
    xr_runtime::HandTrackingFrameF32V2& hand,
    const xr_runtime::ControllerInputV2& controller,
    xr_runtime::ControllerInputConflictPolicy policy,
    float trigger_pinch_threshold,
    float grip_grab_threshold);

xr_runtime::RuntimeControllerStateFrameV1 compose_runtime_controller_state(
    uint64_t sequence,
    uint64_t timestamp_ns,
    const RuntimeControllerSynthesisConfig& cfg,
    const std::optional<xr_runtime::HandTrackingFrameF32V2>& filtered_hand,
    const std::optional<xr_runtime::ControllerInputV2>& controller_input,
    const std::optional<xr_runtime::HmdPoseF64V1>& runtime_hmd_pose);

}  // namespace xr_runtime_adapter::override_controller
