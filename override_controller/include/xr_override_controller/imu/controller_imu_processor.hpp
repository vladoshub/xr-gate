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
  explicit ControllerImuProcessor(float madgwick_beta = 0.04f);

  void reset();
  QuaternionXyzw process(const RawControllerImuSample& sample);
  void set_accelerometer_calibration(const AccelerometerCalibration& calibration);
  const AccelerometerCalibration& accelerometer_calibration() const { return accelerometer_calibration_; }
  bool accelerometer_calibrated() const { return accelerometer_calibration_.enabled; }

  const QuaternionXyzw& orientation() const { return filter_.orientation(); }
  bool orientation_initialized() const { return filter_.orientation_initialized(); }
  const Vec3f& gyro_bias_rad_s() const { return gyro_bias_rad_s_; }
  uint32_t gyro_bias_sample_count() const { return gyro_bias_sample_count_; }
  bool gyro_calibrated() const { return gyro_bias_sample_count_ >= 50; }
  const RawControllerImuSample& corrected_sample() const { return corrected_sample_; }

 private:
  void update_initial_gyro_bias(const RawControllerImuSample& sample);

  Madgwick6Dof filter_;
  AccelerometerCalibration accelerometer_calibration_{};
  RawControllerImuSample corrected_sample_;
  Vec3f gyro_bias_rad_s_;
  uint32_t gyro_bias_sample_count_ = 0;
  uint64_t previous_timestamp_ns_ = 0;
};

}  // namespace xr_override_controller::imu
