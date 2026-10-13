#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace xr_runtime_adapter::imu_alignment {

// A coherent palm-rotation fit can still choose a force frame opposite to
// observed translation. Check its hemisphere with independent position data.
// Remove unknown position and constant velocity from both the optical track
// and double-integrated provider-world acceleration, then match their XZ
// curvature. This does not estimate a new controller mounting or wrist yaw.
class TranslationYawCheck {
 public:
  void clear_optical_motion() {
    pose_count_ = 0;
    support_count_ = 0;
    coherence_ = 0;
  }
  void clear_motion() {
    clear_optical_motion();
    force_count_ = 0;
    force_cursor_ns_ = 0;
  }

  void observe_force(uint64_t timestamp_ns, const float acceleration[3], float gyro_speed) {
    if (timestamp_ns == 0 || timestamp_ns <= force_cursor_ns_) return;
    const uint64_t previous = force_cursor_ns_;
    force_cursor_ns_ = timestamp_ns;
    if (previous == 0 || timestamp_ns - previous > 80'000'000ULL ||
        !std::isfinite(acceleration[0]) || !std::isfinite(acceleration[2]) ||
        !std::isfinite(gyro_speed)) {
      force_count_ = 0;
      clear_optical_motion();
      return;
    }
    const uint64_t cutoff = timestamp_ns > 750'000'000ULL ? timestamp_ns - 750'000'000ULL : 0;
    std::size_t discard = 0;
    while (discard < force_count_ && forces_[discard].end_ns < cutoff) ++discard;
    if (discard) {
      std::move(forces_.begin() + discard, forces_.begin() + force_count_, forces_.begin());
      force_count_ -= discard;
    }
    if (force_count_ == forces_.size()) {
      std::move(forces_.begin() + 1, forces_.end(), forces_.begin());
      --force_count_;
    }
    forces_[force_count_++] = {previous, timestamp_ns, acceleration[0], acceleration[2], gyro_speed};
  }

  void observe_optical(uint64_t timestamp_ns, const float position[3]) {
    if (timestamp_ns == 0 || timestamp_ns <= last_optical_ns_ ||
        !std::isfinite(position[0]) || !std::isfinite(position[2])) return;
    if (last_optical_ns_ && timestamp_ns - last_optical_ns_ > 150'000'000ULL) clear_optical_motion();
    last_optical_ns_ = timestamp_ns;
    // Keep a bounded queue for IMU delivery that trails camera exposures.
    // The fitted 350 ms window is selected from covered poses in update().
    const uint64_t cutoff = timestamp_ns > 750'000'000ULL ? timestamp_ns - 750'000'000ULL : 0;
    std::size_t discard = 0;
    while (discard < pose_count_ && poses_[discard].timestamp_ns < cutoff) ++discard;
    if (discard) {
      std::move(poses_.begin() + discard, poses_.begin() + pose_count_, poses_.begin());
      pose_count_ -= discard;
    }
    if (pose_count_ == poses_.size()) {
      std::move(poses_.begin() + 1, poses_.end(), poses_.begin());
      --pose_count_;
    }
    poses_[pose_count_++] = {timestamp_ns, position[0], position[2]};
  }

