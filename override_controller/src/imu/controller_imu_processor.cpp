#include <xr_override_controller/imu/controller_imu_processor.hpp>
#include <xr_override_controller/backend_control.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xr_override_controller::imu {
namespace {


float vector_norm(const Vec3f& v) {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

QuaternionXyzw normalize_quaternion(QuaternionXyzw q) {
  const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(norm > 1.0e-12f) || !std::isfinite(norm)) return {};
  q.x /= norm;
  q.y /= norm;
  q.z /= norm;
  q.w /= norm;
  return q;
}

}  // namespace

Madgwick6Dof::Madgwick6Dof(float beta) : beta_(std::max(0.0f, beta)) {
  // beta=0 explicitly selects the existing gyro-only reference.
  orientation_initialized_ = beta_ == 0.0f;
}

void Madgwick6Dof::reset() {
  orientation_ = {};
  orientation_initialized_ = beta_ == 0.0f;
}

QuaternionXyzw Madgwick6Dof::update(const Vec3f& gyro,
                                    const Vec3f& accel,
                                    float dt_seconds) {
  const float dt = std::clamp(dt_seconds, 1.0e-4f, 0.05f);
  const float gx = gyro.x;
  const float gy = gyro.y;
  const float gz = gyro.z;
  float ax = accel.x;
  float ay = accel.y;
  float az = accel.z;
  const float accel_norm = vector_norm(accel);
  const float gravity = current_backend_control_snapshot().gravity_magnitude;

  if (!orientation_initialized_ && std::isfinite(accel_norm) && accel_norm > 1.0e-6f &&
      std::abs(accel_norm - gravity) < 1.0f && vector_norm(gyro) < 0.20f) {
    // Initialize the gravity reference at the measurement time. Starting every
    // grip at identity leaves a large tilt error for tens of seconds at normal
    // beta, so rotating force and subtracting gravity invents linear motion.
    // This shortest rotation maps measured body gravity to AHRS world +Z;
    // absolute yaw remains unobservable and is aligned separately by runtime.
    const float nx = ax / accel_norm, ny = ay / accel_norm, nz = az / accel_norm;
    orientation_ = nz < -0.999999f
        ? QuaternionXyzw{1.0f, 0.0f, 0.0f, 0.0f}
        : normalize_quaternion({ny, -nx, 0.0f, 1.0f + nz});
    orientation_initialized_ = true;
    return orientation_;
  }

  // Keep the implementation in the same xyzw convention used by
  // ControllerInputV3 while using the Madgwick paper's q0=w, q1=x, q2=y, q3=z.
  float q1 = orientation_.x;
  float q2 = orientation_.y;
  float q3 = orientation_.z;
  float q0 = orientation_.w;

  float qdot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
  float qdot1 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
  float qdot2 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
  float qdot3 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);

  // Hand motion adds linear acceleration to gravity. Applying the full
  // accelerometer correction during those periods makes the controller wobble.
  // Use it only while the measured magnitude remains plausibly gravity-like;
  // gyro integration continues at full rate outside this window.
  if (orientation_initialized_ && accel_norm > 1.0e-6f && std::isfinite(accel_norm) &&
      std::abs(accel_norm - gravity) <= 3.0f) {
    ax /= accel_norm;
    ay /= accel_norm;
    az /= accel_norm;

    const float _2q0 = 2.0f * q0;
    const float _2q1 = 2.0f * q1;
    const float _2q2 = 2.0f * q2;
    const float _2q3 = 2.0f * q3;
    const float _4q0 = 4.0f * q0;
    const float _4q1 = 4.0f * q1;
    const float _4q2 = 4.0f * q2;
    const float _8q1 = 8.0f * q1;
    const float _8q2 = 8.0f * q2;
    const float q0q0 = q0 * q0;
    const float q1q1 = q1 * q1;
    const float q2q2 = q2 * q2;
    const float q3q3 = q3 * q3;

    float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
    float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay -
               _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
    float s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay -
               _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
    float s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;
    const float step_norm = std::sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
    if (step_norm > 1.0e-9f && std::isfinite(step_norm)) {
      s0 /= step_norm;
      s1 /= step_norm;
      s2 /= step_norm;
      s3 /= step_norm;
      qdot0 -= beta_ * s0;
      qdot1 -= beta_ * s1;
      qdot2 -= beta_ * s2;
      qdot3 -= beta_ * s3;
    }
  }

  q0 += qdot0 * dt;
  q1 += qdot1 * dt;
  q2 += qdot2 * dt;
  q3 += qdot3 * dt;
  orientation_ = normalize_quaternion({q1, q2, q3, q0});
  return orientation_;
}

