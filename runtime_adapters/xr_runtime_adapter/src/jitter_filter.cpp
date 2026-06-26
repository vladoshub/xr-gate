#include "jitter_filter.hpp"

#include <algorithm>
#include <cmath>

namespace xr_runtime_adapter::jitter_filter {
namespace {

constexpr double kQuatNormEpsilon = 1e-12;

bool finite3(double x, double y, double z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool finite3(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool finite4(double w, double x, double y, double z) {
  return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool finite4(float w, float x, float y, float z) {
  return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool threshold_enabled(double threshold_m) {
  return std::isfinite(threshold_m) && threshold_m > 0.0;
}

bool hmd_filter_enabled(const RuntimeJitterFilterConfig& cfg) {
  return cfg.enabled && (threshold_enabled(cfg.hmd_threshold_m) ||
                         threshold_enabled(cfg.hmd_angle_threshold_rad));
}

bool tracker_filter_enabled(const RuntimeJitterFilterConfig& cfg) {
  return cfg.enabled && (threshold_enabled(cfg.tracker_threshold_m) ||
                         threshold_enabled(cfg.tracker_angle_threshold_rad));
}

bool normalize_quat(double& w, double& x, double& y, double& z) {
  if (!finite4(w, x, y, z)) return false;
  const double n2 = w * w + x * x + y * y + z * z;
  if (!std::isfinite(n2) || n2 <= kQuatNormEpsilon) return false;
  const double inv_n = 1.0 / std::sqrt(n2);
  w *= inv_n;
  x *= inv_n;
  y *= inv_n;
  z *= inv_n;
  return true;
}

bool hmd_has_position(const xr_runtime::HmdPoseF64V1& hmd) {
  return hmd.sequence != 0 && finite3(hmd.px, hmd.py, hmd.pz);
}

bool hand_side_has_pose_payload(const xr_runtime::HandSideF32V2& side) {
  const bool active = side.status == 1u || side.status == 2u;
  const bool has_pose = (side.flags & xr_runtime::HAND_POSE_VALID) != 0u;
  const bool has_joints = (side.flags & xr_runtime::HAND_JOINTS_VALID) != 0u && side.joint_count > 0u;
  return active && (has_pose || has_joints);
}

bool hand_side_has_pose_payload(const xr_runtime::HandSideF64V1& side) {
  const bool active = side.status == 1u || side.status == 2u;
  const bool has_pose = (side.flags & xr_runtime::HAND_POSE_VALID) != 0u;
  const bool has_joints = side.joint_count > 0u;
  return active && (has_pose || has_joints);
}

}  // namespace

void PositionDeadbandFilter::reset() {
  has_last_ = false;
  last_x_ = 0.0;
  last_y_ = 0.0;
  last_z_ = 0.0;
}

void PositionDeadbandFilter::filter(double& x, double& y, double& z, double threshold_m) {
  if (!threshold_enabled(threshold_m) || !finite3(x, y, z)) {
    reset();
    return;
  }

  if (!has_last_) {
    last_x_ = x;
    last_y_ = y;
    last_z_ = z;
    has_last_ = true;
    return;
  }

  const double dx = x - last_x_;
  const double dy = y - last_y_;
  const double dz = z - last_z_;
  const double d2 = dx * dx + dy * dy + dz * dz;
  const double threshold2 = threshold_m * threshold_m;

  if (d2 <= threshold2) {
    x = last_x_;
    y = last_y_;
    z = last_z_;
    return;
  }

  last_x_ = x;
  last_y_ = y;
  last_z_ = z;
}

void PositionDeadbandFilter::filter(float& x, float& y, float& z, double threshold_m) {
  double xd = static_cast<double>(x);
  double yd = static_cast<double>(y);
  double zd = static_cast<double>(z);
  filter(xd, yd, zd, threshold_m);
  x = static_cast<float>(xd);
  y = static_cast<float>(yd);
  z = static_cast<float>(zd);
}

void OrientationDeadbandFilter::reset() {
  has_last_ = false;
  last_w_ = 1.0;
  last_x_ = 0.0;
  last_y_ = 0.0;
  last_z_ = 0.0;
}

void OrientationDeadbandFilter::filter(double& qw, double& qx, double& qy, double& qz,
                                       double threshold_rad) {
  if (!threshold_enabled(threshold_rad) || !normalize_quat(qw, qx, qy, qz)) {
    reset();
    return;
  }

  if (!has_last_) {
    last_w_ = qw;
    last_x_ = qx;
    last_y_ = qy;
    last_z_ = qz;
    has_last_ = true;
    return;
  }

  double dot = last_w_ * qw + last_x_ * qx + last_y_ * qy + last_z_ * qz;
  if (dot < 0.0) {
    qw = -qw;
    qx = -qx;
    qy = -qy;
    qz = -qz;
    dot = -dot;
  }
  dot = std::clamp(dot, 0.0, 1.0);
  const double angle_rad = 2.0 * std::acos(dot);

  if (angle_rad <= threshold_rad) {
    qw = last_w_;
    qx = last_x_;
    qy = last_y_;
    qz = last_z_;
    return;
  }

  last_w_ = qw;
  last_x_ = qx;
  last_y_ = qy;
  last_z_ = qz;
}

void OrientationDeadbandFilter::filter(float& qw, float& qx, float& qy, float& qz,
                                       double threshold_rad) {
  double qwd = static_cast<double>(qw);
  double qxd = static_cast<double>(qx);
  double qyd = static_cast<double>(qy);
  double qzd = static_cast<double>(qz);
  filter(qwd, qxd, qyd, qzd, threshold_rad);
  qw = static_cast<float>(qwd);
  qx = static_cast<float>(qxd);
  qy = static_cast<float>(qyd);
  qz = static_cast<float>(qzd);
}

void HmdJitterFilter::reset() {
  last_reset_counter_ = 0;
  position_.reset();
  orientation_.reset();
}

void HmdJitterFilter::filter(xr_runtime::HmdPoseF64V1& hmd, const RuntimeJitterFilterConfig& cfg) {
  if (!hmd_filter_enabled(cfg) || !hmd_has_position(hmd)) {
    position_.reset();
    orientation_.reset();
    if (hmd.sequence == 0) last_reset_counter_ = 0;
    return;
  }

  if (last_reset_counter_ != 0 && hmd.reset_counter != last_reset_counter_) {
    position_.reset();
    orientation_.reset();
  }
  last_reset_counter_ = hmd.reset_counter;

  position_.filter(hmd.px, hmd.py, hmd.pz, cfg.hmd_threshold_m);
  orientation_.filter(hmd.qw, hmd.qx, hmd.qy, hmd.qz, cfg.hmd_angle_threshold_rad);
}

void HandSideJitterFilter::reset() {
  controller_.reset();
  palm_.reset();
  wrist_.reset();
  controller_orientation_.reset();
  palm_orientation_.reset();
  wrist_orientation_.reset();
  for (auto& f : joints_v2_) f.reset();
  for (auto& f : joints_v1_) f.reset();
  for (auto& f : joint_orientations_v2_) f.reset();
  for (auto& f : joint_orientations_v1_) f.reset();
}

void HandSideJitterFilter::filter(xr_runtime::HandSideF32V2& side, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || !hand_side_has_pose_payload(side)) {
    reset();
    return;
  }

  if ((side.flags & xr_runtime::HAND_POSE_VALID) != 0u) {
    controller_.filter(side.controller_px, side.controller_py, side.controller_pz, cfg.tracker_threshold_m);
    palm_.filter(side.palm_px, side.palm_py, side.palm_pz, cfg.tracker_threshold_m);
    wrist_.filter(side.wrist_px, side.wrist_py, side.wrist_pz, cfg.tracker_threshold_m);

    controller_orientation_.filter(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz,
                                   cfg.tracker_angle_threshold_rad);
    palm_orientation_.filter(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz,
                             cfg.tracker_angle_threshold_rad);
    wrist_orientation_.filter(side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz,
                              cfg.tracker_angle_threshold_rad);
  } else {
    controller_.reset();
    palm_.reset();
    wrist_.reset();
    controller_orientation_.reset();
    palm_orientation_.reset();
    wrist_orientation_.reset();
  }

  if ((side.flags & xr_runtime::HAND_JOINTS_VALID) != 0u) {
    const uint32_t n = std::min<uint32_t>(side.joint_count, xr_runtime::HAND_JOINT_COUNT_V2);
    for (uint32_t i = 0; i < n; ++i) {
      joints_v2_[i].filter(side.joints[i].px, side.joints[i].py, side.joints[i].pz, cfg.tracker_threshold_m);
      joint_orientations_v2_[i].filter(side.joints[i].qw, side.joints[i].qx, side.joints[i].qy, side.joints[i].qz,
                                       cfg.tracker_angle_threshold_rad);
    }
    for (uint32_t i = n; i < xr_runtime::HAND_JOINT_COUNT_V2; ++i) {
      joints_v2_[i].reset();
      joint_orientations_v2_[i].reset();
    }
  } else {
    for (auto& f : joints_v2_) f.reset();
    for (auto& f : joint_orientations_v2_) f.reset();
  }
}

void HandSideJitterFilter::filter(xr_runtime::HandSideF64V1& side, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || !hand_side_has_pose_payload(side)) {
    reset();
    return;
  }

  if ((side.flags & xr_runtime::HAND_POSE_VALID) != 0u) {
    palm_.filter(side.palm_px, side.palm_py, side.palm_pz, cfg.tracker_threshold_m);
    wrist_.filter(side.wrist_px, side.wrist_py, side.wrist_pz, cfg.tracker_threshold_m);
    palm_orientation_.filter(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz,
                             cfg.tracker_angle_threshold_rad);
    wrist_orientation_.filter(side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz,
                              cfg.tracker_angle_threshold_rad);
  } else {
    palm_.reset();
    wrist_.reset();
    palm_orientation_.reset();
    wrist_orientation_.reset();
  }

  const uint32_t n = std::min<uint32_t>(side.joint_count, xr_runtime::HAND_JOINT_COUNT_V1);
  for (uint32_t i = 0; i < n; ++i) {
    joints_v1_[i].filter(side.joints[i].px, side.joints[i].py, side.joints[i].pz, cfg.tracker_threshold_m);
    joint_orientations_v1_[i].filter(side.joints[i].qw, side.joints[i].qx, side.joints[i].qy, side.joints[i].qz,
                                     cfg.tracker_angle_threshold_rad);
  }
  for (uint32_t i = n; i < xr_runtime::HAND_JOINT_COUNT_V1; ++i) {
    joints_v1_[i].reset();
    joint_orientations_v1_[i].reset();
  }
}

void HandJitterFilter::reset() {
  last_reset_counter_v2_ = 0;
  last_reset_counter_v1_ = 0;
  left_.reset();
  right_.reset();
}

void HandJitterFilter::filter(xr_runtime::HandTrackingFrameF32V2& hand, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || hand.sequence == 0) {
    left_.reset();
    right_.reset();
    if (hand.sequence == 0) last_reset_counter_v2_ = 0;
    return;
  }

  if (last_reset_counter_v2_ != 0 && hand.reset_counter != last_reset_counter_v2_) {
    left_.reset();
    right_.reset();
  }
  last_reset_counter_v2_ = hand.reset_counter;

  if ((hand.flags & xr_runtime::HAND_FLAG_LEFT_VALID) != 0u) {
    left_.filter(hand.left, cfg);
  } else {
    left_.reset();
  }
  if ((hand.flags & xr_runtime::HAND_FLAG_RIGHT_VALID) != 0u) {
    right_.filter(hand.right, cfg);
  } else {
    right_.reset();
  }
}

void HandJitterFilter::filter(xr_runtime::HandTrackingFrameF64V1& hand, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || hand.sequence == 0) {
    left_.reset();
    right_.reset();
    if (hand.sequence == 0) last_reset_counter_v1_ = 0;
    return;
  }

