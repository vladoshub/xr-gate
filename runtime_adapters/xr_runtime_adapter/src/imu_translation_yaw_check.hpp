#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace xr_runtime_adapter::imu_alignment {

// Compare optical XZ motion with double-integrated provider-world force.
// Both the hemisphere and full-heading fits remove a constant acceleration
// nuisance term, so attitude/gravity error during a movement cannot masquerade
// as a different physical yaw, including a false 180-degree correction.
// It requires observable changing acceleration; a static pose, uniform
// translation or constant acceleration cannot establish the refined heading.
class TranslationYawCheck {
 public:
  enum class FitStatus {
    WaitingForOptical, WaitingForImu, Collecting, ImuGap, Rotating,
    NeedMotion, ScaleMismatch, InconsistentMotion, Confirming, Ready
  };

  void clear_optical_motion() {
    ++motion_epoch_;
    pose_count_ = 0;
    hemisphere_.support_count = heading_.support_count = 0;
    hemisphere_.coherence = heading_.coherence = 0;
    heading_.status = FitStatus::WaitingForOptical;
    heading_.motion_m = heading_.scale_ratio = 0;
    // Keep confirmed hints and their source times. Consumers must check
    // freshness before learning from them; optical loss cannot revoke a
    // physical frame already confirmed for position prediction.
  }
  void clear_motion() {
    clear_optical_motion();
    force_count_ = 0;
    force_cursor_ns_ = 0;
    last_force_sample_valid_ = false;
  }

