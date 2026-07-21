#include <xr_override_controller/imu/controller_imu_processor.hpp>
#include <xr_override_controller/backend_control.hpp>

#include <algorithm>
#include <cmath>

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

Madgwick6Dof::Madgwick6Dof(float beta) : beta_(std::max(0.0f, beta)) {}

void Madgwick6Dof::reset() { orientation_ = {}; }

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

  const float accel_norm = std::sqrt(ax * ax + ay * ay + az * az);
  // Hand motion adds linear acceleration to gravity. Applying the full
  // accelerometer correction during those periods makes the controller wobble.
  // Use it only while the measured magnitude remains plausibly gravity-like;
  // gyro integration continues at full rate outside this window.
  if (accel_norm > 1.0e-6f && std::isfinite(accel_norm) &&
      std::abs(accel_norm - current_backend_control_snapshot().gravity_magnitude) <= 3.0f) {
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

ControllerImuProcessor::ControllerImuProcessor(float madgwick_beta)
    : filter_(madgwick_beta) {}

void ControllerImuProcessor::reset() {
  filter_.reset();
  corrected_sample_ = {};
  gyro_bias_rad_s_ = {};
  gyro_bias_sample_count_ = 0;
  previous_timestamp_ns_ = 0;
}

void ControllerImuProcessor::update_initial_gyro_bias(const RawControllerImuSample& sample) {
  if (!sample.gyroscope_valid || !sample.accelerometer_valid || gyro_bias_sample_count_ >= 200) return;
  const float gyro_magnitude = vector_norm(sample.angular_velocity_rad_s);
  const float accel_magnitude = vector_norm(sample.specific_force_m_s2);
  const float gravity_magnitude = current_backend_control_snapshot().gravity_magnitude;
  if (gyro_magnitude >= 0.20f || std::abs(accel_magnitude - gravity_magnitude) >= 1.0f) return;

  ++gyro_bias_sample_count_;
  const float alpha = 1.0f / static_cast<float>(gyro_bias_sample_count_);
  gyro_bias_rad_s_.x += (sample.angular_velocity_rad_s.x - gyro_bias_rad_s_.x) * alpha;
  gyro_bias_rad_s_.y += (sample.angular_velocity_rad_s.y - gyro_bias_rad_s_.y) * alpha;
  gyro_bias_rad_s_.z += (sample.angular_velocity_rad_s.z - gyro_bias_rad_s_.z) * alpha;
}

QuaternionXyzw ControllerImuProcessor::process(const RawControllerImuSample& sample) {
  update_initial_gyro_bias(sample);
  corrected_sample_ = sample;
  corrected_sample_.angular_velocity_rad_s.x -= gyro_bias_rad_s_.x;
  corrected_sample_.angular_velocity_rad_s.y -= gyro_bias_rad_s_.y;
  corrected_sample_.angular_velocity_rad_s.z -= gyro_bias_rad_s_.z;

  float dt = 1.0f / 100.0f;
  if (previous_timestamp_ns_ != 0 && sample.host_timestamp_ns > previous_timestamp_ns_) {
    dt = static_cast<float>(sample.host_timestamp_ns - previous_timestamp_ns_) / 1.0e9f;
  }
  previous_timestamp_ns_ = sample.host_timestamp_ns;

  if (sample.gyroscope_valid && sample.accelerometer_valid) {
    return filter_.update(corrected_sample_.angular_velocity_rad_s,
                          corrected_sample_.specific_force_m_s2,
                          dt);
  }
  return filter_.orientation();
}

}  // namespace xr_override_controller::imu
