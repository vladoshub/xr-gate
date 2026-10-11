#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <xr_runtime/contracts/runtime_adapter.hpp>

#include "prediction_window_estimator.hpp"

namespace xr_runtime_adapter::hand_filter {

// Observe only poses accepted by the hand gate, in the source camera frame.
// Held/predicted output and rejected detections must never become measurements.
class OpticalHandVelocity {
 public:
  void reset() { *this = {}; }

  xr_runtime::HandSideF32V2 observe(xr_runtime::HandSideF32V2 side,
                                  uint64_t timestamp_ns, bool window_mode,
                                  double window_ms, double max_speed_mps) {
    clear_velocity(side);
    if (timestamp_ns == 0) { reset(); return side; }
    if (last_timestamp_ns_ != 0 && timestamp_ns <= last_timestamp_ns_) return last_;
    const double dt = last_timestamp_ns_ != 0
        ? static_cast<double>(timestamp_ns - last_timestamp_ns_) * 1e-9 : 0;
    const bool usable_interval = dt >= 1e-4 && dt <= 0.25;
    if (!usable_interval) history_.reset();
    history_.add(timestamp_ns, side.controller_px, side.controller_py,
                 side.controller_pz, window_ms);
    if (usable_interval) {
      double v[3] = {};
      const bool valid = window_mode ? history_.estimate_velocity(v) : true;
      if (!window_mode) {
        v[0] = (static_cast<double>(side.controller_px) - last_.controller_px) / dt;
        v[1] = (static_cast<double>(side.controller_py) - last_.controller_py) / dt;
        v[2] = (static_cast<double>(side.controller_pz) - last_.controller_pz) / dt;
      }
      if (valid && finite(v)) {
        limit(v, max_speed_mps);
        side.vx = static_cast<float>(v[0]);
        side.vy = static_cast<float>(v[1]);
        side.vz = static_cast<float>(v[2]);
        side.flags |= xr_runtime::HAND_LINEAR_VELOCITY_VALID;
      }
      xr_runtime::Quatd previous, current;
      if (orientation(last_, previous) && orientation(side, current)) {
        const auto inverse = xr_runtime::Quatd{previous.w, -previous.x, -previous.y, -previous.z};
        auto delta = xr_runtime::quat_multiply(current, inverse);
        if (delta.w < 0) delta = {-delta.w, -delta.x, -delta.y, -delta.z};
        const double sine = std::sqrt(delta.x*delta.x + delta.y*delta.y + delta.z*delta.z);
        double w[3] = {};
        if (sine > 1e-9) {
          const double factor = 2 * std::atan2(sine, std::clamp(delta.w, -1.0, 1.0)) / (sine * dt);
          w[0] = delta.x * factor; w[1] = delta.y * factor; w[2] = delta.z * factor;
        }
        if (finite(w)) {
          limit(w, 30.0);
          side.wx = static_cast<float>(w[0]);
          side.wy = static_cast<float>(w[1]);
          side.wz = static_cast<float>(w[2]);
          side.flags |= xr_runtime::HAND_ANGULAR_VELOCITY_VALID;
        }
      }
    }
    last_ = side;
    last_timestamp_ns_ = timestamp_ns;
    return side;
  }

 private:
  static void clear_velocity(xr_runtime::HandSideF32V2& side) {
    side.flags &= ~(xr_runtime::HAND_LINEAR_VELOCITY_VALID | xr_runtime::HAND_ANGULAR_VELOCITY_VALID);
    side.vx = side.vy = side.vz = side.wx = side.wy = side.wz = 0;
  }
  static bool finite(const double v[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
  }
  static void limit(double v[3], double maximum) {
    const double magnitude = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (maximum > 0 && magnitude > maximum) {
      for (int i=0; i<3; ++i) v[i] *= maximum / magnitude;
    }
  }
  static bool orientation(const xr_runtime::HandSideF32V2& side, xr_runtime::Quatd& q) {
    q = {side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz};
    const double n2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (!std::isfinite(n2) || n2 < 1e-12) return false;
    q = xr_runtime::quat_normalize(q);
    return true;
  }

  uint64_t last_timestamp_ns_ = 0;
  xr_runtime::HandSideF32V2 last_{};
  prediction_window::PositionWindowEstimator<> history_{};
};

} // namespace xr_runtime_adapter::hand_filter