  void observe_force(uint64_t timestamp_ns, const float acceleration[3], float gyro_speed) {
    if (timestamp_ns == 0 || timestamp_ns <= force_cursor_ns_) return;
    const uint64_t previous = force_cursor_ns_;
    force_cursor_ns_ = timestamp_ns;
    last_force_sample_valid_ = std::isfinite(acceleration[0]) &&
        std::isfinite(acceleration[2]) && std::isfinite(gyro_speed);
    if (previous == 0 || timestamp_ns - previous > 80'000'000ULL ||
        !last_force_sample_valid_) {
      if (previous != 0) {
        ++force_gap_count_;
        last_force_gap_ns_ = timestamp_ns;
        last_force_gap_duration_ns_ = timestamp_ns - previous;
      }
      force_count_ = 0;
      clear_optical_motion();
      return;
    }
    const uint64_t cutoff = timestamp_ns > kHistoryNs ? timestamp_ns - kHistoryNs : 0;
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
    const uint64_t cutoff = timestamp_ns > kHistoryNs ? timestamp_ns - kHistoryNs : 0;
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

  void update(bool refine_heading = false) {
    update_fit(hemisphere_, false);
    if (refine_heading) update_fit(heading_, true);
  }

  float select_hemisphere(float rotation_yaw_rad, uint64_t optical_timestamp_ns) const {
    // Retaining a checked frame across loss is different from applying an
    // old motion hint to a new rotation fit. Only current evidence may flip it.
    if (!hemisphere_.valid || optical_timestamp_ns < hemisphere_.hint_ns ||
        optical_timestamp_ns - hemisphere_.hint_ns > 150'000'000ULL ||
        std::cos(rotation_yaw_rad - hemisphere_.hint_rad) >= 0) return rotation_yaw_rad;
    constexpr float pi = 3.14159265358979323846f;
    return std::atan2(std::sin(rotation_yaw_rad + pi), std::cos(rotation_yaw_rad + pi));
  }
  bool valid() const { return hemisphere_.valid; }
  float yaw_hint_rad() const { return hemisphere_.hint_rad; }
  float coherence() const { return hemisphere_.coherence; }
  std::size_t support_count() const { return hemisphere_.support_count; }

  bool heading_valid() const { return heading_.valid; }
  float heading_rad() const { return heading_.hint_rad; }
  uint64_t heading_timestamp_ns() const { return heading_.hint_ns; }
  float heading_coherence() const { return heading_.hint_coherence; }
  float heading_fit_coherence() const { return heading_.coherence; }
  std::size_t heading_support_count() const { return heading_.support_count; }
  FitStatus heading_fit_status() const { return heading_.status; }
  float heading_fit_motion_m() const { return heading_.motion_m; }
  float heading_fit_scale_ratio() const { return heading_.scale_ratio; }
  uint64_t motion_epoch() const { return motion_epoch_; }
  uint32_t force_gap_count() const { return force_gap_count_; }
  uint64_t last_force_gap_duration_ns() const { return last_force_gap_duration_ns_; }
  bool has_recent_force(uint64_t optical_ns) const {
    return last_force_sample_valid_ && force_cursor_ns_ != 0 && (force_cursor_ns_ >= optical_ns ||
        optical_ns - force_cursor_ns_ <= 150'000'000ULL);
  }
  const char* heading_fit_status_name() const {
    switch (heading_.status) {
      case FitStatus::WaitingForOptical: return "waiting_optics";
      case FitStatus::WaitingForImu: return "waiting_imu";
      case FitStatus::Collecting: return "collecting";
      case FitStatus::ImuGap: return "imu_gap";
      case FitStatus::Rotating: return "rotate_slower";
      case FitStatus::NeedMotion: return "need_motion";
      case FitStatus::ScaleMismatch: return "scale_mismatch";
      case FitStatus::InconsistentMotion: return "inconsistent_motion";
      case FitStatus::Confirming: return "confirming";
      case FitStatus::Ready: return "ready";
    }
    return "collecting";
  }
  bool recent_heading(uint64_t optical_ns) const {
    return heading_.valid &&
        optical_ns >= heading_.hint_ns && optical_ns - heading_.hint_ns <= 150'000'000ULL;
  }
  bool fresh_heading(uint64_t optical_ns, uint64_t consumed_ns) const {
    return recent_heading(optical_ns) && heading_.hint_ns > consumed_ns;
  }

 private:
  static constexpr uint64_t kHistoryNs = 2'000'000'000ULL;
  static constexpr std::size_t kPoseCapacity = 128;
  struct Evidence {
    uint64_t last_fit_ns = 0, support_start_ns = 0, hint_ns = 0;
    std::size_t support_count = 0;
    float reference_rad = 0, hint_rad = 0, coherence = 0, hint_coherence = 0;
    float min_delta = 0, max_delta = 0, min_coherence = 1;
    double sine = 0, cosine = 0;
    bool valid = false;
    FitStatus status = FitStatus::WaitingForOptical;
    float motion_m = 0, scale_ratio = 0;
  };

  void update_fit(Evidence& evidence, bool refine_heading) {
    std::size_t count = pose_count_;
    while (count && poses_[count - 1].timestamp_ns > force_cursor_ns_) --count;
    const std::size_t minimum_count = refine_heading ? 16 : 12;
    if (count < minimum_count || force_count_ == 0) {
      evidence.status = pose_count_ == 0 ? FitStatus::WaitingForOptical
          : (count < pose_count_ || force_count_ == 0) ? FitStatus::WaitingForImu
          : last_force_gap_ns_ != 0 && (last_optical_ns_ <= last_force_gap_ns_ ||
              last_optical_ns_ - last_force_gap_ns_ < 1'250'000'000ULL)
              ? FitStatus::ImuGap : FitStatus::Collecting;
      return;
    }
    const uint64_t last = poses_[count - 1].timestamp_ns;
    const uint64_t window_ns = refine_heading ? 1'250'000'000ULL : 750'000'000ULL;
    const uint64_t cutoff = last > window_ns ? last - window_ns : 0;
    std::size_t first_index = 0;
    while (first_index < count && poses_[first_index].timestamp_ns < cutoff) ++first_index;
    count -= first_index;
    if (count < minimum_count) { evidence.status = FitStatus::Collecting; return; }
    const uint64_t first = poses_[first_index].timestamp_ns;
    const uint64_t minimum_span = refine_heading ? 700'000'000ULL : 350'000'000ULL;
    if (last - first < minimum_span) { evidence.status = FitStatus::Collecting; return; }
    if (forces_[0].start_ns > first) { evidence.status = FitStatus::ImuGap; return; }
    if (last <= evidence.last_fit_ns) return;
    evidence.last_fit_ns = last;
    evidence.coherence = evidence.motion_m = evidence.scale_ratio = 0;
    for (std::size_t i = 0; i < force_count_; ++i) {
      if (forces_[i].end_ns > first && forces_[i].start_ns < last && forces_[i].gyro_speed > 1.0f) {
        evidence.status = FitStatus::Rotating;
        return;
      }
    }

    std::array<double, kPoseCapacity> time{};
    std::array<std::array<double, 2>, kPoseCapacity> optical{}, provider{};
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
    if (!remove_nuisance_motion(time, optical, count) ||
        !remove_nuisance_motion(time, provider, count)) {
      evidence.status = FitStatus::NeedMotion;
      return;
    }
    double eo = 0, ep = 0, cosine = 0, sine = 0;
    for (std::size_t i = 0; i < count; ++i) {
      eo += optical[i][0]*optical[i][0] + optical[i][1]*optical[i][1];
      ep += provider[i][0]*provider[i][0] + provider[i][1]*provider[i][1];
      cosine += provider[i][0]*optical[i][0] + provider[i][1]*optical[i][1];
      sine += provider[i][1]*optical[i][0] - provider[i][0]*optical[i][1];
    }
    const double minimum_motion = refine_heading ? 0.003 : 0.004;
    evidence.motion_m = static_cast<float>(std::sqrt(std::min(eo, ep) / count));
    if (std::min(eo, ep) < count * minimum_motion * minimum_motion) {
      evidence.status = FitStatus::NeedMotion;
      return;
    }
    const double ratio = std::sqrt(eo / ep);
    evidence.scale_ratio = static_cast<float>(ratio);
    evidence.coherence = static_cast<float>(std::clamp(std::hypot(cosine, sine) / std::sqrt(eo * ep), 0.0, 1.0));
    if (ratio < 0.5 || ratio > 2.0) { evidence.status = FitStatus::ScaleMismatch; return; }
    if (evidence.coherence < (refine_heading ? 0.97f : 0.95f)) {
      evidence.status = FitStatus::InconsistentMotion;
      return;
    }
    const float candidate = static_cast<float>(std::atan2(sine, cosine));
    const float delta = std::atan2(std::sin(candidate - evidence.reference_rad),
                                   std::cos(candidate - evidence.reference_rad));
    const float min_delta = std::min(evidence.min_delta, delta);
    const float max_delta = std::max(evidence.max_delta, delta);
    constexpr float pi = 3.14159265358979323846f;
    const bool inconsistent = refine_heading ? max_delta - min_delta > 8*pi/180
                                            : std::cos(delta) < 0.8660254f;
    if (evidence.support_count == 0 || last - evidence.support_start_ns > 1'000'000'000ULL || inconsistent) {
      evidence.support_count = 0;
      evidence.support_start_ns = last;
      evidence.reference_rad = candidate;
      evidence.min_delta = evidence.max_delta = 0;
      evidence.sine = evidence.cosine = 0;
      evidence.min_coherence = 1;
    } else {
      evidence.min_delta = min_delta;
      evidence.max_delta = max_delta;
    }
    ++evidence.support_count;
    evidence.sine += std::sin(candidate);
    evidence.cosine += std::cos(candidate);
    evidence.min_coherence = std::min(evidence.min_coherence, evidence.coherence);
    const std::size_t minimum_support = refine_heading ? 5 : 3;
    const uint64_t support_span = refine_heading ? 150'000'000ULL : 60'000'000ULL;
    evidence.status = FitStatus::Confirming;
    if (evidence.support_count >= minimum_support && last - evidence.support_start_ns >= support_span) {
      evidence.hint_rad = static_cast<float>(std::atan2(evidence.sine, evidence.cosine));
      evidence.hint_ns = last;
      evidence.hint_coherence = evidence.min_coherence;
      evidence.valid = true;
      evidence.status = FitStatus::Ready;
    }
  }

  static bool remove_nuisance_motion(const std::array<double, kPoseCapacity>& time,
                                    std::array<std::array<double, 2>, kPoseCapacity>& value,
                                    std::size_t count) {
    double mean_t = 0, mean[2] = {}, denominator = 0, numerator[2] = {};
    for (std::size_t i = 0; i < count; ++i) {
      mean_t += time[i]; mean[0] += value[i][0]; mean[1] += value[i][1];
    }
    mean_t /= count; mean[0] /= count; mean[1] /= count;
    double cubic = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const double dt = time[i] - mean_t;
      denominator += dt * dt;
      cubic += dt * dt * dt;
      for (int axis = 0; axis < 2; ++axis) numerator[axis] += dt * (value[i][axis] - mean[axis]);
    }
    if (!std::isfinite(denominator) || denominator <= 1e-12) return false;
    for (std::size_t i = 0; i < count; ++i) {
      for (int axis = 0; axis < 2; ++axis) value[i][axis] -= mean[axis] + numerator[axis] / denominator * (time[i] - mean_t);
    }
    // Remove this nuisance term only from the heading fit, never from the
    // acceleration samples used for physical position prediction.
    std::array<double, kPoseCapacity> quadratic{};
    double square_sum = 0, projection[2] = {};
    for (std::size_t i = 0; i < count; ++i) {
      const double dt = time[i] - mean_t;
      quadratic[i] = dt*dt - denominator/count - cubic/denominator*dt;
      square_sum += quadratic[i]*quadratic[i];
      for (int axis = 0; axis < 2; ++axis) projection[axis] += quadratic[i]*value[i][axis];
    }
    if (!std::isfinite(square_sum) || square_sum <= 1e-16) return false;
    for (std::size_t i = 0; i < count; ++i) {
      for (int axis = 0; axis < 2; ++axis) value[i][axis] -= projection[axis]/square_sum*quadratic[i];
    }
    return true;
  }

  struct Force { uint64_t start_ns, end_ns; float x, z, gyro_speed; };
  struct Pose { uint64_t timestamp_ns; float x, z; };
  std::array<Force, 512> forces_{};
  std::array<Pose, kPoseCapacity> poses_{};
  std::size_t force_count_ = 0, pose_count_ = 0;
  uint64_t force_cursor_ns_ = 0, last_optical_ns_ = 0;
  uint64_t motion_epoch_ = 0;
  uint64_t last_force_gap_ns_ = 0;
  uint64_t last_force_gap_duration_ns_ = 0;
  uint32_t force_gap_count_ = 0;
  bool last_force_sample_valid_ = false;
  Evidence hemisphere_{}, heading_{};
};

}  // namespace xr_runtime_adapter::imu_alignment
