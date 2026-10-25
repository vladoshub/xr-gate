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
HandSideF32V2 thumb_pose(bool left, double sweep, double bend, unsigned extended = 9,
                         bool hidden = false) {
  auto h = pose(extended, 1, left);
  const auto basis = em::measure_hand(h, .3).palm_basis;
  const auto& a = basis[0];
  const auto& f = basis[1];
  const auto& n = basis[2];
  const double angle = (left ? -.6435011088 : .6435011088) + sweep;
  em::Vec3 base{}, proximal{}, distal{};
  const auto& index = h.joints[5];
  for (size_t k = 0; k < 3; ++k) {
    base[k] = (k == 0   ? index.px
               : k == 1 ? index.py
                        : index.pz) +
              a[k] * (hidden ? 0
                      : left ? -.06
                             : .06) -
              f[k] * (hidden ? .01 : .025);
    proximal[k] = hidden ? f[k] : std::sin(angle) * a[k] + std::cos(angle) * f[k];
    distal[k] = hidden ? -f[k] : std::cos(bend) * proximal[k] - std::sin(bend) * n[k];
  }
  for (size_t i = 1; i <= 4; ++i) {
    em::Vec3 p = base;
    for (size_t k = 0; k < 3; ++k) {
      if (i == 1) p[k] -= .02 * f[k];
      if (i >= 3) p[k] += (hidden ? .005 : .035) * proximal[k];
      if (i == 4) p[k] += .025 * distal[k];
    }
    h.joints[i].px = p[0];
    h.joints[i].py = p[1];
    h.joints[i].pz = p[2];
  }
  return h;
}
em::Config thumb_config(bool left) {
  const auto feature = [&](double x, double y) {
    const auto m = em::measure_hand(thumb_pose(left, x, y), .3);
    check(m.thumb_pad_valid, "synthetic thumb feature invalid");
    return m.thumb_feature;
  };
  em::HandConfig axes;
  em::calibrate_axes(axes, feature(0, .25), feature(-.5, .25), feature(.5, .25), feature(0, 1),
                     feature(0, -.2));
  nlohmann::json directions;
  const char* names[] = {"left", "right", "up", "down"};
  for (size_t i = 0; i < 4; ++i) directions[names[i]] = axes.directions[i];
  nlohmann::json j = {{"smoothing_ms", 0},
                      {"button_confirm_ms", 0},
                      {"pose_confirm_ms", 0},
                      {"trigger_on_curl", true},
                      {"grip_on_curl", true},
                      {"dpad_source", "thumb"},
                      {left ? "right" : "left", {{"enabled", false}}},
                      {left ? "left" : "right",
                       {{"thumb_calibration",
                         {{"feature", "thumb_pose_v1"},
                          {"neutral", axes.neutral},
                          {"axis_x", axes.axis_x},
                          {"axis_y", axes.axis_y},
                          {"directions", directions},
                          {"enable_clearance", .7},
                          {"disable_clearance", .6}}}}}};
  return em::config_from_json(j);
}
void thumb_dpad_and_independent_channels() {
  constexpr uint64_t up = CONTROLLER_BUTTON_DPAD_UP, down = CONTROLLER_BUTTON_DPAD_DOWN,
                     left_bit = CONTROLLER_BUTTON_DPAD_LEFT,
                     right_bit = CONTROLLER_BUTTON_DPAD_RIGHT;
  struct Target {
    double x, y;
    uint64_t buttons;
  };
  for (bool left : {false, true}) {
    const auto cfg = thumb_config(left);
    for (unsigned extended : {0u, 1u, 8u, 9u}) {
      for (const auto target : {Target{0, 1, up},
                                {0, -.2, down},
                                {-.5, .25, left_bit},
                                {.5, .25, right_bit},
                                {-.5, 1, up | left_bit},
                                {.5, 1, up | right_bit},
                                {-.5, -.2, down | left_bit},
                                {.5, -.2, down | right_bit}}) {
        em::Controller c(cfg);
        auto f = frame();
        auto h = thumb_pose(left, target.x, target.y, extended);
        (left ? f.left : f.right) = h;
        auto output = apply(c, f);
        const auto side = left ? output.left : output.right;
        const uint64_t channels = (extended & 1 ? 0 : 1) | (extended & 8 ? 0 : 2);
        check(side.buttons == (target.buttons | channels),
              "thumb cardinal/diagonal or Trigger/Grip conflict");
        tilt(h, .7, -.4);
        yaw(h, 1.2);
        (left ? f.left : f.right) = h;
        advance(f);
        output = apply(c, f);
        const auto rotated = left ? output.left : output.right;
        check(rotated.buttons == side.buttons &&
                  std::abs(rotated.thumbstick_x - side.thumbstick_x) < 1e-5 &&
                  std::abs(rotated.thumbstick_y - side.thumbstick_y) < 1e-5,
              "wrist rotation steered thumb-only D-pad");
        (left ? f.left : f.right) = thumb_pose(left, 0, .25, extended, true);
        advance(f);
        output = apply(c, f);
        const auto tucked = left ? output.left : output.right;
        check(tucked.buttons == channels && tucked.thumbstick_x == 0 && tucked.thumbstick_y == 0,
              "hidden thumb failed to disable only the D-pad");
        h = thumb_pose(left, target.x, target.y, extended);
        h.joints[4].flags = 0;
        (left ? f.left : f.right) = h;
        advance(f);
        output = apply(c, f);
        const auto lost = left ? output.left : output.right;
        check(lost.buttons == channels && lost.thumbstick_x == 0 && lost.thumbstick_y == 0,
              "missing thumb retained axes or cancelled independent buttons");
      }
    }
    em::Controller c(cfg);
    auto f = frame();
    const auto sample = [&](double fraction) {
      (left ? f.left : f.right) = thumb_pose(left, 0, .25 - fraction * .45);
      advance(f);
      const auto o = apply(c, f);
      return left ? o.left : o.right;
    };
    check(sample(.80).thumbstick_y == 0, "relaxed thumb accidentally moved backwards");
    check(sample(.90).thumbstick_y < -.65, "strong backward thumb pose did not activate");
    check(sample(.75).thumbstick_y < 0, "backward thumb hysteresis lost a held direction");
    check(sample(.65).thumbstick_y == 0,
          "backward analog input leaked after leaving strict region");
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
void trigger_calibration_and_wrist_diagonals() {
  for (bool left : {false, true}) {
    auto cfg = immediate();
    cfg.trigger_on_curl = cfg.grip_on_curl = true;
    auto h = pose(9, 1, left);
    bend_finger(h, 0, {10, 35, 60});
    const double curl = em::measure_hand(h, .3).curl[0];
    check(curl > .25 && curl < .65, "partial trigger fixture has changed");
    auto f = frame();
    (left ? f.left : f.right) = h;
    em::Controller original(cfg);
    auto out = apply(original, f);
    check(((left ? out.left : out.right).buttons & 1) == 0,
          "partial trigger fixture already presses the uncalibrated trigger");
    auto& side_cfg = left ? cfg.left : cfg.right;
    side_cfg.trigger_calibrated = true;
    side_cfg.trigger_open_curl = 0;
    side_cfg.trigger_closed_curl = curl / .9;
    em::Controller calibrated(cfg);
    out = apply(calibrated, f);
    check((left ? out.left : out.right).buttons == 1,
          "independent trigger calibration did not recognize a natural partial bend");
    (left ? f.left : f.right).joints[8].flags = 0;
    advance(f);
    out = apply(calibrated, f);
    check((left ? out.left : out.right).trigger == 0, "missing calibrated index retained trigger");
    cfg = immediate();
    cfg.dpad_mode = "eight_way";
    em::Controller pad(cfg);
    f = frame();
    apply(pad, f);
    uint64_t combinations = 0;
    for (int x = -8; x <= 8; ++x)
      for (int y = -8; y <= 8; ++y) {
        h = pose(0, 1, left);
        tilt(h, x * .1, y * .1);
        (left ? f.left : f.right) = h;
        advance(f);
        out = apply(pad, f);
        const uint64_t bits = (left ? out.left : out.right).buttons;
        if ((bits & 64) && (bits & 256)) combinations |= 1;
        if ((bits & 64) && (bits & 512)) combinations |= 2;
        if ((bits & 128) && (bits & 256)) combinations |= 4;
        if ((bits & 128) && (bits & 512)) combinations |= 8;
      }
    check(combinations == 15, "wrist D-pad lacks one or more diagonals");
  }
}
void middle_grip_three_channels_and_rings() {
  for (bool left : {false, true})
    for (bool thumb : {false, true})
      for (bool click_curl : {false, true}) {
        auto cfg = thumb ? thumb_config(left) : immediate();
        cfg.button_layout = "middle_grip";
        cfg.trigger_on_curl = cfg.grip_on_curl = true;
        cfg.thumbstick_click_on_curl = click_curl;
        const unsigned neutral = click_curl ? 11 : 3;
        for (unsigned state = 0; state < 8; ++state) {
          em::Controller c(cfg);
          auto f = frame();
          (left ? f.left : f.right) =
              thumb ? thumb_pose(left, 0, .25, neutral) : pose(neutral, 1, left);
          apply(c, f);
          const unsigned extended =
              (state & 1 ? 0 : 1) | (state & 2 ? 0 : 2) | (bool(state & 4) != click_curl ? 8 : 0);
          auto h = thumb ? thumb_pose(left, 0, 1, extended) : pose(extended, 1, left);
          if (!thumb) tilt(h, .5, 0);
          (left ? f.left : f.right) = h;
          advance(f);
          auto out = apply(c, f);
          auto side = left ? out.left : out.right;
          const uint64_t expected = (state & 3) | (state & 4 ? CONTROLLER_BUTTON_THUMBSTICK : 0);
          check((side.buttons & ~CONTROLLER_BUTTON_DPAD_MASK) == expected &&
                    (side.buttons & CONTROLLER_BUTTON_DPAD_UP),
                "middle Grip, little click and Trigger failed an independent D-pad combination");
          if (state == 7) {
            h.joints[12].flags = 0;
            (left ? f.left : f.right) = h;
            advance(f);
            out = apply(c, f);
            check(((left ? out.left : out.right).buttons & 35) == 33,
                  "missing middle finger cancelled independent Trigger/little click");
          }
          (left ? f.left : f.right) =
              thumb ? thumb_pose(left, 0, .25, extended, true) : pose(extended, 0, left);
          advance(f);
          out = apply(c, f);
          side = left ? out.left : out.right;
          check(side.buttons == expected && side.thumbstick_x == 0 && side.thumbstick_y == 0,
                "tucked thumb cancelled an independent click or Grip");
        }
        em::Controller c(cfg);
        auto f = frame();
        const auto sample = [&](const HandSideF32V2& h) {
          (left ? f.left : f.right) = h;
          advance(f);
          const auto out = apply(c, f);
          return left ? out.left : out.right;
        };
        auto side = sample(pose(3, 0, left));
        check(side.buttons == (click_curl ? CONTROLLER_BUTTON_THUMBSTICK : 0),
              "former V pose still invokes a special gesture");
        side = sample(pose(14, 2, left));
        check(side.buttons == CONTROLLER_BUTTON_A && side.trigger == 0 && side.grip == 0 &&
                  side.thumbstick_x == 0 && side.thumbstick_y == 0,
              "open ring did not exclusively press A");
        side = sample(pose(0, 2, left));
        check(side.buttons == CONTROLLER_BUTTON_B && side.trigger == 0 && side.grip == 0 &&
                  side.thumbstick_x == 0 && side.thumbstick_y == 0,
              "closed ring leaked Grip/click or failed B");
        side = sample(pose(0, 0, left));
        check(side.buttons == 0, "ring release bypassed the three-channel neutral guard");
        sample(pose(neutral, 0, left));
        side = sample(pose(click_curl ? 0 : 8, 0, left));
        check(side.buttons == 35, "all three channels failed after the released neutral");
      }
  auto cfg = immediate();
  cfg.button_layout = "middle_grip";
  cfg.trigger_on_curl = cfg.grip_on_curl = cfg.thumbstick_click_on_curl = true;
  cfg.pose_confirm_ms = 120;
  em::Controller c(cfg);
  auto f = frame();
  f.right = pose(0, 0);
  apply(c, f);
  f.right = pose(0, 2);
  advance(f);
  auto out = apply(c, f);
  check(out.right.buttons == 0, "unconfirmed ring leaked ordinary buttons");
  advance(f, 120);
  out = apply(c, f);
  check(out.right.buttons == 16, "ring confirmation failed");
  cfg = immediate();
  cfg.button_layout = "middle_grip";
  cfg.trigger_on_curl = cfg.grip_on_curl = true;
  for (bool curl : {false, true}) {
    cfg.thumbstick_click_on_curl = curl;
    em::Controller click(cfg);
    f = frame();
    for (bool extended : {false, true}) {
      f.right = pose(3 | (extended ? 8 : 0), 0);
      advance(f);
      out = apply(click, f);
      check(bool(out.right.buttons & 32) == (extended != curl),
            "little click activation direction failed");
    }
  }
  cfg.thumbstick_click_on_curl = true;
  cfg.right.grip_calibrated = true;
  cfg.right.grip_open_curl = 0;
  cfg.right.grip_calibration_finger = "little";
  auto h = pose(11, 0);
  bend_finger(h, 1, {10, 35, 60});
  const double middle = em::measure_hand(h, .3).curl[1];
  check(middle > .2 && middle < .65, "partial middle-finger fixture changed");
  cfg.right.grip_closed_curl = middle / .9;
  f = frame();
  f.right = h;
  em::Controller stale(cfg);
  out = apply(stale, f);
  check(out.right.grip == 0, "old little-finger calibration was applied to middle Grip");
  cfg.right.grip_calibration_finger = "middle";
  em::Controller fitted(cfg);
  out = apply(fitted, f);
  check(out.right.buttons == 2, "middle Grip calibration did not admit natural partial flexion");
  h = pose(11, 0);
  bend_finger(h, 3, {10, 35, 60});
  const double little = em::measure_hand(h, .3).curl[3];
  cfg.right.click_calibrated = true;
  cfg.right.click_open_curl = 0;
  cfg.right.click_closed_curl = little / .9;
  f.right = h;
  advance(f);
  em::Controller fitted_click(cfg);
  out = apply(fitted_click, f);
  check(out.right.buttons == 32, "separate little click calibration failed");
  cfg.menu_hold_ms = 0;
  em::Controller open_rings(cfg);
  f = frame();
  f.left = pose(14, 2, true);
  f.right = pose(14, 2);
  out = apply(open_rings, f);
  check(out.left.buttons == 8 && out.right.buttons == 8, "two A rings were confused with Menu");
  em::Controller menu(cfg);
  f = frame();
  f.left = pose(15, 1, true);
  f.right = pose(15, 1);
  out = apply(menu, f);
  advance(f);
  out = apply(menu, f);
  check(out.left.buttons == 4 && out.right.buttons == 0, "new button layout changed two-hand Menu");
}
void three_finger_click_is_separate() {
  for (bool left : {false, true})
    for (bool thumb : {false, true}) {
      auto cfg = thumb ? thumb_config(left) : immediate();
      cfg.button_layout = "middle_grip";
      cfg.thumbstick_click_gesture = "three_fingers";
      cfg.trigger_on_curl = cfg.grip_on_curl = true;
      // The former click finger may open/close independently of all four
      // Trigger/Grip combinations, with either D-pad source still working.
      for (unsigned state = 0; state < 4; ++state)
        for (bool little_open : {false, true}) {
          em::Controller c(cfg);
          auto f = frame();
          auto& hand = left ? f.left : f.right;
          hand = thumb ? thumb_pose(left, 0, .25, 3) : pose(3, 1, left);
          apply(c, f);
          const unsigned mask = (state & 1 ? 0 : 1) | (state & 2 ? 0 : 2) |
                                (little_open ? 8 : 0);
          hand = thumb ? thumb_pose(left, 0, 1, mask) : pose(mask, 1, left);
          if (!thumb) tilt(hand, .5, 0);
          advance(f);
          auto out = apply(c, f);
          auto side = left ? out.left : out.right;
          check((side.buttons & ~CONTROLLER_BUTTON_DPAD_MASK) == state &&
                    (side.buttons & CONTROLLER_BUTTON_DPAD_UP),
                "retired little click interferes with Trigger/Grip/D-pad");
          hand.joints[20].flags = 0;
          advance(f);
          out = apply(c, f);
          side = left ? out.left : out.right;
          check((side.buttons & ~CONTROLLER_BUTTON_DPAD_MASK) == state,
                "missing little tip cancelled independent Trigger/Grip");
        }
      for (bool little_open : {false, true}) {
        cfg.pose_confirm_ms = 120;
        em::Controller c(cfg);
        auto f = frame();
        auto& hand = left ? f.left : f.right;
        const auto sample = [&](const HandSideF32V2& h, uint64_t ms = 30) {
          hand = h;
          advance(f, ms);
          auto out = apply(c, f);
          return left ? out.left : out.right;
        };
        sample(pose(3, 0, left));
        // Start from both ordinary buttons held, then enter a separate click.
        sample(pose(0, 0, left));
        const unsigned click_mask = little_open ? 15 : 7;
        auto click = thumb ? thumb_pose(left, 0, 1, click_mask, true) : pose(click_mask, 0, left);
        if (!thumb) tilt(click, .6, .4);
        auto side = sample(click);
        check(side.buttons == 0 && side.trigger == 0 && side.grip == 0 &&
                  side.thumbstick_x == 0 && side.thumbstick_y == 0,
              "unconfirmed three-finger candidate leaked another channel");
        side = sample(click, 120);
        check(side.buttons == CONTROLLER_BUTTON_THUMBSTICK && side.trigger == 0 &&
                  side.grip == 0 && side.thumbstick_x == 0 && side.thumbstick_y == 0 &&
                  side.press_counters[5] == 1,
              "three-finger click is not exclusive or failed confirmation");
        side = sample(click, 180);
        check(side.press_counters[5] == 1, "held three-finger pose repeated click presses");
        auto missing_little = click;
        missing_little.joints[20].flags = 0;
        side = sample(missing_little);
        check(side.buttons == CONTROLLER_BUTTON_THUMBSTICK && side.press_counters[5] == 1,
              "missing little tip cancelled the three-finger click");
        side = sample(pose(0, 0, left));
        check(side.buttons == 0 && side.release_counters[5] == 1,
              "three-finger exit did not release once and block gameplay");
        side = sample(pose(14, 2, left), 180);
        side = sample(pose(14, 2, left), 180);
        check(side.buttons == 0, "three-finger transition generated A before neutral");
        // Neutral ignores the former click finger, including when it is open.
        sample(pose(11, 0, left));
        sample(pose(14, 2, left));
        side = sample(pose(14, 2, left), 120);
        check(side.buttons == CONTROLLER_BUTTON_A, "A failed after the two-channel neutral");
        side = sample(click, 180);
        side = sample(click, 180);
        check(side.buttons == 0, "A-to-three-finger transition bypassed neutral");
      }
    }
  auto cfg = immediate();
  cfg.button_layout = "middle_grip";
  cfg.thumbstick_click_gesture = "three_fingers";
  cfg.trigger_on_curl = cfg.grip_on_curl = true;
  cfg.pose_confirm_ms = 120;
  em::Controller transient(cfg);
  auto f = frame();
  f.right = pose(7, 0);
  apply(transient, f);
  f.right = pose(0, 0);
  advance(f, 180);
  check(apply(transient, f).right.buttons == 0,
        "unconfirmed three-finger exit leaked ordinary buttons");
  f.right = pose(3, 0);
  advance(f);
  apply(transient, f);
  f.right = pose(0, 0);
  advance(f);
  check(apply(transient, f).right.buttons == 3, "neutral did not rearm ordinary channels");

  cfg.pose_confirm_ms = cfg.menu_hold_ms = 0;
  em::Controller menu(cfg);
  f = frame();
  f.left = pose(7, 0, true);
  f.right = pose(7, 0);
  auto out = apply(menu, f);
  check(out.left.buttons == 32 && out.right.buttons == 32,
        "two three-finger poses were confused with Menu");
  f.left = pose(15, 1, true);
  f.right = pose(15, 1);
  advance(f);
  out = apply(menu, f);
  check(out.left.buttons == 0 && out.right.buttons == 0,
        "three-finger-to-open-palm transition generated Menu before neutral");
  f.left = pose(3, 0, true);
  f.right = pose(3, 0);
  advance(f);
  apply(menu, f);
  f.left = pose(15, 1, true);
  f.right = pose(15, 1);
  advance(f);
  out = apply(menu, f);
  check(out.left.buttons == 4 && out.right.buttons == 0, "two-hand Menu failed after neutral");

  for (int bad_thumb : {1, 2, 3}) {
    em::Controller rejected(cfg);
    auto invalid = frame();
    invalid.right = pose(7, bad_thumb == 2 ? 2 : 1);
    if (bad_thumb == 3) invalid.right.joints[4].flags = 0;
    check(!(apply(rejected, invalid).right.buttons & CONTROLLER_BUTTON_THUMBSTICK),
          "raised, pinching or missing thumb created a three-finger click");
  }

  em::Controller physical(cfg);
  f = frame();
  f.right = pose(7, 0);
  RuntimeControllerStateFrameV1 merged{};
  merged.right.source_mask = RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
  merged.right.flags = RUNTIME_CONTROLLER_INPUT_VALID;
  merged.right.buttons = CONTROLLER_BUTTON_TRIGGER;
  merged.right.trigger = .8f;
  merged.right.thumbstick_x = .7f;
  physical.apply(merged, &f, f.timestamp_ns);
  check(merged.right.buttons == (CONTROLLER_BUTTON_TRIGGER | CONTROLLER_BUTTON_THUMBSTICK) &&
            merged.right.trigger == .8f && merged.right.thumbstick_x == .7f,
        "exclusive emulated click suppressed physical controller input");
  merged = {};
  merged.right.source_mask = RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
  merged.right.flags = RUNTIME_CONTROLLER_INPUT_VALID;
  merged.right.buttons = CONTROLLER_BUTTON_B;
  physical.apply(merged, &f, f.timestamp_ns, 0, true);
  check(merged.right.buttons == CONTROLLER_BUTTON_B,
        "override gesture block failed for three-finger click");
}
void validation_and_legacy() {
  for (const auto& j :
       {nlohmann::json{{"version", 1}}, nlohmann::json{{"finger_mode", "count"}},
        nlohmann::json{{"deadzone", 1}}, nlohmann::json{{"extension_on", .3}},
        nlohmann::json{{"forward_range_scale", 0}}, nlohmann::json{{"forward_range_scale", 2.1}},
        nlohmann::json{{"pose_yaw_offset_degrees", -181}},
        nlohmann::json{{"pose_yaw_offset_degrees", 181}},
        nlohmann::json{{"pose_yaw_offset_degrees", std::numeric_limits<double>::quiet_NaN()}},
        nlohmann::json{{"dpad_source", "both"}}, nlohmann::json{{"dpad_source", "thumb"}},
        nlohmann::json{{"thumb_back_on", .2}}, nlohmann::json{{"thumb_back_off", .9}},
        nlohmann::json{{"button_layout", "unknown"}},
        nlohmann::json{{"thumbstick_click_gesture", "unknown"}},
        nlohmann::json{{"thumbstick_click_gesture", "three_fingers"}}}) {
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
    thumb_dpad_and_independent_channels();
    trigger_calibration_and_wrist_diagonals();
    middle_grip_three_channels_and_rings();
    three_finger_click_is_separate();
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
