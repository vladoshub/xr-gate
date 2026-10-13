#include <xr_override_controller/imu/controller_imu_processor.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace xr_override_controller::imu;
constexpr float kGravity = 9.80665f;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

Vec3f rotate(QuaternionXyzw q, Vec3f v) {
  const Vec3f t{2 * (q.y * v.z - q.z * v.y),
                2 * (q.z * v.x - q.x * v.z),
                2 * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
          v.y + q.w * t.y + q.z * t.x - q.x * t.z,
          v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

void require_zero_linear_acceleration(QuaternionXyzw q, Vec3f force, float tolerance = 0.01f) {
  const auto g = rotate(q, force);
  const float residual = std::sqrt(g.x * g.x + g.y * g.y +
                                  (g.z - kGravity) * (g.z - kGravity));
  require(std::isfinite(residual) && residual < tolerance,
          "stationary tilted controller invents linear acceleration");
}

RawControllerImuSample stationary(Vec3f force) {
  RawControllerImuSample sample{};
  sample.host_timestamp_ns = 1'000'000'000ULL;
  sample.sample_interval_seconds = 0.01f;
  sample.gyroscope_valid = sample.accelerometer_valid = true;
  sample.specific_force_m_s2 = force;
  return sample;
}

void different_initial_grips_and_reset() {
  const Vec3f forces[] = {{0, 0, kGravity}, {0, 0, -kGravity},
                          {kGravity, 0, 0}, {-kGravity, 0, 0},
                          {0, kGravity, 0}, {0, -kGravity, 0},
                          {0, 0.8660254f * kGravity, -0.5f * kGravity},
                          {0.36f * kGravity, -0.48f * kGravity, 0.8f * kGravity}};
  ControllerImuProcessor processor;
  for (const auto force : forces) {
    processor.reset();
    require(!processor.orientation_initialized(), "reset retained old gravity reference");
    const auto sample = stationary(force);
    const auto q = processor.process(sample);
    require(processor.orientation_initialized(), "stationary gravity did not initialize orientation");
    require_zero_linear_acceleration(q, force, 0.0001f);
    for (int i = 1; i <= 60; ++i) {
      auto next = sample;
      next.host_timestamp_ns += uint64_t(i) * 10'000'000;
      require_zero_linear_acceleration(processor.process(next), force);
    }
  }
}

void wait_for_credible_gravity() {
  ControllerImuProcessor processor;
  auto sample = stationary({0, 0, kGravity});
  sample.angular_velocity_rad_s = {1, 0, 0};
  processor.process(sample);
  require(!processor.orientation_initialized(), "moving startup accepted a gravity reference");
  sample.angular_velocity_rad_s = {};
  sample.specific_force_m_s2 = {0, 0, kGravity + 3};
  processor.process(sample);
  require(!processor.orientation_initialized(), "large linear acceleration initialized orientation");
  sample.specific_force_m_s2 = {};
  processor.process(sample);
  require(!processor.orientation_initialized(), "zero accelerometer initialized orientation");
  sample.specific_force_m_s2.x = std::numeric_limits<float>::infinity();
  processor.process(sample);
  require(!processor.orientation_initialized(), "nonfinite accelerometer initialized orientation");
  sample = stationary({0, -kGravity, 0});
  const auto q = processor.process(sample);
  require(processor.orientation_initialized(), "first usable gravity sample was not accepted");
  require_zero_linear_acceleration(q, sample.specific_force_m_s2);
}

void initialization_does_not_follow_motion_again() {
  ControllerImuProcessor processor;
  auto sample = stationary({0, 0, kGravity});
  const auto q = processor.process(sample);
  // Small linear acceleration has a plausible magnitude but must only enter
  // the ordinary slow correction, never snap the initialized orientation.
  sample.specific_force_m_s2.x = 0.5f;
  const auto next = processor.process(sample);
  const float dot = std::abs(q.x * next.x + q.y * next.y + q.z * next.z + q.w * next.w);
  require(dot > 0.999999f, "initialized gravity reference snapped to a moving sample");
}

void explicit_gyro_only_mode() {
  ControllerImuProcessor processor(0);
  require(processor.orientation_initialized(), "gyro-only mode gained a gravity startup gate");
  auto sample = stationary({0, kGravity, 0});
  sample.angular_velocity_rad_s = {0, 0, 1};
  const auto q = processor.process(sample);
  require(std::abs(q.x) < 1e-6f && std::abs(q.y) < 1e-6f && q.z > 0,
          "gyro-only reference started using accelerometer correction");
}

int main() {
  different_initial_grips_and_reset();
  wait_for_credible_gravity();
  initialization_does_not_follow_motion_again();
  explicit_gyro_only_mode();
  std::cout << "Controller gravity initialization: grips, inverted startup, reset, motion gate and gyro-only: OK\n";
}