ControllerImuProcessor::ControllerImuProcessor(float madgwick_beta,
    GyroBiasCalibrationConfig gyro_bias_config)
    : filter_(madgwick_beta), gyro_bias_config_(gyro_bias_config) {
  const auto positive = [](float value) { return std::isfinite(value) && value > 0; };
  if (!positive(gyro_bias_config_.stable_ms) || gyro_bias_config_.stable_ms > 60'000 ||
      gyro_bias_config_.min_samples < 2 || gyro_bias_config_.min_samples > 100'000 ||
      !positive(gyro_bias_config_.max_gap_ms) ||
      !positive(gyro_bias_config_.max_sample_speed_rad_s) ||
      !positive(gyro_bias_config_.max_bias_rad_s) ||
      gyro_bias_config_.max_bias_rad_s > gyro_bias_config_.max_sample_speed_rad_s ||
      !positive(gyro_bias_config_.max_gyro_stddev_rad_s) ||
      !positive(gyro_bias_config_.max_force_error_m_s2) ||
      !positive(gyro_bias_config_.max_force_range_m_s2)) {
    throw std::invalid_argument("invalid controller gyro bias calibration limits");
  }
}

void ControllerImuProcessor::set_accelerometer_calibration(
    const AccelerometerCalibration& calibration) {
  const auto valid_axis = [](float bias, float scale) {
    return std::isfinite(bias) && std::abs(bias) <= 2.0f &&
           std::isfinite(scale) && scale >= 0.5f && scale <= 2.0f;
  };
  if (calibration.enabled &&
      (!valid_axis(calibration.bias_m_s2.x, calibration.scale.x) ||
       !valid_axis(calibration.bias_m_s2.y, calibration.scale.y) ||
       !valid_axis(calibration.bias_m_s2.z, calibration.scale.z))) {
    throw std::invalid_argument("invalid controller accelerometer calibration");
  }
  accelerometer_calibration_ = calibration;
  // The gravity direction was learned from the old force. Reinitialize AHRS
  // and gyro acquisition when changing the calibration, retaining its values
  // through subsequent reconnect/AHRS resets on this physical device.
  reset();
}

void ControllerImuProcessor::reset() {
  filter_.reset();
  corrected_sample_ = {};
  gyro_bias_rad_s_ = {};
  gyro_bias_ready_ = false;
  reset_gyro_bias_candidate("need_stationary_window");
  previous_timestamp_ns_ = 0;
}

void ControllerImuProcessor::reset_gyro_bias_candidate(const char* reason) {
  gyro_bias_sample_count_ = 0;
  gyro_bias_elapsed_ms_ = 0;
  gyro_bias_mean_ = {};
  gyro_bias_m2_ = {};
  gyro_bias_wait_reason_ = reason;
}

