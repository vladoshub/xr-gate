#include <xr_override_controller/imu/controller_imu_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

constexpr double kEpsilon = 1.0e-5;

void require_near(double actual, double expected, const char* label) {
  if (std::abs(actual - expected) > kEpsilon) {
    std::cerr << label << ": expected " << expected << ", got " << actual << "\n";
    std::exit(1);
  }
}

void require_identity(const xr_runtime::ControllerImuStateV1& state, const char* label) {
  require_near(state.orientation_xyzw[0], 0.0, label);
  require_near(state.orientation_xyzw[1], 0.0, label);
  require_near(state.orientation_xyzw[2], 0.0, label);
  require_near(std::abs(state.orientation_xyzw[3]), 1.0, label);
}

}  // namespace

int main() {
  xr_runtime::ControllerImuStateV1 state{};
  state.data_flags = xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID;

  const double half = std::sqrt(0.5);
  state.orientation_xyzw[0] = static_cast<float>(half);
  state.orientation_xyzw[3] = static_cast<float>(half);

  xr_override_controller::OrientationOffsetConfig post{};
  post.enabled = true;
  post.multiply_order = "post";
  post.quaternion_xyzw = {{-half, 0.0, 0.0, half}};
  require_identity(xr_override_controller::imu::apply_orientation_offset(state, post),
                   "post offset");

  xr_override_controller::OrientationOffsetConfig pre = post;
  pre.multiply_order = "pre";
  require_identity(xr_override_controller::imu::apply_orientation_offset(state, pre),
                   "pre offset");

  // Verify the multiplication order with non-commuting X/Y rotations.
  xr_runtime::ControllerImuStateV1 order_state{};
  order_state.data_flags = xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID;
  order_state.orientation_xyzw[0] = static_cast<float>(half);  // +90 deg X
  order_state.orientation_xyzw[3] = static_cast<float>(half);
  xr_override_controller::OrientationOffsetConfig order_offset{};
  order_offset.enabled = true;
  order_offset.quaternion_xyzw = {{0.0, half, 0.0, half}};  // +90 deg Y
  order_offset.multiply_order = "post";
  const auto post_order =
      xr_override_controller::imu::apply_orientation_offset(order_state, order_offset);
  order_offset.multiply_order = "pre";
  const auto pre_order =
      xr_override_controller::imu::apply_orientation_offset(order_state, order_offset);
  require_near(post_order.orientation_xyzw[2], +0.5, "post order z");
  require_near(pre_order.orientation_xyzw[2], -0.5, "pre order z");

  xr_override_controller::OrientationTransformConfig transform{};
  transform.enabled = true;
  transform.basis_rotation.rx_deg = -90.0;
  state.samples[0].angular_velocity_rad_s[1] = 1.0f;
  state.sample_count = 1;
  const auto transformed =
      xr_override_controller::imu::apply_orientation_transform(state, transform);
  require_near(transformed.samples[0].angular_velocity_rad_s[1], 0.0,
               "basis vector y");
  require_near(transformed.samples[0].angular_velocity_rad_s[2], -1.0,
               "basis vector z");

  const auto combined = xr_override_controller::imu::apply_orientation_calibration(
      state, xr_override_controller::OrientationTransformConfig{}, post);
  require_identity(combined, "combined offset");

  xr_runtime::ControllerImuStateV1 force{};
  force.data_flags = xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID;
  force.orientation_xyzw[0] = static_cast<float>(half);
  force.orientation_xyzw[3] = static_cast<float>(half);
  force.sample_count = 2;
  force.samples[0].specific_force_m_s2[0] = 2;
  force.samples[0].specific_force_m_s2[1] = 3;
  force.samples[0].specific_force_m_s2[2] = 4;
  force.samples[1].angular_velocity_rad_s[0] = -1;
  force.samples[1].angular_velocity_rad_s[1] = 2;
  force.samples[1].angular_velocity_rad_s[2] = 5;
  force.magnetic_field_uT[0] = 7;
  force.magnetic_field_uT[1] = -3;
  force.magnetic_field_uT[2] = 1;
  order_offset.multiply_order = "post";
  const auto body = xr_override_controller::imu::apply_orientation_offset(force, order_offset);
  // +90 X followed by +90 local Y maps (x,y,z) to (z,x,y).
  // Thus calibrated (-4,3,2) and original (2,3,4) both give world (2,-4,3).
  require_near(body.samples[0].specific_force_m_s2[0], -4, "mount force x");
  require_near(body.samples[0].specific_force_m_s2[1], 3, "mount force y");
  require_near(body.samples[0].specific_force_m_s2[2], 2, "mount force z");
  require_near(body.samples[1].angular_velocity_rad_s[0], -5, "mount gyro x");
  require_near(body.samples[1].angular_velocity_rad_s[1], 2, "mount gyro y");
  require_near(body.samples[1].angular_velocity_rad_s[2], -1, "mount gyro z");
  require_near(body.magnetic_field_uT[0], -1, "mount magnetic x");
  require_near(body.magnetic_field_uT[1], -3, "mount magnetic y");
  require_near(body.magnetic_field_uT[2], 7, "mount magnetic z");
  order_offset.quaternion_xyzw[1] = -half;
  const auto restored = xr_override_controller::imu::apply_orientation_offset(body, order_offset);
  for (size_t i = 0; i < force.sample_count; ++i) {
    for (int axis = 0; axis < 3; ++axis) {
      require_near(restored.samples[i].specific_force_m_s2[axis],
                   force.samples[i].specific_force_m_s2[axis], "force round trip");
      require_near(restored.samples[i].angular_velocity_rad_s[axis],
                   force.samples[i].angular_velocity_rad_s[axis], "gyro round trip");
    }
  }
  order_offset.multiply_order = "pre";
  const auto world = xr_override_controller::imu::apply_orientation_offset(force, order_offset);
  require_near(world.samples[0].specific_force_m_s2[2], 4, "pre offset leaves body force unchanged");

  std::cout << "controller_imu_transform_test: OK\n";
  return 0;
}
