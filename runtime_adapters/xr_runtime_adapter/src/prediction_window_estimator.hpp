#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace xr_runtime_adapter::prediction_window {

// Rolling position history used only when an explicit prediction-window mode
// is enabled.  The velocity estimate is the least-squares slope over every
// real pose retained in the configured time window, rather than the delta of
// the final two samples or a backend-provided instantaneous velocity.
template <std::size_t Capacity = 128>
class PositionWindowEstimator {
 public:
  static_assert(Capacity >= 2, "prediction window requires at least two samples");

  void reset() { count_ = 0; }

  void add(uint64_t timestamp_ns,
           double x,
           double y,
           double z,
           double window_ms) {
    if (timestamp_ns == 0 || !finite3(x, y, z)) return;

    if (count_ > 0 && timestamp_ns < samples_[count_ - 1].timestamp_ns) {
      // A timestamp rollback usually means that the source/backend restarted.
      // Do not mix samples from different source epochs.
      reset();
    }

    if (count_ > 0 && timestamp_ns == samples_[count_ - 1].timestamp_ns) {
      samples_[count_ - 1] = {timestamp_ns, x, y, z};
    } else {
      if (count_ == Capacity) {
        std::move(samples_.begin() + 1, samples_.end(), samples_.begin());
        --count_;
      }
      samples_[count_++] = {timestamp_ns, x, y, z};
    }

    prune(timestamp_ns, window_ms);
  }

  bool estimate_velocity(double out_velocity[3],
                         std::size_t* out_sample_count = nullptr,
                         double* out_span_ms = nullptr) const {
    if (out_velocity == nullptr) return false;
    out_velocity[0] = 0.0;
    out_velocity[1] = 0.0;
    out_velocity[2] = 0.0;
    if (out_sample_count != nullptr) *out_sample_count = count_;
    if (out_span_ms != nullptr) *out_span_ms = 0.0;
    if (count_ < 2) return false;

    const uint64_t first_ns = samples_[0].timestamp_ns;
    const uint64_t last_ns = samples_[count_ - 1].timestamp_ns;
    if (last_ns <= first_ns) return false;
    if (out_span_ms != nullptr) {
      *out_span_ms = static_cast<double>(last_ns - first_ns) / 1.0e6;
    }

    double mean_t = 0.0;
    double mean_p[3] = {};
    for (std::size_t i = 0; i < count_; ++i) {
      const double t = static_cast<double>(samples_[i].timestamp_ns - first_ns) / 1.0e9;
      mean_t += t;
      mean_p[0] += samples_[i].x;
      mean_p[1] += samples_[i].y;
      mean_p[2] += samples_[i].z;
    }
    const double inv_count = 1.0 / static_cast<double>(count_);
    mean_t *= inv_count;
    mean_p[0] *= inv_count;
    mean_p[1] *= inv_count;
    mean_p[2] *= inv_count;

    double denominator = 0.0;
    double numerator[3] = {};
    for (std::size_t i = 0; i < count_; ++i) {
      const double t = static_cast<double>(samples_[i].timestamp_ns - first_ns) / 1.0e9;
      const double dt = t - mean_t;
      denominator += dt * dt;
      numerator[0] += dt * (samples_[i].x - mean_p[0]);
      numerator[1] += dt * (samples_[i].y - mean_p[1]);
      numerator[2] += dt * (samples_[i].z - mean_p[2]);
    }
    if (!std::isfinite(denominator) || denominator <= 1.0e-12) return false;

    out_velocity[0] = numerator[0] / denominator;
    out_velocity[1] = numerator[1] / denominator;
    out_velocity[2] = numerator[2] / denominator;
    return finite3(out_velocity[0], out_velocity[1], out_velocity[2]);
  }

  [[nodiscard]] std::size_t sample_count() const { return count_; }

 private:
  struct Sample {
    uint64_t timestamp_ns = 0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  static bool finite3(double x, double y, double z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }

  void prune(uint64_t newest_timestamp_ns, double window_ms) {
    const double clamped_ms = std::max(0.0, window_ms);
    const double window_ns_double = clamped_ms * 1.0e6;
    const uint64_t window_ns = window_ns_double >= static_cast<double>(UINT64_MAX)
                                   ? UINT64_MAX
                                   : static_cast<uint64_t>(window_ns_double);
    const uint64_t cutoff_ns = newest_timestamp_ns > window_ns
                                   ? newest_timestamp_ns - window_ns
                                   : 0;

    std::size_t first_kept = 0;
    while (first_kept < count_ && samples_[first_kept].timestamp_ns < cutoff_ns) {
      ++first_kept;
    }
    if (first_kept == 0) return;
    std::move(samples_.begin() + static_cast<std::ptrdiff_t>(first_kept),
              samples_.begin() + static_cast<std::ptrdiff_t>(count_),
              samples_.begin());
    count_ -= first_kept;
  }

  std::array<Sample, Capacity> samples_{};
  std::size_t count_ = 0;
};

}  // namespace xr_runtime_adapter::prediction_window