void ControllerImuProcessor::update_initial_gyro_bias(const RawControllerImuSample& sample) {
  if (gyro_bias_ready_) return;
  if (!sample.gyroscope_valid || !sample.accelerometer_valid ||
      sample.host_timestamp_ns == 0 ||
      (previous_timestamp_ns_ && sample.host_timestamp_ns <= previous_timestamp_ns_)) {
    reset_gyro_bias_candidate("invalid_sample_or_time");
    return;
  }
  const float gyro_magnitude = vector_norm(sample.angular_velocity_rad_s);
  const float accel_magnitude = vector_norm(sample.specific_force_m_s2);
  const float gravity_magnitude = current_backend_control_snapshot().gravity_magnitude;
  if (!std::isfinite(gyro_magnitude) || !std::isfinite(accel_magnitude) ||
      gyro_magnitude > gyro_bias_config_.max_sample_speed_rad_s ||
      std::abs(accel_magnitude - gravity_magnitude) > gyro_bias_config_.max_force_error_m_s2) {
    reset_gyro_bias_candidate("motion_or_invalid_force");
    return;
  }
  double interval_ms = double(sample.sample_interval_seconds) * 1000.0;
  if (sample.sample_interval_seconds == 0 && previous_timestamp_ns_) {
    interval_ms = double(sample.host_timestamp_ns - previous_timestamp_ns_) * 1e-6;
  }
  if (!std::isfinite(interval_ms) || interval_ms < 0 ||
      (previous_timestamp_ns_ && (interval_ms <= 0 ||
       interval_ms > gyro_bias_config_.max_gap_ms))) {
    reset_gyro_bias_candidate("sample_gap");
    return;
  }
  const float gyro[] = {sample.angular_velocity_rad_s.x,
                        sample.angular_velocity_rad_s.y, sample.angular_velocity_rad_s.z};
  const float force[] = {sample.specific_force_m_s2.x,
                         sample.specific_force_m_s2.y, sample.specific_force_m_s2.z};
  if (gyro_bias_sample_count_ == 0) {
    std::copy_n(force, 3, gyro_bias_force_min_.begin());
    std::copy_n(force, 3, gyro_bias_force_max_.begin());
  } else {
    gyro_bias_elapsed_ms_ += interval_ms;
  }
  ++gyro_bias_sample_count_;
  double force_range2 = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    const double delta = double(gyro[axis]) - gyro_bias_mean_[axis];
    gyro_bias_mean_[axis] += delta / gyro_bias_sample_count_;
    gyro_bias_m2_[axis] += delta * (double(gyro[axis]) - gyro_bias_mean_[axis]);
    gyro_bias_force_min_[axis] = std::min(gyro_bias_force_min_[axis], force[axis]);
    gyro_bias_force_max_[axis] = std::max(gyro_bias_force_max_[axis], force[axis]);
    const double range = gyro_bias_force_max_[axis] - gyro_bias_force_min_[axis];
    force_range2 += range * range;
  }
  if (std::sqrt(force_range2) > gyro_bias_config_.max_force_range_m_s2) {
    reset_gyro_bias_candidate("changing_gravity_or_acceleration");
    return;
  }
  gyro_bias_wait_reason_ = "collecting";
  if (gyro_bias_elapsed_ms_ + 1e-3 < gyro_bias_config_.stable_ms ||
      gyro_bias_sample_count_ < gyro_bias_config_.min_samples) return;
  double mean2 = 0, variance = 0;
  for (size_t axis = 0; axis < 3; ++axis) {
    mean2 += gyro_bias_mean_[axis] * gyro_bias_mean_[axis];
    variance += gyro_bias_m2_[axis] / double(gyro_bias_sample_count_ - 1);
  }
  // Constant slow yaw and gyro bias cannot be distinguished by a 6DoF IMU
  // alone. Bound the accepted mean separately from zero-mean sensor noise.
  if (std::sqrt(mean2) > gyro_bias_config_.max_bias_rad_s ||
      std::sqrt(std::max(0.0, variance)) > gyro_bias_config_.max_gyro_stddev_rad_s) {
    reset_gyro_bias_candidate("possible_rotation_or_excess_noise");
    return;
  }
  gyro_bias_rad_s_ = {float(gyro_bias_mean_[0]), float(gyro_bias_mean_[1]),
                       float(gyro_bias_mean_[2])};
  gyro_bias_ready_ = true;
  gyro_bias_wait_reason_ = "ready";
  // No provisional bias is applied. The old unadvertised orientation may
  // have integrated startup motion; initialize its gravity reference afresh.
  filter_.reset();
}

QuaternionXyzw ControllerImuProcessor::process(const RawControllerImuSample& sample) {
  corrected_sample_ = sample;
  if (sample.accelerometer_valid && accelerometer_calibration_.enabled) {
    const auto& c = accelerometer_calibration_;
    auto& f = corrected_sample_.specific_force_m_s2;
    f.x = (f.x - c.bias_m_s2.x) * c.scale.x;
    f.y = (f.y - c.bias_m_s2.y) * c.scale.y;
    f.z = (f.z - c.bias_m_s2.z) * c.scale.z;
  }
  update_initial_gyro_bias(corrected_sample_);
  corrected_sample_.angular_velocity_rad_s.x -= gyro_bias_rad_s_.x;
  corrected_sample_.angular_velocity_rad_s.y -= gyro_bias_rad_s_.y;
  corrected_sample_.angular_velocity_rad_s.z -= gyro_bias_rad_s_.z;

  float dt = 1.0f / 100.0f;
  if (std::isfinite(sample.sample_interval_seconds) && sample.sample_interval_seconds > 0.0f) {
    dt = sample.sample_interval_seconds;
  } else if (previous_timestamp_ns_ != 0 && sample.host_timestamp_ns > previous_timestamp_ns_) {
    dt = static_cast<float>(sample.host_timestamp_ns - previous_timestamp_ns_) / 1.0e9f;
  }
  previous_timestamp_ns_ = sample.host_timestamp_ns;

  if (sample.gyroscope_valid && sample.accelerometer_valid &&
      std::isfinite(vector_norm(corrected_sample_.angular_velocity_rad_s)) &&
      std::isfinite(vector_norm(corrected_sample_.specific_force_m_s2))) {
    return filter_.update(corrected_sample_.angular_velocity_rad_s,
                          corrected_sample_.specific_force_m_s2,
                          dt);
  }
  return filter_.orientation();
}

}  // namespace xr_override_controller::imu
