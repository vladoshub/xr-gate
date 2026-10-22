#pragma once

#include <array>
#include <cstdint>

namespace xr_override_controller::imu {

struct Vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct QuaternionXyzw {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

// Fixed, device-specific correction in native sensor axes, before any provider
// basis/mounting transform. Do not normalize each force sample: that would
// remove the linear acceleration needed for position prediction.
struct AccelerometerCalibration {
  bool enabled = false;
  Vec3f bias_m_s2{};
  Vec3f scale{1.0f, 1.0f, 1.0f};
};

// Transport-neutral IMU sample. A Gear VR, MPU-6050 serial/BLE, or another
// motion-controller provider only has to convert native units into SI units
// and feed this structure to ControllerImuProcessor.
struct RawControllerImuSample {
  uint64_t host_timestamp_ns = 0;
  uint64_t device_timestamp_ticks = 0;
  // Optional device-clock interval. AHRS integration must not follow BLE
  // receive jitter or changes to the host/device clock mapping.
  float sample_interval_seconds = 0.0f;
  Vec3f angular_velocity_rad_s;
  Vec3f specific_force_m_s2;
  Vec3f magnetic_field_uT;
  bool gyroscope_valid = false;
  bool accelerometer_valid = false;
  bool magnetometer_valid = false;
};

// Transport-neutral startup limits. Providers expose these through their
// configuration rather than teaching the estimator about a particular device.
struct GyroBiasCalibrationConfig {
  float stable_ms = 1500.0f;
  uint32_t min_samples = 100;
  float max_gap_ms = 50.0f;
  float max_sample_speed_rad_s = 0.20f;
  float max_bias_rad_s = 0.01745329252f;  // 1 degree/s mean, not sample noise.
  float max_gyro_stddev_rad_s = 0.05f;
  float max_force_error_m_s2 = 1.0f;
  float max_force_range_m_s2 = 0.35f;
};

class Madgwick6Dof {
 public:
  explicit Madgwick6Dof(float beta = 0.04f);

  void reset();
  QuaternionXyzw update(const Vec3f& gyro_rad_s,
                        const Vec3f& specific_force_m_s2,
                        float dt_seconds);
  const QuaternionXyzw& orientation() const { return orientation_; }
  bool orientation_initialized() const { return orientation_initialized_; }

 private:
  float beta_ = 0.04f;
  QuaternionXyzw orientation_;
  bool orientation_initialized_ = false;
};

class ControllerImuProcessor {
 public:
  explicit ControllerImuProcessor(float madgwick_beta = 0.04f,
      GyroBiasCalibrationConfig gyro_bias_config = {});

  void reset();
  QuaternionXyzw process(const RawControllerImuSample& sample);
  void set_accelerometer_calibration(const AccelerometerCalibration& calibration);
  const AccelerometerCalibration& accelerometer_calibration() const { return accelerometer_calibration_; }
  bool accelerometer_calibrated() const { return accelerometer_calibration_.enabled; }

  const QuaternionXyzw& orientation() const { return filter_.orientation(); }
  bool orientation_initialized() const { return filter_.orientation_initialized(); }
  const Vec3f& gyro_bias_rad_s() const { return gyro_bias_rad_s_; }
  uint32_t gyro_bias_sample_count() const { return gyro_bias_sample_count_; }
  bool gyro_calibrated() const { return gyro_bias_ready_; }
  bool orientation_ready_for_runtime() const {
    return gyro_bias_ready_ && filter_.orientation_initialized();
  }
  float gyro_bias_stable_ms() const { return static_cast<float>(gyro_bias_elapsed_ms_); }
  const char* gyro_bias_wait_reason() const { return gyro_bias_wait_reason_; }
  const GyroBiasCalibrationConfig& gyro_bias_config() const { return gyro_bias_config_; }
  const RawControllerImuSample& corrected_sample() const { return corrected_sample_; }

 private:
  void update_initial_gyro_bias(const RawControllerImuSample& sample);
  void reset_gyro_bias_candidate(const char* reason);

  Madgwick6Dof filter_;
  AccelerometerCalibration accelerometer_calibration_{};
  RawControllerImuSample corrected_sample_;
  Vec3f gyro_bias_rad_s_;
  uint32_t gyro_bias_sample_count_ = 0;
  GyroBiasCalibrationConfig gyro_bias_config_;
  bool gyro_bias_ready_ = false;
  double gyro_bias_elapsed_ms_ = 0;
  std::array<double, 3> gyro_bias_mean_{};
  std::array<double, 3> gyro_bias_m2_{};
  std::array<float, 3> gyro_bias_force_min_{};
  std::array<float, 3> gyro_bias_force_max_{};
  const char* gyro_bias_wait_reason_ = "need_stationary_window";
  uint64_t previous_timestamp_ns_ = 0;
};

}  // namespace xr_override_controller::imu
