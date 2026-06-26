#pragma once

#include <cstdint>
#include <string>

#include <xr_runtime/contracts/runtime_adapter.hpp>

namespace xr_runtime_adapter::hmd_filter {

struct HmdPoseStabilityFilterConfig {
  bool enabled = false;

  // Reject pose jumps faster than max_distance_m per window_ms.
  // For example: window_ms=250 and max_distance_m=0.50 means ~2 m/s.
  double window_ms = 250.0;
  double max_distance_m = 0.50;

  // To avoid log spam, unstable messages are throttled, but every message
  // includes the full current unstable duration.
  double log_interval_ms = 250.0;
};

struct HmdPoseStabilityFilterDecision {
  bool input_stable = true;
  bool output_substituted = false;
  bool used_startup_pose = false;
  bool used_last_good_pose = false;
  bool first_good_pose_seen = false;
  double unstable_ms = 0.0;
  double step_distance_m = 0.0;
  double allowed_distance_m = 0.0;
  double dt_ms = 0.0;
  std::string reason;
};

class HmdPoseStabilityFilter {
 public:
  void configure(HmdPoseStabilityFilterConfig cfg);
  [[nodiscard]] const HmdPoseStabilityFilterConfig& config() const { return cfg_; }

  void reset();

  xr_runtime::HmdPoseF64V1 filter(const xr_runtime::HmdPoseF64V1& input,
                                  int64_t receive_timestamp_ns,
                                  HmdPoseStabilityFilterDecision* decision = nullptr);

  [[nodiscard]] bool has_good_pose() const { return has_last_good_; }
  [[nodiscard]] uint64_t rejected_count() const { return rejected_count_; }
  [[nodiscard]] uint64_t startup_pose_count() const { return startup_pose_count_; }
  [[nodiscard]] uint64_t held_last_good_count() const { return held_last_good_count_; }
  [[nodiscard]] double current_unstable_ms(int64_t now_ns) const;
  [[nodiscard]] std::string summary_string() const;

 private:
  static bool is_pose_finite_and_normal(const xr_runtime::HmdPoseF64V1& hmd);
  static xr_runtime::HmdPoseF64V1 make_startup_pose(const xr_runtime::HmdPoseF64V1& input,
                                                    int64_t now_ns);
  static xr_runtime::HmdPoseF64V1 make_substituted_pose(xr_runtime::HmdPoseF64V1 pose,
                                                        const xr_runtime::HmdPoseF64V1& input,
                                                        int64_t now_ns);

  bool should_log_unstable(int64_t now_ns) const;
  void maybe_log_unstable(const HmdPoseStabilityFilterDecision& decision,
                          const xr_runtime::HmdPoseF64V1& input,
                          int64_t now_ns);

  HmdPoseStabilityFilterConfig cfg_{};
  bool has_last_good_ = false;
  xr_runtime::HmdPoseF64V1 last_good_{};
  int64_t last_good_receive_ns_ = 0;
  uint64_t last_reset_counter_ = 0;

  int64_t unstable_since_ns_ = 0;
  int64_t last_unstable_log_ns_ = 0;
  uint64_t rejected_count_ = 0;
  uint64_t startup_pose_count_ = 0;
  uint64_t held_last_good_count_ = 0;
};

}  // namespace xr_runtime_adapter::hmd_filter
