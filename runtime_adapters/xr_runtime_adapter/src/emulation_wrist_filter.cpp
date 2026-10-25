#include "emulation_wrist_filter.hpp"

#include <algorithm>
#include <cmath>

namespace xr_runtime_adapter::emulation {
namespace {
using Q = std::array<double, 4>;
constexpr double pi = 3.14159265358979323846;
Q conjugate(Q q) { return {q[0], -q[1], -q[2], -q[3]}; }
Q multiply(const Q& a, const Q& b) {
  return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
          a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
          a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
          a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}
bool normalize(Q& q) {
  double n = 0;
  for (double x : q) n += x * x;
  if (!std::isfinite(n) || n < 1e-8) return false;
  for (auto& x : q) x /= std::sqrt(n);
  return true;
}
Vec3 rotate(const Q& q, const Vec3& v) {
  const Q result = multiply(multiply(q, {0, v[0], v[1], v[2]}), conjugate(q));
  return {result[1], result[2], result[3]};
}
template <class T>
void position(T& x, T& y, T& z, const Q& correction, const Vec3& pivot) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
  const auto r = rotate(correction, {x - pivot[0], y - pivot[1], z - pivot[2]});
  x = T(pivot[0] + r[0]);
  y = T(pivot[1] + r[1]);
  z = T(pivot[2] + r[2]);
}
template <class T>
void orientation(T& w, T& x, T& y, T& z, const Q& correction) {
  Q q{w, x, y, z};
  if (!normalize(q)) return;
  q = multiply(correction, q);
  w = T(q[0]);
  x = T(q[1]);
  y = T(q[2]);
  z = T(q[3]);
}
Q yaw_correction(double degrees) {
  const double half = degrees * pi / 360;
  return {std::cos(half), 0, std::sin(half), 0};
}
template <class Hand>
bool valid_output_pose(const Hand& hand, const Vec3& pivot, Q q) {
  return (hand.status == 1 || hand.status == 2) && (hand.flags & xr_runtime::HAND_POSE_VALID) &&
         std::isfinite(pivot[0]) && std::isfinite(pivot[1]) && std::isfinite(pivot[2]) &&
         normalize(q);
}
template <class Hand>
void rotate_hand_payload(Hand& hand, const Q& correction, const Vec3& pivot) {
  position(hand.palm_px, hand.palm_py, hand.palm_pz, correction, pivot);
  position(hand.wrist_px, hand.wrist_py, hand.wrist_pz, correction, pivot);
  orientation(hand.palm_qw, hand.palm_qx, hand.palm_qy, hand.palm_qz, correction);
  orientation(hand.wrist_qw, hand.wrist_qx, hand.wrist_qy, hand.wrist_qz, correction);
  const auto count =
      std::min<size_t>(hand.joint_count, sizeof(hand.joints) / sizeof(hand.joints[0]));
  if (hand.flags & xr_runtime::HAND_JOINTS_VALID) {
    for (size_t i = 0; i < count; ++i) {
      auto& joint = hand.joints[i];
      if (!joint.flags) continue;
      position(joint.px, joint.py, joint.pz, correction, pivot);
      orientation(joint.qw, joint.qx, joint.qy, joint.qz, correction);
    }
  }
  // The controller position is the fixed pivot, so its world linear velocity stays unchanged.
  // A final world-space yaw rotates the world angular velocity with the orientation.
  if (hand.flags & xr_runtime::HAND_ANGULAR_VELOCITY_VALID) {
    const auto w = rotate(correction, {hand.wx, hand.wy, hand.wz});
    hand.wx = w[0];
    hand.wy = w[1];
    hand.wz = w[2];
  }
}
void output_yaw(xr_runtime::HandSideF32V2& hand, double degrees) {
  const Vec3 pivot{hand.controller_px, hand.controller_py, hand.controller_pz};
  if (!degrees || !valid_output_pose(hand, pivot,
                                     {hand.controller_qw, hand.controller_qx, hand.controller_qy,
                                      hand.controller_qz}))
    return;
  const auto correction = yaw_correction(degrees);
  rotate_hand_payload(hand, correction, pivot);
  orientation(hand.controller_qw, hand.controller_qx, hand.controller_qy, hand.controller_qz,
              correction);
}
void output_yaw(xr_runtime::HandSideF64V1& hand, double degrees) {
  const Vec3 pivot{hand.palm_px, hand.palm_py, hand.palm_pz};
  if (!degrees ||
      !valid_output_pose(hand, pivot, {hand.palm_qw, hand.palm_qx, hand.palm_qy, hand.palm_qz}))
    return;
  rotate_hand_payload(hand, yaw_correction(degrees), pivot);
}
void output_yaw(xr_runtime::RuntimeControllerSideStateV1& side, double degrees) {
  if (!degrees || !(side.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) ||
      !(side.source_mask & xr_runtime::RUNTIME_CONTROLLER_SOURCE_HAND_ORIENTATION) ||
      (side.source_mask & xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_ORIENTATION))
    return;
  const auto correction = yaw_correction(degrees);
  auto& q = side.orientation_xyzw;
  orientation(q[3], q[0], q[1], q[2], correction);
  const auto w = rotate(
      correction, {side.angular_velocity[0], side.angular_velocity[1], side.angular_velocity[2]});
  for (size_t i = 0; i < 3; ++i) side.angular_velocity[i] = float(w[i]);
}
}  // namespace

