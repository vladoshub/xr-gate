#include "emulation_controller.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <xr_runtime/input/controller_input_mapping.hpp>

#include "gestures.hpp"
#include "override_controller.hpp"

namespace {
namespace em = xr_runtime_adapter::emulation;
using namespace xr_runtime;
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
HandSideF32V2 pose(unsigned extended = 0, int thumb = 1, bool left = false) {
  HandSideF32V2 h{};
  h.handedness = left ? 1 : 2;
  h.status = 1;
  h.flags = 9;
  h.confidence = 1;
  h.joint_count = 21;
  for (unsigned i = 0; i < 21; ++i) {
    h.joints[i].joint_id = i;
    h.joints[i].flags = 2;
    h.joints[i].confidence = 1;
    h.joints[i].qw = 1;
  }
  const auto put = [&](unsigned i, float x, float y, float z) {
    h.joints[i].px = x;
    h.joints[i].py = y;
    h.joints[i].pz = z;
  };
  put(1, .025, .015, 0);
  put(2, .035, .025, 0);
  put(3, .06, .032, .002);
  put(4, .115, .04, .005);
  const float xs[] = {.04, 0, -.02, -.04}, ys[] = {.07, .08, .075, .065};
  for (unsigned f = 0; f < 4; ++f) {
    for (unsigned k = 0; k < 4; ++k) put(5 + 4 * f + k, xs[f], ys[f] + k * .02f, 0);
    if (!(extended & (1u << f))) {
      put(6 + 4 * f, xs[f], ys[f] + .02, 0);
      put(7 + 4 * f, xs[f], ys[f] + .015, -.025);
      put(8 + 4 * f, xs[f], ys[f], -.025);
    }
  }
  if (thumb == 0) put(4, .005, .045, .005);
  if (thumb == 2) {
    put(6, .04, .10, 0);
    put(7, .04, .115, -.02598);
    put(8, .04, .109795, -.05553);
    put(4, h.joints[8].px + .006f, h.joints[8].py, h.joints[8].pz);
  }
  if (left)
    for (auto& j : h.joints) j.px = -j.px;
  for (auto& j : h.joints) {
    const float y = j.py, z = j.pz;
    j.py = float((y + z) / std::sqrt(2));
    j.pz = float((z - y) / std::sqrt(2));
  }
  return h;
}
void tilt(HandSideF32V2& h, double x_angle, double y_angle) {
  for (auto& j : h.joints) {
    const double x = j.px, y = j.py, z = j.pz;
    const double x1 = x * std::cos(y_angle) + y * std::sin(y_angle),
                 y1 = -x * std::sin(y_angle) + y * std::cos(y_angle);
    j.px = float(x1);
    j.py = float(y1 * std::cos(x_angle) + z * std::sin(x_angle));
    j.pz = float(-y1 * std::sin(x_angle) + z * std::cos(x_angle));
  }
}
void yaw(HandSideF32V2& h, double angle) {
  for (auto& joint : h.joints) {
    const double x = joint.px, z = joint.pz;
    joint.px = float(x * std::cos(angle) + z * std::sin(angle));
    joint.pz = float(-x * std::sin(angle) + z * std::cos(angle));
  }
}
void bend_finger(HandSideF32V2& h, unsigned finger, const std::array<double, 3>& degrees) {
  auto point = h.joints[5 + 4 * finger];
  const double lengths[] = {.025, .020, .017};
  for (unsigned k = 0; k < 3; ++k) {
    const double angle = degrees[k] * std::acos(-1) / 180;
    point.py += float(lengths[k] * (std::cos(angle) - std::sin(angle)) / std::sqrt(2));
    point.pz -= float(lengths[k] * (std::sin(angle) + std::cos(angle)) / std::sqrt(2));
    auto& joint = h.joints[6 + 4 * finger + k];
    joint.px = point.px;
    joint.py = point.py;
    joint.pz = point.pz;
  }
}
HandTrackingFrameF32V2 frame() {
  HandTrackingFrameF32V2 f{};
  f.version = 2;
  f.size_bytes = sizeof(f);
  f.sequence = 1;
  f.timestamp_ns = f.source_timestamp_ns = 1000000000;
  f.left = pose(0, 1, true);
  f.right = pose();
  return f;
}
void advance(HandTrackingFrameF32V2& f, uint64_t ms = 30) {
  ++f.sequence;
  f.timestamp_ns += ms * 1000000;
  f.source_timestamp_ns = f.timestamp_ns;
}
RuntimeControllerStateFrameV1 apply(em::Controller& c, const HandTrackingFrameF32V2& f,
                                    uint64_t now = 0) {
  RuntimeControllerStateFrameV1 out{};
  c.apply(out, &f, now ? now : f.timestamp_ns);
  return out;
}
em::Config immediate() {
  return em::config_from_json(
      {{"smoothing_ms", 0}, {"button_confirm_ms", 0}, {"pose_confirm_ms", 0}});
}
void forward_backward_default_and_inversion() {
  for (bool left : {false, true}) {
    const auto neutral_pose = [&] {
      auto h = pose(0, 1, left);
      yaw(h, .9);
      return h;
    };
    for (bool inverted : {false, true}) {
      auto cfg = em::config_from_json(
          {{"smoothing_ms", 0}, {left ? "left" : "right", {{"invert_y", inverted}}}});
      em::Controller c(cfg);
      auto f = frame();
      auto& hand = left ? f.left : f.right;
      hand = neutral_pose();
      apply(c, f);
      for (double pitch : {.65, -.65}) {
        hand = pose(0, 1, left);
        tilt(hand, pitch, 0);
        yaw(hand, .9);
        advance(f);
        const auto out = apply(c, f);
        const auto& side = left ? out.left : out.right;
        const bool forward = (pitch > 0) != inverted;
        check(forward ? side.thumbstick_y > .9 : side.thumbstick_y < -.9,
              "forward/back wrist pitch has wrong default/inverted Y sign");
        check(
            (side.buttons & (forward ? CONTROLLER_BUTTON_DPAD_UP : CONTROLLER_BUTTON_DPAD_DOWN)) &&
                !(side.buttons &
                  (forward ? CONTROLLER_BUTTON_DPAD_DOWN : CONTROLLER_BUTTON_DPAD_UP)),
            "wrist Y axis and D-pad direction disagree");
      }
      hand = pose(0, 1, left);
      yaw(hand, -1.3);
      advance(f);
      const auto yawed = apply(c, f);
      const auto& side = left ? yawed.left : yawed.right;
      check(std::abs(side.thumbstick_x) < 1e-5 && std::abs(side.thumbstick_y) < 1e-5,
            "world yaw changed the neutral stick position");
    }
  }
}
void earlier_forward_activation_preserves_other_directions() {
  for (bool left : {false, true}) {
    auto normal_config = immediate();
    auto easier_config = em::config_from_json(
        {{"smoothing_ms", 0}, {"button_confirm_ms", 0}, {"forward_range_scale", .65}});
    em::Controller normal(normal_config), easier(easier_config);
    auto f = frame();
    apply(normal, f);
    apply(easier, f);
    bool earlier_press = false;
    for (int step = 0; step <= 60; ++step) {
      auto& h = left ? f.left : f.right;
      h = pose(0, 1, left);
      tilt(h, step * .01, 0);
      advance(f);
      const auto a = apply(normal, f), b = apply(easier, f);
      const auto& old_side = left ? a.left : a.right;
      const auto& new_side = left ? b.left : b.right;
      if (old_side.thumbstick_y == 0)
        check(new_side.thumbstick_y == 0, "forward sensitivity shrank the calibrated neutral");
      if ((new_side.buttons & CONTROLLER_BUTTON_DPAD_UP) &&
          !(old_side.buttons & CONTROLLER_BUTTON_DPAD_UP)) {
        check(old_side.thumbstick_y >= .65 * .65 - 1e-5 && old_side.thumbstick_y < .65,
              "forward digital activation did not use the reduced working range");
        earlier_press = true;
      }
    }
    check(earlier_press, "forward D-pad still requires the old wrist tilt");
    // At the edge of the radial deadzone, the existing tiny-axis cutoff must
    // still suppress optical noise before increasing forward sensitivity.
    for (double pitch : {.09035, .0904, .09045}) {
      auto& h = left ? f.left : f.right;
      h = pose(0, 1, left);
      tilt(h, pitch, 0);
      advance(f);
      const auto a = apply(normal, f), b = apply(easier, f);
      check((left ? a.left : a.right).thumbstick_y == 0 &&
                (left ? b.left : b.right).thumbstick_y == 0,
            "forward gain revived subthreshold neutral jitter");
    }
    for (const auto angles : {std::array<double, 2>{-.5, 0}, {0, -.5}, {0, .5}}) {
      auto& h = left ? f.left : f.right;
      h = pose(0, 1, left);
      tilt(h, angles[0], angles[1]);
      advance(f);
      const auto a = apply(normal, f), b = apply(easier, f);
      const auto& old_side = left ? a.left : a.right;
      const auto& new_side = left ? b.left : b.right;
      check(old_side.thumbstick_x == new_side.thumbstick_x &&
                old_side.thumbstick_y == new_side.thumbstick_y &&
                old_side.buttons == new_side.buttons,
            "forward sensitivity changed back/left/right movement");
    }
    auto& h = left ? f.left : f.right;
    h = pose(0, 0, left);
    tilt(h, .5, 0);
    advance(f);
    const auto out = apply(easier, f);
    const auto& side = left ? out.left : out.right;
    check(side.thumbstick_x == 0 && side.thumbstick_y == 0 && side.buttons == 0,
          "tucked thumb no longer disables the sensitive forward direction");
  }
}
void grip_with_natural_finger_bends() {
  for (bool left : {false, true}) {
    em::Controller c(immediate());
    auto f = frame();
    apply(c, f);
    auto& hand = left ? f.left : f.right;
    for (const std::array<double, 3>& folded :
         {std::array<double, 3>{35, 75, 125}, std::array<double, 3>{80, 80, 80}}) {
      hand = pose(8, 1, left);
      bend_finger(hand, 1, folded);
      bend_finger(hand, 2, folded);
      bend_finger(hand, 3, {5, 15, 25});
      advance(f);
      auto out = apply(c, f);
      const auto side = left ? out.left : out.right;
      check((side.buttons & 3) == CONTROLLER_BUTTON_GRIP && side.grip > .9,
            "extended little finger failed Grip with naturally bent middle/ring");
      // Folding the little finger at its knuckle must release, even when its
      // own three phalanges remain straight (a long MCP-to-tip chord).
      bend_finger(hand, 3, {85, 85, 85});
      advance(f);
      out = apply(c, f);
      check(((left ? out.left : out.right).buttons & CONTROLLER_BUTTON_GRIP) == 0,
            "knuckle-folded little finger retained Grip");
      hand = pose(9, 1, left);
      bend_finger(hand, 1, folded);
      bend_finger(hand, 2, folded);
      advance(f);
      out = apply(c, f);
      check(((left ? out.left : out.right).buttons & 3) == 3,
            "natural finger bends prevent simultaneous trigger/grip");
    }
    // Spreading an otherwise open little finger in the palm plane is not curl.
    hand = pose(15, 1, left);
    for (unsigned k = 1; k < 4; ++k) hand.joints[17 + k].px += (left ? 1 : -1) * k * .012f;
    const auto measured = em::measure_hand(hand, .3);
    check(measured.curl[3] < .3, "open little-finger splay was mistaken for flexion");
  }
}
void trigger_and_v_across_wrist_orientations() {
  for (bool left : {false, true}) {
    for (double pitch : {-1.2, -.6, 0., .6, 1.2}) {
      for (double roll : {-.8, 0., .8}) {
        for (double turn : {-2., 0., 2.}) {
          em::Controller c(immediate());
          auto f = frame();
          auto& hand = left ? f.left : f.right;
          const auto sample = [&](unsigned fingers, int thumb) {
            hand = pose(fingers, thumb, left);
            tilt(hand, pitch, roll);
            yaw(hand, turn);
            advance(f);
            const auto out = apply(c, f);
            return left ? out.left : out.right;
          };
          sample(0, 0);
          auto out = sample(1, 1);
          check((out.buttons & 3) == CONTROLLER_BUTTON_TRIGGER && out.trigger > .9,
                "rigid wrist rotation cancelled extended-index Trigger");
          for (int thumb : {0, 1}) {
            out = sample(3, thumb);
            check(out.buttons == CONTROLLER_BUTTON_THUMBSTICK && out.trigger == 0 &&
                      out.grip == 0 && out.thumbstick_x == 0 && out.thumbstick_y == 0,
                  "V click depends on thumb/wrist or leaks other channels");
          }
          out = sample(7, 0);
          check(out.buttons == 0, "retired three-finger click still active");
          out = sample(9, 1);
          check((out.buttons & 3) == 0, "leaving V bypassed gameplay neutral guard");
          sample(0, 0);
          out = sample(9, 1);
          check((out.buttons & 3) == 3, "closed neutral failed to rearm after V");
        }
      }
    }
  }
}
void calibrated_start_with_held_channels_and_hooked_grip() {
  for (bool left : {false, true}) {
    auto cfg = immediate();
    auto& hc = left ? cfg.left : cfg.right;
    const auto feature = [&](double pitch, double roll) {
      auto h = pose(0, 1, left);
      tilt(h, pitch, roll);
      const auto m = em::measure_hand(h, .3);
      return em::wrist_feature(m, m.palm_basis);
    };
    em::calibrate_axes(hc, feature(0, 0), feature(0, -.5), feature(0, .5), feature(.5, 0),
                       feature(-.5, 0));
    for (unsigned extended : {1u, 8u, 9u}) {
      em::Controller c(cfg);
      auto f = frame();
      auto& hand = left ? f.left : f.right;
      hand = pose(extended, 1, left);
      tilt(hand, .5, 0);
      const auto out = apply(c, f);
      const auto& side = left ? out.left : out.right;
      check(side.thumbstick_y > .9 &&
                (side.buttons & 3) == ((extended & 1 ? CONTROLLER_BUTTON_TRIGGER : 0) |
                                       (extended & 8 ? CONTROLLER_BUTTON_GRIP : 0)),
            "saved calibration required a fist before held Trigger/Grip could move");
    }
    em::Controller c(immediate());
    auto f = frame();
    apply(c, f);
    auto& hand = left ? f.left : f.right;
    hand = pose(8, 1, left);
    bend_finger(hand, 3, {10, 30, 100});
    advance(f);
    const auto out = apply(c, f);
    check(((left ? out.left : out.right).buttons & 3) == CONTROLLER_BUTTON_GRIP,
          "a hooked little tip prevented starting Grip with unfolded MCP/PIP");
  }
}
void hooked_fingertip_and_special_transition() {
  em::Controller c(immediate());
  auto f = frame();
  apply(c, f);
  f.right = pose(8);
  advance(f);
  check((apply(c, f).right.buttons & 3) == 2, "Grip setup failed");
  bend_finger(f.right, 3, {5, 10, 100});
  advance(f);
  check((apply(c, f).right.buttons & 3) == 2, "hooked little fingertip cancelled held Grip");
  bend_finger(f.right, 3, {80, 120, 160});
  advance(f);
  check((apply(c, f).right.buttons & 3) == 0, "folding little finger did not release Grip");
  f.right = pose(0, 2);
  advance(f);
  check(apply(c, f).right.buttons == CONTROLLER_BUTTON_A, "A setup failed");
  f.right = pose(14, 2);
  advance(f);
  auto out = apply(c, f);
  check(out.right.buttons == CONTROLLER_BUTTON_B && out.right.trigger == 0 && out.right.grip == 0,
        "confirmed A-to-B transition was locked behind gameplay rearm");
  f.right = pose(9);
  advance(f);
  check(apply(c, f).right.buttons == 0, "A/B transition bypassed gameplay neutral guard");
  f.right = pose(0, 0);
  advance(f);
  apply(c, f);
  f.right = pose(9);
  advance(f);
  check((apply(c, f).right.buttons & 3) == 3, "neutral did not rearm gameplay after B");
  f.right = pose(0, 0);
  f.right.joints[4] = f.right.joints[8];
  f.right.joints[4].joint_id = 4;
  advance(f);
  check(apply(c, f).right.buttons == 0, "closed fist touching thumb/index was mistaken for A");
}
void independent_channels_and_wrist() {
  em::Controller c(immediate());
  auto f = frame();
  apply(c, f);
  f.right = pose(9);
  tilt(f.right, 0, .65);
  advance(f);
  auto out = apply(c, f);
  check((out.right.buttons & 3) == 3 && out.right.trigger > .9 && out.right.grip > .9,
        "extended index/little must press trigger+grip together");
  check(out.right.thumbstick_x > .9 && (out.right.buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        "wrist tilt must steer raised-thumb D-pad");
  // The same wrist orientation with a different thumb direction must not steer.
  auto moved = pose(9);
  moved.joints[4].px = .11f;
  moved.joints[4].py = .055f;
  moved.joints[4].pz = .025f;
  tilt(moved, 0, .65);
  f.right = moved;
  advance(f);
  auto moved_out = apply(c, f);
  check(std::abs(out.right.thumbstick_x - moved_out.right.thumbstick_x) < 1e-5 &&
            std::abs(out.right.thumbstick_y - moved_out.right.thumbstick_y) < 1e-5,
        "thumb direction leaked into wrist axes");
  f.right = pose(9, 0);
  tilt(f.right, 0, .65);
  advance(f);
  out = apply(c, f);
  check(out.right.thumbstick_x == 0 && out.right.thumbstick_y == 0 &&
            (out.right.buttons & CONTROLLER_BUTTON_DPAD_MASK) == 0,
        "tucked thumb must stop D-pad immediately");
  check((out.right.buttons & 3) == 3, "tucked thumb must not release trigger/grip");
  f.right = pose(8, 0);
  advance(f);
  out = apply(c, f);
  check((out.right.buttons & 3) == 2, "curling index must release trigger only");
  f.right = pose(1, 0);
  advance(f);
  out = apply(c, f);
  check((out.right.buttons & 3) == 1, "curling little must release grip only");
  f.right = pose(0, 0);
  advance(f);
  out = apply(c, f);
  check(out.right.buttons == 0, "curled operating hand must be neutral");
  // Mirror-hand tilt is measured in the same direction for both hands.
  f.left = pose(9, 1, true);
  tilt(f.left, 0, .65);
  advance(f);
  out = apply(c, f);
  check(out.left.thumbstick_x > .9 && (out.left.buttons & 3) == 3,
        "left wrist/index/little mapping differs");
}
void no_thumb_steering_and_invalid_joints() {
  auto cfg = immediate();
  cfg.left.enabled = false;
  em::Controller c(cfg);
  auto f = frame();
  apply(c, f);
  f.right = pose(9);
  f.right.joints[4].py = .08f;
  f.right.joints[4].pz = .035f;
  advance(f);
  auto out = apply(c, f);
  check(out.right.thumbstick_x == 0 && out.right.thumbstick_y == 0,
        "moving only thumb must keep centred D-pad");
  f.right.joints[8].confidence = 0;
  advance(f);
  out = apply(c, f);
  check((out.right.buttons & 3) == 2, "invalid index tip must release trigger, not good grip");
  f.right = pose(9);
  f.right.joints[4].px = std::numeric_limits<float>::quiet_NaN();
  advance(f);
  out = apply(c, f);
  check((out.right.buttons & 3) == 3 && out.right.thumbstick_x == 0 && out.right.thumbstick_y == 0,
        "invalid thumb must not block independent trigger/grip");
  out = apply(c, f, f.timestamp_ns + 121000000);
  check(out.right.buttons == 0 && out.right.trigger == 0 && out.right.grip == 0,
        "frozen optical source retained input");
  f.right = pose(9);
  advance(f);
  out = apply(c, f, f.timestamp_ns - 500000);
  check((out.right.buttons & 3) == 3, "sub-millisecond source clock skew released valid input");
  out = apply(c, f, f.timestamp_ns - 10000000);
  check(out.right.buttons == 0, "far-future source timestamp was accepted");
  RuntimeControllerStateFrameV1 untouched{};
  std::memset(&untouched.left, 0x5a, sizeof(untouched.left));
  const auto original = untouched.left;
  c.apply(untouched, &f, f.timestamp_ns);
  check(std::memcmp(&untouched.left, &original, sizeof(original)) == 0,
        "disabled side was mutated");
}
void photo_poses_and_rearm() {
  for (bool left : {false, true}) {
    em::Controller c(immediate());
    auto f = frame();
    apply(c, f);
    const auto sample = [&](const HandSideF32V2& h) {
      (left ? f.left : f.right) = h;
      advance(f);
      auto out = apply(c, f);
      return left ? out.left : out.right;
    };
    for (const auto entry : {std::pair<unsigned, uint64_t>{0, CONTROLLER_BUTTON_A},
                             {14, CONTROLLER_BUTTON_B},
                             {3, CONTROLLER_BUTTON_THUMBSTICK}}) {
      sample(pose(0, 0, left));
      auto out = sample(pose(entry.first, entry.first == 3 ? 0 : 2, left));
      check(out.buttons == entry.second && out.trigger == 0 && out.grip == 0 &&
                out.thumbstick_x == 0 && out.thumbstick_y == 0,
            "A/B/pad click is not exclusive or wrong photo pose");
      ControllerInputMappingConfig mapping;
      const auto mapped = map_controller_input(&out, left, mapping);
      if (entry.second == CONTROLLER_BUTTON_THUMBSTICK)
        check(mapped.trackpad_click && mapped.stick_click && !mapped.trigger_click &&
                  !mapped.grip_click,
              "photo pad click differs from GearVR mapping");
      // Returning directly to extended trigger/grip after a special pose must not shoot/grab.
      out = sample(pose(9, 1, left));
      for (int k = 0; k < 8; ++k) out = sample(pose(9, 1, left));
      check(out.buttons == 0 && out.trigger == 0 && out.grip == 0,
            "special-pose release leaked trigger/grip");
      sample(pose(0, 0, left));
      out = sample(pose(9, 1, left));
      check((out.buttons & 3) == 3, "closed-finger neutral did not rearm channels");
    }
    sample(pose(0, 0, left));
    auto uncertain = pose(0, 2, left);
    uncertain.joints[16].confidence = 0;
    auto out = sample(uncertain);
    check(out.buttons == 0, "unknown ring finger created a false A/B");
  }
}
void two_hand_menu() {
  auto cfg = immediate();
  cfg.menu_hold_ms = 90;
  em::Controller c(cfg);
  auto f = frame();
  apply(c, f);
  f.left = pose(15, 1, true);
  for (int i = 0; i < 8; ++i) {
    advance(f);
    auto out = apply(c, f);
    check(!(out.left.buttons & CONTROLLER_BUTTON_MENU) &&
              !(out.right.buttons & CONTROLLER_BUTTON_MENU),
          "one open palm fired Menu");
  }
  f.right = pose(15, 1);
  advance(f);
  auto out = apply(c, f);
  check(out.left.buttons == 0 && out.right.buttons == 0, "Menu fired before two-hand confirmation");
  auto duplicate = apply(c, f, f.timestamp_ns + 100000000);
  check(duplicate.left.buttons == 0 && duplicate.right.buttons == 0,
        "repeat runtime tick advanced Menu confirmation");
  advance(f, 100);
  out = apply(c, f);
  check(out.left.buttons == CONTROLLER_BUTTON_MENU && out.right.buttons == 0,
        "two open palms must create exactly one Menu");
  check(
      out.left.trigger == 0 && out.left.grip == 0 && out.right.trigger == 0 && out.right.grip == 0,
      "Menu leaked trigger/grip");
  for (int i = 0; i < 5; ++i) {
    advance(f);
    out = apply(c, f);
  }
  check(out.left.press_counters[2] == 1, "held Menu emitted repeated presses");
  f.right = pose(9);
  advance(f);
  out = apply(c, f);
  check(out.left.buttons == 0 && out.right.buttons == 0 && out.left.release_counters[2] == 1,
        "breaking Menu chord did not release or leaked gameplay");
  f.left = pose(0, 0, true);
  f.right = pose(0, 0);
  advance(f);
  apply(c, f);
  f.right = pose(9);
  advance(f);
  out = apply(c, f);
  check((out.right.buttons & 3) == 3, "neutral after Menu did not restore controller");
  check(!(out.left.buttons & CONTROLLER_BUTTON_SYSTEM) &&
            !(out.right.buttons & CONTROLLER_BUTTON_SYSTEM),
        "obsolete one-hand Home leaked");
  // Missing/low-confidence second hand may never supply the two-hand chord.
  f.left = pose(15, 1, true);
  f.right = pose(15);
  f.right.status = 2;
  for (int i = 0; i < 8; ++i) {
    advance(f);
    out = apply(c, f);
    check(!(out.left.buttons & CONTROLLER_BUTTON_MENU), "lost second hand fired Menu");
  }
}
void source_reset_and_confirmation() {
  auto cfg = immediate();
  cfg.button_confirm_ms = 45;
  em::Controller c(cfg);
  auto f = frame();
  f.right = pose(9);
  auto out = apply(c, f);
  check((out.right.buttons & 3) == 0, "extension clicked before confirmation");
  out = apply(c, f, f.timestamp_ns + 60000000);
  check((out.right.buttons & 3) == 0, "duplicate optical sample confirmed extension");
  advance(f, 60);
  out = apply(c, f);
  check((out.right.buttons & 3) == 3, "confirmed extension did not press");
  ++f.reset_counter;
  advance(f);
  out = apply(c, f);
  check(out.right.buttons == 0, "source reset retained press");
  advance(f, 60);
  out = apply(c, f);
  check(out.right.press_counters[0] == 2 && out.right.press_counters[1] == 2,
        "reset broke edge counters");
  f.right.status = 2;
  advance(f);
  out = apply(c, f);
  check(out.right.buttons == 0 && out.right.release_counters[0] == 2,
        "loss did not release held buttons");
  auto delay = immediate();
  delay.pose_confirm_ms = 120;
  em::Controller special(delay);
  f = frame();
  apply(special, f);
  f.right = pose(0, 2);
  advance(f);
  out = apply(special, f);
  check(out.right.buttons == 0, "A fired before pose confirmation");
  advance(f, 120);
  out = apply(special, f);
  check(out.right.buttons == CONTROLLER_BUTTON_A, "confirmed A did not fire");
}
void physical_controller_merge() {
  namespace oc = xr_runtime_adapter::override_controller;
  for (bool left : {false, true}) {
    em::Controller c(immediate());
    auto f = frame();
    oc::RuntimeControllerSynthesisConfig cfg;
    cfg.left_hand_gestures_enabled = cfg.right_hand_gestures_enabled = false;
    ControllerInputV3 input;
    auto& physical = left ? input.left : input.right;
    physical.status = CONTROLLER_INPUT_ACTIVE;
    physical.flags = CONTROLLER_DEVICE_BUTTONS_VALID | CONTROLLER_DEVICE_ANALOG_VALID;
    physical.stable_device_hash = 42;
    const auto step = [&](bool connected = true) {
      advance(f);
      auto out = oc::compose_runtime_controller_state(
          f.sequence, f.timestamp_ns, cfg, f, std::nullopt,
          connected ? std::optional<ControllerInputV3>(input) : std::nullopt, std::nullopt, 9.81f);
      const auto original = left ? out.left : out.right;
      c.apply(out, &f, f.timestamp_ns);
      auto merged = left ? out.left : out.right;
      check(std::memcmp(merged.position, original.position, sizeof(merged.position)) == 0 &&
                std::memcmp(merged.orientation_xyzw, original.orientation_xyzw,
                            sizeof(merged.orientation_xyzw)) == 0,
            "input merge changed controller pose");
      return merged;
    };
    step();
    auto& hand = left ? f.left : f.right;
    hand = pose(9, 1, left);
    tilt(hand, 0, .65);
    physical.buttons = CONTROLLER_BUTTON_B;
    physical.press_counters[4] = 1;
    auto out = step();
    check((out.buttons & (CONTROLLER_BUTTON_B | 3)) == (CONTROLLER_BUTTON_B | 3) &&
              out.thumbstick_x > .9,
          "physical button must combine with trigger/grip/wrist axes");
    check((out.source_mask & RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT) != 0 &&
              out.source_device_hash == 42 && out.press_counters[4] == 1,
          "physical source identity/press lost");
    physical.thumbstick_x = -.7f;
    physical.thumbstick_y = .2f;
    physical.touches = CONTROLLER_BUTTON_THUMBSTICK;
    out = step();
    check(std::abs(out.thumbstick_x + .7f) < 1e-6 && std::abs(out.thumbstick_y - .2f) < 1e-6 &&
              (out.buttons & CONTROLLER_BUTTON_DPAD_RIGHT) == 0,
          "physical pad must override conflicting wrist axes/directions");
    physical.thumbstick_x = physical.thumbstick_y = 0;
    out = step();
    check(out.thumbstick_x == 0 && out.thumbstick_y == 0 &&
              (out.buttons & CONTROLLER_BUTTON_DPAD_MASK) == 0,
          "physical touch at centre must centre wrist movement");
    physical.touches = 0;
    out = step();
    check(out.thumbstick_x > .9, "idle physical controller blocked wrist movement");
    // Both sources hold trigger: releasing either must not produce another edge.
    physical.buttons |= CONTROLLER_BUTTON_TRIGGER;
    physical.press_counters[0] = 1;
    out = step();
    check(out.press_counters[0] == 1 && out.release_counters[0] == 0,
          "overlapping physical trigger duplicated emulated press");
    hand = pose(0, 0, left);
    out = step();
    check((out.buttons & CONTROLLER_BUTTON_TRIGGER) && out.release_counters[0] == 0 &&
              out.trigger == 1,
          "gesture release cancelled held physical trigger");
    hand.status = 2;
    out = step();
    check((out.flags & RUNTIME_CONTROLLER_INPUT_VALID) &&
              (out.buttons & (CONTROLLER_BUTTON_B | CONTROLLER_BUTTON_TRIGGER)) ==
                  (CONTROLLER_BUTTON_B | CONTROLLER_BUTTON_TRIGGER),
          "lost hand disabled physical controller buttons");
    physical.buttons = 0;
    physical.release_counters[0] = physical.release_counters[4] = 1;
    out = step();
    check(out.buttons == 0 && out.release_counters[0] == 1 && out.release_counters[4] == 1,
          "physical release was lost or duplicated");
    physical.buttons = CONTROLLER_BUTTON_MENU;
    physical.press_counters[2] = 1;
    out = step();
    check(out.buttons == CONTROLLER_BUTTON_MENU, "physical Menu was blocked by two-hand rule");
    out = step(false);
    check(out.buttons == 0 && out.release_counters[2] == 1 &&
              (out.flags & RUNTIME_CONTROLLER_INPUT_VALID) == 0,
          "disconnected physical controller retained input");
  }
}
void physical_short_taps_and_counter_reset() {
  em::Controller c(immediate());
  auto f = frame();
  RuntimeControllerSideStateV1 physical;
  physical.flags = RUNTIME_CONTROLLER_CONNECTED | RUNTIME_CONTROLLER_INPUT_VALID;
  physical.source_mask = RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
  physical.source_device_hash = 42;
  physical.press_counters[3] = physical.release_counters[3] = 100;
  const auto step = [&] {
    advance(f);
    RuntimeControllerStateFrameV1 out;
    out.right = physical;
    c.apply(out, &f, f.timestamp_ns);
    return out.right;
  };
  auto out = step();
  check(out.press_counters[3] == 0, "historical physical taps replayed on first connection");
  ++physical.press_counters[3];
  ++physical.release_counters[3];
  out = step();
  check(out.buttons == 0 && out.press_counters[3] == 1 && out.release_counters[3] == 1 &&
            (out.changed_buttons & CONTROLLER_BUTTON_A),
        "short physical tap between runtime ticks was lost");
  out = step();
  check(out.changed_buttons == 0 && out.press_counters[3] == 1,
        "duplicate physical snapshot replayed a tap");
  physical.press_counters[3] = physical.release_counters[3] = 0;
  out = step();
  check(out.press_counters[3] == 1 && out.release_counters[3] == 1,
        "physical counter reset generated phantom taps");
  physical.source_device_hash = 43;
  physical.press_counters[3] = physical.release_counters[3] = 0xffffffffu;
  step();
  physical.press_counters[3] = physical.release_counters[3] = 0;
  out = step();
  check(out.press_counters[3] == 2 && out.release_counters[3] == 2,
        "physical uint32 counter wrap lost a tap");
}
void gesture_block_gate_keeps_physical_input() {
  auto cfg = immediate();
  cfg.button_confirm_ms = 45;
  em::Controller c(cfg);
  auto f = frame();
  f.right = pose(9);
  apply(c, f);
  advance(f, 60);
  check((apply(c, f).right.buttons & 3) == 3, "test trigger/grip not active before block");
  const auto step = [&](bool blocked) {
    RuntimeControllerStateFrameV1 out;
    out.right.flags = RUNTIME_CONTROLLER_CONNECTED | RUNTIME_CONTROLLER_INPUT_VALID;
    out.right.source_mask = RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
    out.right.buttons = CONTROLLER_BUTTON_B;
    c.apply(out, &f, f.timestamp_ns, 0, blocked);
    return out;
  };
  advance(f);
  auto out = step(true);
  check(out.right.buttons == CONTROLLER_BUTTON_B && out.right.trigger == 0 && out.right.grip == 0 &&
            out.right.release_counters[0] == 1 && out.right.release_counters[1] == 1 &&
            (out.flags & RUNTIME_CONTROLLER_FRAME_RIGHT_INPUT_VALID),
        "gesture block must immediately release emulation while keeping physical input valid");
  advance(f, 500);
  out = step(true);
  check(out.right.buttons == CONTROLLER_BUTTON_B && out.right.press_counters[4] == 1,
        "held physical button repeated while gestures blocked");
  advance(f);
  out = step(false);
  check(out.right.buttons == CONTROLLER_BUTTON_B, "unblocking reused stale gesture confirmation");
  advance(f, 60);
  out = step(false);
  check((out.right.buttons & 3) == 3 && out.right.press_counters[0] == 2,
        "new gestures did not resume after unblocking and fresh confirmation");
}
void validation_and_legacy() {
  for (const auto& j :
       {nlohmann::json{{"version", 1}}, nlohmann::json{{"finger_mode", "count"}},
        nlohmann::json{{"deadzone", 1}}, nlohmann::json{{"extension_on", .3}},
        nlohmann::json{{"forward_range_scale", 0}}, nlohmann::json{{"forward_range_scale", 2.1}}}) {
    bool rejected = false;
    try {
      (void)em::config_from_json(j);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "invalid/obsolete profile accepted");
  }
  em::HandConfig hc;
  em::calibrate_axes(hc, {0, 0, 0}, {-.5, 0, 0}, {.5, 0, 0}, {0, .5, 0}, {0, -.5, 0});
  bool rejected = false;
  try {
    em::calibrate_axes(hc, {0, 0, 0}, {-.5, 0, 0}, {.5, 0, 0}, {.5, 0, 0}, {-.5, 0, 0});
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "collinear wrist calibration accepted");
  auto f = frame();
  f.right.palm_py = .04f;
  f.right.joints[4] = f.right.joints[8];
  f.right.joints[4].joint_id = 4;
  xr_runtime_adapter::gestures::RuntimeGestureHysteresisState state;
  xr_runtime_adapter::gestures::derive_missing_runtime_hand_v2_gestures(f, .7f, .7f, .35f, .35f, 0,
                                                                        0, state);
  check(f.right.grab_active && !f.right.pinch_active, "legacy gestures changed");
}
}  // namespace
int main() {
  try {
    forward_backward_default_and_inversion();
    earlier_forward_activation_preserves_other_directions();
    grip_with_natural_finger_bends();
    trigger_and_v_across_wrist_orientations();
    calibrated_start_with_held_channels_and_hooked_grip();
    hooked_fingertip_and_special_transition();
    independent_channels_and_wrist();
    no_thumb_steering_and_invalid_joints();
    photo_poses_and_rearm();
    two_hand_menu();
    source_reset_and_confirmation();
    physical_controller_merge();
    physical_short_taps_and_counter_reset();
    gesture_block_gate_keeps_physical_input();
    validation_and_legacy();
    std::cout << "Wrist tilt, extension channels, photo poses and two-hand Menu passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
