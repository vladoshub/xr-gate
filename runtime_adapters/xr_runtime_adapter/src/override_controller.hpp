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
  // Use HMD-relative fallback only while the physical controller side has
  // active input (buttons/axes/trigger/grip). This keeps lost hands from being
  // visually glued to the HMD when the user is idle.
  HmdRelativeWithControllerInput,
  // Use HMD-relative fallback whenever the physical controller side is present.
  // This is useful for always-visible synthetic controllers, but intentionally
  // distinct from hmd_relative_with_input.
  HmdRelativeWithControllerPresent,
  HmdRelative,
};

enum class RuntimeControllerMovementSpace {
  Controller,
  // Keep axes/buttons unchanged, but while movement input is active publish controller
  // orientation from HMD yaw. This fixes games/bindings that use controller pose
  // direction for locomotion instead of analog axis vector direction.
  HmdPose,
};

enum class RuntimeControllerOrientationSource {
  // Use the orientation already published by the hand-tracking backend.
  HandTrackingBackend,
  // Use ControllerInputV3::imu.orientation_xyzw when this side publishes a
  // current valid IMU orientation. If it does not, fall back independently for
  // this side to HandTrackingBackend.
  ImuOverrideControllerRuntime,
};

// Optional per-side conversion from an IMU provider/sensor frame into the
// runtime controller frame. The basis transform changes coordinate convention:
//   q' = basis * q * inverse(basis)
// The offset then aligns the physical sensor/controller mounting. A post/local
// offset is the normal mounting-offset form:
//   q_out = q' * offset
struct RuntimeControllerImuOrientationConfig {
  bool transform_enabled = false;
  // Coordinate-axis inversion applied before basis_rotation. These are basis
  // convention changes, not per-component quaternion negation.
  bool invert_x = false;
  bool invert_y = false;
  bool invert_z = false;
  float basis_rotation_xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  bool offset_enabled = false;
  bool offset_pre_multiply = false;
  float offset_rotation_xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

// Optional IMU-assisted motion features. They are evaluated independently per
// controller side and are active only while that side actually uses
// IMU_OVERRIDE_CONTROLLER_RUNTIME with current valid sensor data.
struct RuntimeControllerImuMotionConfig {
  bool acceleration_integration_enabled = false;
  bool position_prediction_enabled = false;
  bool yaw_correction_enabled = false;

  // specific_force_m_s2 includes gravity. Runtime tracking space uses +Y up.
  float gravity_mps2 = 9.80665f;
  float acceleration_deadband_mps2 = 0.15f;
  float max_linear_acceleration_mps2 = 12.0f;

  // Position loss handling mirrors HandPoseStabilityFilter:
  // hold the last optical coordinate, then predict, then invalidate.
  float hold_lost_ms = 0.0f;
  float predict_lost_ms = 600.0f;
  float max_prediction_velocity_mps = 3.0f;
  float prediction_damping = 1.0f;
  bool publish_predicted_velocity = false;
  float reacquire_blend_ms = 0.0f;

  // Optical hand orientation slowly updates a retained world-yaw offset. The
  // offset remains active while the controller is outside the camera FOV.
  float yaw_correction_alpha = 0.05f;
  float yaw_correction_max_step_deg = 2.0f;
};

struct RuntimeControllerImuSideRuntimeState {
  bool has_position_anchor = false;
  bool prediction_active = false;
  bool prediction_started = false;
  bool reacquire_blend_active = false;
  bool yaw_correction_valid = false;
  uint64_t last_update_ns = 0;
  uint64_t last_optical_pose_ns = 0;
  uint64_t reacquire_blend_start_ns = 0;
  float anchor_position_m[3] = {};
  float anchor_velocity_mps[3] = {};
  float position_m[3] = {};
  float velocity_mps[3] = {};
  float acceleration_position_delta_m[3] = {};
  float acceleration_velocity_delta_mps[3] = {};
  float reacquire_blend_from_position_m[3] = {};
  float reacquire_blend_from_velocity_mps[3] = {};
  float yaw_correction_rad = 0.0f;
};

struct RuntimeControllerSynthesisState {
  RuntimeControllerImuSideRuntimeState left{};
  RuntimeControllerImuSideRuntimeState right{};
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
  // Locomotion reference mode for controller synthesis.
  // Controller: old behavior, movement follows controller/hand yaw.
  // HmdPose: keep axes/buttons unchanged, but use HMD yaw as controller orientation
  //          while movement input is active.
  RuntimeControllerMovementSpace left_hand_tracking_movement_space = RuntimeControllerMovementSpace::Controller;
  RuntimeControllerMovementSpace right_hand_tracking_movement_space = RuntimeControllerMovementSpace::Controller;
  RuntimeControllerMovementSpace left_imu_movement_space = RuntimeControllerMovementSpace::Controller;
  RuntimeControllerMovementSpace right_imu_movement_space = RuntimeControllerMovementSpace::Controller;

