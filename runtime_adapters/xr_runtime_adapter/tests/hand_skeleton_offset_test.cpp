#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "coordinate_util.hpp"

namespace {
namespace cu = xr_runtime_adapter::coordinate_util;
using namespace xr_runtime;
constexpr double pi = 3.14159265358979323846;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
  check(std::abs(actual - expected) < 2e-5, message);
}
template <class T>
void set_q(T& w, T& x, T& y, T& z, const cu::Qd& q) {
  w = T(q.w);
  x = T(q.x);
  y = T(q.y);
  z = T(q.z);
}
void same_q(const cu::Qd& actual, const cu::Qd& expected) {
  const double dot =
      actual.w * expected.w + actual.x * expected.x + actual.y * expected.y + actual.z * expected.z;
  near(std::abs(dot), 1, "bone/controller rotation diverged");
}
cu::V3d point(const cu::V3d& before, const cu::V3d& pivot, const cu::Qd& q) {
  const auto delta = cu::q_rotate(q, {before.x - pivot.x, before.y - pivot.y, before.z - pivot.z});
  return {pivot.x + delta.x, pivot.y + delta.y, pivot.z + delta.z};
}
template <class Joint>
void check_joint(const Joint& before, const Joint& after, const cu::V3d& pivot, const cu::Qd& q) {
  const auto p = point({before.px, before.py, before.pz}, pivot, q);
  near(after.px, p.x, "joint X did not rotate rigidly about the controller");
  near(after.py, p.y, "joint Y did not rotate rigidly about the controller");
  near(after.pz, p.z, "joint Z did not rotate rigidly about the controller");
  same_q({after.qw, after.qx, after.qy, after.qz},
         cu::q_mul(q, {before.qw, before.qx, before.qy, before.qz}));
  check(after.flags == before.flags && after.radius_m == before.radius_m,
        "offset modified bone metadata");
}
HandTrackingFrameF32V2 raw_frame(double pitch) {
  HandTrackingFrameF32V2 f{};
  f.sequence = 1;
  f.flags = HAND_FLAG_LEFT_VALID | HAND_FLAG_RIGHT_VALID;
  for (auto* h : {&f.left, &f.right}) {
    const double sign = h == &f.left ? 1 : -1;
    h->status = 1;
    h->flags = HAND_POSE_VALID | HAND_JOINTS_VALID | HAND_LINEAR_VELOCITY_VALID |
               HAND_ANGULAR_VELOCITY_VALID;
    h->controller_px = float(sign);
    h->controller_py = 2;
    h->controller_pz = 3;
    h->palm_px = float(sign + .02);
    h->palm_py = 2.03f;
    h->palm_pz = 2.97f;
    h->wrist_px = float(sign - .03);
    h->wrist_py = 1.97f;
    h->wrist_pz = 3.02f;
    const auto q =
        cu::q_mul(cu::q_from_axis_angle(0, 1, 0, sign * .4), cu::q_from_axis_angle(1, 0, 0, pitch));
    const auto palm = cu::q_mul(q, cu::q_from_axis_angle(0, 0, 1, .2));
    const auto wrist = cu::q_mul(q, cu::q_from_axis_angle(1, 0, 0, -.3));
    set_q(h->controller_qw, h->controller_qx, h->controller_qy, h->controller_qz, q);
    set_q(h->palm_qw, h->palm_qx, h->palm_qy, h->palm_qz, palm);
    set_q(h->wrist_qw, h->wrist_qx, h->wrist_qy, h->wrist_qz, wrist);
    h->vx = 1;
    h->vy = 2;
    h->vz = 3;
    h->wx = -.3f;
    h->wy = .2f;
    h->wz = .4f;
    h->joint_count = 21;
    for (unsigned i = 0; i < 21; ++i) {
      auto& j = h->joints[i];
      j.joint_id = i;
      j.flags = 2;
      j.confidence = 1;
      j.radius_m = .008f;
      j.px = float(sign + .003 * i);
      j.py = float(2 + .004 * (i % 4));
      j.pz = float(3 - .005 * i);
      set_q(j.qw, j.qx, j.qy, j.qz, cu::q_mul(q, cu::q_from_axis_angle(0, 0, 1, .03 * i)));
    }
  }
  return f;
}
cu::StreamTransformConfig config(bool pre, bool joints) {
  cu::StreamTransformConfig cfg;
  cfg.coordinate_transform.scale = 1.2;
  cfg.coordinate_transform.offset_m = {.1, -.2, .3};
  auto& o = cfg.hand_orientation_offset;
  o.enabled = true;
  o.apply_to_joints = joints;
  o.pre_multiply = pre;
  o.left.enabled = o.right.enabled = true;
  o.left.q = cu::q_from_axis_angle(0, 0, 1, -pi / 3);
  o.right.q = cu::q_from_axis_angle(0, 0, 1, pi / 3);
  return cfg;
}
void rigid_frames() {
  for (bool pre : {false, true})
    for (bool joints : {false, true})
      for (bool deferred : {false, true})
        for (double pitch : {0., .5, 1.3}) {
          auto cfg = config(pre, joints);
          const auto raw = raw_frame(pitch);
          auto before = raw;
          cu::apply_hand_frame_transform(before, cfg, nullptr, nullptr, false, false);
          auto actual = raw;
          cu::apply_hand_frame_transform(actual, cfg, nullptr, nullptr, true, true, nullptr,
                                         deferred);
          if (deferred) {
            for (unsigned i = 0; i < 21; ++i)
              near(actual.left.joints[i].px, before.left.joints[i].px,
                   "deferred skeleton leaked into recognition");
            cu::apply_aligned_hand_skeleton_offset(actual, cfg);
          }
          for (bool left : {false, true}) {
            const auto& b = left ? before.left : before.right;
            const auto& a = left ? actual.left : actual.right;
            const auto mounting =
                left ? cfg.hand_orientation_offset.left.q : cfg.hand_orientation_offset.right.q;
            const cu::Qd base{b.controller_qw, b.controller_qx, b.controller_qy, b.controller_qz};
            const auto final = pre ? cu::q_mul(mounting, base) : cu::q_mul(base, mounting);
            const auto correction = joints ? cu::q_mul(final, cu::q_conj(base)) : cu::Qd{};
            const cu::V3d pivot{b.controller_px, b.controller_py, b.controller_pz};
            near(a.controller_px, b.controller_px, "offset moved controller X");
            near(a.controller_py, b.controller_py, "offset moved controller Y");
            near(a.controller_pz, b.controller_pz, "offset moved controller Z");
            same_q({a.controller_qw, a.controller_qx, a.controller_qy, a.controller_qz}, final);
            for (unsigned i = 0; i < 21; ++i)
              check_joint(b.joints[i], a.joints[i], pivot, correction);
            if (joints) {
              const auto palm = point({b.palm_px, b.palm_py, b.palm_pz}, pivot, correction);
              const auto wrist = point({b.wrist_px, b.wrist_py, b.wrist_pz}, pivot, correction);
              near(a.palm_px, palm.x, "palm did not follow skeleton");
              near(a.palm_py, palm.y, "palm Y did not follow skeleton");
              near(a.wrist_pz, wrist.z, "wrist did not follow skeleton");
              same_q({a.palm_qw, a.palm_qx, a.palm_qy, a.palm_qz},
                     cu::q_mul(correction, {b.palm_qw, b.palm_qx, b.palm_qy, b.palm_qz}));
              same_q({a.wrist_qw, a.wrist_qx, a.wrist_qy, a.wrist_qz},
                     cu::q_mul(correction, {b.wrist_qw, b.wrist_qx, b.wrist_qy, b.wrist_qz}));
            }
            const auto w = pre && joints ? cu::q_rotate(mounting, {b.wx, b.wy, b.wz})
                                         : cu::V3d{b.wx, b.wy, b.wz};
            near(a.wx, w.x, "world angular velocity X changed incorrectly");
            near(a.wy, w.y, "world angular velocity Y changed incorrectly");
            near(a.wz, w.z, "world angular velocity Z changed incorrectly");
            check(a.vx == b.vx && a.vy == b.vy && a.vz == b.vz,
                  "mounting offset rotated controller linear velocity");
          }
        }
}
void disabled_and_invalid() {
  auto cfg = config(false, true);
  auto before = raw_frame(.5);
  cu::apply_hand_frame_transform(before, cfg, nullptr, nullptr, false, false);
  auto skipped = before;
  cu::apply_aligned_hand_skeleton_offset(skipped, cfg, false, false);
  check(std::memcmp(&before, &skipped, sizeof(before)) == 0, "runtime/IMU gate ignored");
  cfg.hand_orientation_offset.left.enabled = false;
  auto actual = raw_frame(.5);
  cu::apply_hand_frame_transform(actual, cfg, nullptr, nullptr);
  check(std::memcmp(&before.left, &actual.left, sizeof(before.left)) == 0,
        "disabled side acquired a mounting offset");
  cfg.hand_orientation_offset.left.enabled = true;
  actual = raw_frame(.5);
  actual.left.joints[8].flags = 0;
  actual.left.joints[9].qw = actual.left.joints[9].qx = actual.left.joints[9].qy =
      actual.left.joints[9].qz = 0;
  auto unrotated = actual;
  cu::apply_hand_frame_transform(unrotated, cfg, nullptr, nullptr, false, false);
  actual.left.joint_count = 1000;
  cu::apply_hand_frame_transform(actual, cfg, nullptr, nullptr);
  near(actual.left.joints[8].px, unrotated.left.joints[8].px, "invalid joint was rotated");
  check(actual.left.joints[9].qw == 0 && actual.left.joints[9].qx == 0 &&
            actual.left.joints[9].qy == 0 && actual.left.joints[9].qz == 0,
        "invalid joint orientation was invented");
}
void filtered_offsets() {
  const auto filter = cu::q_from_axis_angle(1, 0, 0, .17);
  const auto rotate = [&](HandTrackingFrameF32V2& frame) {
    for (auto* h : {&frame.left, &frame.right}) {
      const cu::V3d pivot{h->controller_px, h->controller_py, h->controller_pz};
      const auto pose = [&](auto& x, auto& y, auto& z, auto& w, auto& qx, auto& qy, auto& qz) {
        const auto p = point({x, y, z}, pivot, filter);
        x = p.x;
        y = p.y;
        z = p.z;
        set_q(w, qx, qy, qz, cu::q_mul(filter, {w, qx, qy, qz}));
      };
      pose(h->controller_px, h->controller_py, h->controller_pz, h->controller_qw, h->controller_qx,
           h->controller_qy, h->controller_qz);
      pose(h->palm_px, h->palm_py, h->palm_pz, h->palm_qw, h->palm_qx, h->palm_qy, h->palm_qz);
      pose(h->wrist_px, h->wrist_py, h->wrist_pz, h->wrist_qw, h->wrist_qx, h->wrist_qy,
           h->wrist_qz);
      for (auto& j : h->joints) pose(j.px, j.py, j.pz, j.qw, j.qx, j.qy, j.qz);
    }
  };
  for (bool pre : {false, true}) {
    const auto cfg = config(pre, true);
    auto before = raw_frame(.5), after = before;
    cu::apply_hand_frame_transform(before, cfg, nullptr, nullptr, false, false);
    cu::apply_hand_frame_transform(after, cfg, nullptr, nullptr, true, true, nullptr, true);
    const std::array<cu::Qd, 2> reference{{{after.left.controller_qw, after.left.controller_qx,
                                            after.left.controller_qy, after.left.controller_qz},
                                           {after.right.controller_qw, after.right.controller_qx,
                                            after.right.controller_qy, after.right.controller_qz}}};
    rotate(before);
    rotate(after);
    cu::apply_aligned_hand_skeleton_offset(after, cfg, true, true, &reference);
    for (bool left : {false, true}) {
      const auto& b = left ? before.left : before.right;
      const auto& a = left ? after.left : after.right;
      const cu::Qd base{b.controller_qw, b.controller_qx, b.controller_qy, b.controller_qz};
      const auto expected_root = cu::q_mul(filter, reference[left ? 0 : 1]);
      same_q({a.controller_qw, a.controller_qx, a.controller_qy, a.controller_qz}, expected_root);
      const auto correction = cu::q_mul(expected_root, cu::q_conj(base));
      const cu::V3d pivot{b.controller_px, b.controller_py, b.controller_pz};
      for (unsigned i = 0; i < 21; ++i) check_joint(b.joints[i], a.joints[i], pivot, correction);
      same_q({a.palm_qw, a.palm_qx, a.palm_qy, a.palm_qz},
             cu::q_mul(correction, {b.palm_qw, b.palm_qx, b.palm_qy, b.palm_qz}));
    }
  }
}
void legacy_frame() {
  for (bool pre : {false, true}) {
    auto cfg = config(pre, true);
    HandTrackingFrameF64V1 raw{};
    auto& h = raw.left;
    h.status = 1;
    h.flags = HAND_POSE_VALID | HAND_JOINTS_VALID;
    h.palm_px = 1;
    h.palm_py = 2;
    h.palm_pz = 3;
    h.wrist_px = 1.1;
    h.wrist_py = 2.1;
    h.wrist_pz = 3.1;
    const auto root = cu::q_from_axis_angle(1, 0, 0, .7);
    set_q(h.palm_qw, h.palm_qx, h.palm_qy, h.palm_qz, root);
    set_q(h.wrist_qw, h.wrist_qx, h.wrist_qy, h.wrist_qz, root);
    h.joint_count = HAND_JOINT_COUNT_V1;
    for (auto& j : h.joints) {
      j.flags = 1;
      j.px = 1.2;
      j.py = 2.2;
      j.pz = 3.2;
      j.qw = 1;
    }
    auto before = raw;
    cu::apply_hand_frame_transform(before, cfg, nullptr, nullptr, false, false);
    auto after = raw;
    cu::apply_hand_frame_transform(after, cfg, nullptr, nullptr);
    const auto base =
        cu::Qd{before.left.palm_qw, before.left.palm_qx, before.left.palm_qy, before.left.palm_qz};
    const auto final = pre ? cu::q_mul(cfg.hand_orientation_offset.left.q, base)
                           : cu::q_mul(base, cfg.hand_orientation_offset.left.q);
    const auto correction = cu::q_mul(final, cu::q_conj(base));
    const cu::V3d pivot{before.left.palm_px, before.left.palm_py, before.left.palm_pz};
    for (unsigned i = 0; i < HAND_JOINT_COUNT_V1; ++i)
      check_joint(before.left.joints[i], after.left.joints[i], pivot, correction);
    near(after.left.palm_px, pivot.x, "legacy palm pivot moved");
    same_q({after.left.palm_qw, after.left.palm_qx, after.left.palm_qy, after.left.palm_qz}, final);
  }
}
}  // namespace
int main() {
  try {
    rigid_frames();
    disabled_and_invalid();
    filtered_offsets();
    legacy_frame();
    std::cout << "Rigid skeleton mounting offsets, deferred output and legacy frames passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
