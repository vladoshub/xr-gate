#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include <xr_runtime/contracts/runtime_adapter.hpp>

#include "prediction_window_estimator.hpp"

namespace xr_runtime_adapter::body_tracker_filter {

class BodyTrackerVelocityEstimator {
 public:
  struct Estimate {
    std::optional<std::array<float, 3>> linear;
    std::optional<std::array<float, 3>> angular;
  };

  Estimate observe(uint64_t timestamp_ns, const std::array<float, 3>& position, bool position_valid,
                   xr_runtime::Quatd orientation, bool orientation_valid, double window_ms) {
    if (timestamp_ns == 0) {
      reset();
      return {};
    }
    if (timestamp_ns == timestamp_ns_) return last_;

    const uint64_t interval_ns = timestamp_ns > timestamp_ns_ ? timestamp_ns - timestamp_ns_ : 0;
    const bool continuous = timestamp_ns_ != 0 && interval_ns >= 100000 && interval_ns <= 250000000;
    if (!continuous) reset();
    last_ = {};
    if (position_valid && finite(position)) {
      const double sample_window_ms = continuous ? (interval_ns + 1) / 1.0e6 : 0.0;
      history_.add(timestamp_ns, position[0], position[1], position[2], std::max(window_ms, sample_window_ms));
      double velocity[3]{};
      if (history_.estimate_velocity(velocity)) {
        const std::array<float, 3> linear{float(velocity[0]), float(velocity[1]), float(velocity[2])};
        if (finite(linear)) last_.linear = linear;
      }
    } else {
      history_.reset();
    }

    const double norm2 = orientation.w * orientation.w + orientation.x * orientation.x +
                         orientation.y * orientation.y + orientation.z * orientation.z;
    orientation_valid &= std::isfinite(norm2) && norm2 > 1e-12;
    if (orientation_valid) {
      orientation = xr_runtime::quat_normalize(orientation);
      if (previous_orientation_valid_) {
        const auto& q = previous_orientation_;
        auto delta = xr_runtime::quat_multiply(orientation, xr_runtime::Quatd{q.w, -q.x, -q.y, -q.z});
        if (delta.w < 0) delta = {-delta.w, -delta.x, -delta.y, -delta.z};
        const double sine = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        const double radians_per_second = sine > 1e-9
            ? 2.0 * std::atan2(sine, delta.w) / (sine * interval_ns * 1e-9) : 0.0;
        const std::array<float, 3> angular{float(delta.x * radians_per_second), float(delta.y * radians_per_second),
                                           float(delta.z * radians_per_second)};
        if (finite(angular)) last_.angular = angular;
      }
      previous_orientation_ = orientation;
    }
    previous_orientation_valid_ = orientation_valid;
    timestamp_ns_ = timestamp_ns;
    return last_;
  }

 private:
  void reset() {
    timestamp_ns_ = 0;
    previous_orientation_valid_ = false;
    history_.reset();
    last_ = {};
  }

  static bool finite(const std::array<float, 3>& vector) {
    return std::all_of(vector.begin(), vector.end(), [](float value) { return std::isfinite(value); });
  }

  uint64_t timestamp_ns_ = 0;
  prediction_window::PositionWindowEstimator<32> history_;
  xr_runtime::Quatd previous_orientation_{};
  bool previous_orientation_valid_ = false;
  Estimate last_{};
};

}  // namespace xr_runtime_adapter::body_tracker_filter