  RuntimeControllerOrientationSource left_orientation_source =
      RuntimeControllerOrientationSource::HandTrackingBackend;
  RuntimeControllerOrientationSource right_orientation_source =
      RuntimeControllerOrientationSource::HandTrackingBackend;

  RuntimeControllerImuOrientationConfig left_imu_orientation{};
  RuntimeControllerImuOrientationConfig right_imu_orientation{};
  RuntimeControllerImuMotionConfig left_imu_motion{};
  RuntimeControllerImuMotionConfig right_imu_motion{};

  LostHandPoseFallbackMode lost_hand_pose_fallback = LostHandPoseFallbackMode::PoseInvalid;

  // Controls only visual hand-derived gestures when building RuntimeControllerStateV1.
  // External ControllerInputV3 buttons/axes remain authoritative and are not affected.
  bool left_hand_gestures_enabled = false;
  bool right_hand_gestures_enabled = false;
};

RuntimeControllerMovementSpace parse_runtime_controller_movement_space(
    const std::string& value,
    const char* option_name);
const char* runtime_controller_movement_space_name(RuntimeControllerMovementSpace value);

RuntimeControllerOrientationSource parse_runtime_controller_orientation_source(
    const std::string& value,
    const char* option_name);
const char* runtime_controller_orientation_source_name(RuntimeControllerOrientationSource value);

LostHandPoseFallbackMode parse_lost_hand_pose_fallback_mode(const std::string& value, const char* option_name);
const char* lost_hand_pose_fallback_mode_name(LostHandPoseFallbackMode mode);

uint32_t runtime_controller_button_mask();
uint32_t parse_runtime_button_target(const std::string& value, const char* option_name);
uint32_t controller_buttons_to_runtime_mask(uint64_t buttons);

bool controller_device_is_present(const xr_runtime::ControllerDeviceStateV3& controller);
bool controller_device_has_nonzero_input(const xr_runtime::ControllerDeviceStateV3& controller);
bool controller_device_has_imu(const xr_runtime::ControllerDeviceStateV3& controller);
bool controller_device_has_active_imu(const xr_runtime::ControllerDeviceStateV3& controller);
bool controller_device_has_active_orientation_imu(const xr_runtime::ControllerDeviceStateV3& controller);
RuntimeControllerOrientationSource effective_runtime_controller_orientation_source(
    RuntimeControllerOrientationSource configured,
    const xr_runtime::ControllerDeviceStateV3* controller);
bool controller_input_has_present_controller(const xr_runtime::ControllerInputV3& controller);
bool controller_input_has_nonzero_input(const xr_runtime::ControllerInputV3& controller);

void apply_controller_gesture_override(
    xr_runtime::HandTrackingFrameF32V2& hand,
    const xr_runtime::ControllerInputV3& controller,
    xr_runtime::ControllerInputConflictPolicy policy,
    float trigger_pinch_threshold,
    float grip_grab_threshold);

xr_runtime::RuntimeControllerStateFrameV1 compose_runtime_controller_state(
    uint64_t sequence,
    uint64_t timestamp_ns,
    const RuntimeControllerSynthesisConfig& cfg,
    const std::optional<xr_runtime::HandTrackingFrameF32V2>& filtered_hand,
    const std::optional<xr_runtime::ControllerInputV3>& controller_input,
    const std::optional<xr_runtime::HmdPoseF64V1>& runtime_hmd_pose,
    RuntimeControllerSynthesisState* runtime_state = nullptr);

}  // namespace xr_runtime_adapter::override_controller
