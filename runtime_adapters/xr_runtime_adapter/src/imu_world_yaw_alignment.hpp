#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace xr_runtime_adapter::imu_alignment {

// The optical palm and the IMU have different local axes. For a rigid grip:
//   optical(t) = world_yaw * imu(t) * mounting.
// World-relative rotations cancel the unknown mounting:
//   optical(b) * inverse(optical(a))
//       = world_yaw * (imu(b) * inverse(imu(a))) * inverse(world_yaw).
// Match their horizontal rotation axes to identify the provider->XR yaw.
// One static pose, or rotations only about world Y, cannot identify this yaw.
class WorldYawAlignment {
 public:
  void observe_imu(uint64_t timestamp_ns, const float xyzw[4]) {
    Rotation q;
    if (timestamp_ns == 0 || !read_rotation(xyzw, q) ||
        (imu_count_ != 0 && timestamp_ns <= imu_[imu_count_ - 1].timestamp_ns)) return;
    if (imu_count_ == imu_.size()) {
      std::move(imu_.begin() + 1, imu_.end(), imu_.begin());
      --imu_count_;
    }
    imu_[imu_count_++] = {timestamp_ns, q};
  }

  // Lose the optical motion window, not the retained provider->XR alignment.
  // Keep its timestamp guard so an old cached pose cannot start a new window.
  void clear_optical_motion() {
    observation_count_ = 0;
    pending_count_ = 0;
    pair_count_ = 0;
    fresh_pair_count_ = 0;
    coherence_ = 0;
  }

  bool observe_optical(uint64_t timestamp_ns, const float xyzw[4], float& yaw_rad) {
    Rotation optical;
    if (timestamp_ns == 0 || timestamp_ns <= last_optical_timestamp_ns_ ||
        !read_rotation(xyzw, optical)) return false;
    if (last_optical_timestamp_ns_ != 0 &&
        timestamp_ns - last_optical_timestamp_ns_ > 150'000'000ULL) clear_optical_motion();
    last_optical_timestamp_ns_ = timestamp_ns;
    // Bluetooth/serial delivery may lag the camera. Keep accepted optical
    // poses until their corresponding IMU time arrives, instead of comparing
    // them with an earlier/latest quaternion or extrapolating its orientation.
    if (pending_count_ == pending_.size()) {
      std::move(pending_.begin() + 1, pending_.end(), pending_.begin());
      --pending_count_;
    }
    pending_[pending_count_++] = {timestamp_ns, optical};
    bool added = false;
    std::size_t consumed = 0;
    while (consumed < pending_count_ && imu_count_ != 0 &&
           pending_[consumed].timestamp_ns <= imu_[imu_count_ - 1].timestamp_ns) {
      Rotation provider;
      const auto& candidate = pending_[consumed++];
      if (!imu_at(candidate.timestamp_ns, provider)) continue;
      append_observation(candidate.timestamp_ns, provider, candidate.orientation);
      added = true;
    }
    if (consumed != 0) {
      std::move(pending_.begin() + consumed, pending_.begin() + pending_count_, pending_.begin());
      pending_count_ -= consumed;
    }
    return added && estimate(yaw_rad);
  }

  std::size_t observation_count() const { return observation_count_; }
  std::size_t pair_count() const { return pair_count_; }
  std::size_t fresh_pair_count() const { return fresh_pair_count_; }
  float coherence() const { return coherence_; }

 private:
  struct Rotation { float w = 1, x = 0, y = 0, z = 0; };
  struct ImuObservation { uint64_t timestamp_ns = 0; Rotation orientation; };
  struct OpticalObservation {
    uint64_t timestamp_ns = 0;
    Rotation provider, optical;
  };

  void append_observation(uint64_t timestamp_ns, Rotation provider, Rotation optical) {
    const uint64_t cutoff = timestamp_ns > 750'000'000ULL ? timestamp_ns - 750'000'000ULL : 0;
    std::size_t discard = 0;
    while (discard < observation_count_ && observations_[discard].timestamp_ns < cutoff) ++discard;
    if (discard != 0) {
      std::move(observations_.begin() + discard,
                observations_.begin() + observation_count_, observations_.begin());
      observation_count_ -= discard;
    }
    if (observation_count_ == observations_.size()) {
      std::move(observations_.begin() + 1, observations_.end(), observations_.begin());
      --observation_count_;
    }
    observations_[observation_count_++] = {timestamp_ns, provider, optical};
  }

