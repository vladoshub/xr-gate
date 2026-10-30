#include "emulation_controller.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <xr_runtime/input/controller_input_mapping.hpp>

#include "gestures.hpp"
#include "simple_gestures.hpp"
#include "simple_controller_gestures.hpp"
#include "simple_touchpad.hpp"
#include "override_controller.hpp"

namespace {
namespace em = xr_runtime_adapter::emulation;
using namespace xr_runtime;
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
uint64_t face(uint64_t bits, bool left) {
  if (!left) return bits;
  if (bits & CONTROLLER_BUTTON_A) bits |= CONTROLLER_BUTTON_X;
  if (bits & CONTROLLER_BUTTON_B) bits |= CONTROLLER_BUTTON_Y;
  return bits & ~(CONTROLLER_BUTTON_A | CONTROLLER_BUTTON_B);
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
em::Config thumb_config(bool left, bool skeleton = false) {
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
  if (skeleton) {
    nlohmann::json poses;
    const std::array<std::array<double,2>,8> targets{{
        {.5,.25},{.5,1},{0,1},{-.5,1},{-.5,.25},{-.5,-.2},{0,-.2},{.5,-.2}}};
    for (size_t i=0;i<8;++i)
      poses[em::thumb_direction_names[i]]=em::measure_hand(thumb_pose(left,targets[i][0],targets[i][1]),.3).thumb_pose;
    j[left ? "left" : "right"]["thumb_calibration"]={
        {"feature","thumb_skeleton_v2"},
        {"neutral_poses",{em::measure_hand(thumb_pose(left,0,.25),.3).thumb_pose}},
        {"directions",poses},{"enable_clearance",.7},{"disable_clearance",.6}};
  }
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
    for (bool skeleton : {false, true}) {
    const auto cfg = thumb_config(left,skeleton);
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
      auto h = thumb_pose(left, 0, .25 - fraction * .45);
      if (skeleton) {
        const auto neutral=thumb_pose(left,0,.25), down_pose=thumb_pose(left,0,-.2);
        for (size_t k=1;k<=4;++k) {
          h.joints[k].px=neutral.joints[k].px+fraction*(down_pose.joints[k].px-neutral.joints[k].px);
          h.joints[k].py=neutral.joints[k].py+fraction*(down_pose.joints[k].py-neutral.joints[k].py);
          h.joints[k].pz=neutral.joints[k].pz+fraction*(down_pose.joints[k].pz-neutral.joints[k].pz);
        }
      }
      (left ? f.left : f.right) = h;
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
}
void thumb_skeleton_uses_base_motion_and_neutral_range() {
  for (bool left : {false,true}) {
    auto cfg=thumb_config(left,true);
    auto& tc=(left ? cfg.left : cfg.right).thumb;
    const auto hand=[&](double lateral,double bend) {
      auto h=thumb_pose(left,0,bend);
      const auto basis=em::measure_hand(h,.3).palm_basis;
      for (size_t i=1;i<=4;++i) {
        h.joints[i].px+=lateral*basis[0][0];
        h.joints[i].py+=lateral*basis[0][1];
        h.joints[i].pz+=lateral*basis[0][2];
      }
      return h;
    };
    tc.neutral_poses={em::measure_hand(hand(-.001,.25),.3).thumb_pose,
                      em::measure_hand(hand(.001,.25),.3).thumb_pose};
    for (size_t i=0;i<8;++i) {
      const auto axis=em::thumb_direction_axes[i];
      tc.pose_directions[i]=em::measure_hand(hand(axis[0]*.015,axis[1]>0 ? 1 : axis[1]<0 ? -.2 : .25),.3).thumb_pose;
    }
    em::validate_thumb_skeleton(tc);
    const auto a=em::measure_hand(hand(-.015,.25),.3),b=em::measure_hand(hand(.015,.25),.3);
    for (size_t i=0;i<3;++i)
      check(std::abs(a.thumb_feature[i]-b.thumb_feature[i])<1e-5,
            "fixture must have identical legacy thumb features");
    constexpr uint64_t expected[]={512,576,64,320,256,384,128,640};
    for (size_t i=0;i<8;++i) {
      em::Controller c(cfg);auto f=frame();
      const auto axis=em::thumb_direction_axes[i];
      (left ? f.left : f.right)=hand(axis[0]*.015,axis[1]>0 ? 1 : axis[1]<0 ? -.2 : .25);
      const auto o=apply(c,f);
      check((left ? o.left : o.right).buttons==expected[i],"full thumb skeleton lost base opposition/diagonal");
    }
    for (double offset : {-.001,0.,.001}) {
      em::Controller c(cfg);auto f=frame();(left ? f.left : f.right)=hand(offset,.25);
      const auto o=apply(c,f);const auto& side=left ? o.left : o.right;
      check(side.thumbstick_x==0 && side.thumbstick_y==0 && side.buttons==0,
            "neutral range generated movement");
    }
    auto broken=tc;broken.neutral_poses.clear();
    bool rejected=false;try {em::validate_thumb_skeleton(broken);} catch (const std::exception&) {rejected=true;}
    check(rejected,"empty skeleton neutral accepted");
    broken=tc;broken.pose_directions[1]=broken.pose_directions[0];
    rejected=false;try {em::validate_thumb_skeleton(broken);} catch (const std::exception&) {rejected=true;}
    check(rejected,"identical thumb directions accepted");
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
      check(out.buttons == face(entry.second,left) && out.trigger == 0 && out.grip == 0 &&
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
        check(side.buttons == face(CONTROLLER_BUTTON_A,left) && side.trigger == 0 && side.grip == 0 &&
                  side.thumbstick_x == 0 && side.thumbstick_y == 0,
              "open ring did not exclusively press A");
        side = sample(pose(0, 2, left));
        check(side.buttons == face(CONTROLLER_BUTTON_B,left) && side.trigger == 0 && side.grip == 0 &&
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
  check(out.left.buttons == CONTROLLER_BUTTON_X && out.right.buttons == CONTROLLER_BUTTON_A,
        "two rings were confused with Menu or lost left X");
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
        const auto ready = thumb ? thumb_pose(left,0,.25,3,true) : pose(3,0,left);
        sample(ready);
        sample(ready,120);
        // Operating either button revokes arming. A return directly from both
        // held buttons must not become a click even if the ring is mistracked.
        sample(pose(0, 0, left));
        const unsigned click_mask = little_open ? 15 : 7;
        auto click = thumb ? thumb_pose(left, 0, 1, click_mask, true) : pose(click_mask, 0, left);
        if (!thumb) tilt(click, .6, .4);
        sample(click);
        check(sample(click,180).buttons==0,
              "return from both gameplay fingers generated a click");
        sample(ready);
        sample(ready,120);
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
        check(side.buttons == face(CONTROLLER_BUTTON_A,left), "A/X failed after the two-channel neutral");
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
  f.right=pose(3,0);
  apply(transient,f);
  advance(f,120);
  apply(transient,f);
  f.right = pose(7, 0);
  advance(f);
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
  check(out.left.buttons==0 && out.right.buttons==0,"startup pose bypassed click arming");
  f.left=pose(3,0,true);f.right=pose(3,0);advance(f);apply(menu,f);
  f.left=pose(7,0,true);f.right=pose(7,0);advance(f);out=apply(menu,f);
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
  f.right=pose(3,0);apply(physical,f);advance(f);
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
void thumb_inversion_and_click_rearming() {
  check(em::config_from_json({{"thumb_invert_y",true}}).thumb_invert_y,"thumb inversion option ignored");
  for (bool left : {false,true}) for (bool skeleton : {false,true}) {
    auto cfg=thumb_config(left,skeleton);cfg.thumb_invert_y=true;
    constexpr uint64_t expected[]={128,64,256,512,384,640,320,576};
    const std::array<std::array<double,2>,8> targets{{
        {0,1},{0,-.2},{-.5,.25},{.5,.25},{-.5,1},{.5,1},{-.5,-.2},{.5,-.2}}};
    for (size_t i=0;i<targets.size();++i) {
      em::Controller c(cfg);auto f=frame();(left ? f.left : f.right)=thumb_pose(left,targets[i][0],targets[i][1]);
      const auto o=apply(c,f);const auto& side=left ? o.left : o.right;
      check(side.buttons==expected[i],"thumb inversion lost an axis or diagonal");
      auto h=left ? f.left : f.right;tilt(h,.6,-.4);yaw(h,.9);(left ? f.left : f.right)=h;advance(f);
      const auto rotated=apply(c,f);const auto& after=left ? rotated.left : rotated.right;
      check(after.buttons==side.buttons && std::abs(after.thumbstick_y-side.thumbstick_y)<1e-5,
            "inverted thumb pad depends on wrist rotation");
    }
    em::Controller gate(cfg);auto f=frame();
    const auto sample=[&](double fraction) {
      auto h=thumb_pose(left,0,.25+fraction*.75);
      if (skeleton) {
        const auto n=thumb_pose(left,0,.25),up=thumb_pose(left,0,1);
        for (size_t k=1;k<=4;++k) {
          h.joints[k].px=n.joints[k].px+fraction*(up.joints[k].px-n.joints[k].px);
          h.joints[k].py=n.joints[k].py+fraction*(up.joints[k].py-n.joints[k].py);
          h.joints[k].pz=n.joints[k].pz+fraction*(up.joints[k].pz-n.joints[k].pz);
        }
      }
      (left ? f.left : f.right)=h;advance(f);const auto o=apply(gate,f);return left ? o.left : o.right;
    };
    check(sample(.8).thumbstick_y==0,"inverted back lost its strict entry threshold");
    check(sample(.9).thumbstick_y<-.65,"inverted back did not enter the strict region");
    check(sample(.75).thumbstick_y<0,"inverted back lost hysteresis");
    check(sample(.65).thumbstick_y==0,"inverted back leaked after strict release");
  }
  for (bool left : {false,true}) for (bool thumb : {false,true}) for(bool paired : {false,true}) {
    auto cfg=thumb ? thumb_config(left,true) : immediate();
    cfg.button_layout="middle_grip";cfg.thumbstick_click_gesture="three_fingers";
    cfg.trigger_on_curl=cfg.grip_on_curl=true;cfg.pose_confirm_ms=120;
    em::Controller c(cfg);auto f=frame();
    const auto sample=[&](unsigned fingers,bool hidden,uint64_t ms) {
      (left ? f.left : f.right)=thumb ? thumb_pose(left,0,1,fingers,hidden) : pose(fingers,hidden ? 0 : 1,left);
      advance(f,ms);const auto o=apply(c,f);return left ? o.left : o.right;
    };
    check(sample(3,false,30).buttons&CONTROLLER_BUTTON_DPAD_UP || !thumb,"pad fixture did not start active");
    for (int i=0;i<30;++i) {
      const auto side=sample(7,true,100);
      check(side.buttons==0 && side.thumbstick_x==0 && side.thumbstick_y==0,
            "folding thumb generated a click, including after a long hold");
    }
    sample(3,true,30);sample(3,true,60);
    check(sample(7,true,500).buttons==0,"short tucked neutral armed a click");
    sample(3,true,30);sample(3,true,120);
    sample(7,true,30);
    auto side=sample(7,true,120);
    check(side.buttons==CONTROLLER_BUTTON_THUMBSTICK && side.press_counters[5]==1,
          "intentional click failed after confirmed tucked neutral");
    side=sample(15,true,120);
    check(side.buttons==CONTROLLER_BUTTON_THUMBSTICK && side.press_counters[5]==1,
          "little finger changed/repeated a confirmed click");
    (left ? f.left : f.right).status=0;advance(f);apply(c,f);
    check(sample(7,true,500).buttons==0,"tracking reacquisition bypassed click arming");
    sample(3,true,30);sample(3,true,120);sample(3,false,30);
    check(sample(7,true,500).buttons==0,"reopened thumb did not disarm the click");
    if (thumb) {
      sample(3,true,30);sample(3,true,120);
      auto tip_only=thumb_pose(left,0,.25,7,false);
      const auto basis=em::measure_hand(tip_only,.3).palm_basis;
      tip_only.joints[4].px=tip_only.joints[5].px+.01*basis[2][0];
      tip_only.joints[4].py=tip_only.joints[5].py+.01*basis[2][1];
      tip_only.joints[4].pz=tip_only.joints[5].pz+.01*basis[2][2];
      const auto m=em::measure_hand(tip_only,.3);const auto& hc=left ? cfg.left : cfg.right;
      check(m.thumb_valid && m.thumb_pad_valid && m.thumb_clearance<cfg.thumb_disable_clearance &&
                m.thumb_pad_clearance>=hc.thumb.disable_clearance,
            "fixture must fold only the tip while leaving the D-pad base active");
      (left ? f.left : f.right)=tip_only;advance(f,500);const auto o=apply(c,f);
      check(!((left ? o.left : o.right).buttons&CONTROLLER_BUTTON_THUMBSTICK),
            "folded thumb tip clicked before the D-pad base switched off");
    }
  }
  auto wrist_cfg=immediate();auto inverted=wrist_cfg;inverted.thumb_invert_y=true;
  em::Controller normal(wrist_cfg),other(inverted);auto f=frame();apply(normal,f);apply(other,f);
  tilt(f.right,.5,0);advance(f);const auto a=apply(normal,f),b=apply(other,f);
  check(a.right.buttons==b.right.buttons && a.right.thumbstick_y==b.right.thumbstick_y,
        "thumb-only inversion changed wrist steering");
}
void finger_pair_channels_and_click_motion_guard() {
  auto cfg=immediate();
  cfg.button_layout="middle_grip";cfg.thumbstick_click_gesture="three_fingers";
  cfg.trigger_on_curl=cfg.grip_on_curl=true;
  const unsigned masks[]={3,2,1,0};
  em::FingerPairCalibration calibration;
  calibration.calibrated=true;
  for (size_t i=0;i<4;++i) {
    const auto m=em::measure_hand(pose(masks[i],0),.3);
    check(m.finger_pair_valid,"pair feature missing for a tracked hand");
    calibration.poses[i].push_back(m.finger_pair_pose);
  }
  nlohmann::json json_cal={{"feature","finger_angles_v1"},
      {"open",calibration.poses[0]},{"index",calibration.poses[1]},
      {"middle",calibration.poses[2]},{"both",calibration.poses[3]}};
  auto parsed=em::config_from_json({{"button_layout","middle_grip"},
      {"left",{{"finger_pair_calibration",json_cal}}}});
  check(parsed.left.finger_pair.calibrated && !parsed.right.finger_pair.calibrated,
        "pair calibration leaked to the other hand");
  auto disabled=json_cal;disabled["enabled"]=false;
  check(!em::config_from_json({{"left",{{"finger_pair_calibration",disabled}}}}).left.finger_pair.calibrated,
        "pair calibration could not be disabled");
  for (const auto* key : {"open","index","middle","both"}) {
    auto invalid=json_cal;invalid[key]=nlohmann::json::array();
    bool rejected=false;
    try {em::config_from_json({{"left",{{"finger_pair_calibration",invalid}}}});}
    catch(const std::exception&){rejected=true;}
    check(rejected,"incomplete pair calibration accepted");
  }
  for (unsigned mutation=0;mutation<5;++mutation) {
    auto bad=json_cal;
    switch(mutation) {
      case 0:bad["feature"]="unknown";break;
      case 1:bad["index"]=bad["open"];break;
      case 2:bad["open"][0][0]=-1;break;
      case 3:bad["max_distance"]=0;break;
      default:bad["open"][0][0]=std::numeric_limits<double>::infinity();break;
    }
    bool rejected=false;
    try {em::config_from_json({{"left",{{"finger_pair_calibration",bad}}}});}
    catch(const std::exception&){rejected=true;}
    check(rejected,"invalid pair calibration accepted");
  }
  cfg.left.finger_pair=cfg.right.finger_pair=calibration;
  for (bool left : {false,true}) for (bool on_curl : {false,true}) {
    auto config=cfg;config.trigger_on_curl=config.grip_on_curl=on_curl;
    for (size_t i=0;i<4;++i) {
      auto h=pose(masks[i],1,left);
      tilt(h,.55,-.45);yaw(h,.8);
      for(auto&j:h.joints){j.px=j.px*1.2f+.3f;j.py=j.py*1.2f-.2f;j.pz=j.pz*1.2f+.1f;}
      const auto m=em::measure_hand(h,.3);
      const auto strength=em::project_finger_pair(calibration,m);
      check(std::abs(strength[0]-double(i&1))<1e-8 &&
                std::abs(strength[1]-double((i>>1)&1))<1e-8,
            "pair feature depends on wrist angle, mirror, size or translation");
      em::Controller c(config);auto f=frame();(left ? f.left:f.right)=h;
      auto result=apply(c,f);const auto& side=left ? result.left:result.right;
      const uint64_t expected=on_curl ? i : (i^3);
      check((side.buttons&3)==expected,"pair channels lost independence or polarity");
      (left ? f.left:f.right).joints[7].flags=0;advance(f);
      result=apply(c,f);
      check(((left ? result.left:result.right).buttons&3)==0,
            "invalid pair joints did not release both polarities");
    }
    auto strict=calibration;strict.max_distance=.05;
    auto m=em::measure_hand(pose(3,0,left),.3);m.finger_pair_pose.fill(2);
    check(em::project_finger_pair(strict,m)[0]<0,"out-of-range pair observation accepted");
  }
  // A calibrated channel may see motion that has not crossed the old generic
  // curl threshold. It must still revoke click arming.
  for (bool left : {false,true}) for (bool middle : {false,true}) {
    auto config=cfg;config.pose_confirm_ms=120;
    em::Controller c(config);auto f=frame();
    const auto sample=[&](unsigned fingers,uint64_t ms) {
      (left ? f.left:f.right)=pose(fingers,0,left);advance(f,ms);
      const auto result=apply(c,f);return left ? result.left:result.right;
    };
    sample(3,30);sample(3,120); // Arm, then operate a gameplay finger.
    sample(middle ? 1:2,30);sample(middle ? 1:2,120);
    check(!(sample(7,30).buttons&CONTROLLER_BUTTON_THUMBSTICK),
          "return from finger motion created a click candidate");
    check(!(sample(7,500).buttons&CONTROLLER_BUTTON_THUMBSTICK),
          "return from finger motion created a delayed click");
    sample(3,30);sample(3,120);sample(7,30);
    check(sample(7,120).buttons==CONTROLLER_BUTTON_THUMBSTICK,
          "motion guard prevented an intentional click after neutral");
  }
}
void recorded_finger_pair_regressions() {
  const auto path=std::filesystem::path(__FILE__).parent_path()/"data/emulation_finger_pair_poses.json";
  std::ifstream in(path);
  check(bool(in),"recorded finger-pair fixture missing");
  nlohmann::json data;in>>data;
  auto cfg=em::config_from_json({{"button_layout","middle_grip"},
      {"trigger_on_curl",true},{"grip_on_curl",true},
      {"button_confirm_ms",0},{"dpad_mode","off"},
      {"left",{{"finger_pair_calibration",data.at("calibration")}}},
      {"right",{{"finger_pair_calibration",data.at("calibration")}}}});
  for(bool left:{false,true}) for(const auto& item:data.at("cases")) {
    auto h=pose(0,1,left);
    for(size_t i=0;i<21;++i) {
      auto p=item.at("joints").at(i).get<em::Vec3>();
      h.joints[i].px=float(left ? -p[0]:p[0]);h.joints[i].py=float(p[1]);h.joints[i].pz=float(p[2]);
    }
    for(bool rotate:{false,true}) {
      if(rotate){tilt(h,.65,-.45);yaw(h,.7);}
      auto f=frame();(left ? f.left:f.right)=h;em::Controller c(cfg);
      const auto result=apply(c,f);
      check(((left ? result.left:result.right).buttons&3)==item.at("expected_buttons").get<uint64_t>(),
            "recorded index/middle separation or simultaneous curl regressed");
    }
  }
  for(bool left:{false,true}) for(bool open:{false,true}) {
    auto f=frame();(left ? f.left:f.right)=pose(open ? 14:0,2,left);
    em::Controller c(cfg);apply(c,f);advance(f,120);const auto result=apply(c,f);
    check((left ? result.left:result.right).buttons==face(open ? CONTROLLER_BUTTON_A:CONTROLLER_BUTTON_B,left),
          "pair calibration changed A/B ring priority");
  }
}
void stricter_thumb_back_and_little_click() {
  for(bool left:{false,true}) {
    auto cfg=thumb_config(left,true);
    cfg.thumb_invert_y=true;cfg.thumb_back_on=.98;cfg.thumb_back_off=.90;
    cfg.thumb_back_side_ratio=.8;
    const auto neutral=thumb_pose(left,0,.25),right=thumb_pose(left,.5,.25),back=thumb_pose(left,0,1);
    em::Controller pad(cfg);auto f=frame();
    const auto steer=[&](double x,double y) {
      auto h=neutral;
      for(size_t i=1;i<=4;++i) {
        h.joints[i].px=neutral.joints[i].px+x*(right.joints[i].px-neutral.joints[i].px)+y*(back.joints[i].px-neutral.joints[i].px);
        h.joints[i].py=neutral.joints[i].py+x*(right.joints[i].py-neutral.joints[i].py)+y*(back.joints[i].py-neutral.joints[i].py);
        h.joints[i].pz=neutral.joints[i].pz+x*(right.joints[i].pz-neutral.joints[i].pz)+y*(back.joints[i].pz-neutral.joints[i].pz);
      }
      (left ? f.left:f.right)=h;advance(f);const auto result=apply(pad,f);return left ? result.left:result.right;
    };
    check(steer(0,.94).thumbstick_y==0,"weak back entered stricter gate");
    check(steer(0,1).thumbstick_y<-.9,"deliberate back was blocked");
    check(steer(0,.94).thumbstick_y<0,"strict back lost hold hysteresis");
    check(steer(0,.88).thumbstick_y==0,"back did not release at the raised threshold");
    auto side=steer(2,1.05);
    check(side.thumbstick_x>.9 && side.thumbstick_y==0 && !(side.buttons&CONTROLLER_BUTTON_DPAD_DOWN),
          "large horizontal movement leaked back into analog/digital output");
    for(double x:{-1.,1.}) {
      (left ? f.left:f.right)=thumb_pose(left,x*.5,1);advance(f);
      const auto diagonal=apply(pad,f);side=left ? diagonal.left:diagonal.right;
      check(side.thumbstick_y<-.6 && std::abs(side.thumbstick_x)>.6 &&
            (side.buttons&CONTROLLER_BUTTON_DPAD_DOWN),"strict back removed deliberate diagonals");
    }
  }
  for(bool left:{false,true}) {
    auto cfg=immediate();cfg.button_layout="middle_grip";
    cfg.thumbstick_click_gesture="index_middle_little";
    cfg.trigger_on_curl=cfg.grip_on_curl=true;cfg.pose_confirm_ms=120;
    cfg.dpad_mode="off";
    em::Controller c(cfg);auto f=frame();
    const auto sample=[&](unsigned fingers,int thumb=0,uint64_t ms=30) {
      (left ? f.left:f.right)=pose(fingers,thumb,left);advance(f,ms);
      const auto result=apply(c,f);return left ? result.left:result.right;
    };
    sample(11);check(sample(11,0,500).buttons==0,"held pinky on startup armed click");
    sample(3);sample(3,0,120);sample(7);
    check(sample(7,0,500).buttons==0,"old ring-finger pose activated pinky click");
    sample(3);sample(3,0,120);sample(11);
    auto side=sample(11,0,120);
    check(side.buttons==CONTROLLER_BUTTON_THUMBSTICK && side.press_counters[5]==1,
          "pinky click did not confirm exactly once");
    side=sample(11,0,500);check(side.press_counters[5]==1,"held pinky repeated a click");
    side=sample(3);
    check(side.buttons==0 && side.release_counters[5]==1,"folded pinky did not release click immediately");
    sample(3,0,120);sample(2);sample(2,0,120);sample(11);
    check(sample(11,0,500).buttons==0,"return from Trigger became a pinky click");
    sample(3);sample(3,0,120);sample(10);
    check(sample(10,0,120).buttons==CONTROLLER_BUTTON_TRIGGER,
          "pinky interfered with an independently held Trigger");
    sample(3);sample(3,0,120);sample(11,1);
    check(!(sample(11,1,500).buttons&CONTROLLER_BUTTON_THUMBSTICK),
          "raised thumb did not block pinky click");
    (left ? f.left:f.right).status=0;advance(f);apply(c,f);
    check(sample(11,0,500).buttons==0,"pinky reacquisition bypassed neutral");
    sample(3);sample(3,0,120);sample(11);
    check(sample(11,0,120).buttons==CONTROLLER_BUTTON_THUMBSTICK,
          "pinky click did not rearm after a released neutral");
  }
}
void grip_release_and_left_face_counters() {
  for(bool left:{false,true}) for(bool pair:{false,true}) {
    auto cfg=immediate();cfg.button_layout="middle_grip";
    cfg.trigger_on_curl=cfg.grip_on_curl=true;cfg.dpad_mode="off";
    cfg.button_confirm_ms=45;cfg.grip_release_threshold=.60;
    auto partial=pose(0,0,left);bend_finger(partial,1,{10,35,60});
    const auto measurement=em::measure_hand(partial,.3);
    for(auto* hc:{&cfg.left,&cfg.right}) {
      if(pair) {
        hc->finger_pair.calibrated=true;
        for(size_t i=0;i<4;++i) {
          const unsigned masks[]={3,2,1,0};
          hc->finger_pair.poses[i].push_back(em::measure_hand(pose(masks[i],0,left),.3).finger_pair_pose);
        }
        // Put the partial pose at an observable half-strength point between
        // the index-only and both-curled endpoints, exercising the same gate.
        auto& a=hc->finger_pair.poses[1][0];auto& b=hc->finger_pair.poses[3][0];
        const auto p=measurement.finger_pair_pose;
        for(size_t k=0;k<a.size();++k){a[k]=p[k];b[k]=p[k];}
        a[3]=p[3]-.2;b[3]=p[3]+.2;
      } else {
        hc->grip_calibrated=true;hc->grip_calibration_finger="middle";
        hc->grip_closed_curl=measurement.curl[1]/.55;
      }
    }
    auto old_cfg=cfg;old_cfg.grip_release_threshold=-1;
    em::Controller faster(cfg),old(old_cfg);auto f=frame();
    const auto sample=[&](const HandSideF32V2& h,uint64_t ms) {
      (left ? f.left:f.right)=h;advance(f,ms);auto a=apply(faster,f),b=apply(old,f);
      return std::pair<RuntimeControllerSideStateV1,RuntimeControllerSideStateV1>{left ? a.left:a.right,left ? b.left:b.right};
    };
    auto held=pose(0,0,left);
    if(pair) { // Ensure the held shape has an exact both-curled endpoint.
      auto m=em::measure_hand(held,.3);
      for(auto* hc:{&cfg.left,&cfg.right})hc->finger_pair.poses[3].push_back(m.finger_pair_pose);
      old_cfg=cfg;old_cfg.grip_release_threshold=-1;faster=em::Controller(cfg);old=em::Controller(old_cfg);
    }
    sample(held,30);auto state=sample(held,60);
    check(state.first.buttons&CONTROLLER_BUTTON_GRIP,"Grip did not engage after normal confirmation");
    state=sample(partial,1);
    check(!(state.first.buttons&CONTROLLER_BUTTON_GRIP) && state.first.grip==0 &&
          state.first.release_counters[1]==1,"new Grip threshold did not release on the first fresh sample");
    check(state.second.buttons&CONTROLLER_BUTTON_GRIP,"old threshold fixture was not still held");
  }
  auto cfg=immediate();cfg.button_layout="middle_grip";cfg.dpad_mode="off";
  cfg.trigger_on_curl=cfg.grip_on_curl=true;
  em::Controller c(cfg);auto f=frame();f.left=pose(14,2,true);f.right=pose(14,2);
  auto out=apply(c,f);
  check(out.left.buttons==CONTROLLER_BUTTON_X && out.left.touches==CONTROLLER_BUTTON_X &&
        out.left.press_counters[11]==1 && out.left.press_counters[3]==0 &&
        out.right.buttons==CONTROLLER_BUTTON_A && out.right.press_counters[3]==1,
        "left X/right A buttons, touches or counters were wrong");
  ControllerInputMappingConfig mapping;
  auto input=map_controller_input(&out.left,true,mapping);
  check(input.x_button && input.x_touch && !input.a && !input.a_touch,"shared driver mapping aliased X to A");
  f.left=pose(0,2,true);f.right=pose(0,2);advance(f);out=apply(c,f);
  check(out.left.buttons==CONTROLLER_BUTTON_Y && out.left.press_counters[12]==1 &&
        out.left.release_counters[11]==1 && out.left.press_counters[4]==0 &&
        out.right.buttons==CONTROLLER_BUTTON_B,"left Y/right B transition or counters were wrong");
  f.left.status=0;advance(f);out=apply(c,f);
  check(out.left.buttons==0 && out.left.release_counters[12]==1,"lost left hand did not release Y");
  em::Controller mixed(cfg);f=frame();f.left=pose(14,2,true);
  RuntimeControllerStateFrameV1 physical{};
  physical.left.source_mask=RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
  physical.left.flags=RUNTIME_CONTROLLER_INPUT_VALID;
  physical.left.buttons=physical.left.touches=CONTROLLER_BUTTON_A;
  physical.left.press_counters[3]=1;
  mixed.apply(physical,&f,f.timestamp_ns);
  check(physical.left.buttons==(CONTROLLER_BUTTON_A|CONTROLLER_BUTTON_X) &&
        physical.left.touches==(CONTROLLER_BUTTON_A|CONTROLLER_BUTTON_X) &&
        physical.left.press_counters[3]==1 && physical.left.press_counters[11]==1,
        "left gesture mapping renamed or double-counted a physical button");
}
void ring_finger_independent_channels() {
  for (bool left : {false,true}) for (bool thumb : {false,true}) for(bool paired : {false,true}) {
    auto cfg=thumb ? thumb_config(left,true) : immediate();
    cfg.button_layout="middle_grip";cfg.thumbstick_click_gesture="ring_finger";
    cfg.trigger_on_curl=cfg.grip_on_curl=true;
    cfg.ring_click_mcp_min_degrees=78;cfg.ring_click_mcp_max_degrees=100;
    cfg.ring_click_pip_max_degrees=30;cfg.ring_click_dip_max_degrees=12;
    cfg.ring_click_hysteresis_degrees=7;
    auto marker=pose(3,1,left);bend_finger(marker,2,{85,100,100});
    for(auto* hc:{&cfg.left,&cfg.right}) {
      hc->ring_click_pose.calibrated=true;
      hc->ring_click_pose.poses.push_back(em::measure_hand(marker,.3).ring_pose);
    }
    if(paired) for(auto* hc : {&cfg.left,&cfg.right}) {
      hc->finger_pair.calibrated=true;
      const unsigned masks[]={3,2,1,0};
      for(size_t k=0;k<4;++k)hc->finger_pair.poses[k].push_back(em::measure_hand(pose(masks[k],1,left),.3).finger_pair_pose);
    }
    for(unsigned buttons=0;buttons<4;++buttons) for(bool tucked:{false,true}) {
      em::Controller c(cfg);auto f=frame();
      auto& h=left ? f.left:f.right;
      h=thumb ? thumb_pose(left,0,.25,3,tucked) : pose(3,tucked?0:1,left);
      apply(c,f);advance(f);
      const unsigned mask=(buttons&1 ? 0:1)|(buttons&2 ? 0:2);
      h=thumb ? thumb_pose(left,0,1,mask,tucked) : pose(mask,tucked?0:1,left);
      bend_finger(h,2,{85,100,100});
      if(!thumb)tilt(h,.5,0);
      auto out=apply(c,f);auto side=left ? out.left:out.right;
      check((side.buttons&~CONTROLLER_BUTTON_DPAD_MASK)==(buttons|CONTROLLER_BUTTON_THUMBSTICK),
            "half-ring click did not coexist with independent Trigger/Grip/thumb");
      if(thumb && !tucked)check(side.buttons&CONTROLLER_BUTTON_DPAD_UP,"ring click blocked thumb D-pad");
      if(!thumb && !tucked)check(side.buttons&CONTROLLER_BUTTON_DPAD_UP,"ring click blocked wrist D-pad");
      advance(f,150);out=apply(c,f);side=left ? out.left:out.right;
      check(side.press_counters[5]==1,"held ring repeated a click");
      h.joints[14].flags=0;advance(f,1);out=apply(c,f);side=left ? out.left:out.right;
      check(!(side.buttons&CONTROLLER_BUTTON_THUMBSTICK) && (side.buttons&3)==buttons,
            "missing ring joints did not release only the ring channel");
    }
    // A tip near the index is not a special-pose lock while the ring marker is held.
    em::Controller c(cfg);auto f=frame();auto& h=left?f.left:f.right;
    h=pose(3,1,left);apply(c,f);advance(f);bend_finger(h,2,{85,100,100});
    h.joints[4].px=h.joints[8].px+.006f;h.joints[4].py=h.joints[8].py;h.joints[4].pz=h.joints[8].pz;
    auto out=apply(c,f);check(((left?out.left:out.right).buttons&~CONTROLLER_BUTTON_DPAD_MASK)==CONTROLLER_BUTTON_THUMBSTICK,
                            "thumb/index proximity blocked an independent ring click");
  }
  auto cfg=em::config_from_json({{"button_layout","middle_grip"},{"thumbstick_click_gesture","ring_finger"},
    {"trigger_on_curl",true},{"grip_on_curl",true},{"button_confirm_ms",45},{"dpad_mode","off"}});
  em::Controller c(cfg);auto f=frame();f.right=pose(3,1);bend_finger(f.right,2,{85,100,100});
  apply(c,f);advance(f,200);check(!(apply(c,f).right.buttons&32),"held startup ring bypassed release");
  f.right=pose(3,1);advance(f,1);apply(c,f);bend_finger(f.right,2,{85,100,100});advance(f,1);
  check(!(apply(c,f).right.buttons&32),"ring bypassed normal press confirmation");
  check(!(apply(c,f,f.timestamp_ns+50000000).right.buttons&32),"duplicate source confirmed ring click");
  advance(f,45);auto out=apply(c,f);check(out.right.buttons==32 && out.right.press_counters[5]==1,"ring confirmation failed");
  f.right.status=0;advance(f,1);out=apply(c,f);check(out.right.buttons==0 && out.right.release_counters[5]==1,"lost hand kept click");
  f.right.status=1;advance(f,100);check(apply(c,f).right.buttons==0,"reacquired held ring bypassed release");
  f.right=pose(3,1);advance(f);apply(c,f);bend_finger(f.right,2,{85,100,100});advance(f);apply(c,f);advance(f,45);
  check(apply(c,f).right.buttons==32,"ring did not rearm after release");
}
void recorded_palm_and_thumb_activity_regressions() {
  const auto path=std::filesystem::path(__FILE__).parent_path()/"data/emulation_palm_thumb_poses.json";
  std::ifstream in(path);check(bool(in),"palm/thumb fixture missing");nlohmann::json data;in>>data;
  auto cfg=em::config_from_json(data.at("profile"));
  const auto load=[&](const nlohmann::json& item, bool rotated=false) {
    auto f=frame();
    for(bool left:{false,true}) {
      auto& h=left?f.left:f.right;h=pose(15,1,left);
      const auto& j=item.at("hands").at(left?"left":"right");
      h.status=j.at("status");h.flags=j.at("flags");h.confidence=j.at("confidence");
      for(size_t i=0;i<j.at("joints").size();++i) {
        auto p=j.at("joints").at(i).get<em::Vec3>();
        if(rotated)p={p[2]+1,p[1]+2,-p[0]+3};
        h.joints[i].px=p[0];h.joints[i].py=p[1];h.joints[i].pz=p[2];
      }
    }
    return f;
  };
  for(bool rotated:{false,true}) for(const auto& item:data.at("cases")) {
    auto f=load(item,rotated);em::Controller c(cfg);
    auto viewer=item.at("viewer").get<em::Vec3>();
    if(rotated)viewer={viewer[2]+1,viewer[1]+2,-viewer[0]+3};
    RuntimeControllerStateFrameV1 out{};
    for(int n=0;n<12;++n){advance(f,50);out={};c.apply(out,&f,f.timestamp_ns,0,false,&viewer);}
    for(bool left:{false,true}) {
      const auto& side=left?out.left:out.right;
      const uint64_t expected=item.at("expected_buttons").at(left?"left":"right");
      check(side.buttons==expected,"recorded palm click/menu/hidden thumb classification failed");
      check(std::hypot(side.thumbstick_x,side.thumbstick_y)<.001,"reserved palm or neutral leaked D-pad");
      if(expected)check(side.press_counters[expected==32?5:2]==1,"held palm repeated press");
    }
    if(item.at("id")==1 || item.at("id")==8) {
      out={};c.apply(out,&f,f.timestamp_ns,0,false,nullptr);
      check(out.left.buttons==0 && out.right.buttons==0,"lost HMD reference kept palm click/menu on duplicate hands");
    }
  }
  // Same source sample cannot satisfy confirmation; fresh samples can.
  auto f=load(data.at("cases").at(0));auto viewer=data.at("cases").at(0).at("viewer").get<em::Vec3>();
  em::Controller c(cfg);RuntimeControllerStateFrameV1 out{};
  c.apply(out,&f,f.timestamp_ns,0,false,&viewer);out={};
  c.apply(out,&f,f.timestamp_ns+60000000,0,false,&viewer);
  check(out.right.buttons==0,"duplicate palm sample advanced confirmation");
  advance(f,60);out={};c.apply(out,&f,f.timestamp_ns,0,false,&viewer);
  check(out.right.buttons==32,"fresh palm failed confirmation");
  out={};c.apply(out,&f,f.timestamp_ns,0,true,&viewer);
  check(out.right.buttons==0,"blocked gestures kept palm click");
  // One outward palm and one inward palm must never become Menu.
  auto mixed=load(data.at("cases").at(7));mixed.right=load(data.at("cases").at(0)).right;
  viewer=data.at("cases").at(7).at("viewer").get<em::Vec3>();
  em::Controller mixed_c(cfg);
  for(int n=0;n<12;++n){advance(mixed,50);out={};mixed_c.apply(out,&mixed,mixed.timestamp_ns,0,false,&viewer);}
  check(!(out.left.buttons&4) && !(out.right.buttons&4),"mixed-facing hands became Menu");
  // Hidden thumb veto wins even if a permissive back template also matches.
  auto hidden=load(data.at("cases").at(1));
  auto h=em::measure_hand(hidden.right,.3);auto conflict=cfg;
  conflict.right.thumb_back_pose.calibrated=true;
  conflict.right.thumb_back_pose.poses={h.thumb_pose};
  conflict.right.thumb_back_pose.ip_max_degrees=180;
  conflict.right.thumb_back_pose.ip_release_degrees=180;
  em::Controller hidden_c(conflict);out={};hidden_c.apply(out,&hidden,hidden.timestamp_ns);
  check(out.right.thumbstick_y==0 && !(out.right.buttons&CONTROLLER_BUTTON_DPAD_DOWN),"back template overrode tucked thumb");
  // A/B and ordinary gameplay still use their original rearm rules.
  auto bad=data.at("profile");bad["palm_facing_off"]=.9;
  bool rejected=false;try{em::config_from_json(bad);}catch(...){rejected=true;}
  check(rejected,"inverted palm thresholds accepted");
  bad=data.at("profile");bad["right"]["thumb_activity_calibration"]["neutral"]=bad["right"]["thumb_activity_calibration"]["tucked"];
  rejected=false;try{em::config_from_json(bad);}catch(...){rejected=true;}
  check(rejected,"overlapping hidden and neutral thumb poses accepted");
  bad=data.at("profile");bad["right"]["thumb_activity_calibration"]["tucked"][0].push_back(0);
  rejected=false;try{em::config_from_json(bad);}catch(...){rejected=true;}
  check(rejected,"wrong-sized thumb pose accepted");
}
void recorded_touchpad_and_back_regressions() {
  const auto path=std::filesystem::path(__FILE__).parent_path()/"data/emulation_touchpad_back_poses.json";
  std::ifstream in(path);check(bool(in),"touchpad/back fixture missing");nlohmann::json data;in>>data;
  for(bool left:{false,true}) for(bool rotate:{false,true}) {
    auto cfg=thumb_config(left,true);cfg.button_layout="middle_grip";cfg.thumbstick_click_gesture="ring_finger";
    cfg.thumb_invert_y=true;
    auto& bc=(left?cfg.left:cfg.right).thumb_back_pose;
    const auto& b=data.at("thumb_back_pose");bc.calibrated=true;bc.reference_invert_y=true;
    bc.poses=b.at("poses").get<std::vector<em::ThumbPose>>();
    bc.neutral_poses=b.at("neutral_poses").get<std::vector<em::ThumbPose>>();
    bc.neutral_distance=b.at("neutral_distance").get<double>();
    auto& rc=(left?cfg.left:cfg.right).ring_click_pose;rc.calibrated=true;
    rc.poses=data.at("ring_click_pose").at("poses").get<std::vector<em::RingPose>>();
    if(left)for(auto& p:rc.poses)for(size_t i=0;i<p.size();i+=3)p[i]=-p[i];
    if(left) for(auto* poses : {&bc.poses,&bc.neutral_poses})
      for(auto& p:*poses)for(size_t i=0;i<p.size();i+=3)p[i]=-p[i];
    em::Controller c(cfg);auto f=frame();
    for(const auto& item:data.at("cases")) {
      auto h=pose(3,1,left);
      for(size_t i=0;i<21;++i){auto p=item.at("joints").at(i).get<em::Vec3>();h.joints[i].px=left?-p[0]:p[0];h.joints[i].py=p[1];h.joints[i].pz=p[2];}
      if(rotate){tilt(h,.55,-.35);yaw(h,.8);}
      const auto measurement=em::measure_hand(h,.3);
      if(item.at("click").get<bool>())check(measurement.curl[2]>.9 && measurement.ring_angles_valid,
          "recorded click must exercise saturated generic curl");
      (left?f.left:f.right)=h;advance(f);auto out=apply(c,f);const auto side=left?out.left:out.right;
      check(bool(side.buttons&32)==item.at("click").get<bool>(),"recorded half-ring press/release failed");
      if(!item.at("back").get<bool>())
        check(std::hypot(side.thumbstick_x,side.thumbstick_y)<.001,"recorded neutral drifted while clicking or releasing");
      if(item.at("back").get<bool>()) {
        check(measurement.thumb_pad_clearance<(left?cfg.left:cfg.right).thumb.disable_clearance,
              "recorded back must be inside former hidden-thumb cutoff");
        check(side.thumbstick_y<-.99 && std::abs(side.thumbstick_x)<.001 && (side.buttons&CONTROLLER_BUTTON_DPAD_DOWN),
              "recorded curled thumb did not produce pure backward motion");
        auto opposite=cfg;opposite.thumb_invert_y=false;em::Controller other(opposite);auto flipped=apply(other,f);
        check((left?flipped.left:flipped.right).thumbstick_y>.99,"template ignored global thumb inversion");
        // A slightly off-template held pose keeps its hysteresis, but cannot
        // enter from rest. Outside the release radius the low IP cuts it off.
        const double width=std::hypot(std::hypot(h.joints[5].px-h.joints[17].px,h.joints[5].py-h.joints[17].py),h.joints[5].pz-h.joints[17].pz);
        auto nearby=h;nearby.joints[1].px+=float(.10*width);(left?f.left:f.right)=nearby;advance(f,1);
        auto held=apply(c,f);check((left?held.left:held.right).thumbstick_y<-.99,"back template lost hold hysteresis");
        em::Controller fresh(cfg);auto initial=apply(fresh,f);
        check((left?initial.left:initial.right).thumbstick_y==0,"back template entered outside press radius");
        auto far=h;far.joints[1].px+=float(.15*width);(left?f.left:f.right)=far;advance(f,1);
        auto released=apply(c,f);check((left?released.left:released.right).thumbstick_y==0,"out-of-template back failed release");
        (left?f.left:f.right)=h;advance(f);apply(c,f);
        auto blocked=out;c.apply(blocked,&f,f.timestamp_ns,0,true);
        check((left?blocked.left:blocked.right).buttons==0,"blocked gestures kept template back");
      }
    }
  }
  for(const auto& bad : {nlohmann::json{{"ring_click_mcp_min_degrees",170}},
                        nlohmann::json{{"ring_click_pip_max_degrees",-1}},
                        nlohmann::json{{"button_layout","legacy"},{"thumbstick_click_gesture","ring_finger"}}}) {
    bool rejected=false;try{em::config_from_json(bad);}catch(const std::exception&){rejected=true;}
    check(rejected,"invalid ring options accepted");
  }
  auto j=nlohmann::json{{"right",{{"thumb_back_pose",data.at("thumb_back_pose")}}}};
  check(em::config_from_json(j).right.thumb_back_pose.calibrated,"back template not parsed");
  j["right"]["thumb_back_pose"]["enabled"]=false;
  check(!em::config_from_json(j).right.thumb_back_pose.calibrated,"back template could not be disabled");
  j["right"]["thumb_back_pose"]["release_distance"]=.01;
  bool rejected=false;try{em::config_from_json(j);}catch(const std::exception&){rejected=true;}
  check(rejected,"invalid template release radius accepted");
}
void simple_palm_grip_regressions() {
  const auto path=std::filesystem::path(__FILE__).parent_path()/"data/emulation_palm_thumb_poses.json";
  std::ifstream in(path);check(bool(in),"palm/thumb fixture missing");nlohmann::json data;in>>data;
  for(bool rotated:{false,true}) for(const auto& item:data.at("cases")) {
    auto f=frame();
    auto viewer=item.at("viewer").get<em::Vec3>();
    if(rotated)viewer={viewer[2]+1,viewer[1]+2,-viewer[0]+3};
    for(bool left:{false,true}) {
      auto& h=left?f.left:f.right;
      const auto& j=item.at("hands").at(left?"left":"right");
      h.status=j.at("status");h.flags=j.at("flags");h.confidence=j.at("confidence");
      for(size_t i=0;i<j.at("joints").size();++i) {
        auto p=j.at("joints").at(i).get<em::Vec3>();
        if(rotated)p={p[2]+1,p[1]+2,-p[0]+3};
        h.joints[i].px=p[0];h.joints[i].py=p[1];h.joints[i].pz=p[2];
      }
      h.pinch_strength=.8f;h.pinch_active=1;
      h.grab_strength=1;h.grab_active=1; // Backend fist must not survive.
    }
    xr_runtime_adapter::gestures::SimplePalmGrip detector;
    detector.apply(f,&viewer,f.timestamp_ns,0,true,true);
    check(!f.left.grab_active && !f.right.grab_active,"palm confirmation skipped");
    detector.apply(f,&viewer,f.timestamp_ns+90000000,0,true,true);
    check(!f.left.grab_active && !f.right.grab_active,"duplicate hands confirmed Grip");
    advance(f,60);detector.apply(f,&viewer,f.timestamp_ns,0,true,true);
    for(bool left:{false,true}) {
      auto& h=left?f.left:f.right;
      const uint64_t expected=item.at("expected_buttons").at(left?"left":"right");
      check(h.grab_active==((expected&CONTROLLER_BUTTON_THUMBSTICK)?1u:0u),"simple Grip mismatches recorded open palm");
      check(h.pinch_active && h.pinch_strength==.8f,"simple Grip modified pinch");
    }
    detector.apply(f,nullptr,f.timestamp_ns,0,true,true);
    check(!f.left.grab_active && !f.right.grab_active,"lost HMD kept simple Grip on duplicate hands");
    advance(f,20);detector.apply(f,&viewer,f.timestamp_ns,0,true,true);
    advance(f,60);detector.apply(f,&viewer,f.timestamp_ns,0,true,true);
    detector.apply(f,&viewer,f.timestamp_ns,1,true,true);
    check(!f.left.grab_active && !f.right.grab_active,"origin reset kept Grip confirmation");
    f.right.grab_strength=.7f;f.right.grab_active=1;
    detector.apply(f,&viewer,f.timestamp_ns,1,false,false,false,true);
    check(!f.left.grab_active,"blocked side kept simple Grip");
    check(f.right.grab_active && f.right.grab_strength==.7f,"simple Grip overwrote physical grip");
    advance(f,60);detector.apply(f,&viewer,f.timestamp_ns,1,true,true);
    advance(f,60);detector.apply(f,&viewer,f.timestamp_ns,1,true,true);
    detector.apply(f,&viewer,f.timestamp_ns+200000000,1,true,true);
    check(!f.left.grab_active && !f.right.grab_active,"stale hands kept simple Grip");
  }
}

HandSideF32V2 simple_pose(int kind,bool left) {
  auto h=pose(kind==0?3:kind==4?1:0,kind==3?2:0,left);
  if(kind==0)for(size_t f=0;f<2;++f)for(size_t k=1;k<4;++k)
    h.joints[5+4*f+k].px+=float(double(k)*(left?-1.:1.)*(f==0?1.:-1.)*.012);
  if(kind==1) {
    const auto m=em::measure_hand(h,.3);const auto base=h.joints[5];
    for(size_t i=0;i<4;++i) {
      auto& j=h.joints[1+i];const double d=-.035+double(i)*(.11/3);
      j.px=base.px+.025*m.palm_basis[0][0]+d*m.palm_basis[1][0];
      j.py=base.py+.025*m.palm_basis[0][1]+d*m.palm_basis[1][1];
      j.pz=base.pz+.025*m.palm_basis[0][2]+d*m.palm_basis[1][2];
    }
  }
  return h;
}
HandSideF32V2 menu_point(bool left,bool inward=true) {
  auto h=simple_pose(4,left);const auto m=em::measure_hand(h,.3);
  const double sign=left||!inward?1:-1;
  for(auto& j:h.joints) {
    const em::Vec3 p{j.px,j.py,j.pz};em::Vec3 local{};
    for(size_t k=0;k<3;++k)for(size_t axis=0;axis<3;++axis)local[k]+=p[axis]*m.palm_basis[k][axis];
    j.px=float(sign*local[1]+(left?-.2:.2));j.py=float(local[2]+1);j.pz=float(sign*local[0]-.4);
  }
  return h;
}
RuntimeControllerStateFrameV1 simple_apply(xr_runtime_adapter::gestures::SimpleControllerGestures& c,
    HandTrackingFrameF32V2& f,bool left_pad=false,bool right_pad=false) {
  c.apply(f,nullptr,f.timestamp_ns,0,true,true,false,false,left_pad,right_pad);
  RuntimeControllerStateFrameV1 out{};
  for(bool left:{false,true}) {
    const auto& h=left?f.left:f.right;auto& s=left?out.left:out.right;
    s.buttons=h.reserved0|(h.pinch_active?CONTROLLER_BUTTON_TRIGGER:0)|(h.grab_active?CONTROLLER_BUTTON_GRIP:0);
    s.trigger=h.pinch_strength;s.grip=h.grab_strength;
  }
  c.publish(out);return out;
}
void simple_new_layout_and_conflicts() {
  using xr_runtime_adapter::gestures::SimpleControllerGestures;
  const uint64_t expected[]={CONTROLLER_BUTTON_A,CONTROLLER_BUTTON_B,
      CONTROLLER_BUTTON_TRIGGER|CONTROLLER_BUTTON_GRIP,CONTROLLER_BUTTON_TRIGGER};
  for(bool left:{false,true})for(int kind=0;kind<4;++kind)for(double angle:{0.,1.2,-1.0})for(double scale:{.8,1.2}) {
    SimpleControllerGestures c;auto f=frame();f.left.status=f.right.status=0;
    auto& h=left?f.left:f.right;h=simple_pose(kind,left);yaw(h,angle);
    for(auto& j:h.joints){j.px*=scale;j.py*=scale;j.pz*=scale;}
    h.pinch_strength=h.grab_strength=1;h.pinch_active=h.grab_active=1;h.reserved0=0xffff;
    auto out=simple_apply(c,f);check((left?out.left:out.right).buttons==0,"simple pose skipped confirmation");
    advance(f,44);out=simple_apply(c,f);check((left?out.left:out.right).buttons==0,"simple pose confirmed too soon");
    advance(f,1);out=simple_apply(c,f);auto s=left?out.left:out.right;
    if(s.buttons!=face(expected[kind],left)) {
      const auto m=em::measure_hand(h,.3);
      throw std::runtime_error("simple kind="+std::to_string(kind)+" left="+std::to_string(left)+
          " angle="+std::to_string(angle)+" actual="+std::to_string(s.buttons)+" curl="+
          std::to_string(m.curl[0])+","+std::to_string(m.curl[1])+","+std::to_string(m.curl[2])+","+std::to_string(m.curl[3]));
    }
    if(kind==2)check(s.trigger==1 && s.grip==1,"simple fist did not hold both axes");
    if(kind==3)check(s.trigger>0 && s.grip==0,"pinch mistaken for fist");
    const auto counters=s.press_counters[0]+s.press_counters[1]+s.press_counters[left?11:3]+s.press_counters[left?12:4];
    advance(f,60);out=simple_apply(c,f);s=left?out.left:out.right;
    check(counters==s.press_counters[0]+s.press_counters[1]+s.press_counters[left?11:3]+s.press_counters[left?12:4],"held simple gesture repeated presses");
    h.status=2;advance(f);out=simple_apply(c,f);check((left?out.left:out.right).buttons==0,"lost simple hand kept buttons");
  }
  SimpleControllerGestures c;auto f=frame();f.left=menu_point(true);f.right=menu_point(false);
  simple_apply(c,f);advance(f,45);auto out=simple_apply(c,f);
  check(out.left.buttons==CONTROLLER_BUTTON_MENU && out.right.buttons==0,"inward pointing did not make one Menu");
  for(auto* h:{&f.left,&f.right}){yaw(*h,.9);for(auto& j:h->joints){j.px+=.3f;j.py+=.2f;}}
  advance(f);out=simple_apply(c,f);check(out.left.buttons==CONTROLLER_BUTTON_MENU,"Menu depends on world orientation");
  f.left=menu_point(true);
  f.right=menu_point(false,false);advance(f);out=simple_apply(c,f);
  check(!(out.left.buttons&CONTROLLER_BUTTON_MENU),"parallel pointing kept Menu");
  f.right=menu_point(false);advance(f);simple_apply(c,f);advance(f,45);out=simple_apply(c,f,true,false);
  check(!(out.left.buttons&CONTROLLER_BUTTON_MENU),"touchpad hand joined Menu");
  f.left=simple_pose(2,true);f.left.pinch_active=f.left.grab_active=0;f.left.pinch_strength=f.left.grab_strength=0;
  f.left.reserved0=CONTROLLER_BUTTON_THUMBSTICK;advance(f,60);out=simple_apply(c,f,true,false);
  check(out.left.buttons==CONTROLLER_BUTTON_THUMBSTICK && out.left.trigger==0 && out.left.grip==0,"touchpad fist leaked gameplay channels");
  SimpleControllerGestures pinch;f=frame();f.left.status=0;f.right=simple_pose(3,false);
  simple_apply(pinch,f);
  pinch.apply(f,nullptr,f.timestamp_ns+90000000,0,true,true,false,false,false,false);
  check(!f.right.pinch_active,"duplicate sample confirmed simple pinch");
  advance(f,45);out=simple_apply(pinch,f);check(out.right.buttons==1,"pinch was not confirmed");
  f.right.joints[4].px=f.right.joints[8].px+.04f;
  advance(f);out=simple_apply(pinch,f);check(out.right.buttons==1,"pinch hysteresis released too soon");
  f.right.joints[4].px=f.right.joints[8].px+.06f;
  advance(f);out=simple_apply(pinch,f);check(out.right.buttons==0,"released pinch became thumbs-up/fist");
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
        nlohmann::json{{"thumb_invert_y", "true"}},
        nlohmann::json{{"thumb_back_on", .2}}, nlohmann::json{{"thumb_back_off", .9}},
        nlohmann::json{{"thumb_back_side_ratio", 1.1}},
        nlohmann::json{{"grip_release_threshold", .8}},
        nlohmann::json{{"grip_release_threshold", -.5}},
        nlohmann::json{{"thumbstick_click_gesture", "index_middle_little"}},
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
    thumb_skeleton_uses_base_motion_and_neutral_range();
    trigger_calibration_and_wrist_diagonals();
    middle_grip_three_channels_and_rings();
    three_finger_click_is_separate();
    ring_finger_independent_channels();
    recorded_touchpad_and_back_regressions();
    recorded_palm_and_thumb_activity_regressions();
    thumb_inversion_and_click_rearming();
    finger_pair_channels_and_click_motion_guard();
    recorded_finger_pair_regressions();
    stricter_thumb_back_and_little_click();
    grip_release_and_left_face_counters();
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
    simple_palm_grip_regressions();
    simple_new_layout_and_conflicts();
    validation_and_legacy();
    std::cout << "Wrist tilt, extension channels, photo poses and two-hand Menu passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
