#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace xr_runtime_adapter::imu_alignment {

// Confirm a first rotation-derived force frame over continuously matched
// optical/IMU poses. A held grip can finish confirmation without inventing
// fresh yaw evidence from a stationary hand. Real hand motion is allowed:
// stability refers to the force-frame yaw and palm-to-sensor relationship.
class YawStabilityConfirmation {
 public:
  void reset() {
    active_ = false;
    start_ns_ = 0;
    min_delta_ = max_delta_ = 0;
  }

  bool observe(uint64_t timestamp_ns, const float provider_xyzw[4],
               const float optical_xyzw[4], std::optional<float> estimate_rad,
               float stable_ms, float max_range_deg, bool verify_rigid_grip = true) {
    if (timestamp_ns == 0 || timestamp_ns <= last_pose_ns_) return false;
    const uint64_t previous = last_pose_ns_;
    last_pose_ns_ = timestamp_ns;
    if (previous && timestamp_ns - previous > 150'000'000ULL) reset();
    Rotation provider, optical;
    if (!read_rotation(provider_xyzw, provider) || !read_rotation(optical_xyzw, optical) ||
        !std::isfinite(stable_ms) || !std::isfinite(max_range_deg) ||
        (estimate_rad && !std::isfinite(*estimate_rad))) {
      reset();
      return false;
    }
    if (!active_ && !estimate_rad) return false;
    const float yaw = estimate_rad ? *estimate_rad : candidate_rad_;
    constexpr float pi = 3.14159265358979323846f;
    const float limit = std::clamp(max_range_deg, 0.0f, 180.0f) * pi / 180;
    const Rotation inverse_world{std::cos(yaw/2), 0, -std::sin(yaw/2), 0};
    const Rotation inverse_provider{provider.w, -provider.x, -provider.y, -provider.z};
    const Rotation grip = multiply(multiply(inverse_provider, inverse_world), optical);
    const float delta = std::atan2(std::sin(yaw - reference_rad_), std::cos(yaw - reference_rad_));
    const float min_delta = std::min(min_delta_, delta), max_delta = std::max(max_delta_, delta);
    const float dot = std::clamp(std::abs(grip.w*reference_grip_.w + grip.x*reference_grip_.x +
        grip.y*reference_grip_.y + grip.z*reference_grip_.z), 0.0f, 1.0f);
    if (!active_ || max_delta - min_delta > limit ||
        (verify_rigid_grip && dot < std::cos(limit/2))) {
      active_ = true;
      start_ns_ = timestamp_ns;
      reference_rad_ = yaw;
      reference_grip_ = grip;
      min_delta_ = max_delta_ = 0;
    } else {
      min_delta_ = min_delta;
      max_delta_ = max_delta;
    }
    candidate_rad_ = yaw;
    return static_cast<double>(last_pose_ns_ - start_ns_) / 1.0e6 >= std::max(0.0f, stable_ms);
  }

  bool pending() const { return active_; }
  float candidate_rad() const { return candidate_rad_; }
  float elapsed_ms() const {
    return active_ ? static_cast<float>(static_cast<double>(last_pose_ns_ - start_ns_) / 1.0e6) : 0;
  }
  float range_deg() const {
    return active_ ? (max_delta_ - min_delta_) * 180 / 3.14159265358979323846f : 0;
  }

 private:
  struct Rotation { float w = 1, x = 0, y = 0, z = 0; };
  static bool read_rotation(const float xyzw[4], Rotation& q) {
    for (int i = 0; i < 4; ++i) if (!std::isfinite(xyzw[i])) return false;
    const float norm = std::sqrt(xyzw[0]*xyzw[0] + xyzw[1]*xyzw[1] +
                                xyzw[2]*xyzw[2] + xyzw[3]*xyzw[3]);
    if (!std::isfinite(norm) || norm < 1e-6f) return false;
    q = {xyzw[3]/norm, xyzw[0]/norm, xyzw[1]/norm, xyzw[2]/norm};
    return true;
  }
  static Rotation multiply(Rotation a, Rotation b) {
    return {a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w};
  }
  bool active_ = false;
  uint64_t start_ns_ = 0, last_pose_ns_ = 0;
  float reference_rad_ = 0, candidate_rad_ = 0, min_delta_ = 0, max_delta_ = 0;
  Rotation reference_grip_{};
};

}  // namespace xr_runtime_adapter::imu_alignment
