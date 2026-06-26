#pragma once

#include <array>
#include <cstdint>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_tracking/contracts/body_tracker_set_contract.hpp>

namespace xr_runtime_adapter::jitter_filter {

struct RuntimeJitterFilterConfig {
  bool enabled = false;
  double hmd_threshold_m = 0.0;
  double tracker_threshold_m = 0.0;
  double hmd_angle_threshold_rad = 0.0;
  double tracker_angle_threshold_rad = 0.0;
};

class PositionDeadbandFilter {
 public:
  void reset();
  void filter(float& x, float& y, float& z, double threshold_m);
  void filter(double& x, double& y, double& z, double threshold_m);

 private:
  bool has_last_ = false;
  double last_x_ = 0.0;
  double last_y_ = 0.0;
  double last_z_ = 0.0;
};

class OrientationDeadbandFilter {
 public:
  void reset();
  void filter(float& qw, float& qx, float& qy, float& qz, double threshold_rad);
  void filter(double& qw, double& qx, double& qy, double& qz, double threshold_rad);

 private:
  bool has_last_ = false;
  double last_w_ = 1.0;
  double last_x_ = 0.0;
  double last_y_ = 0.0;
  double last_z_ = 0.0;
};

class HmdJitterFilter {
 public:
  void reset();
  void filter(xr_runtime::HmdPoseF64V1& hmd, const RuntimeJitterFilterConfig& cfg);

 private:
  uint64_t last_reset_counter_ = 0;
  PositionDeadbandFilter position_;
  OrientationDeadbandFilter orientation_;
};

class HandSideJitterFilter {
 public:
  void reset();
  void filter(xr_runtime::HandSideF32V2& side, const RuntimeJitterFilterConfig& cfg);
  void filter(xr_runtime::HandSideF64V1& side, const RuntimeJitterFilterConfig& cfg);

 private:
  PositionDeadbandFilter controller_;
  PositionDeadbandFilter palm_;
  PositionDeadbandFilter wrist_;
  OrientationDeadbandFilter controller_orientation_;
  OrientationDeadbandFilter palm_orientation_;
  OrientationDeadbandFilter wrist_orientation_;
  std::array<PositionDeadbandFilter, xr_runtime::HAND_JOINT_COUNT_V2> joints_v2_{};
  std::array<PositionDeadbandFilter, xr_runtime::HAND_JOINT_COUNT_V1> joints_v1_{};
  std::array<OrientationDeadbandFilter, xr_runtime::HAND_JOINT_COUNT_V2> joint_orientations_v2_{};
  std::array<OrientationDeadbandFilter, xr_runtime::HAND_JOINT_COUNT_V1> joint_orientations_v1_{};
};

class HandJitterFilter {
 public:
  void reset();
  void filter(xr_runtime::HandTrackingFrameF32V2& hand, const RuntimeJitterFilterConfig& cfg);
  void filter(xr_runtime::HandTrackingFrameF64V1& hand, const RuntimeJitterFilterConfig& cfg);

 private:
  uint64_t last_reset_counter_v2_ = 0;
  uint64_t last_reset_counter_v1_ = 0;
  HandSideJitterFilter left_;
  HandSideJitterFilter right_;
};

class BodyTrackerJitterFilter {
 public:
  void reset();
  void filter(xr_tracking::BodyTrackerSetFrameF32V1& frame, const RuntimeJitterFilterConfig& cfg);

 private:
  std::array<PositionDeadbandFilter, xr_tracking::BODY_TRACKER_MAX_TRACKERS> tracker_positions_{};
  std::array<OrientationDeadbandFilter, xr_tracking::BODY_TRACKER_MAX_TRACKERS> tracker_orientations_{};
};

class RuntimeJitterFilter {
 public:
  RuntimeJitterFilter() = default;
  explicit RuntimeJitterFilter(RuntimeJitterFilterConfig cfg) : cfg_(cfg) {}

  void configure(RuntimeJitterFilterConfig cfg);
  [[nodiscard]] const RuntimeJitterFilterConfig& config() const { return cfg_; }

  void reset();
  void filter_hmd(xr_runtime::HmdPoseF64V1& hmd);
  void filter_hand(xr_runtime::HandTrackingFrameF32V2& hand);
  void filter_hand(xr_runtime::HandTrackingFrameF64V1& hand);
  void filter_body_trackers(xr_tracking::BodyTrackerSetFrameF32V1& frame);

 private:
  RuntimeJitterFilterConfig cfg_{};
  HmdJitterFilter hmd_{};
  HandJitterFilter hand_{};
  BodyTrackerJitterFilter body_{};
};

}  // namespace xr_runtime_adapter::jitter_filter
