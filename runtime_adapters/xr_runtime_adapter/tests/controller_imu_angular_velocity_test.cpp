#include "override_controller.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace xr_runtime;
using namespace xr_runtime_adapter::override_controller;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr uint64_t kStart = 1'000'000'000ULL;

void near(float actual, float expected, const char* message, float tolerance = 1e-5f) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
    throw std::runtime_error(std::string(message) + ": actual=" +
                             std::to_string(actual) + " expected=" + std::to_string(expected));
  }
}

struct Fixture {
  RuntimeControllerSynthesisConfig cfg{};
  RuntimeControllerSynthesisState state{};
  ControllerInputV3 input{};
  HandTrackingFrameF32V2 hand{};

  Fixture() {
    cfg.left_orientation_source = cfg.right_orientation_source =
        RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
    for (auto* motion : {&cfg.left_imu_motion, &cfg.right_imu_motion}) {
      motion->position_prediction_enabled = motion->acceleration_integration_enabled = false;
    }
    hand.flags = HAND_FLAG_LEFT_VALID | HAND_FLAG_RIGHT_VALID;
    hand.hand_count = 2;
    for (auto* side : {&hand.left, &hand.right}) {
      side->flags = HAND_POSE_VALID;
      side->status = 1;
      side->confidence = 1;
      side->controller_qw = 1;
    }
    hand.left.handedness = 1;
    hand.right.handedness = 2;
    for (auto* side : {&input.left, &input.right}) {
      side->status = CONTROLLER_INPUT_ACTIVE;
      side->flags = CONTROLLER_DEVICE_BUTTONS_VALID | CONTROLLER_DEVICE_IMU_ACTIVE;
      side->buttons = CONTROLLER_BUTTON_TRIGGER;
      side->trigger = 1;
      side->imu.status = CONTROLLER_IMU_ACTIVE;
      side->imu.data_flags = CONTROLLER_IMU_ORIENTATION_VALID | CONTROLLER_IMU_GYROSCOPE_VALID;
      side->imu.orientation_xyzw[3] = 1;
      side->imu.sample_count = 1;
      side->imu.samples[0].flags = CONTROLLER_IMU_GYROSCOPE_VALID;
      side->imu.samples[0].angular_velocity_rad_s[0] = 1;
    }
  }

  RuntimeControllerStateFrameV1 tick(int ms, bool optical_yaw = false) {
    const uint64_t ns = kStart + uint64_t(ms) * 1'000'000;
    input.sequence = hand.sequence = uint64_t(ms) + 1;
    input.timestamp_ns = hand.timestamp_ns = hand.source_timestamp_ns = ns;
    for (auto* side : {&input.left, &input.right}) {
      side->imu.sequence = input.sequence;
      side->imu.orientation_timestamp_ns = side->imu.latest_sample_timestamp_ns = ns;
      side->imu.samples[0].timestamp_ns = ns;
    }
    return compose_runtime_controller_state(input.sequence, ns, cfg, hand,
        optical_yaw ? std::optional(hand) : std::nullopt,
        input, std::nullopt, 9.81f, &state);
  }
};

void body_gyro_is_rotated_into_tracking_space() {
  // A controller yawed +90 degrees turns around its own X axis. In tracking
  // coordinates that is rotation around -Z, on both controller sides.
  for (int offset_mode : {0, 1, 2}) {
    Fixture f;
    for (auto* side : {&f.input.left, &f.input.right}) {
      side->imu.orientation_xyzw[1] = side->imu.orientation_xyzw[3] = std::sqrt(0.5f);
    }
    for (auto* cfg : {&f.cfg.left_imu_orientation, &f.cfg.right_imu_orientation}) {
      cfg->offset_enabled = offset_mode != 0;
      cfg->offset_pre_multiply = offset_mode == 2;
      if (offset_mode == 1) {
        // A local grip mount changes body axes, but not world angular velocity.
        cfg->offset_rotation_xyzw[2] = cfg->offset_rotation_xyzw[3] = std::sqrt(0.5f);
      } else if (offset_mode == 2) {
        // A world +90 degree yaw changes -Z into -X.
        cfg->offset_rotation_xyzw[1] = cfg->offset_rotation_xyzw[3] = std::sqrt(0.5f);
      }
    }
    const auto frame = f.tick(0);
    for (const auto* side : {&frame.left, &frame.right}) {
      near(side->angular_velocity[0], offset_mode == 2 ? -1 : 0, "wrong world gyro X");
      near(side->angular_velocity[1], 0, "wrong world gyro Y");
      near(side->angular_velocity[2], offset_mode == 2 ? 0 : -1, "wrong world gyro Z");
      near(side->trigger, 1, "velocity conversion changed physical trigger");
    }
  }
}