  static Rotation normalize(Rotation q) {
    const float norm = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    q.w /= norm; q.x /= norm; q.y /= norm; q.z /= norm;
    return q;
  }
  static bool read_rotation(const float xyzw[4], Rotation& q) {
    for (int axis = 0; axis < 4; ++axis) if (!std::isfinite(xyzw[axis])) return false;
    q = {xyzw[3], xyzw[0], xyzw[1], xyzw[2]};
    const float norm_sq = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (!std::isfinite(norm_sq) || norm_sq < 1e-12f) return false;
    q = normalize(q);
    return true;
  }
  static Rotation delta(Rotation latest, Rotation previous) {
    previous.x = -previous.x; previous.y = -previous.y; previous.z = -previous.z;
    Rotation q = normalize({
        latest.w*previous.w - latest.x*previous.x - latest.y*previous.y - latest.z*previous.z,
        latest.w*previous.x + latest.x*previous.w + latest.y*previous.z - latest.z*previous.y,
        latest.w*previous.y - latest.x*previous.z + latest.y*previous.w + latest.z*previous.x,
        latest.w*previous.z + latest.x*previous.y - latest.y*previous.x + latest.z*previous.w});
    if (q.w < 0) { q.w = -q.w; q.x = -q.x; q.y = -q.y; q.z = -q.z; }
    return q;
  }
  static float norm_vector(const Rotation& q) {
    return std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z);
  }

  bool imu_at(uint64_t timestamp_ns, Rotation& q) const {
    if (imu_count_ == 0) return false;
    const auto end = imu_.begin() + imu_count_;
    const auto next = std::lower_bound(imu_.begin(), end, timestamp_ns,
        [](const ImuObservation& v, uint64_t time) { return v.timestamp_ns < time; });
    if (next != end && next->timestamp_ns == timestamp_ns) { q = next->orientation; return true; }
    if (next == imu_.begin() || next == end) return false;
    const auto& before = *(next - 1);
    if (next->timestamp_ns - before.timestamp_ns > 80'000'000ULL) return false;
    const float t = static_cast<float>(timestamp_ns - before.timestamp_ns) /
                    static_cast<float>(next->timestamp_ns - before.timestamp_ns);
    Rotation after = next->orientation;
    const auto& a = before.orientation;
    const float sign = a.w*after.w + a.x*after.x + a.y*after.y + a.z*after.z < 0 ? -1.0f : 1.0f;
    // Normalized interpolation is sufficient over these short IMU intervals.
    q = normalize({a.w*(1-t) + sign*after.w*t, a.x*(1-t) + sign*after.x*t,
                   a.y*(1-t) + sign*after.y*t, a.z*(1-t) + sign*after.z*t});
    return true;
  }

  bool estimate(float& yaw_rad) {
    pair_count_ = 0;
    fresh_pair_count_ = 0;
    coherence_ = 0;
    if (observation_count_ < 6 ||
        observations_[observation_count_ - 1].timestamp_ns - observations_[0].timestamp_ns < 150'000'000ULL) return false;
    double sum_cos = 0, sum_sin = 0, sum_weight = 0;
    uint64_t newest_support_timestamp_ns = last_fit_support_timestamp_ns_;
    std::array<bool, 32> used{};
    for (std::size_t a = 0; a + 1 < observation_count_; ++a) {
      for (std::size_t b = a + 1; b < observation_count_; ++b) {
        const uint64_t span = observations_[b].timestamp_ns - observations_[a].timestamp_ns;
        if (span < 80'000'000ULL || span > 500'000'000ULL) continue;
        const auto provider = delta(observations_[b].provider, observations_[a].provider);
        const auto optical = delta(observations_[b].optical, observations_[a].optical);
        const float provider_horizontal = std::hypot(provider.x, provider.z);
        const float optical_horizontal = std::hypot(optical.x, optical.z);
        // At least eight degrees of pitch/roll excitation. Reject pairs whose
        // angle or world-Y component disagrees: they do not describe rigid
        // corresponding rotations, or their timestamps are not comparable.
        if (provider_horizontal < 0.07f || optical_horizontal < 0.07f) continue;
        const float provider_norm = norm_vector(provider), optical_norm = norm_vector(optical);
        const float tolerance = 0.04f + 0.2f * std::max(provider_norm, optical_norm);
        if (std::abs(provider_norm - optical_norm) > tolerance ||
            std::abs(provider.y - optical.y) > tolerance) continue;
        const double weight = std::min(provider_horizontal * provider_horizontal,
                                       optical_horizontal * optical_horizontal);
        const double scale = weight / (provider_horizontal * optical_horizontal);
        sum_cos += scale * (provider.x * optical.x + provider.z * optical.z);
        sum_sin += scale * (provider.z * optical.x - provider.x * optical.z);
        sum_weight += weight;
        used[a] = used[b] = true;
        ++pair_count_;
        if (observations_[b].timestamp_ns > last_fit_support_timestamp_ns_) {
          ++fresh_pair_count_;
          newest_support_timestamp_ns = std::max(newest_support_timestamp_ns,
                                                 observations_[b].timestamp_ns);
        }
      }
    }
    if (pair_count_ < 5 || std::count(used.begin(), used.end(), true) < 5 || sum_weight <= 0) return false;
    coherence_ = static_cast<float>(std::min(1.0, std::hypot(sum_cos, sum_sin) / sum_weight));
    // A noisy/inconsistent palm track must not rotate a previously calibrated
    // force frame. Require several agreeing observations in the motion window.
    // Adding a rejected/non-corresponding optical pose is not fresh evidence.
    // The old coherent pairs remain in the window for diagnostics, but must
    // not drive another smoothing step every time an unrelated pose arrives.
    if (coherence_ < 0.985f || fresh_pair_count_ == 0) return false;
    yaw_rad = static_cast<float>(std::atan2(sum_sin, sum_cos));
    if (!std::isfinite(yaw_rad)) return false;
    last_fit_support_timestamp_ns_ = newest_support_timestamp_ns;
    return true;
  }

  std::array<ImuObservation, 256> imu_{};
  std::array<ImuObservation, 16> pending_{};
  std::array<OpticalObservation, 32> observations_{};
  std::size_t imu_count_ = 0, pending_count_ = 0, observation_count_ = 0, pair_count_ = 0;
  std::size_t fresh_pair_count_ = 0;
  uint64_t last_optical_timestamp_ns_ = 0;
  uint64_t last_fit_support_timestamp_ns_ = 0;
  float coherence_ = 0;
};

}  // namespace xr_runtime_adapter::imu_alignment