void apply_pose_yaw_offset(xr_runtime::HandTrackingFrameF32V2& f, const Config& c, bool left,
                           bool right) {
  if (c.left.enabled && left) output_yaw(f.left, c.pose_yaw_offset_degrees);
  if (c.right.enabled && right) output_yaw(f.right, -c.pose_yaw_offset_degrees);
}
void apply_pose_yaw_offset(xr_runtime::HandTrackingFrameF64V1& f, const Config& c, bool left,
                           bool right) {
  if (c.left.enabled && left) output_yaw(f.left, c.pose_yaw_offset_degrees);
  if (c.right.enabled && right) output_yaw(f.right, -c.pose_yaw_offset_degrees);
}
void apply_pose_yaw_offset(xr_runtime::RuntimeControllerStateFrameV1& f, const Config& c) {
  if (c.left.enabled) output_yaw(f.left, c.pose_yaw_offset_degrees);
  if (c.right.enabled) output_yaw(f.right, -c.pose_yaw_offset_degrees);
}

void WristPoseFilter::side(xr_runtime::HandSideF32V2& h, State& s,
                           const xr_runtime::HandTrackingFrameF32V2& frame, const Config& c,
                           uint64_t now, uint64_t origin, bool enabled) {
  const uint64_t stamp = frame.source_timestamp_ns ? frame.source_timestamp_ns : frame.timestamp_ns;
  const bool fresh =
      stamp && (stamp <= now ? double(now - stamp) / 1e6 <= c.max_age_ms : stamp - now <= 2000000);
  Q raw{h.controller_qw, h.controller_qx, h.controller_qy, h.controller_qz};
  const auto& wrist = h.joints[0];
  const Vec3 pivot{wrist.px, wrist.py, wrist.pz};
  if (!enabled || !c.wrist_filter_enabled || c.wrist_filter_degrees <= 0 || !frame.sequence ||
      !fresh || h.status != 1 || (h.flags & 9u) != 9u || !std::isfinite(h.confidence) ||
      h.confidence < c.min_confidence || h.joint_count < 1 || wrist.joint_id != 0 || !wrist.flags ||
      !std::isfinite(wrist.confidence) || wrist.confidence < c.min_confidence ||
      !std::isfinite(pivot[0]) || !std::isfinite(pivot[1]) || !std::isfinite(pivot[2]) ||
      !normalize(raw)) {
    s = {};
    return;
  }
  if (!s.active || s.reset != frame.reset_counter || s.origin != origin || stamp < s.stamp ||
      frame.sequence < s.sequence) {
    s = {};
    s.active = true;
    s.orientation = raw;
  } else if (stamp != s.stamp) {
    Q target = raw;
    double dot = 0;
    for (size_t i = 0; i < 4; ++i) dot += s.orientation[i] * target[i];
    if (dot < 0)
      for (auto& x : target) x = -x;
    const double angle = 2 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0));
    const double threshold = c.wrist_filter_degrees * pi / 180;
    if (angle > threshold) {
      const auto previous = s.orientation;
      const double t = (angle - threshold) / angle;
      const double half = angle / 2, denominator = std::sin(half);
      for (size_t i = 0; i < 4; ++i)
        s.orientation[i] =
            (std::sin((1 - t) * half) * previous[i] + std::sin(t * half) * target[i]) / denominator;
      normalize(s.orientation);
      Q delta = multiply(s.orientation, conjugate(previous));
      if (delta[0] < 0)
        for (auto& x : delta) x = -x;
      const double vector = std::hypot(delta[1], std::hypot(delta[2], delta[3]));
      const double dt = double(stamp - s.stamp) / 1e9;
      const double k = vector > 1e-10 ? 2 * std::atan2(vector, delta[0]) / (vector * dt) : 0;
      s.angular_velocity = {delta[1] * k, delta[2] * k, delta[3] * k};
    } else {
      s.angular_velocity = {};
    }
  }
  s.stamp = stamp;
  s.sequence = frame.sequence;
  s.reset = frame.reset_counter;
  s.origin = origin;
  const Q correction = multiply(s.orientation, conjugate(raw));
  position(h.controller_px, h.controller_py, h.controller_pz, correction, pivot);
  position(h.palm_px, h.palm_py, h.palm_pz, correction, pivot);
  position(h.wrist_px, h.wrist_py, h.wrist_pz, correction, pivot);
  orientation(h.controller_qw, h.controller_qx, h.controller_qy, h.controller_qz, correction);
  orientation(h.palm_qw, h.palm_qx, h.palm_qy, h.palm_qz, correction);
  orientation(h.wrist_qw, h.wrist_qx, h.wrist_qy, h.wrist_qz, correction);
  for (uint32_t i = 0; i < std::min(h.joint_count, xr_runtime::HAND_JOINT_COUNT_V2); ++i) {
    auto& j = h.joints[i];
    if (!j.flags || !std::isfinite(j.confidence) || j.confidence < c.min_confidence) continue;
    position(j.px, j.py, j.pz, correction, pivot);
    orientation(j.qw, j.qx, j.qy, j.qz, correction);
  }
  if (h.flags & xr_runtime::HAND_ANGULAR_VELOCITY_VALID) {
    h.wx = float(s.angular_velocity[0]);
    h.wy = float(s.angular_velocity[1]);
    h.wz = float(s.angular_velocity[2]);
  }
}
void WristPoseFilter::apply(xr_runtime::HandTrackingFrameF32V2& f, const Config& c, uint64_t now,
                            uint64_t origin, bool left, bool right) {
  side(f.left, left_, f, c, now, origin, left && c.left.enabled);
  side(f.right, right_, f, c, now, origin, right && c.right.enabled);
}
}  // namespace xr_runtime_adapter::emulation