void native_grip_axes_and_runtime_basis_are_respected() {
  Fixture f;
  // Native post-mount calibration has already changed identity sensor axes
  // into a grip rolled +90 degrees: sensor X is now grip -Y.
  for (auto* side : {&f.input.left, &f.input.right}) {
    side->imu.orientation_xyzw[2] = side->imu.orientation_xyzw[3] = std::sqrt(0.5f);
    side->imu.samples[0].angular_velocity_rad_s[0] = 0;
    side->imu.samples[0].angular_velocity_rad_s[1] = -1;
  }
  auto frame = f.tick(0);
  for (const auto* side : {&frame.left, &frame.right}) {
    near(side->angular_velocity[0], 1, "native grip mount changed world rotation");
    near(side->angular_velocity[1], 0, "native grip gyro remained local");
    near(side->angular_velocity[2], 0, "native grip mount added world Z rotation");
  }
  for (auto* cfg : {&f.cfg.left_imu_orientation, &f.cfg.right_imu_orientation}) {
    cfg->transform_enabled = true;
    cfg->basis_rotation_xyzw[1] = cfg->basis_rotation_xyzw[3] = std::sqrt(0.5f);
  }
  frame = f.tick(10);
  for (const auto* side : {&frame.left, &frame.right}) {
    near(side->angular_velocity[0], 0, "runtime basis left gyro in old world frame");
    near(side->angular_velocity[1], 0, "runtime basis invented gyro Y");
    near(side->angular_velocity[2], -1, "runtime basis did not rotate world gyro");
  }
}

void yaw_correction_and_angular_velocity_stay_aligned_for_six_minutes() {
  Fixture f;
  for (auto* motion : {&f.cfg.left_imu_motion, &f.cfg.right_imu_motion}) {
    motion->yaw_correction_enabled = motion->yaw_correction_smooth = true;
    motion->yaw_reference_stable_ms = 0;
    motion->yaw_correction_continuous = true;
  }
  for (auto* side : {&f.hand.left, &f.hand.right}) {
    side->controller_qy = side->controller_qw = std::sqrt(0.5f);
  }
  for (int ms = 0; ms <= 360'000; ms += 10) {
    // A gradual 0.1 deg/s provider yaw drift must not turn a physical X-axis
    // flick into a different world-space direction as the session ages.
    const float drift = ms * 0.0001f * kPi / 180;
    for (auto* side : {&f.input.left, &f.input.right}) {
      side->imu.orientation_xyzw[1] = std::sin(drift / 2);
      side->imu.orientation_xyzw[3] = std::cos(drift / 2);
    }
    const auto frame = f.tick(ms, true);
    for (const auto* side : {&frame.left, &frame.right}) {
      // These are world directions for the output yaw (qx=qz=0), independent
      // of the configured retained yaw offset and the provider's raw yaw.
      const float qy = side->orientation_xyzw[1], qw = side->orientation_xyzw[3];
      near(side->angular_velocity[0], 1 - 2 * qy * qy, "yaw-corrected gyro X disagrees with pose");
      near(side->angular_velocity[1], 0, "yaw-corrected gyro invented Y");
      near(side->angular_velocity[2], -2 * qw * qy, "yaw-corrected gyro Z disagrees with pose");
      near(side->angular_velocity[2], -1, "slow drift changed flick direction", 0.002f);
      near(side->trigger, 1, "long session changed physical trigger");
    }
  }
}
}  // namespace

int main() {
  body_gyro_is_rotated_into_tracking_space();
  native_grip_axes_and_runtime_basis_are_respected();
  yaw_correction_and_angular_velocity_stay_aligned_for_six_minutes();
  std::cout << "IMU angular velocity: tracking space, grip mounts, basis and six-minute yaw drift: OK\n";
}