  if (last_reset_counter_v1_ != 0 && hand.reset_counter != last_reset_counter_v1_) {
    left_.reset();
    right_.reset();
  }
  last_reset_counter_v1_ = hand.reset_counter;

  if ((hand.flags & xr_runtime::HAND_FLAG_LEFT_VALID) != 0u) {
    left_.filter(hand.left, cfg);
  } else {
    left_.reset();
  }
  if ((hand.flags & xr_runtime::HAND_FLAG_RIGHT_VALID) != 0u) {
    right_.filter(hand.right, cfg);
  } else {
    right_.reset();
  }
}

void BodyTrackerJitterFilter::reset() {
  for (auto& f : tracker_positions_) f.reset();
  for (auto& f : tracker_orientations_) f.reset();
}

void BodyTrackerJitterFilter::filter(xr_tracking::BodyTrackerSetFrameF32V1& frame, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || frame.sequence == 0) {
    reset();
    return;
  }

  frame.tracker_count = std::min<uint32_t>(frame.tracker_count, xr_tracking::BODY_TRACKER_MAX_TRACKERS);
  for (uint32_t i = 0; i < frame.tracker_count; ++i) {
    auto& pose = frame.trackers[i].pose;
    tracker_positions_[i].filter(pose.px, pose.py, pose.pz, cfg.tracker_threshold_m);
    tracker_orientations_[i].filter(pose.qw, pose.qx, pose.qy, pose.qz, cfg.tracker_angle_threshold_rad);
  }
  for (uint32_t i = frame.tracker_count; i < xr_tracking::BODY_TRACKER_MAX_TRACKERS; ++i) {
    tracker_positions_[i].reset();
    tracker_orientations_[i].reset();
  }
}

void RuntimeJitterFilter::configure(RuntimeJitterFilterConfig cfg) {
  cfg_ = cfg;
  if (!cfg_.enabled) reset();
}

void RuntimeJitterFilter::reset() {
  hmd_.reset();
  hand_.reset();
  body_.reset();
}

void RuntimeJitterFilter::filter_hmd(xr_runtime::HmdPoseF64V1& hmd) {
  hmd_.filter(hmd, cfg_);
}

void RuntimeJitterFilter::filter_hand(xr_runtime::HandTrackingFrameF32V2& hand) {
  hand_.filter(hand, cfg_);
}

void RuntimeJitterFilter::filter_hand(xr_runtime::HandTrackingFrameF64V1& hand) {
  hand_.filter(hand, cfg_);
}

void RuntimeJitterFilter::filter_body_trackers(xr_tracking::BodyTrackerSetFrameF32V1& frame) {
  body_.filter(frame, cfg_);
}

}  // namespace xr_runtime_adapter::jitter_filter
