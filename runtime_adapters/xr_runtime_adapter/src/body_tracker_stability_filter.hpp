#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <xr_tracking/contracts/body_tracker_set_contract.hpp>

namespace xr_runtime_adapter::body_tracker_filter {

struct BodyTrackerStabilityGateConfig {
  bool enabled = false;
  double hold_lost_ms = 150.0;
  double predict_lost_ms = 350.0;
  double max_prediction_velocity_mps = 0.8;
  double max_prediction_acceleration_mps2 = 0.0;
  double prediction_damping = 0.35;
  double synthetic_publish_hz = 90.0;
  uint32_t predicted_status = xr_tracking::BODY_TRACKER_STATUS_TRACKING;
};

uint32_t parse_body_tracker_predicted_status(const std::string& value);

class BodyTrackerStabilityFilter {
 public:
  void configure(BodyTrackerStabilityGateConfig cfg);
  void reset();

  xr_tracking::BodyTrackerSetFrameF32V1 filter_observed(xr_tracking::BodyTrackerSetFrameF32V1 frame,
                                                        uint64_t now_ns);

  std::optional<xr_tracking::BodyTrackerSetFrameF32V1> predicted_frame(uint64_t now_ns);

 private:
  struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  struct State {
    bool active = false;
    uint64_t key = 0;
    xr_tracking::BodyTrackerF32V1 last_good{};
    uint64_t last_good_ns = 0;
    Vec3 velocity_mps{};
    bool has_velocity = false;
  };

  static bool finite3(float x, float y, float z);
  static double norm(Vec3 v);
  static Vec3 sub(Vec3 a, Vec3 b);
  static Vec3 scale(Vec3 v, double s);
  static Vec3 add(Vec3 a, Vec3 b);
  static Vec3 pose_position(const xr_tracking::BodyTrackerF32V1& tracker);
  static void assign_position(xr_tracking::BodyTrackerF32V1& tracker, Vec3 p);
  static uint64_t tracker_key(const xr_tracking::BodyTrackerF32V1& tracker, uint32_t slot_index);
  static bool tracker_pose_is_good(const xr_tracking::BodyTrackerF32V1& tracker);
  static void append_tracker(xr_tracking::BodyTrackerSetFrameF32V1& frame,
                             const xr_tracking::BodyTrackerF32V1& tracker);
  static int64_t ms_to_ns(double ms);

  Vec3 clamp_velocity(Vec3 v) const;
  Vec3 clamp_acceleration(Vec3 desired, Vec3 previous, double dt_s) const;
  State& find_or_create_state(uint64_t key);
  State* find_state(uint64_t key);
  void update_state(const xr_tracking::BodyTrackerF32V1& tracker, uint64_t key, uint64_t sample_ns);
  std::optional<xr_tracking::BodyTrackerF32V1> predicted_tracker_for_key(uint64_t key, uint64_t now_ns);
  std::optional<xr_tracking::BodyTrackerF32V1> predicted_tracker(State& state, uint64_t now_ns);
  int64_t synthetic_interval_ns() const;

  BodyTrackerStabilityGateConfig cfg_{};
  std::array<State, xr_tracking::BODY_TRACKER_MAX_TRACKERS> states_{};
  bool has_template_ = false;
  xr_tracking::BodyTrackerSetFrameF32V1 template_frame_{};
  uint64_t last_synthetic_publish_ns_ = 0;
};

}  // namespace xr_runtime_adapter::body_tracker_filter
