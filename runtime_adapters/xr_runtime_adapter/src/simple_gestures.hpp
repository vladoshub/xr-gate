#pragma once

#include "emulation_controller.hpp"

#include <algorithm>
#include <cmath>

namespace xr_runtime_adapter::gestures {

inline emulation::Config simple_palm_config() {
  emulation::Config c;
  // Same uncalibrated shape thresholds as the shipped open-palm click preset.
  c.extension_on = .65;
  c.little_extension_on = .55;
  c.thumb_enable_clearance = .78;
  c.thumb_extension_on = .76;
  return c;
}

// SIMPLE_GESTURES alone replaces visual grab/fist with an open palm facing the
// HMD. Pinch, extra bindings and physical-controller input keep their old path.
class SimplePalmGrip {
 public:
  void apply(xr_runtime::HandTrackingFrameF32V2& hands,
             const emulation::Vec3* viewer, uint64_t now, uint64_t origin,
             bool left_enabled, bool right_enabled,
             bool left_physical = false, bool right_physical = false) {
    if (origin != origin_ || hands.reset_counter != reset_) {
      left_ = right_ = {};
      origin_ = origin;
      reset_ = hands.reset_counter;
    }
    const uint64_t stamp = hands.source_timestamp_ns ? hands.source_timestamp_ns : hands.timestamp_ns;
    const bool fresh = hands.sequence && stamp &&
        (stamp <= now ? double(now - stamp) / 1e6 <= config_.max_age_ms : stamp - now <= 2000000);
    update(hands.left, left_, viewer, stamp, fresh && left_enabled, left_physical);
    update(hands.right, right_, viewer, stamp, fresh && right_enabled, right_physical);
    if ((hands.left.flags | hands.right.flags) & xr_runtime::HAND_GRAB_VALID)
      hands.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
  }

 private:
  struct Side {
    std::array<bool, 4> extended{};
    bool facing = false, active = false;
    uint64_t since = 0, sample = 0;
  };

  void update(xr_runtime::HandSideF32V2& hand, Side& s,
              const emulation::Vec3* viewer, uint64_t stamp,
              bool enabled, bool physical) {
    const auto write = [&] {
      hand.grab_strength = s.active ? 1.f : 0.f;
      hand.grab_active = s.active ? 1u : 0u;
      hand.flags |= xr_runtime::HAND_GRAB_VALID;
    };
    if (physical) {
      s = {};
      return;  // Do not overwrite controller grip copied into the hand stream.
    }
    const bool valid = enabled && hand.status == 1 &&
        (hand.flags & (xr_runtime::HAND_POSE_VALID | xr_runtime::HAND_JOINTS_VALID)) ==
            (xr_runtime::HAND_POSE_VALID | xr_runtime::HAND_JOINTS_VALID) &&
        std::isfinite(hand.confidence) && hand.confidence >= config_.min_confidence;
    if (!valid) {
      s = {};
      write();
      return;
    }
    if (stamp < s.sample) s = {};
    const auto measured = emulation::measure_hand(hand, config_.min_confidence);
    for (size_t i = 0; i < s.extended.size(); ++i) {
      const double on = i == 3 ? config_.little_extension_on : config_.extension_on;
      const double off = i == 3 ? config_.little_extension_off : config_.extension_off;
      s.extended[i] = measured.curl[i] >= 0 &&
          1 - measured.curl[i] >= (s.extended[i] ? off : on);
    }
    const bool extended = std::all_of(s.extended.begin(), s.extended.end(), [](bool v) { return v; });
    const auto pose = emulation::measure_open_palm_pose(measured, extended, config_, viewer, s.facing, false);
    if (!pose.toward_viewer) {
      s.facing = s.active = false;
      s.since = 0;
    } else if (stamp != s.sample) {
      if (!s.facing) s.since = stamp;
      s.facing = true;
      s.active = double(stamp - s.since) / 1e6 >= config_.button_confirm_ms;
    }
    s.sample = stamp;
    write();
  }

  emulation::Config config_ = simple_palm_config();
  Side left_, right_;
  uint64_t origin_ = 0, reset_ = 0;
};

}  // namespace xr_runtime_adapter::gestures
