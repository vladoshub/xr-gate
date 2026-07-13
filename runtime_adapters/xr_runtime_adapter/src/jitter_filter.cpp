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

bool smoothing_enabled(double alpha) {
  return std::isfinite(alpha) && alpha > 0.0 && alpha < 1.0;
}

double clamp_smoothing_alpha(double alpha) {
  if (!std::isfinite(alpha)) return 1.0;
  return std::clamp(alpha, 0.0, 1.0);
}

bool hmd_filter_enabled(const RuntimeJitterFilterConfig& cfg) {
  return cfg.enabled && (threshold_enabled(cfg.hmd_threshold_m) ||
                         threshold_enabled(cfg.hmd_angle_threshold_rad) ||
                         threshold_enabled(cfg.hmd_velocity_threshold_mps) ||
                         threshold_enabled(cfg.hmd_angular_velocity_threshold_radps) ||
                         smoothing_enabled(cfg.hmd_velocity_smooth_alpha) ||
                         smoothing_enabled(cfg.hmd_angular_velocity_smooth_alpha) ||
                         smoothing_enabled(cfg.hmd_position_smooth_alpha) ||
                         smoothing_enabled(cfg.hmd_orientation_smooth_alpha));
}

bool tracker_filter_enabled(const RuntimeJitterFilterConfig& cfg) {
  return cfg.enabled && (threshold_enabled(cfg.tracker_threshold_m) ||
                         threshold_enabled(cfg.tracker_angle_threshold_rad) ||
                         threshold_enabled(cfg.tracker_velocity_threshold_mps) ||
                         threshold_enabled(cfg.tracker_angular_velocity_threshold_radps) ||
                         smoothing_enabled(cfg.tracker_velocity_smooth_alpha) ||
                         smoothing_enabled(cfg.tracker_angular_velocity_smooth_alpha) ||
                         smoothing_enabled(cfg.tracker_position_smooth_alpha) ||
                         smoothing_enabled(cfg.tracker_orientation_smooth_alpha));
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

void slerp_from_last(double lw, double lx, double ly, double lz,
                     double cw, double cx, double cy, double cz,
                     double alpha,
                     double& out_w, double& out_x, double& out_y, double& out_z) {
  alpha = clamp_smoothing_alpha(alpha);

  double dot = lw * cw + lx * cx + ly * cy + lz * cz;
  if (dot < 0.0) {
    cw = -cw;
    cx = -cx;
    cy = -cy;
    cz = -cz;
    dot = -dot;
  }
  dot = std::clamp(dot, 0.0, 1.0);

  if (dot > 0.9995) {
    out_w = lw + alpha * (cw - lw);
    out_x = lx + alpha * (cx - lx);
    out_y = ly + alpha * (cy - ly);
    out_z = lz + alpha * (cz - lz);
    normalize_quat(out_w, out_x, out_y, out_z);
    return;
  }

  const double theta_0 = std::acos(dot);
  const double theta = theta_0 * alpha;
  const double sin_theta = std::sin(theta);
  const double sin_theta_0 = std::sin(theta_0);
  if (std::abs(sin_theta_0) <= 1e-12) {
    out_w = cw;
    out_x = cx;
    out_y = cy;
    out_z = cz;
    return;
  }

  const double s0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
  const double s1 = sin_theta / sin_theta_0;
  out_w = s0 * lw + s1 * cw;
  out_x = s0 * lx + s1 * cx;
  out_y = s0 * ly + s1 * cy;
  out_z = s0 * lz + s1 * cz;
  normalize_quat(out_w, out_x, out_y, out_z);
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

void VectorDeadbandSmoothingFilter::reset() {
  has_last_ = false;
  last_x_ = 0.0;
  last_y_ = 0.0;
  last_z_ = 0.0;
}

void VectorDeadbandSmoothingFilter::filter(double& x, double& y, double& z,
                                           double zero_threshold, double smooth_alpha) {
  const bool use_deadband = threshold_enabled(zero_threshold);
  const bool use_smoothing = smoothing_enabled(smooth_alpha);
  if ((!use_deadband && !use_smoothing) || !finite3(x, y, z)) {
    if (!finite3(x, y, z)) {
      x = 0.0;
      y = 0.0;
      z = 0.0;
    }
    reset();
    return;
  }

  const double norm2 = x * x + y * y + z * z;
  if (use_deadband && norm2 <= zero_threshold * zero_threshold) {
    x = 0.0;
    y = 0.0;
    z = 0.0;
    last_x_ = 0.0;
    last_y_ = 0.0;
    last_z_ = 0.0;
    has_last_ = true;
    return;
  }

  if (use_smoothing && has_last_) {
    const double a = clamp_smoothing_alpha(smooth_alpha);
    x = last_x_ + a * (x - last_x_);
    y = last_y_ + a * (y - last_y_);
    z = last_z_ + a * (z - last_z_);
  }

  last_x_ = x;
  last_y_ = y;
  last_z_ = z;
  has_last_ = true;
}

void VectorDeadbandSmoothingFilter::filter(float& x, float& y, float& z,
                                           double zero_threshold, double smooth_alpha) {
  double xd = static_cast<double>(x);
  double yd = static_cast<double>(y);
  double zd = static_cast<double>(z);
  filter(xd, yd, zd, zero_threshold, smooth_alpha);
  x = static_cast<float>(xd);
  y = static_cast<float>(yd);
  z = static_cast<float>(zd);
}

void PositionDeadbandFilter::filter(double& x, double& y, double& z, double threshold_m, double smooth_alpha) {
  const bool use_deadband = threshold_enabled(threshold_m);
  const bool use_smoothing = smoothing_enabled(smooth_alpha);
  if ((!use_deadband && !use_smoothing) || !finite3(x, y, z)) {
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

  if (use_deadband) {
    const double threshold2 = threshold_m * threshold_m;
    if (d2 <= threshold2) {
      x = last_x_;
      y = last_y_;
      z = last_z_;
      return;
    }
  }

  if (use_smoothing) {
    const double a = clamp_smoothing_alpha(smooth_alpha);
    x = last_x_ + a * dx;
    y = last_y_ + a * dy;
    z = last_z_ + a * dz;
  }

  last_x_ = x;
  last_y_ = y;
  last_z_ = z;
}

void PositionDeadbandFilter::filter(float& x, float& y, float& z, double threshold_m, double smooth_alpha) {
  double xd = static_cast<double>(x);
  double yd = static_cast<double>(y);
  double zd = static_cast<double>(z);
  filter(xd, yd, zd, threshold_m, smooth_alpha);
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
                                       double threshold_rad, double smooth_alpha) {
  const bool use_deadband = threshold_enabled(threshold_rad);
  const bool use_smoothing = smoothing_enabled(smooth_alpha);
  if ((!use_deadband && !use_smoothing) || !normalize_quat(qw, qx, qy, qz)) {
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

  double cw = qw;
  double cx = qx;
  double cy = qy;
  double cz = qz;
  double dot = last_w_ * cw + last_x_ * cx + last_y_ * cy + last_z_ * cz;
  if (dot < 0.0) {
    cw = -cw;
    cx = -cx;
    cy = -cy;
    cz = -cz;
    dot = -dot;
  }
  dot = std::clamp(dot, 0.0, 1.0);
  const double angle_rad = 2.0 * std::acos(dot);

  if (use_deadband && angle_rad <= threshold_rad) {
    qw = last_w_;
    qx = last_x_;
    qy = last_y_;
    qz = last_z_;
    return;
  }

  if (use_smoothing) {
    slerp_from_last(last_w_, last_x_, last_y_, last_z_, cw, cx, cy, cz, smooth_alpha, qw, qx, qy, qz);
  } else {
    qw = cw;
    qx = cx;
    qy = cy;
    qz = cz;
  }

  last_w_ = qw;
  last_x_ = qx;
  last_y_ = qy;
  last_z_ = qz;
}

void OrientationDeadbandFilter::filter(float& qw, float& qx, float& qy, float& qz,
                                       double threshold_rad, double smooth_alpha) {
  double qwd = static_cast<double>(qw);
  double qxd = static_cast<double>(qx);
  double qyd = static_cast<double>(qy);
  double qzd = static_cast<double>(qz);
  filter(qwd, qxd, qyd, qzd, threshold_rad, smooth_alpha);
  qw = static_cast<float>(qwd);
  qx = static_cast<float>(qxd);
  qy = static_cast<float>(qyd);
  qz = static_cast<float>(qzd);
}

void HmdJitterFilter::reset() {
  last_reset_counter_ = 0;
  position_.reset();
  orientation_.reset();
  linear_velocity_.reset();
  angular_velocity_.reset();
}

void HmdJitterFilter::filter(xr_runtime::HmdPoseF64V1& hmd, const RuntimeJitterFilterConfig& cfg) {
  if (!hmd_filter_enabled(cfg) || !hmd_has_position(hmd)) {
    position_.reset();
    orientation_.reset();
    linear_velocity_.reset();
    angular_velocity_.reset();
    if (hmd.sequence == 0) last_reset_counter_ = 0;
    return;
  }

  if (last_reset_counter_ != 0 && hmd.reset_counter != last_reset_counter_) {
    position_.reset();
    orientation_.reset();
    linear_velocity_.reset();
    angular_velocity_.reset();
  }
  last_reset_counter_ = hmd.reset_counter;

  position_.filter(hmd.px, hmd.py, hmd.pz, cfg.hmd_threshold_m, cfg.hmd_position_smooth_alpha);
  orientation_.filter(hmd.qw, hmd.qx, hmd.qy, hmd.qz, cfg.hmd_angle_threshold_rad, cfg.hmd_orientation_smooth_alpha);

  if ((hmd.flags & xr_runtime::HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
    linear_velocity_.filter(hmd.vx, hmd.vy, hmd.vz,
                            cfg.hmd_velocity_threshold_mps,
                            cfg.hmd_velocity_smooth_alpha);
  } else {
    linear_velocity_.reset();
  }
  if ((hmd.flags & xr_runtime::HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) {
    angular_velocity_.filter(hmd.wx, hmd.wy, hmd.wz,
                             cfg.hmd_angular_velocity_threshold_radps,
                             cfg.hmd_angular_velocity_smooth_alpha);
  } else {
    angular_velocity_.reset();
  }
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
  linear_velocity_.reset();
  angular_velocity_.reset();
}

void HandSideJitterFilter::filter(xr_runtime::HandSideF32V2& side, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || !hand_side_has_pose_payload(side)) {
    reset();
    return;
  }

  if ((side.flags & xr_runtime::HAND_POSE_VALID) != 0u) {
    controller_.filter(side.controller_px, side.controller_py, side.controller_pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
    palm_.filter(side.palm_px, side.palm_py, side.palm_pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
    wrist_.filter(side.wrist_px, side.wrist_py, side.wrist_pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);

    controller_orientation_.filter(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz,
                                   cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
    palm_orientation_.filter(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz,
                             cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
    wrist_orientation_.filter(side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz,
                              cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
    if ((side.flags & xr_runtime::HAND_LINEAR_VELOCITY_VALID) != 0u) {
      linear_velocity_.filter(side.vx, side.vy, side.vz,
                              cfg.tracker_velocity_threshold_mps,
                              cfg.tracker_velocity_smooth_alpha);
    } else {
      linear_velocity_.reset();
    }
    if ((side.flags & xr_runtime::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
      angular_velocity_.filter(side.wx, side.wy, side.wz,
                               cfg.tracker_angular_velocity_threshold_radps,
                               cfg.tracker_angular_velocity_smooth_alpha);
    } else {
      angular_velocity_.reset();
    }
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
      joints_v2_[i].filter(side.joints[i].px, side.joints[i].py, side.joints[i].pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
      joint_orientations_v2_[i].filter(side.joints[i].qw, side.joints[i].qx, side.joints[i].qy, side.joints[i].qz,
                                       cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
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
    palm_.filter(side.palm_px, side.palm_py, side.palm_pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
    wrist_.filter(side.wrist_px, side.wrist_py, side.wrist_pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
    palm_orientation_.filter(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz,
                             cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
    wrist_orientation_.filter(side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz,
                              cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
    if ((side.flags & xr_runtime::HAND_LINEAR_VELOCITY_VALID) != 0u) {
      linear_velocity_.filter(side.vx, side.vy, side.vz,
                              cfg.tracker_velocity_threshold_mps,
                              cfg.tracker_velocity_smooth_alpha);
    } else {
      linear_velocity_.reset();
    }
    if ((side.flags & xr_runtime::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
      angular_velocity_.filter(side.wx, side.wy, side.wz,
                               cfg.tracker_angular_velocity_threshold_radps,
                               cfg.tracker_angular_velocity_smooth_alpha);
    } else {
      angular_velocity_.reset();
    }
  } else {
    palm_.reset();
    wrist_.reset();
    palm_orientation_.reset();
    wrist_orientation_.reset();
  }

  const uint32_t n = std::min<uint32_t>(side.joint_count, xr_runtime::HAND_JOINT_COUNT_V1);
  for (uint32_t i = 0; i < n; ++i) {
    joints_v1_[i].filter(side.joints[i].px, side.joints[i].py, side.joints[i].pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
    joint_orientations_v1_[i].filter(side.joints[i].qw, side.joints[i].qx, side.joints[i].qy, side.joints[i].qz,
                                     cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
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
  for (auto& f : tracker_linear_velocities_) f.reset();
  for (auto& f : tracker_angular_velocities_) f.reset();
}

void BodyTrackerJitterFilter::filter(xr_tracking::BodyTrackerSetFrameF32V1& frame, const RuntimeJitterFilterConfig& cfg) {
  if (!tracker_filter_enabled(cfg) || frame.sequence == 0) {
    reset();
    return;
  }

  frame.tracker_count = std::min<uint32_t>(frame.tracker_count, xr_tracking::BODY_TRACKER_MAX_TRACKERS);
  for (uint32_t i = 0; i < frame.tracker_count; ++i) {
    auto& tracker = frame.trackers[i];
    auto& pose = tracker.pose;
    tracker_positions_[i].filter(pose.px, pose.py, pose.pz, cfg.tracker_threshold_m, cfg.tracker_position_smooth_alpha);
    tracker_orientations_[i].filter(pose.qw, pose.qx, pose.qy, pose.qz, cfg.tracker_angle_threshold_rad, cfg.tracker_orientation_smooth_alpha);
    if ((tracker.flags & xr_tracking::BODY_TRACKER_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
      tracker_linear_velocities_[i].filter(pose.vx, pose.vy, pose.vz,
                                           cfg.tracker_velocity_threshold_mps,
                                           cfg.tracker_velocity_smooth_alpha);
    } else {
      tracker_linear_velocities_[i].reset();
    }
    tracker_angular_velocities_[i].filter(pose.wx, pose.wy, pose.wz,
                                          cfg.tracker_angular_velocity_threshold_radps,
                                          cfg.tracker_angular_velocity_smooth_alpha);
  }
  for (uint32_t i = frame.tracker_count; i < xr_tracking::BODY_TRACKER_MAX_TRACKERS; ++i) {
    tracker_positions_[i].reset();
    tracker_orientations_[i].reset();
    tracker_linear_velocities_[i].reset();
    tracker_angular_velocities_[i].reset();
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
  controller_state_.reset();
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

void RuntimeControllerStateJitterFilter::reset() {
  left_orientation_.reset();
  right_orientation_.reset();
  left_linear_velocity_.reset();
  right_linear_velocity_.reset();
  left_angular_velocity_.reset();
  right_angular_velocity_.reset();
}

void RuntimeControllerStateJitterFilter::filter(
    xr_runtime::RuntimeControllerStateFrameV1& frame, const RuntimeJitterFilterConfig& cfg) {
  filter(frame, cfg, cfg, false, false);
}

void RuntimeControllerStateJitterFilter::filter(
    xr_runtime::RuntimeControllerStateFrameV1& frame,
    const RuntimeJitterFilterConfig& left_cfg,
    const RuntimeJitterFilterConfig& right_cfg,
    bool filter_left_orientation,
    bool filter_right_orientation) {
  auto filter_side = [](xr_runtime::RuntimeControllerSideStateV1& side,
                        const RuntimeJitterFilterConfig& cfg,
                        bool filter_orientation,
                        OrientationDeadbandFilter& orientation,
                        VectorDeadbandSmoothingFilter& linear_velocity,
                        VectorDeadbandSmoothingFilter& angular_velocity) {
    if (!tracker_filter_enabled(cfg)) {
      orientation.reset();
      linear_velocity.reset();
      angular_velocity.reset();
      return;
    }
    if (filter_orientation &&
        (side.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) != 0u) {
      orientation.filter(side.orientation_xyzw[3],
                         side.orientation_xyzw[0],
                         side.orientation_xyzw[1],
                         side.orientation_xyzw[2],
                         cfg.tracker_angle_threshold_rad,
                         cfg.tracker_orientation_smooth_alpha);
    } else {
      orientation.reset();
    }
    linear_velocity.filter(side.linear_velocity[0],
                           side.linear_velocity[1],
                           side.linear_velocity[2],
                           cfg.tracker_velocity_threshold_mps,
                           cfg.tracker_velocity_smooth_alpha);
    angular_velocity.filter(side.angular_velocity[0],
                            side.angular_velocity[1],
                            side.angular_velocity[2],
                            cfg.tracker_angular_velocity_threshold_radps,
                            cfg.tracker_angular_velocity_smooth_alpha);
  };

  filter_side(frame.left, left_cfg, filter_left_orientation,
              left_orientation_, left_linear_velocity_, left_angular_velocity_);
  filter_side(frame.right, right_cfg, filter_right_orientation,
              right_orientation_, right_linear_velocity_, right_angular_velocity_);
}

void RuntimeJitterFilter::filter_runtime_controller_state(
    xr_runtime::RuntimeControllerStateFrameV1& frame) {
  controller_state_.filter(frame, cfg_);
}

void RuntimeJitterFilter::filter_runtime_controller_state(
    xr_runtime::RuntimeControllerStateFrameV1& frame,
    const RuntimeJitterFilterConfig& left_cfg,
    const RuntimeJitterFilterConfig& right_cfg,
    bool filter_left_orientation,
    bool filter_right_orientation) {
  controller_state_.filter(frame, left_cfg, right_cfg,
                           filter_left_orientation, filter_right_orientation);
}

}  // namespace xr_runtime_adapter::jitter_filter