  void update() {
    std::size_t count = pose_count_;
    while (count && poses_[count - 1].timestamp_ns > force_cursor_ns_) --count;
    if (count < 7 || force_count_ == 0) return;
    const uint64_t last = poses_[count - 1].timestamp_ns;
    const uint64_t cutoff = last > 350'000'000ULL ? last - 350'000'000ULL : 0;
    std::size_t first_index = 0;
    while (first_index < count && poses_[first_index].timestamp_ns < cutoff) ++first_index;
    count -= first_index;
    if (count < 7) return;
    const uint64_t first = poses_[first_index].timestamp_ns;
    if (last <= last_fit_ns_ || last - first < 200'000'000ULL || forces_[0].start_ns > first) return;
    last_fit_ns_ = last;
    for (std::size_t i = 0; i < force_count_; ++i) {
      if (forces_[i].end_ns > first && forces_[i].start_ns < last && forces_[i].gyro_speed > 1.0f) return;
    }

    std::array<double, 32> time{};
    std::array<std::array<double, 2>, 32> optical{}, provider{};
    for (std::size_t i = 0; i < count; ++i) {
      const auto& pose = poses_[first_index + i];
      time[i] = static_cast<double>(pose.timestamp_ns - first) / 1.0e9;
      optical[i] = {pose.x, pose.z};
      for (std::size_t j = 0; j < force_count_; ++j) {
        const auto& force = forces_[j];
        const uint64_t from = std::max(first, force.start_ns);
        const uint64_t to = std::min(pose.timestamp_ns, force.end_ns);
        if (to <= from) continue;
        const double a = static_cast<double>(pose.timestamp_ns - from) / 1.0e9;
        const double b = static_cast<double>(pose.timestamp_ns - to) / 1.0e9;
        const double weight = 0.5 * (a * a - b * b);
        provider[i][0] += force.x * weight;
        provider[i][1] += force.z * weight;
      }
    }
    if (!remove_linear_motion(time, optical, count) || !remove_linear_motion(time, provider, count)) return;
    double eo = 0, ep = 0, cosine = 0, sine = 0;
    for (std::size_t i = 0; i < count; ++i) {
      eo += optical[i][0]*optical[i][0] + optical[i][1]*optical[i][1];
      ep += provider[i][0]*provider[i][0] + provider[i][1]*provider[i][1];
      cosine += provider[i][0]*optical[i][0] + provider[i][1]*optical[i][1];
      sine += provider[i][1]*optical[i][0] - provider[i][0]*optical[i][1];
    }
    if (std::min(eo, ep) < count * 0.004 * 0.004) return;
    const double ratio = std::sqrt(eo / ep);
    coherence_ = static_cast<float>(std::clamp(std::hypot(cosine, sine) / std::sqrt(eo * ep), 0.0, 1.0));
    if (coherence_ < 0.95f || ratio < 0.5 || ratio > 2.0) return;
    const float candidate = static_cast<float>(std::atan2(sine, cosine));
    // Distinct optical endpoints, at least 60 ms of agreeing support. Runtime
    // ticks and repeated IMU delivery cannot count the same image twice.
    if (support_count_ == 0 || last - support_start_ns_ > 1'000'000'000ULL ||
        std::cos(candidate - support_reference_rad_) < 0.8660254f) {
      support_count_ = 0;
      support_start_ns_ = last;
      support_reference_rad_ = candidate;
      support_sine_ = support_cosine_ = 0;
    }
    ++support_count_;
    support_sine_ += std::sin(candidate);
    support_cosine_ += std::cos(candidate);
    if (support_count_ >= 3 && last - support_start_ns_ >= 60'000'000ULL) {
      yaw_hint_rad_ = static_cast<float>(std::atan2(support_sine_, support_cosine_));
      hint_valid_ = true;
    }
  }

  float select_hemisphere(float rotation_yaw_rad) const {
    if (!hint_valid_ || std::cos(rotation_yaw_rad - yaw_hint_rad_) >= 0) return rotation_yaw_rad;
    constexpr float pi = 3.14159265358979323846f;
    return std::atan2(std::sin(rotation_yaw_rad + pi), std::cos(rotation_yaw_rad + pi));
  }
  bool valid() const { return hint_valid_; }
  float yaw_hint_rad() const { return yaw_hint_rad_; }
  float coherence() const { return coherence_; }
  std::size_t support_count() const { return support_count_; }

 private:
  static bool remove_linear_motion(const std::array<double, 32>& time,
                                   std::array<std::array<double, 2>, 32>& value,
                                   std::size_t count) {
    double mean_t = 0, mean[2] = {}, denominator = 0, numerator[2] = {};
    for (std::size_t i = 0; i < count; ++i) {
      mean_t += time[i]; mean[0] += value[i][0]; mean[1] += value[i][1];
    }
    mean_t /= count; mean[0] /= count; mean[1] /= count;
    for (std::size_t i = 0; i < count; ++i) {
      const double dt = time[i] - mean_t;
      denominator += dt * dt;
      for (int axis = 0; axis < 2; ++axis) numerator[axis] += dt * (value[i][axis] - mean[axis]);
    }
    if (!std::isfinite(denominator) || denominator <= 1e-12) return false;
    for (std::size_t i = 0; i < count; ++i) {
      for (int axis = 0; axis < 2; ++axis) value[i][axis] -= mean[axis] + numerator[axis] / denominator * (time[i] - mean_t);
    }
    return true;
  }
  struct Force { uint64_t start_ns, end_ns; float x, z, gyro_speed; };
  struct Pose { uint64_t timestamp_ns; float x, z; };
  std::array<Force, 512> forces_{};
  std::array<Pose, 32> poses_{};
  std::size_t force_count_ = 0, pose_count_ = 0, support_count_ = 0;
  uint64_t force_cursor_ns_ = 0, last_optical_ns_ = 0, last_fit_ns_ = 0, support_start_ns_ = 0;
  float support_reference_rad_ = 0, yaw_hint_rad_ = 0, coherence_ = 0;
  double support_sine_ = 0, support_cosine_ = 0;
  bool hint_valid_ = false;
};

}  // namespace xr_runtime_adapter::imu_alignment
