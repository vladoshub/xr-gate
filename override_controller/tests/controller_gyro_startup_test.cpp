#include <xr_override_controller/imu/controller_imu_processor.hpp>
#include <xr_override_controller/imu/gyro_bias_options.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace xr_override_controller::imu;
constexpr float g = 9.80665f;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
RawControllerImuSample sample(Vec3f gyro = {}) {
  RawControllerImuSample s;
  s.host_timestamp_ns = 1'000'000'000;
  s.sample_interval_seconds = .01f;
  s.specific_force_m_s2 = {0, 0, g};
  s.angular_velocity_rad_s = gyro;
  s.gyroscope_valid = s.accelerometer_valid = true;
  return s;
}
void feed(ControllerImuProcessor& p, RawControllerImuSample& s, int count) {
  for (int i = 0; i < count; ++i) {
    s.host_timestamp_ns += 10'000'000;
    p.process(s);
  }
}
void stationary_bias_is_atomic_and_preserves_real_motion() {
  ControllerImuProcessor p;
  auto s = sample({.002f, -.003f, .004f});
  feed(p, s, 150);
  require(!p.gyro_calibrated() && !p.orientation_ready_for_runtime(), "partial window advertised as calibrated");
  require(p.gyro_bias_rad_s().z == 0, "partial window changed applied bias");
  feed(p, s, 1);
  require(p.gyro_calibrated() && p.orientation_ready_for_runtime(), "stationary window was not accepted");
  require(std::abs(p.gyro_bias_rad_s().z - .004f) < 1e-7f, "wrong stationary bias");
  const auto bias = p.gyro_bias_rad_s();
  s.angular_velocity_rad_s.z += 1;
  feed(p, s, 100);
  require(p.gyro_bias_rad_s().z == bias.z, "real motion relearned as bias after startup");
  require(std::abs(p.corrected_sample().angular_velocity_rad_s.z - 1) < 1e-6f, "calibration suppressed real rotation");
  p.reset();
  require(!p.gyro_calibrated() && p.gyro_bias_rad_s().z == 0, "reset retained accepted bias");
}
void captured_failure_slow_yaw_cannot_be_learned() {
  ControllerImuProcessor p;
  auto s = sample({0, 0, -.075f});  // Previously learned as -4.3 degree/s bias.
  feed(p, s, 500);
  require(!p.gyro_calibrated() && p.gyro_bias_rad_s().z == 0, "slow real yaw was learned as bias");
  s.angular_velocity_rad_s = {};
  feed(p, s, 301);
  require(p.gyro_calibrated(), "rejected startup could not recover after becoming stationary");
  feed(p, s, 1000);
  const auto q = p.orientation();
  require(std::abs(2 * std::atan2(q.z, q.w)) < 1e-5f, "stationary controller retained invented rotation");
}
void noncontiguous_and_invalid_measurements_do_not_finish_window() {
  for (int interruption = 0; interruption < 5; ++interruption) {
    ControllerImuProcessor p;
    auto s = sample({.001f, 0, 0});
    feed(p, s, 100);
    auto bad = s;
    bad.host_timestamp_ns += 10'000'000;
    if (interruption == 0) bad.angular_velocity_rad_s.z = .4f;
    if (interruption == 1) bad.specific_force_m_s2.x = .5f;
    if (interruption == 2) bad.gyroscope_valid = false;
    if (interruption == 3) bad.angular_velocity_rad_s.x = std::numeric_limits<float>::quiet_NaN();
    if (interruption == 4) { bad.sample_interval_seconds = .1f; bad.host_timestamp_ns += 90'000'000; }
    p.process(bad);
    s.host_timestamp_ns = bad.host_timestamp_ns;
    feed(p, s, 100);
    require(!p.gyro_calibrated(), "two short windows across motion/gap became one calibration");
    feed(p, s, 60);
    require(p.gyro_calibrated(), "fresh stable window did not recover after interruption");
  }
}
void noisy_stationary_samples_and_different_rates() {
  for (int hz : {100, 200, 400}) {
    ControllerImuProcessor p;
    auto s = sample();
    s.sample_interval_seconds = 1.f / hz;
    for (int i = 0; i <= 2 * hz; ++i) {
      // Large zero-mean horizontal sample noise must not be treated like a
      // constant bias or prevent acceptance of this stationary controller.
      s.angular_velocity_rad_s = {.001f, .002f, (i % 2 ? -.06f : .06f)};
      s.host_timestamp_ns += 1'000'000'000 / hz;
      p.process(s);
    }
    // Default noise bound is configurable; use an explicitly calibrated
    // profile for a sensor whose RMS exceeds that default.
    if (p.gyro_calibrated()) throw std::runtime_error("excess noise unexpectedly accepted");
    GyroBiasCalibrationConfig cfg;
    cfg.max_gyro_stddev_rad_s = .07f;
    ControllerImuProcessor noisy(.04f, cfg);
    s = sample(); s.sample_interval_seconds = 1.f / hz;
    for (int i = 0; i <= 2 * hz; ++i) {
      s.angular_velocity_rad_s = {.001f, .002f, (i % 2 ? -.06f : .06f)};
      s.host_timestamp_ns += 1'000'000'000 / hz;
      noisy.process(s);
    }
    require(noisy.gyro_calibrated(), "zero-mean noise confused with constant startup motion");
    require(std::abs(noisy.gyro_bias_rad_s().z) < .001f, "sample noise learned as gyro bias");
  }
}
void options_are_shared_and_strict() {
  GyroBiasCalibrationConfig cfg;
  require(apply_gyro_bias_option(cfg, "gyro_bias.stable_ms", "2500"), "stable window option unavailable");
  require(cfg.stable_ms == 2500, "option not applied");
  for (const auto* value : {"nan", "inf", "-1", "0", "1x"}) {
    bool failed = false;
    try { apply_gyro_bias_option(cfg, "gyro_bias.max_bias_rad_s", value); }
    catch (const std::exception&) { failed = true; }
    require(failed, "invalid calibration option accepted");
  }
  bool failed = false;
  try { apply_gyro_bias_option(cfg, "gyro_bias.min_samples", "2.5"); }
  catch (const std::exception&) { failed = true; }
  require(failed, "fractional sample count accepted");
  cfg.max_bias_rad_s = 1;
  failed = false;
  try { ControllerImuProcessor p(.04f, cfg); }
  catch (const std::invalid_argument&) { failed = true; }
  require(failed, "inconsistent calibration limits accepted");
}
int main() {
  stationary_bias_is_atomic_and_preserves_real_motion();
  captured_failure_slow_yaw_cannot_be_learned();
  noncontiguous_and_invalid_measurements_do_not_finish_window();
  noisy_stationary_samples_and_different_rates();
  options_are_shared_and_strict();
  std::cout << "Gyro startup: slow-yaw regression, atomic bias, continuous time, motion, gaps, noise, resets and configuration: OK\n";
}
