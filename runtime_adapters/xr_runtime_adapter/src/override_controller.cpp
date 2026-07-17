#include "override_controller.hpp"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

bool controller_side_is_present(const xr_runtime::ControllerDeviceStateV3& controller) {
  return controller.status == xr_runtime::CONTROLLER_INPUT_ACTIVE ||
         controller.status == xr_runtime::CONTROLLER_INPUT_CONNECTED;
}

bool controller_side_has_input(const xr_runtime::ControllerDeviceStateV3& controller) {
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

bool controller_side_has_nonzero_input(const xr_runtime::ControllerDeviceStateV3& controller) {
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

Qf apply_basis_transform(Qf basis, Qf q) {
  basis = normalize_q(basis);
  return q_mul(q_mul(basis, normalize_q(q)), q_conj(basis));
}

Qf apply_axis_inversion(Qf q, bool invert_x, bool invert_y, bool invert_z) {
  q = normalize_q(q);
  const float sx = invert_x ? -1.0f : 1.0f;
  const float sy = invert_y ? -1.0f : 1.0f;
  const float sz = invert_z ? -1.0f : 1.0f;

  // For a coordinate-basis reflection S=diag(sx,sy,sz), orientation changes as
  // R' = S * R * S^-1. Quaternion vector components are axial, therefore:
  // v' = det(S) * S * v.
  q.x *= sy * sz;
  q.y *= sx * sz;
  q.z *= sx * sy;
  return normalize_q(q);
}

void apply_axis_inversion_to_axial_vector(float v[3],
                                          bool invert_x,
                                          bool invert_y,
                                          bool invert_z) {
  const float sx = invert_x ? -1.0f : 1.0f;
  const float sy = invert_y ? -1.0f : 1.0f;
  const float sz = invert_z ? -1.0f : 1.0f;
  v[0] *= sy * sz;
  v[1] *= sx * sz;
  v[2] *= sx * sy;
}

void q_rotate(Qf q, const float in[3], float out[3]) {
  q = normalize_q(q);
  const Qf p{0.0f, in[0], in[1], in[2]};
  const Qf r = q_mul_raw(q_mul_raw(q, p), q_conj(q));
  out[0] = r.x;
  out[1] = r.y;
  out[2] = r.z;
}



bool normalize_horizontal(float v[3]) {
  v[1] = 0.0f;
  const float n = std::sqrt(v[0] * v[0] + v[2] * v[2]);
  if (!std::isfinite(n) || n <= 1.0e-5f) return false;
  v[0] /= n;
  v[2] /= n;
  return true;
}

bool horizontal_yaw_basis_from_q(Qf q, float right[3], float forward[3]) {
  // Runtime/OpenVR convention used by the HMD-relative offsets: X=right, Y=up/down,
  // -Z=forward.  Project to the horizontal XZ plane so pitch/roll of the hand does
  // not affect locomotion direction.
  const float local_forward[3]{0.0f, 0.0f, -1.0f};
  q_rotate(q, local_forward, forward);
  if (!normalize_horizontal(forward)) return false;

  // Build an orthogonal horizontal right vector from the projected forward vector.
  right[0] = -forward[2];
  right[1] = 0.0f;
  right[2] = forward[0];
  return normalize_horizontal(right);
}

RuntimeControllerMovementSpace effective_runtime_controller_movement_space(
    const RuntimeControllerSynthesisConfig& cfg,
    bool left,
    RuntimeControllerOrientationSource orientation_source) {
  if (orientation_source == RuntimeControllerOrientationSource::ImuOverrideControllerRuntime) {
    return left ? cfg.left_imu_movement_space : cfg.right_imu_movement_space;
  }
  return left ? cfg.left_hand_tracking_movement_space
              : cfg.right_hand_tracking_movement_space;
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

bool finite_q_xyzw(const float q[4]) {
  return std::isfinite(q[0]) && std::isfinite(q[1]) &&
         std::isfinite(q[2]) && std::isfinite(q[3]);
}

bool nonzero_q_xyzw(const float q[4]) {
  const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  return std::isfinite(n2) && n2 > 1.0e-8f;
}

bool latest_valid_imu_sample(const xr_runtime::ControllerImuStateV1& imu,
                             uint32_t required_flag,
                             const xr_runtime::ControllerImuSampleV1*& sample) {
  const uint32_t count = std::min<uint32_t>(imu.sample_count, xr_runtime::CONTROLLER_IMU_MAX_SAMPLES);
  for (uint32_t i = count; i > 0; --i) {
    const auto& candidate = imu.samples[i - 1];
    if ((candidate.flags & required_flag) != 0u) {
      sample = &candidate;
      return true;
    }
  }
  sample = nullptr;
  return false;
}

void apply_axis_inversion_to_polar_vector(float v[3],
                                          bool invert_x,
                                          bool invert_y,
                                          bool invert_z) {
  if (invert_x) v[0] = -v[0];
  if (invert_y) v[1] = -v[1];
  if (invert_z) v[2] = -v[2];
}

bool finite_v3(const float v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float v3_length(const float v[3]) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void cross_v3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

void clamp_v3_length(float v[3], float max_length) {
  if (!std::isfinite(max_length) || max_length <= 0.0f) return;
  const float length = v3_length(v);
  if (!std::isfinite(length) || length <= max_length || length <= 1.0e-6f) return;
  const float scale = max_length / length;
  v[0] *= scale;
  v[1] *= scale;
  v[2] *= scale;
}

float wrap_pi(float angle) {
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kTwoPi = 2.0f * kPi;
  if (!std::isfinite(angle)) return 0.0f;
  while (angle > kPi) angle -= kTwoPi;
  while (angle < -kPi) angle += kTwoPi;
  return angle;
}

bool yaw_rad_from_q(Qf q, float& yaw) {
  float right[3]{};
  float forward[3]{};
  if (!horizontal_yaw_basis_from_q(q, right, forward)) return false;
  yaw = std::atan2(-forward[0], -forward[2]);
  return std::isfinite(yaw);
}

Qf yaw_q(float yaw) {
  return normalize_q({std::cos(0.5f * yaw), 0.0f, std::sin(0.5f * yaw), 0.0f});
}

struct RuntimeImuSample {
  bool orientation_valid = false;
  bool angular_velocity_valid = false;
  bool specific_force_valid = false;
  uint64_t timestamp_ns = 0;
  uint64_t angular_velocity_timestamp_ns = 0;
  // Canonical IMU orientation after axis/basis conversion but before the
  // configurable presentation/mounting orientation_offset. Yaw correction
  // compares this clean reference with the equally clean optical reference.
  Qf yaw_reference_orientation{};
  // Final IMU orientation used by controller output and acceleration logic.
  // This includes orientation_offset when configured.
  Qf orientation{};
  float angular_velocity_rad_s[3] = {};
  float specific_force_m_s2[3] = {};
};

RuntimeImuSample runtime_imu_sample(
    const xr_runtime::ControllerDeviceStateV3& controller,
    const RuntimeControllerImuOrientationConfig& cfg) {
  RuntimeImuSample out{};
  if (!controller_device_has_active_orientation_imu(controller)) return out;

  const Qf basis = q_from_xyzw(cfg.basis_rotation_xyzw);
  const Qf offset = q_from_xyzw(cfg.offset_rotation_xyzw);

  Qf orientation = q_from_xyzw(controller.imu.orientation_xyzw);
  if (cfg.transform_enabled) {
    orientation = apply_axis_inversion(
        orientation, cfg.invert_x, cfg.invert_y, cfg.invert_z);
    orientation = apply_basis_transform(basis, orientation);
  }
  // Keep the axis/basis-corrected pose as the clean yaw reference. The
  // optional orientation_offset is a final controller presentation/mounting
  // adjustment and must not be mistaken for IMU yaw drift.
  out.yaw_reference_orientation = orientation;
  if (cfg.offset_enabled) {
    orientation = cfg.offset_pre_multiply
        ? q_mul(offset, orientation)
        : q_mul(orientation, offset);
  }
  out.orientation = orientation;
  out.orientation_valid = true;
  out.timestamp_ns = controller.imu.orientation_timestamp_ns != 0
      ? controller.imu.orientation_timestamp_ns
      : controller.imu.latest_sample_timestamp_ns;

  const xr_runtime::ControllerImuSampleV1* latest_gyro = nullptr;
  if (latest_valid_imu_sample(controller.imu, xr_runtime::CONTROLLER_IMU_GYROSCOPE_VALID,
                              latest_gyro) && latest_gyro != nullptr) {
    float angular_velocity[3] = {
        latest_gyro->angular_velocity_rad_s[0],
        latest_gyro->angular_velocity_rad_s[1],
        latest_gyro->angular_velocity_rad_s[2],
    };
    float transformed[3] = {};
    if (cfg.transform_enabled) {
      apply_axis_inversion_to_axial_vector(
          angular_velocity, cfg.invert_x, cfg.invert_y, cfg.invert_z);
      q_rotate(basis, angular_velocity, transformed);
      std::copy(std::begin(transformed), std::end(transformed), angular_velocity);
    }
    if (cfg.offset_enabled && !cfg.offset_pre_multiply) {
      q_rotate(q_conj(offset), angular_velocity, transformed);
      std::copy(std::begin(transformed), std::end(transformed), angular_velocity);
    }
    if (finite_v3(angular_velocity)) {
      std::copy(std::begin(angular_velocity), std::end(angular_velocity),
                out.angular_velocity_rad_s);
      out.angular_velocity_valid = true;
      out.angular_velocity_timestamp_ns = latest_gyro->timestamp_ns;
      out.timestamp_ns = std::max(out.timestamp_ns, latest_gyro->timestamp_ns);
    }
  }

  const xr_runtime::ControllerImuSampleV1* latest_accel = nullptr;
  if (latest_valid_imu_sample(controller.imu, xr_runtime::CONTROLLER_IMU_ACCELEROMETER_VALID,
                              latest_accel) && latest_accel != nullptr) {
    float specific_force[3] = {
        latest_accel->specific_force_m_s2[0],
        latest_accel->specific_force_m_s2[1],
        latest_accel->specific_force_m_s2[2],
    };
    float transformed[3] = {};
    if (cfg.transform_enabled) {
      apply_axis_inversion_to_polar_vector(
          specific_force, cfg.invert_x, cfg.invert_y, cfg.invert_z);
      q_rotate(basis, specific_force, transformed);
      std::copy(std::begin(transformed), std::end(transformed), specific_force);
    }
    if (cfg.offset_enabled && !cfg.offset_pre_multiply) {
      q_rotate(q_conj(offset), specific_force, transformed);
      std::copy(std::begin(transformed), std::end(transformed), specific_force);
    }
    if (finite_v3(specific_force)) {
      std::copy(std::begin(specific_force), std::end(specific_force),
                out.specific_force_m_s2);
      out.specific_force_valid = true;
      out.timestamp_ns = std::max(out.timestamp_ns, latest_accel->timestamp_ns);
    }
  }

  return out;
}

bool real_optical_hand_pose(const xr_runtime::HandSideF32V2* hand_side) {
  return hand_side != nullptr && hand_side->status == 1u &&
         (hand_side->flags & xr_runtime::HAND_POSE_VALID) != 0u &&
         std::isfinite(hand_side->controller_px) &&
         std::isfinite(hand_side->controller_py) &&
         std::isfinite(hand_side->controller_pz);
}

uint64_t nonnegative_ms_to_ns(float value_ms);

void reset_yaw_trigger_observation(
    RuntimeControllerImuSideRuntimeState& state) {
  state.yaw_trigger_hold_start_ns = 0;
  state.yaw_trigger_range_valid = false;
  state.yaw_trigger_reference_error_rad = 0.0f;
  state.yaw_trigger_min_unwrapped_error_rad = 0.0f;
  state.yaw_trigger_max_unwrapped_error_rad = 0.0f;
}

void begin_yaw_check(RuntimeControllerImuSideRuntimeState& state) {
  state.yaw_check_active = true;
  state.yaw_check_reacquire =
      state.yaw_correction_valid && state.yaw_correction_requested;
  state.yaw_check_last_frame_sequence = 0;
  reset_yaw_trigger_observation(state);
}

void finish_yaw_check(
    RuntimeControllerImuSideRuntimeState& state,
    uint64_t timestamp_ns) {
  // The periodic interval starts only after the current check has completed.
  // A hold-window restart therefore postpones the next interval naturally.
  state.last_yaw_correction_update_ns = timestamp_ns;
  state.yaw_correction_requested = false;
  state.yaw_check_active = false;
  state.yaw_check_reacquire = false;
  state.yaw_check_last_frame_sequence = 0;
  reset_yaw_trigger_observation(state);
}

void start_yaw_blend(
    RuntimeControllerImuSideRuntimeState& state,
    float desired_yaw_correction_rad,
    float duration_ms,
    bool reacquire,
    uint64_t timestamp_ns) {
  const float error = wrap_pi(desired_yaw_correction_rad - state.yaw_correction_rad);
  state.yaw_blend_from_rad = state.yaw_correction_rad;
  state.yaw_blend_to_rad = desired_yaw_correction_rad;
  state.yaw_blend_duration_ms = std::max(0.0f, duration_ms);
  state.yaw_blend_start_ns = timestamp_ns;
  state.yaw_blend_reacquire = reacquire;
  state.yaw_blend_active = state.yaw_blend_duration_ms > 0.0f;
  state.yaw_last_step_rad = error;
  state.yaw_last_action = reacquire
      ? (state.yaw_blend_active ? "reacquire_blend_start"
                                : "reacquire_apply_direct")
      : (state.yaw_blend_active ? "periodic_blend_start"
                                : "periodic_apply_direct");
  if (!state.yaw_blend_active) {
    state.yaw_correction_rad = desired_yaw_correction_rad;
  }
  ++state.yaw_apply_count;
  if (reacquire) ++state.yaw_reacquire_apply_count;
}

void apply_latest_yaw_correction(
    RuntimeControllerImuSideRuntimeState& state,
    const RuntimeControllerImuMotionConfig& cfg,
    float desired_yaw_correction_rad,
    uint64_t timestamp_ns) {
  constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

  desired_yaw_correction_rad = wrap_pi(desired_yaw_correction_rad);
  state.yaw_last_desired_rad = desired_yaw_correction_rad;

  if (!state.yaw_correction_valid) {
    state.yaw_correction_rad = desired_yaw_correction_rad;
    state.yaw_correction_valid = true;
    state.yaw_last_error_rad = 0.0f;
    state.yaw_last_step_rad = 0.0f;
    state.yaw_last_action = "initial_align";
    ++state.yaw_apply_count;
    finish_yaw_check(state, timestamp_ns);
    return;
  }

  const bool reacquire = state.yaw_check_reacquire;
  const float error =
      wrap_pi(desired_yaw_correction_rad - state.yaw_correction_rad);
  state.yaw_last_error_rad = error;
  state.yaw_last_step_rad = 0.0f;

  const float deadband_deg = reacquire
      ? cfg.yaw_correction_reacquire_deadband_deg
      : cfg.yaw_correction_deadband_deg;
  const float deadband = std::max(0.0f, deadband_deg) * kDegToRad;
  if (std::abs(error) <= deadband) {
    state.yaw_last_action = reacquire
        ? "reacquire_deadband"
        : "periodic_deadband";
    finish_yaw_check(state, timestamp_ns);
    return;
  }

  const float blend_ms = reacquire
      ? cfg.yaw_correction_reacquire_blend_ms
      : cfg.yaw_correction_blend_ms;
  start_yaw_blend(
      state, desired_yaw_correction_rad, blend_ms, reacquire, timestamp_ns);
  finish_yaw_check(state, timestamp_ns);
}

void advance_yaw_blend(
    RuntimeControllerImuSideRuntimeState& state,
    uint64_t timestamp_ns) {
  if (!state.yaw_blend_active) return;

  const uint64_t duration_ns = std::max<uint64_t>(
      1, nonnegative_ms_to_ns(state.yaw_blend_duration_ms));
  const uint64_t elapsed_ns = timestamp_ns >= state.yaw_blend_start_ns
      ? timestamp_ns - state.yaw_blend_start_ns
      : 0;
  const float t = std::clamp(
      static_cast<float>(elapsed_ns) / static_cast<float>(duration_ns),
      0.0f, 1.0f);
  const float smooth_t = t * t * (3.0f - 2.0f * t);
  const float delta = wrap_pi(state.yaw_blend_to_rad - state.yaw_blend_from_rad);
  state.yaw_correction_rad = wrap_pi(
      state.yaw_blend_from_rad + delta * smooth_t);

  if (t >= 1.0f) {
    state.yaw_correction_rad = state.yaw_blend_to_rad;
    state.yaw_blend_active = false;
    state.yaw_last_action = state.yaw_blend_reacquire
        ? "reacquire_blend_done"
        : "periodic_blend_done";
  }
}

bool yaw_correction_check_due(
    const RuntimeControllerImuSideRuntimeState& state,
    const RuntimeControllerImuMotionConfig& cfg,
    uint64_t timestamp_ns) {
  if (!state.yaw_correction_valid) return true;
  if (state.yaw_blend_active) return false;
  if (state.yaw_correction_requested) return true;
  if (!cfg.yaw_correction_continuous) return false;

  const uint64_t interval_ns = nonnegative_ms_to_ns(cfg.yaw_correction_interval_ms);
  if (interval_ns == 0 || state.last_yaw_correction_update_ns == 0) return true;
  return timestamp_ns >= state.last_yaw_correction_update_ns &&
         timestamp_ns - state.last_yaw_correction_update_ns >= interval_ns;
}

void restart_periodic_yaw_trigger_window(
    RuntimeControllerImuSideRuntimeState& state,
    float latest_error_rad,
    uint64_t timestamp_ns,
    const char* action) {
  state.yaw_trigger_hold_start_ns = timestamp_ns;
  state.yaw_trigger_range_valid = true;
  state.yaw_trigger_reference_error_rad = latest_error_rad;
  state.yaw_trigger_min_unwrapped_error_rad = latest_error_rad;
  state.yaw_trigger_max_unwrapped_error_rad = latest_error_rad;
  state.yaw_last_action = action;
}

void update_yaw_correction(
    RuntimeControllerImuSideRuntimeState& state,
    const RuntimeControllerImuMotionConfig& cfg,
    Qf imu_orientation,
    const xr_runtime::HandSideF32V2* optical_hand_side,
    uint64_t optical_frame_sequence,
    uint64_t timestamp_ns) {
  if (!cfg.yaw_correction_enabled) return;

  // A real reacquire request has priority over an in-progress periodic check.
  if (state.yaw_correction_requested && state.yaw_check_active &&
      !state.yaw_check_reacquire) {
    state.yaw_check_active = false;
    state.yaw_check_last_frame_sequence = 0;
    reset_yaw_trigger_observation(state);
  }

  if (!state.yaw_check_active &&
      !yaw_correction_check_due(state, cfg, timestamp_ns)) {
    return;
  }
  if (!state.yaw_check_active) {
    begin_yaw_check(state);
  }

  // Evaluate only when Mercury/backend publishes a new hand pose. The IMU
  // orientation passed to this call is the latest available orientation at
  // that moment, so every hold-window update compares the newest pair rather
  // than poses captured when the window started.
  const bool new_backend_frame =
      optical_frame_sequence != 0 &&
      optical_frame_sequence != state.yaw_check_last_frame_sequence;
  if (!new_backend_frame) return;
  state.yaw_check_last_frame_sequence = optical_frame_sequence;

  if (!real_optical_hand_pose(optical_hand_side)) {
    if (!state.yaw_check_reacquire) {
      reset_yaw_trigger_observation(state);
      state.yaw_last_action = "periodic_invalid_pose_reset";
    }
    return;
  }

  const float optical_q_xyzw[4] = {
      optical_hand_side->controller_qx,
      optical_hand_side->controller_qy,
      optical_hand_side->controller_qz,
      optical_hand_side->controller_qw,
  };
  if (!finite_q_xyzw(optical_q_xyzw) || !nonzero_q_xyzw(optical_q_xyzw)) {
    if (!state.yaw_check_reacquire) {
      reset_yaw_trigger_observation(state);
      state.yaw_last_action = "periodic_invalid_orientation_reset";
    }
    return;
  }

  const Qf optical_orientation = normalize_q({
      optical_hand_side->controller_qw,
      optical_hand_side->controller_qx,
      optical_hand_side->controller_qy,
      optical_hand_side->controller_qz,
  });
  float imu_yaw = 0.0f;
  float optical_yaw = 0.0f;
  if (!yaw_rad_from_q(imu_orientation, imu_yaw) ||
      !yaw_rad_from_q(optical_orientation, optical_yaw)) {
    if (!state.yaw_check_reacquire) {
      reset_yaw_trigger_observation(state);
      state.yaw_last_action = "periodic_invalid_yaw_reset";
    }
    return;
  }

  const float desired = wrap_pi(optical_yaw - imu_yaw);
  state.yaw_last_desired_rad = desired;

  // Initial alignment and reacquire always use the latest valid pose pair.
  // Reacquire has its own deadband/blend but no periodic hold/range filter.
  if (!state.yaw_correction_valid || state.yaw_check_reacquire) {
    apply_latest_yaw_correction(state, cfg, desired, timestamp_ns);
    return;
  }

  constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
  const float latest_error = wrap_pi(desired - state.yaw_correction_rad);
  state.yaw_last_error_rad = latest_error;
  state.yaw_last_step_rad = 0.0f;
  const float deadband =
      std::max(0.0f, cfg.yaw_correction_deadband_deg) * kDegToRad;

  // No persistent mismatch at this interval: complete the check now and start
  // the next interval from this latest observation.
  if (std::abs(latest_error) <= deadband) {
    state.yaw_last_action = "periodic_deadband";
    finish_yaw_check(state, timestamp_ns);
    return;
  }

  // Disabled filter: the first latest pose pair above deadband immediately
  // becomes the periodic target. INTERVAL_MS still applies between checks.
  if (!cfg.yaw_correction_trigger_filter) {
    state.yaw_last_action = "periodic_trigger_filter_disabled";
    apply_latest_yaw_correction(state, cfg, desired, timestamp_ns);
    return;
  }

  if (!state.yaw_trigger_range_valid ||
      state.yaw_trigger_hold_start_ns == 0) {
    restart_periodic_yaw_trigger_window(
        state, latest_error, timestamp_ns, "periodic_trigger_hold_start");
    return;
  }

  // Track the range of the latest residual errors over the entire temporal
  // hold window, with wrap-safe unwrapping around the first error. If motion
  // makes the range too large, restart the full hold from the latest sample.
  const float unwrapped_error = state.yaw_trigger_reference_error_rad +
      wrap_pi(latest_error - state.yaw_trigger_reference_error_rad);
  state.yaw_trigger_min_unwrapped_error_rad = std::min(
      state.yaw_trigger_min_unwrapped_error_rad, unwrapped_error);
  state.yaw_trigger_max_unwrapped_error_rad = std::max(
      state.yaw_trigger_max_unwrapped_error_rad, unwrapped_error);
  const float observed_range =
      state.yaw_trigger_max_unwrapped_error_rad -
      state.yaw_trigger_min_unwrapped_error_rad;
  const float max_range =
      std::max(0.0f, cfg.yaw_correction_trigger_max_range_deg) * kDegToRad;
  if (observed_range > max_range) {
    restart_periodic_yaw_trigger_window(
        state, latest_error, timestamp_ns, "periodic_trigger_range_restart");
    return;
  }

  const uint64_t hold_ns =
      nonnegative_ms_to_ns(cfg.yaw_correction_trigger_hold_ms);
  const bool hold_complete =
      hold_ns == 0 ||
      (timestamp_ns >= state.yaw_trigger_hold_start_ns &&
       timestamp_ns - state.yaw_trigger_hold_start_ns >= hold_ns);
  if (!hold_complete) {
    state.yaw_last_action = "periodic_wait_trigger_hold";
    return;
  }

  // Use the newest optical/IMU difference at completion, not the first,
  // minimum, maximum or average value observed during the hold.
  apply_latest_yaw_correction(state, cfg, desired, timestamp_ns);
}

void apply_runtime_yaw_correction(
    RuntimeImuSample& imu,
    const RuntimeControllerImuMotionConfig& motion_cfg,
    RuntimeControllerImuSideRuntimeState* runtime_state,
    const xr_runtime::HandSideF32V2* optical_hand_side,
    uint64_t optical_frame_sequence,
    uint64_t timestamp_ns) {
  if (!imu.orientation_valid || runtime_state == nullptr ||
      !motion_cfg.yaw_correction_enabled) {
    return;
  }
  advance_yaw_blend(*runtime_state, timestamp_ns);
  update_yaw_correction(*runtime_state, motion_cfg,
                        imu.yaw_reference_orientation,
                        optical_hand_side, optical_frame_sequence, timestamp_ns);
  if (runtime_state->yaw_correction_valid) {
    imu.orientation = q_mul(yaw_q(runtime_state->yaw_correction_rad), imu.orientation);
  }
}

void apply_imu_orientation_override(
    xr_runtime::RuntimeControllerSideStateV1& out,
    const RuntimeImuSample& imu) {
  if ((out.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) == 0u) return;
  if (!imu.orientation_valid) return;

  set_orientation_xyzw(out, imu.orientation);

  out.source_mask &= ~(xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_ORIENTATION |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_STATIC_ORIENTATION);
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_ORIENTATION;
  out.last_pose_ns = imu.timestamp_ns;

  if (imu.angular_velocity_valid) {
    out.angular_velocity[0] = imu.angular_velocity_rad_s[0];
    out.angular_velocity[1] = imu.angular_velocity_rad_s[1];
    out.angular_velocity[2] = imu.angular_velocity_rad_s[2];
  }
}

bool runtime_world_linear_acceleration(
    const RuntimeImuSample& imu,
    const RuntimeControllerImuMotionConfig& cfg,
    RuntimeControllerImuSideRuntimeState* state,
    uint64_t runtime_timestamp_ns,
    float out_acceleration[3]) {
  if (!cfg.acceleration_integration_enabled ||
      !imu.orientation_valid || !imu.specific_force_valid) {
    return false;
  }

  q_rotate(imu.orientation, imu.specific_force_m_s2, out_acceleration);
  out_acceleration[1] -= std::max(0.0f, cfg.gravity_mps2);
  if (!finite_v3(out_acceleration)) return false;

  const bool compensate_centripetal =
      cfg.lever_arm_enabled &&
      cfg.lever_arm_centripetal_compensation_enabled;
  const bool compensate_tangential =
      cfg.lever_arm_enabled &&
      cfg.lever_arm_tangential_compensation_enabled;
  if (state != nullptr && (compensate_centripetal || compensate_tangential) &&
      imu.angular_velocity_valid && finite_v3(cfg.lever_arm_local_m)) {
    // The exact physical IMU location inside the controller is not known.
    // Approximate pivot-to-sensor with the same local vector used by the
    // trajectory lever arm so both parts of the model stay geometrically
    // consistent and remain independently opt-in.
    float lever_world[3] = {};
    float angular_velocity_world[3] = {};
    q_rotate(imu.orientation, cfg.lever_arm_local_m, lever_world);
    q_rotate(imu.orientation, imu.angular_velocity_rad_s,
             angular_velocity_world);

    if (finite_v3(lever_world) && finite_v3(angular_velocity_world)) {
      if (compensate_centripetal) {
        float omega_cross_r[3] = {};
        float centripetal_acceleration[3] = {};
        cross_v3(angular_velocity_world, lever_world, omega_cross_r);
        cross_v3(angular_velocity_world, omega_cross_r,
                 centripetal_acceleration);
        if (finite_v3(centripetal_acceleration)) {
          for (int axis = 0; axis < 3; ++axis) {
            out_acceleration[axis] -= centripetal_acceleration[axis];
          }
        }
      }

      if (compensate_tangential) {
        const uint64_t sample_timestamp_ns =
            imu.angular_velocity_timestamp_ns != 0
                ? imu.angular_velocity_timestamp_ns
                : (imu.timestamp_ns != 0 ? imu.timestamp_ns
                                         : runtime_timestamp_ns);
        bool have_filtered_alpha =
            state->lever_arm_angular_history_valid;

        if (!state->lever_arm_angular_history_valid) {
          state->lever_arm_angular_history_valid = true;
          state->lever_arm_angular_history_timestamp_ns = sample_timestamp_ns;
          std::copy(std::begin(angular_velocity_world),
                    std::end(angular_velocity_world),
                    state->lever_arm_previous_angular_velocity_world_rad_s);
          std::fill(
              std::begin(state->lever_arm_filtered_angular_acceleration_world_rad_s2),
              std::end(state->lever_arm_filtered_angular_acceleration_world_rad_s2),
              0.0f);
          have_filtered_alpha = false;
        } else if (sample_timestamp_ns != 0 &&
                   sample_timestamp_ns !=
                       state->lever_arm_angular_history_timestamp_ns) {
          const bool monotonic =
              sample_timestamp_ns >
              state->lever_arm_angular_history_timestamp_ns;
          const uint64_t delta_ns = monotonic
              ? sample_timestamp_ns -
                    state->lever_arm_angular_history_timestamp_ns
              : 0;
          // A long pause or timestamp rollback is not a usable derivative.
          // Re-anchor without changing any prediction state or transition.
          if (!monotonic || delta_ns > 100'000'000ull) {
            std::fill(
                std::begin(state->lever_arm_filtered_angular_acceleration_world_rad_s2),
                std::end(state->lever_arm_filtered_angular_acceleration_world_rad_s2),
                0.0f);
            have_filtered_alpha = false;
          } else if (delta_ns > 0) {
            const float dt_s = static_cast<float>(delta_ns) / 1.0e9f;
            float raw_angular_acceleration[3] = {
                (angular_velocity_world[0] -
                 state->lever_arm_previous_angular_velocity_world_rad_s[0]) /
                    dt_s,
                (angular_velocity_world[1] -
                 state->lever_arm_previous_angular_velocity_world_rad_s[1]) /
                    dt_s,
                (angular_velocity_world[2] -
                 state->lever_arm_previous_angular_velocity_world_rad_s[2]) /
                    dt_s,
            };
            clamp_v3_length(
                raw_angular_acceleration,
                cfg.lever_arm_max_angular_acceleration_rad_s2);
            const float alpha = std::clamp(
                cfg.lever_arm_angular_acceleration_smooth_alpha,
                0.0f, 1.0f);
            for (int axis = 0; axis < 3; ++axis) {
              state->lever_arm_filtered_angular_acceleration_world_rad_s2[axis] =
                  state->lever_arm_filtered_angular_acceleration_world_rad_s2[axis] *
                      (1.0f - alpha) +
                  raw_angular_acceleration[axis] * alpha;
            }
            clamp_v3_length(
                state->lever_arm_filtered_angular_acceleration_world_rad_s2,
                cfg.lever_arm_max_angular_acceleration_rad_s2);
            have_filtered_alpha = finite_v3(
                state->lever_arm_filtered_angular_acceleration_world_rad_s2);
          }

          state->lever_arm_angular_history_timestamp_ns = sample_timestamp_ns;
          std::copy(std::begin(angular_velocity_world),
                    std::end(angular_velocity_world),
                    state->lever_arm_previous_angular_velocity_world_rad_s);
        }

        if (have_filtered_alpha) {
          float tangential_acceleration[3] = {};
          cross_v3(
              state->lever_arm_filtered_angular_acceleration_world_rad_s2,
              lever_world, tangential_acceleration);
          if (finite_v3(tangential_acceleration)) {
            for (int axis = 0; axis < 3; ++axis) {
              out_acceleration[axis] -= tangential_acceleration[axis];
            }
          }
        }
      } else {
        state->lever_arm_angular_history_valid = false;
        state->lever_arm_angular_history_timestamp_ns = 0;
      }
    }
  } else if (state != nullptr && compensate_tangential) {
    state->lever_arm_angular_history_valid = false;
    state->lever_arm_angular_history_timestamp_ns = 0;
  }

  if (!finite_v3(out_acceleration)) return false;
  const float deadband = std::max(0.0f, cfg.acceleration_deadband_mps2);
  const float magnitude = v3_length(out_acceleration);
  if (!std::isfinite(magnitude)) return false;
  if (magnitude <= deadband) {
    out_acceleration[0] = 0.0f;
    out_acceleration[1] = 0.0f;
    out_acceleration[2] = 0.0f;
  } else if (deadband > 0.0f && magnitude > 1.0e-6f) {
    const float adjusted = magnitude - deadband;
    const float scale = adjusted / magnitude;
    out_acceleration[0] *= scale;
    out_acceleration[1] *= scale;
    out_acceleration[2] *= scale;
  }
  clamp_v3_length(out_acceleration, cfg.max_linear_acceleration_mps2);
  return true;
}

void invalidate_runtime_controller_pose(xr_runtime::RuntimeControllerSideStateV1& out) {
  out.flags &= ~(xr_runtime::RUNTIME_CONTROLLER_POSE_VALID |
                 xr_runtime::RUNTIME_CONTROLLER_TRACKED |
                 xr_runtime::RUNTIME_CONTROLLER_SYNTHETIC_POSE);
  out.flags |= xr_runtime::RUNTIME_CONTROLLER_POSE_INVALID;
  out.source_mask &= ~(xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_POSITION |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_LAST_GOOD_HAND_POSE |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_SYNTHETIC_POSE |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_POSITION_PREDICTION);
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_POSE_INVALID;
  out.linear_velocity[0] = 0.0f;
  out.linear_velocity[1] = 0.0f;
  out.linear_velocity[2] = 0.0f;
}

uint64_t nonnegative_ms_to_ns(float value_ms) {
  const double value = std::max(0.0, static_cast<double>(value_ms)) * 1.0e6;
  if (value >= static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(value);
}

uint64_t saturating_add_u64(uint64_t a, uint64_t b) {
  if (std::numeric_limits<uint64_t>::max() - a < b) {
    return std::numeric_limits<uint64_t>::max();
  }
  return a + b;
}

void clear_imu_position_loss_state(RuntimeControllerImuSideRuntimeState& state,
                                   bool clear_anchor) {
  state.prediction_active = false;
  state.prediction_started = false;
  state.reacquire_blend_active = false;
  state.reacquire_blend_start_ns = 0;
  state.last_update_ns = 0;
  std::fill(std::begin(state.acceleration_position_delta_m),
            std::end(state.acceleration_position_delta_m), 0.0f);
  std::fill(std::begin(state.acceleration_velocity_delta_mps),
            std::end(state.acceleration_velocity_delta_mps), 0.0f);
  state.prediction_path_active = false;
  state.prediction_path_m = 0.0f;
  std::fill(std::begin(state.prediction_path_last_position_m),
            std::end(state.prediction_path_last_position_m), 0.0f);
  state.prediction_path_last_timestamp_ns = 0;
  state.lever_arm_angular_history_valid = false;
  state.lever_arm_angular_history_timestamp_ns = 0;
  std::fill(std::begin(state.lever_arm_previous_angular_velocity_world_rad_s),
            std::end(state.lever_arm_previous_angular_velocity_world_rad_s),
            0.0f);
  std::fill(std::begin(state.lever_arm_filtered_angular_acceleration_world_rad_s2),
            std::end(state.lever_arm_filtered_angular_acceleration_world_rad_s2),
            0.0f);
  if (clear_anchor) {
    state.has_position_anchor = false;
    state.last_optical_pose_ns = 0;
    std::fill(std::begin(state.anchor_position_m),
              std::end(state.anchor_position_m), 0.0f);
    std::fill(std::begin(state.anchor_velocity_mps),
              std::end(state.anchor_velocity_mps), 0.0f);
    state.lever_arm_anchor_valid = false;
    std::fill(std::begin(state.lever_arm_anchor_world_m),
              std::end(state.lever_arm_anchor_world_m), 0.0f);
    std::fill(std::begin(state.lever_arm_pivot_anchor_position_m),
              std::end(state.lever_arm_pivot_anchor_position_m), 0.0f);
    std::fill(std::begin(state.lever_arm_pivot_anchor_velocity_mps),
              std::end(state.lever_arm_pivot_anchor_velocity_mps), 0.0f);
    state.position_history.reset();
    state.position_history_last_frame_sequence = 0;
  }
}

void update_lever_arm_anchor(
    RuntimeControllerImuSideRuntimeState& state,
    const RuntimeImuSample& imu,
    const RuntimeControllerImuMotionConfig& cfg) {
  state.lever_arm_anchor_valid = false;
  if (!cfg.lever_arm_enabled || !imu.orientation_valid ||
      !finite_v3(cfg.lever_arm_local_m)) {
    return;
  }

  float lever_world[3] = {};
  q_rotate(imu.orientation, cfg.lever_arm_local_m, lever_world);
  if (!finite_v3(lever_world)) return;

  state.lever_arm_anchor_valid = true;
  for (int axis = 0; axis < 3; ++axis) {
    state.lever_arm_anchor_world_m[axis] = lever_world[axis];
    state.lever_arm_pivot_anchor_position_m[axis] =
        state.anchor_position_m[axis] - lever_world[axis];
    state.lever_arm_pivot_anchor_velocity_mps[axis] =
        state.anchor_velocity_mps[axis];
  }

  // The optical controller velocity already contains tangential motion caused
  // by rotation about the pivot. Remove omega x r so that adding the live
  // rotated lever arm during prediction does not count it twice.
  if (imu.angular_velocity_valid) {
    float angular_velocity_world[3] = {};
    float tangential_velocity[3] = {};
    q_rotate(imu.orientation, imu.angular_velocity_rad_s,
             angular_velocity_world);
    cross_v3(angular_velocity_world, lever_world, tangential_velocity);
    if (finite_v3(tangential_velocity)) {
      for (int axis = 0; axis < 3; ++axis) {
        state.lever_arm_pivot_anchor_velocity_mps[axis] -=
            tangential_velocity[axis];
      }
      clamp_v3_length(state.lever_arm_pivot_anchor_velocity_mps,
                      cfg.max_prediction_velocity_mps);
    }
  }
}

void accept_optical_position_anchor(
    RuntimeControllerImuSideRuntimeState& state,
    const xr_runtime::HandSideF32V2& hand_side,
    const RuntimeImuSample& imu,
    const RuntimeControllerImuMotionConfig& cfg,
    uint64_t timestamp_ns) {
  state.has_position_anchor = true;
  state.last_optical_pose_ns = timestamp_ns;
  state.last_update_ns = timestamp_ns;
  state.anchor_position_m[0] = hand_side.controller_px;
  state.anchor_position_m[1] = hand_side.controller_py;
  state.anchor_position_m[2] = hand_side.controller_pz;
  std::copy(std::begin(state.anchor_position_m),
            std::end(state.anchor_position_m), state.position_m);

  if (cfg.prediction_window_mode) {
    double estimated_velocity[3] = {};
    if (state.position_history.estimate_velocity(estimated_velocity)) {
      state.anchor_velocity_mps[0] = static_cast<float>(estimated_velocity[0]);
      state.anchor_velocity_mps[1] = static_cast<float>(estimated_velocity[1]);
      state.anchor_velocity_mps[2] = static_cast<float>(estimated_velocity[2]);
      clamp_v3_length(state.anchor_velocity_mps, cfg.max_prediction_velocity_mps);
    } else {
      std::fill(std::begin(state.anchor_velocity_mps),
                std::end(state.anchor_velocity_mps), 0.0f);
    }
  } else if ((hand_side.flags & xr_runtime::HAND_LINEAR_VELOCITY_VALID) != 0u &&
             std::isfinite(hand_side.vx) && std::isfinite(hand_side.vy) &&
             std::isfinite(hand_side.vz)) {
    state.anchor_velocity_mps[0] = hand_side.vx;
    state.anchor_velocity_mps[1] = hand_side.vy;
    state.anchor_velocity_mps[2] = hand_side.vz;
    clamp_v3_length(state.anchor_velocity_mps, cfg.max_prediction_velocity_mps);
  } else {
    std::fill(std::begin(state.anchor_velocity_mps),
              std::end(state.anchor_velocity_mps), 0.0f);
  }
  std::copy(std::begin(state.anchor_velocity_mps),
            std::end(state.anchor_velocity_mps), state.velocity_mps);

  // Lever-arm mode changes only the trajectory calculation used later while
  // Predicting. Capture an equivalent pivot anchor without changing any
  // prediction state or transition.
  update_lever_arm_anchor(state, imu, cfg);

  clear_imu_position_loss_state(state, false);
  state.last_update_ns = timestamp_ns;
}

void publish_synthetic_controller_position(
    xr_runtime::RuntimeControllerSideStateV1& out,
    const float position_m[3],
    const float velocity_mps[3],
    bool publish_velocity,
    bool imu_prediction,
    uint64_t pose_timestamp_ns) {
  out.flags &= ~xr_runtime::RUNTIME_CONTROLLER_POSE_INVALID;
  out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED |
               xr_runtime::RUNTIME_CONTROLLER_POSE_VALID |
               xr_runtime::RUNTIME_CONTROLLER_TRACKED |
               xr_runtime::RUNTIME_CONTROLLER_SYNTHETIC_POSE |
               xr_runtime::RUNTIME_CONTROLLER_POSE_STALE;
  out.source_mask &= ~(xr_runtime::RUNTIME_CONTROLLER_SOURCE_POSE_INVALID |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_POSITION |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_POSITION_PREDICTION);
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_LAST_GOOD_HAND_POSE |
                     xr_runtime::RUNTIME_CONTROLLER_SOURCE_SYNTHETIC_POSE;
  if (imu_prediction) {
    out.source_mask |=
        xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_POSITION_PREDICTION;
  }
  out.position[0] = position_m[0];
  out.position[1] = position_m[1];
  out.position[2] = position_m[2];
  if (publish_velocity) {
    out.linear_velocity[0] = velocity_mps[0];
    out.linear_velocity[1] = velocity_mps[1];
    out.linear_velocity[2] = velocity_mps[2];
  } else {
    out.linear_velocity[0] = 0.0f;
    out.linear_velocity[1] = 0.0f;
    out.linear_velocity[2] = 0.0f;
  }
  out.last_pose_ns = pose_timestamp_ns;
}

void update_or_apply_imu_position_prediction(
    xr_runtime::RuntimeControllerSideStateV1& out,
    const RuntimeImuSample& imu,
    const RuntimeControllerImuMotionConfig& cfg,
    RuntimeControllerImuSideRuntimeState* state,
    const xr_runtime::HandSideF32V2* hand_side,
    uint64_t timestamp_ns,
    uint64_t hand_frame_sequence,
    uint64_t hand_source_timestamp_ns) {
  if (state == nullptr) return;

  const uint64_t hold_ns = nonnegative_ms_to_ns(cfg.hold_lost_ms);
  const uint64_t predict_ns = nonnegative_ms_to_ns(cfg.predict_lost_ms);
  const uint64_t lost_output_ns = saturating_add_u64(hold_ns, predict_ns);
  const uint64_t blend_ns = nonnegative_ms_to_ns(cfg.reacquire_blend_ms);

  if (real_optical_hand_pose(hand_side)) {
    const float optical_position[3] = {
        hand_side->controller_px,
        hand_side->controller_py,
        hand_side->controller_pz,
    };
    if (cfg.prediction_window_mode &&
        (hand_frame_sequence == 0 ||
         hand_frame_sequence != state->position_history_last_frame_sequence)) {
      const uint64_t sample_timestamp_ns =
          hand_source_timestamp_ns != 0 ? hand_source_timestamp_ns : timestamp_ns;
      state->position_history.add(sample_timestamp_ns,
                                  optical_position[0],
                                  optical_position[1],
                                  optical_position[2],
                                  cfg.prediction_window_ms);
      state->position_history_last_frame_sequence = hand_frame_sequence;
    }
    float optical_velocity[3] = {};
    const bool optical_velocity_valid =
        (hand_side->flags & xr_runtime::HAND_LINEAR_VELOCITY_VALID) != 0u &&
        std::isfinite(hand_side->vx) && std::isfinite(hand_side->vy) &&
        std::isfinite(hand_side->vz);
    if (optical_velocity_valid) {
      optical_velocity[0] = hand_side->vx;
      optical_velocity[1] = hand_side->vy;
      optical_velocity[2] = hand_side->vz;
      clamp_v3_length(optical_velocity, cfg.max_prediction_velocity_mps);
    }

    // Match HandPoseStabilityFilter: blend only when returning from an actual
    // prediction phase. A hand that returned during hold can be accepted
    // directly because the published coordinate never moved.
    if (!state->reacquire_blend_active && state->prediction_active &&
        blend_ns > 0 && state->has_position_anchor) {
      state->reacquire_blend_active = true;
      state->reacquire_blend_start_ns = timestamp_ns;
      std::copy(std::begin(state->position_m), std::end(state->position_m),
                state->reacquire_blend_from_position_m);
      std::copy(std::begin(state->velocity_mps), std::end(state->velocity_mps),
                state->reacquire_blend_from_velocity_mps);
      state->prediction_active = false;
    }

    if (state->reacquire_blend_active) {
      const uint64_t elapsed_ns = timestamp_ns >= state->reacquire_blend_start_ns
          ? timestamp_ns - state->reacquire_blend_start_ns
          : 0;
      const float t = blend_ns > 0
          ? std::clamp(static_cast<float>(elapsed_ns) /
                           static_cast<float>(blend_ns),
                       0.0f, 1.0f)
          : 1.0f;
      for (int axis = 0; axis < 3; ++axis) {
        state->position_m[axis] =
            state->reacquire_blend_from_position_m[axis] +
            (optical_position[axis] -
             state->reacquire_blend_from_position_m[axis]) * t;
        const float target_velocity =
            optical_velocity_valid ? optical_velocity[axis] : 0.0f;
        state->velocity_mps[axis] =
            state->reacquire_blend_from_velocity_mps[axis] +
            (target_velocity -
             state->reacquire_blend_from_velocity_mps[axis]) * t;
      }

      if (t < 1.0f) {
        // As in HandPoseStabilityFilter, the blended output becomes the latest
        // continuity anchor. If optical tracking drops again mid-blend, the
        // next hold/predict cycle starts from what was actually published.
        state->has_position_anchor = true;
        state->last_optical_pose_ns = timestamp_ns;
        state->last_update_ns = timestamp_ns;
        std::copy(std::begin(state->position_m), std::end(state->position_m),
                  state->anchor_position_m);
        std::copy(std::begin(state->velocity_mps), std::end(state->velocity_mps),
                  state->anchor_velocity_mps);
        state->prediction_path_active = false;
        state->prediction_path_m = 0.0f;
        std::copy(std::begin(state->position_m), std::end(state->position_m),
                  state->prediction_path_last_position_m);
        state->prediction_path_last_timestamp_ns = timestamp_ns;
        update_lever_arm_anchor(*state, imu, cfg);
        publish_synthetic_controller_position(
            out, state->position_m, state->velocity_mps, true, true,
            imu.timestamp_ns != 0 ? imu.timestamp_ns : timestamp_ns);
        return;
      }
    }

    accept_optical_position_anchor(*state, *hand_side, imu, cfg, timestamp_ns);
    return;
  }

  if (!cfg.position_prediction_enabled || !imu.orientation_valid ||
      !state->has_position_anchor || state->last_optical_pose_ns == 0 ||
      timestamp_ns <= state->last_optical_pose_ns) {
    state->prediction_active = false;
    state->prediction_started = false;
    state->reacquire_blend_active = false;
    return;
  }

  const uint64_t elapsed_ns = timestamp_ns - state->last_optical_pose_ns;
  if (lost_output_ns == 0 || elapsed_ns > lost_output_ns) {
    clear_imu_position_loss_state(*state, true);
    invalidate_runtime_controller_pose(out);
    return;
  }

  state->reacquire_blend_active = false;

  // Phase 1 is identical to image-based hand prediction: hold the last good
  // optical coordinate and expose no stale velocity to downstream prediction.
  if (elapsed_ns <= hold_ns || predict_ns == 0) {
    state->prediction_active = false;
    state->prediction_started = false;
    std::copy(std::begin(state->anchor_position_m),
              std::end(state->anchor_position_m), state->position_m);
    std::fill(std::begin(state->velocity_mps),
              std::end(state->velocity_mps), 0.0f);
    publish_synthetic_controller_position(
        out, state->position_m, state->velocity_mps, false, false,
        imu.timestamp_ns != 0 ? imu.timestamp_ns : timestamp_ns);
    return;
  }

  // Phase 2 uses the same prediction window, damping curve and timeout as the
  // image path. Only coordinate calculation differs: optional IMU acceleration
  // is added to the last optical velocity trajectory.
  const uint64_t prediction_elapsed_ns = elapsed_ns - hold_ns;
  const uint64_t clamped_prediction_ns =
      std::min(prediction_elapsed_ns, predict_ns);
  const float prediction_elapsed_s =
      static_cast<float>(clamped_prediction_ns) / 1.0e9f;
  const float progress = predict_ns > 0
      ? std::clamp(static_cast<float>(clamped_prediction_ns) /
                       static_cast<float>(predict_ns),
                   0.0f, 1.0f)
      : 1.0f;
  const float damping = std::clamp(cfg.prediction_damping, 0.0f, 1.0f);

  if (!state->prediction_started) {
    state->prediction_started = true;
    state->last_update_ns = timestamp_ns;
    std::fill(std::begin(state->acceleration_position_delta_m),
              std::end(state->acceleration_position_delta_m), 0.0f);
    std::fill(std::begin(state->acceleration_velocity_delta_mps),
              std::end(state->acceleration_velocity_delta_mps), 0.0f);
    state->prediction_path_active = true;
    state->prediction_path_m = 0.0f;
    std::copy(std::begin(state->anchor_position_m),
              std::end(state->anchor_position_m),
              state->prediction_path_last_position_m);
    state->prediction_path_last_timestamp_ns = state->last_optical_pose_ns;
  }

  uint64_t dt_ns = state->last_update_ns != 0 &&
                           timestamp_ns > state->last_update_ns
                       ? timestamp_ns - state->last_update_ns
                       : 0;
  dt_ns = std::min<uint64_t>(dt_ns, 50'000'000ull);
  const float dt_s = static_cast<float>(dt_ns) / 1.0e9f;
  state->last_update_ns = timestamp_ns;

  float acceleration[3] = {};
  const bool acceleration_valid = runtime_world_linear_acceleration(
      imu, cfg, state, timestamp_ns, acceleration);
  if (dt_s > 0.0f && acceleration_valid) {
    // Fade the acceleration contribution with the same remaining-window factor
    // used for published prediction velocity. This keeps noisy IMU acceleration
    // from causing a final-frame kick immediately before timeout.
    const float acceleration_weight = 1.0f - progress;
    for (int axis = 0; axis < 3; ++axis) {
      const float a = acceleration[axis] * acceleration_weight;
      state->acceleration_position_delta_m[axis] +=
          state->acceleration_velocity_delta_mps[axis] * dt_s +
          0.5f * a * dt_s * dt_s;
      state->acceleration_velocity_delta_mps[axis] += a * dt_s;
    }
    clamp_v3_length(state->acceleration_velocity_delta_mps,
                    cfg.max_prediction_velocity_mps);
  }

  const float integrated_time_s =
      prediction_elapsed_s * (1.0f - 0.5f * progress);
  const bool lever_arm_requested =
      cfg.lever_arm_enabled && state->lever_arm_anchor_valid &&
      imu.orientation_valid && finite_v3(cfg.lever_arm_local_m);
  float current_lever_world[3] = {};
  bool use_lever_arm = false;
  if (lever_arm_requested) {
    q_rotate(imu.orientation, cfg.lever_arm_local_m, current_lever_world);
    use_lever_arm = finite_v3(current_lever_world);
  }
  const float* base_anchor_position = use_lever_arm
      ? state->lever_arm_pivot_anchor_position_m
      : state->anchor_position_m;
  const float* base_anchor_velocity = use_lever_arm
      ? state->lever_arm_pivot_anchor_velocity_mps
      : state->anchor_velocity_mps;

  for (int axis = 0; axis < 3; ++axis) {
    const float base_delta =
        base_anchor_velocity[axis] * damping * integrated_time_s;
    state->position_m[axis] = base_anchor_position[axis] + base_delta +
                              state->acceleration_position_delta_m[axis] +
                              (use_lever_arm ? current_lever_world[axis] : 0.0f);
    state->velocity_mps[axis] =
        (base_anchor_velocity[axis] * damping +
         state->acceleration_velocity_delta_mps[axis]) *
        (1.0f - progress);
  }

  // Keep published velocity consistent with the curved position trajectory.
  // This changes only the value produced inside Predicting; timeout and all
  // state-machine transitions remain untouched.
  if (use_lever_arm && imu.angular_velocity_valid) {
    float angular_velocity_world[3] = {};
    float tangential_velocity[3] = {};
    q_rotate(imu.orientation, imu.angular_velocity_rad_s,
             angular_velocity_world);
    cross_v3(angular_velocity_world, current_lever_world,
             tangential_velocity);
    if (finite_v3(tangential_velocity)) {
      const float remaining = 1.0f - progress;
      for (int axis = 0; axis < 3; ++axis) {
        state->velocity_mps[axis] += tangential_velocity[axis] * remaining;
      }
    }
  }
  clamp_v3_length(state->velocity_mps, cfg.max_prediction_velocity_mps);

  const float max_prediction_path_m = cfg.max_prediction_path_m;
  if (std::isfinite(max_prediction_path_m) && max_prediction_path_m > 0.0f &&
      state->prediction_path_active &&
      timestamp_ns > state->prediction_path_last_timestamp_ns) {
    const float step_delta[3] = {
        state->position_m[0] - state->prediction_path_last_position_m[0],
        state->position_m[1] - state->prediction_path_last_position_m[1],
        state->position_m[2] - state->prediction_path_last_position_m[2],
    };
    const float step_m = v3_length(step_delta);
    if (std::isfinite(step_m) &&
        state->prediction_path_m + step_m > max_prediction_path_m) {
      clear_imu_position_loss_state(*state, true);
      invalidate_runtime_controller_pose(out);
      return;
    }
    if (std::isfinite(step_m)) state->prediction_path_m += step_m;
    std::copy(std::begin(state->position_m), std::end(state->position_m),
              state->prediction_path_last_position_m);
    state->prediction_path_last_timestamp_ns = timestamp_ns;
  }

  publish_synthetic_controller_position(
      out, state->position_m, state->velocity_mps,
      cfg.publish_predicted_velocity, true,
      imu.timestamp_ns != 0 ? imu.timestamp_ns : timestamp_ns);
  state->prediction_active = true;
}


bool controller_side_has_movement_input(const xr_runtime::ControllerDeviceStateV3& controller) {
  if (!controller_side_is_present(controller)) return false;
  const uint64_t buttons = normalize_controller_dpad_buttons(controller.buttons);
  const uint64_t dpad_mask = xr_runtime::CONTROLLER_BUTTON_DPAD_UP |
                             xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN |
                             xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT |
                             xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT;
  return (buttons & dpad_mask) != 0ull ||
         std::abs(controller.thumbstick_x) > 0.05f ||
         std::abs(controller.thumbstick_y) > 0.05f;
}

Qf horizontal_yaw_q_from_q(Qf q) {
  float right[3]{};
  float forward[3]{};
  if (!horizontal_yaw_basis_from_q(q, right, forward)) return {};
  // local forward is -Z. For a yaw-only quaternion around +Y:
  // yaw=0 -> forward=(0,0,-1). Therefore yaw = atan2(-x, -z).
  const float yaw = std::atan2(-forward[0], -forward[2]);
  return normalize_q({std::cos(0.5f * yaw), 0.0f, std::sin(0.5f * yaw), 0.0f});
}

void apply_hmd_yaw_orientation_for_movement(
    xr_runtime::RuntimeControllerSideStateV1& out,
    const xr_runtime::HmdPoseF64V1& hmd,
    const float static_q_xyzw[4]) {
  if ((out.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) == 0u) return;
  if ((hmd.flags & xr_runtime::HMD_FLAG_POSE_VALID) == 0u || hmd.tracking_status != 2u) return;

  const Qf hmd_q = normalize_q({static_cast<float>(hmd.qw),
                                static_cast<float>(hmd.qx),
                                static_cast<float>(hmd.qy),
                                static_cast<float>(hmd.qz)});
  const Qf hmd_yaw_q = horizontal_yaw_q_from_q(hmd_q);
  set_orientation_xyzw(out, q_mul(hmd_yaw_q, q_from_xyzw(static_q_xyzw)));

  // Position still comes from hand tracking; only orientation is synthesized for
  // locomotion. Keep the hand-position source, but do not advertise hand yaw.
  out.source_mask &= ~(xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_ORIENTATION |
                       xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_ORIENTATION);
  out.source_mask |= xr_runtime::RUNTIME_CONTROLLER_SOURCE_STATIC_ORIENTATION;
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

  // Runtime hand gate publishes held/predicted lost-hand poses as status=2
  // (degraded). Treat them as valid controller poses; otherwise SteamVR/OpenVR
  // controller synthesis immediately falls back to invalid/HMD-relative pose and
  // the 1-1.5s runtime hand prediction is invisible to the controller path.
  if (side.status != 1u && side.status != 2u && side.status != 3u) return false;
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
    const xr_runtime::ControllerDeviceStateV3* controller_side) {
  switch (mode) {
    case LostHandPoseFallbackMode::PoseInvalid:
      return false;
    case LostHandPoseFallbackMode::HmdRelativeWithControllerInput:
      return controller_side != nullptr && controller_side_has_nonzero_input(*controller_side);
    case LostHandPoseFallbackMode::HmdRelativeWithControllerPresent:
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
                                 const xr_runtime::ControllerDeviceStateV3& controller,
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
                  const xr_runtime::HandTrackingFrameF32V2* optical_yaw_hand,
                  const xr_runtime::ControllerInputV3* controller_input,
                  const xr_runtime::HmdPoseF64V1* hmd,
                  uint64_t timestamp_ns,
                  RuntimeControllerImuSideRuntimeState* runtime_state) {
  out.role = left ? xr_runtime::CONTROLLER_SIDE_LEFT : xr_runtime::CONTROLLER_SIDE_RIGHT;
  copy_debug_source(out, xr_runtime::runtime_controller_mode_name(cfg.mode));

  const xr_runtime::HandSideF32V2* hand_side = nullptr;
  bool valid_hand_side = false;
  if (hand != nullptr) {
    hand_side = left ? &hand->left : &hand->right;
    valid_hand_side = hand_side_is_valid(*hand, *hand_side, left);
  }

  const xr_runtime::HandSideF32V2* optical_yaw_hand_side = nullptr;
  uint64_t optical_yaw_frame_sequence = 0;
  if (optical_yaw_hand != nullptr) {
    optical_yaw_hand_side = left ? &optical_yaw_hand->left : &optical_yaw_hand->right;
    optical_yaw_frame_sequence = optical_yaw_hand->sequence;
  }

  const xr_runtime::ControllerDeviceStateV3* controller_side = nullptr;
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

  const bool using_lost_hand_hmd_relative_pose =
      !valid_hand_side && use_hmd_relative_lost_hand_fallback &&
      (out.source_mask &
       xr_runtime::RUNTIME_CONTROLLER_SOURCE_HMD_RELATIVE_POSE) != 0u;

  if (controller_side != nullptr) {
    fill_inputs_from_controller(out, *controller_side, cfg);
    const RuntimeControllerOrientationSource configured_orientation_source =
        left ? cfg.left_orientation_source : cfg.right_orientation_source;
    const RuntimeControllerOrientationSource orientation_source =
        effective_runtime_controller_orientation_source(configured_orientation_source, controller_side);
    if (orientation_source == RuntimeControllerOrientationSource::ImuOverrideControllerRuntime) {
      const RuntimeControllerImuOrientationConfig& imu_orientation_cfg =
          left ? cfg.left_imu_orientation : cfg.right_imu_orientation;
      const RuntimeControllerImuMotionConfig& imu_motion_cfg =
          left ? cfg.left_imu_motion : cfg.right_imu_motion;
      const RuntimeImuSample physical_imu =
          runtime_imu_sample(*controller_side, imu_orientation_cfg);
      RuntimeImuSample presentation_imu = physical_imu;

      // Yaw correction is a presentation-only wrist-orientation adjustment.
      // It samples the raw backend hand frame so predicted or held runtime
      // poses never enter the optical correction window, but it must not alter
      // the physical IMU frame used by spatial position prediction.
      apply_runtime_yaw_correction(
          presentation_imu, imu_motion_cfg, runtime_state,
          optical_yaw_hand_side, optical_yaw_frame_sequence, timestamp_ns);

      // Keep the uncorrected physical IMU sample for position prediction,
      // lever-arm trajectory, acceleration world transform and rotational
      // acceleration compensation. Periodic/reacquire yaw correction therefore
      // cannot move the controller in space or bend its predicted trajectory.
      update_or_apply_imu_position_prediction(
          out, physical_imu, imu_motion_cfg, runtime_state, hand_side,
          timestamp_ns, hand != nullptr ? hand->sequence : 0,
          hand != nullptr ? hand->source_timestamp_ns : 0);

      // The lost-hand HMD-relative fallback owns the complete published
      // orientation. Otherwise publish only the presentation copy with yaw
      // correction; position above remains based on the raw physical sample.
      if (!using_lost_hand_hmd_relative_pose) {
        apply_imu_orientation_override(out, presentation_imu);
      }
    } else if (runtime_state != nullptr) {
      // The feature is dormant while this side uses hand-tracking fallback.
      // Drop the old optical anchor so a later IMU reconnect cannot resume a
      // prediction from stale coordinates.
      clear_imu_position_loss_state(*runtime_state, true);
    }

    const RuntimeControllerMovementSpace movement_space =
        effective_runtime_controller_movement_space(cfg, left, orientation_source);
    if (movement_space == RuntimeControllerMovementSpace::HmdPose &&
               hmd != nullptr &&
               controller_side_has_movement_input(*controller_side)) {
      // Pose-space fix: for games/bindings that use controller yaw as locomotion
      // reference and treat D-pad/stick as forward/back/strafe commands.
      apply_hmd_yaw_orientation_for_movement(out, *hmd, static_q);
    }
  }

  const bool hand_gestures_enabled = left ? cfg.left_hand_gestures_enabled
                                          : cfg.right_hand_gestures_enabled;
  if (cfg.mode == xr_runtime::RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY &&
      valid_hand_side && hand_gestures_enabled) {
    // Whether hand gestures are allowed while an external ControllerInputV3 stream
    // exists is decided by xr_runtime_adapter per frame.  Do not key this off
    // controller_side presence here, otherwise hand_plus_controller cannot combine
    // physical controller input with pinch/grab gestures.
    merge_hand_gestures(out, *hand_side);
  }

  if ((out.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) == 0u &&
      controller_side != nullptr && controller_side_is_present(*controller_side)) {
    out.flags |= xr_runtime::RUNTIME_CONTROLLER_CONNECTED;
  }
}

}  // namespace

RuntimeControllerMovementSpace parse_runtime_controller_movement_space(
    const std::string& value,
    const char* option_name) {
  if (value == "controller" || value == "hand" ||
      value == "controller_local" || value == "local") {
    return RuntimeControllerMovementSpace::Controller;
  }
  if (value == "hmd_pose" || value == "head_pose" ||
      value == "hmd_orientation" || value == "head_orientation" ||
      value == "hmd_yaw" || value == "head_yaw") {
    return RuntimeControllerMovementSpace::HmdPose;
  }
  throw std::runtime_error(std::string(option_name) +
                           " must be one of: controller, hmd_pose");
}

const char* runtime_controller_movement_space_name(RuntimeControllerMovementSpace value) {
  switch (value) {
    case RuntimeControllerMovementSpace::Controller:
      return "controller";
    case RuntimeControllerMovementSpace::HmdPose:
      return "hmd_pose";
  }
  return "unknown";
}

RuntimeControllerOrientationSource parse_runtime_controller_orientation_source(
    const std::string& value,
    const char* option_name) {
  if (value == "HAND_TRACKING_BACKEND" || value == "hand_tracking_backend" ||
      value == "hand_tracking" || value == "backend") {
    return RuntimeControllerOrientationSource::HandTrackingBackend;
  }
  if (value == "IMU_OVERRIDE_CONTROLLER_RUNTIME" ||
      value == "imu_override_controller_runtime" ||
      value == "imu") {
    return RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
  }
  throw std::runtime_error(std::string(option_name) +
                           " must be one of: HAND_TRACKING_BACKEND, "
                           "IMU_OVERRIDE_CONTROLLER_RUNTIME");
}

const char* runtime_controller_orientation_source_name(RuntimeControllerOrientationSource value) {
  switch (value) {
    case RuntimeControllerOrientationSource::HandTrackingBackend:
      return "HAND_TRACKING_BACKEND";
    case RuntimeControllerOrientationSource::ImuOverrideControllerRuntime:
      return "IMU_OVERRIDE_CONTROLLER_RUNTIME";
  }
  return "UNKNOWN";
}

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
  if (value == "hmd_relative_with_controller_present" ||
      value == "hmd_relative_when_controller_present" ||
      value == "hmd_relative_with_stream" ||
      value == "hmd_relative_when_stream_present") {
    return LostHandPoseFallbackMode::HmdRelativeWithControllerPresent;
  }
  if (value == "hmd_relative" || value == "body_locked" || value == "body") {
    return LostHandPoseFallbackMode::HmdRelative;
  }
  throw std::runtime_error(std::string(option_name) +
                           " must be one of: pose_invalid, hmd_relative_with_input, "
                           "hmd_relative_with_controller_present, hmd_relative");
}

const char* lost_hand_pose_fallback_mode_name(LostHandPoseFallbackMode mode) {
  switch (mode) {
    case LostHandPoseFallbackMode::PoseInvalid:
      return "pose_invalid";
    case LostHandPoseFallbackMode::HmdRelativeWithControllerInput:
      return "hmd_relative_with_input";
    case LostHandPoseFallbackMode::HmdRelativeWithControllerPresent:
      return "hmd_relative_with_controller_present";
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

bool controller_device_is_present(const xr_runtime::ControllerDeviceStateV3& controller) {
  return controller_side_is_present(controller);
}

bool controller_device_has_nonzero_input(const xr_runtime::ControllerDeviceStateV3& controller) {
  return controller_side_has_nonzero_input(controller);
}

bool controller_device_has_imu(const xr_runtime::ControllerDeviceStateV3& controller) {
  return (controller.flags & xr_runtime::CONTROLLER_DEVICE_IMU_PRESENT) != 0u ||
         xr_runtime::controller_imu_is_present(controller.imu);
}

bool controller_device_has_active_imu(const xr_runtime::ControllerDeviceStateV3& controller) {
  return (controller.flags & xr_runtime::CONTROLLER_DEVICE_IMU_ACTIVE) != 0u &&
         xr_runtime::controller_imu_has_current_data(controller.imu);
}

bool controller_device_has_active_orientation_imu(
    const xr_runtime::ControllerDeviceStateV3& controller) {
  if (!controller_device_has_active_imu(controller)) return false;
  if ((controller.imu.data_flags & xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID) == 0u) {
    return false;
  }
  return finite_q_xyzw(controller.imu.orientation_xyzw) &&
         nonzero_q_xyzw(controller.imu.orientation_xyzw);
}

RuntimeControllerOrientationSource effective_runtime_controller_orientation_source(
    RuntimeControllerOrientationSource configured,
    const xr_runtime::ControllerDeviceStateV3* controller) {
  if (configured == RuntimeControllerOrientationSource::ImuOverrideControllerRuntime &&
      controller != nullptr &&
      controller_device_has_active_orientation_imu(*controller)) {
    return RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
  }
  return RuntimeControllerOrientationSource::HandTrackingBackend;
}

bool controller_input_has_present_controller(const xr_runtime::ControllerInputV3& controller) {
  return controller_device_is_present(controller.left) ||
         controller_device_is_present(controller.right);
}

bool controller_input_has_nonzero_input(const xr_runtime::ControllerInputV3& controller) {
  return controller_device_has_nonzero_input(controller.left) ||
         controller_device_has_nonzero_input(controller.right);
}

void apply_controller_gesture_override(
    xr_runtime::HandTrackingFrameF32V2& hand,
    const xr_runtime::ControllerInputV3& controller,
    xr_runtime::ControllerInputConflictPolicy policy,
    float trigger_pinch_threshold,
    float grip_grab_threshold) {
  auto apply_side = [&](xr_runtime::HandSideF32V2& side, const xr_runtime::ControllerDeviceStateV3& controller_side) {
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
    const std::optional<xr_runtime::HandTrackingFrameF32V2>& optical_yaw_hand,
    const std::optional<xr_runtime::ControllerInputV3>& controller_input,
    const std::optional<xr_runtime::HmdPoseF64V1>& runtime_hmd_pose,
    RuntimeControllerSynthesisState* runtime_state) {
  xr_runtime::RuntimeControllerStateFrameV1 frame{};
  frame.sequence = sequence;
  frame.timestamp_ns = timestamp_ns;

  const xr_runtime::HandTrackingFrameF32V2* hand = filtered_hand ? &(*filtered_hand) : nullptr;
  const xr_runtime::HandTrackingFrameF32V2* yaw_hand =
      optical_yaw_hand ? &(*optical_yaw_hand) : nullptr;
  const xr_runtime::ControllerInputV3* controller = controller_input ? &(*controller_input) : nullptr;
  const xr_runtime::HmdPoseF64V1* hmd = runtime_hmd_pose ? &(*runtime_hmd_pose) : nullptr;

  compose_side(frame.left, true, cfg, hand, yaw_hand, controller, hmd, timestamp_ns,
               runtime_state != nullptr ? &runtime_state->left : nullptr);
  compose_side(frame.right, false, cfg, hand, yaw_hand, controller, hmd, timestamp_ns,
               runtime_state != nullptr ? &runtime_state->right : nullptr);

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
