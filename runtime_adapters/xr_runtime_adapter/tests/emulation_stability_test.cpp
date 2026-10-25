#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "emulation_wrist_filter.hpp"

namespace {
namespace em = xr_runtime_adapter::emulation;
using namespace xr_runtime;
using Json = nlohmann::json;
constexpr double pi = 3.14159265358979323846;
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
HandSideF32V2 hand(const Json& data, int phase, const char* side) {
  HandSideF32V2 h{};
  h.handedness = std::string(side) == "left" ? 1 : 2;
  h.status = 1;
  h.confidence = 1;
  h.flags = HAND_POSE_VALID | HAND_JOINTS_VALID | HAND_ANGULAR_VELOCITY_VALID;
  h.joint_count = 21;
  h.controller_qw = h.palm_qw = h.wrist_qw = 1;
  h.controller_py = h.palm_py = .03f;
  for (const auto& c : data.at("cases"))
    if (c.at("phase") == phase) {
      const auto& points = c.at("hands").at(side);
      for (size_t i = 0; i < 21; ++i) {
        auto& j = h.joints[i];
        j.joint_id = i;
        j.flags = 2;
        j.confidence = 1;
        j.qw = 1;
        j.px = points[i][0];
        j.py = points[i][1];
        j.pz = points[i][2];
      }
      return h;
    }
  throw std::runtime_error("fixture pose missing");
}
HandTrackingFrameF32V2 frame(HandSideF32V2 h, uint64_t sequence = 1) {
  HandTrackingFrameF32V2 f{};
  f.sequence = sequence;
  f.timestamp_ns = f.source_timestamp_ns = 1000000000 + sequence * 30000000;
  if (h.handedness == 1)
    f.left = h;
  else
    f.right = h;
  return f;
}
void turn(HandSideF32V2& h, double degrees, em::Vec3 translation = {}) {
  const double a = degrees * pi / 180, c = std::cos(a), s = std::sin(a);
  const auto point = [&](float& x, float& y, float& z) {
    const double old_x = x, old_z = z;
    x = float(c * old_x + s * old_z + translation[0]);
    y += translation[1];
    z = float(-s * old_x + c * old_z + translation[2]);
  };
  point(h.controller_px, h.controller_py, h.controller_pz);
  point(h.palm_px, h.palm_py, h.palm_pz);
  point(h.wrist_px, h.wrist_py, h.wrist_pz);
  h.controller_qw = h.palm_qw = h.wrist_qw = std::cos(a / 2);
  h.controller_qy = h.palm_qy = h.wrist_qy = std::sin(a / 2);
  for (auto& j : h.joints) {
    point(j.px, j.py, j.pz);
    j.qw = std::cos(a / 2);
    j.qy = std::sin(a / 2);
  }
}
double yaw(const HandSideF32V2& h) {
  return 2 * std::atan2(h.controller_qy, h.controller_qw) * 180 / pi;
}
void whole_hand_filter(const Json& data) {
  auto cfg = em::config_from_json(data.at("profile"));
  em::WristPoseFilter filter;
  const auto base = hand(data, 51, "right");
  auto f = frame(base);
  filter.apply(f, cfg, f.timestamp_ns, 0);
  auto moved = base;
  turn(moved, .7, {1, 2, 3});
  f = frame(moved, 2);
  const auto before = em::measure_hand(moved, .3);
  filter.apply(f, cfg, f.timestamp_ns, 0);
  check(std::abs(yaw(f.right)) < 1e-4, "subthreshold wrist rotation leaked");
  for (size_t i = 0; i < 21; ++i) {
    check(std::abs(f.right.joints[i].px - base.joints[i].px - 1) < 1e-5 &&
              std::abs(f.right.joints[i].py - base.joints[i].py - 2) < 1e-5 &&
              std::abs(f.right.joints[i].pz - base.joints[i].pz - 3) < 1e-5,
          "shared correction lost a bone or froze hand translation");
    check(std::abs(f.right.joints[i].qy) < 1e-5, "bone orientation was not corrected");
  }
  const auto after = em::measure_hand(f.right, .3);
  for (size_t i = 0; i < 4; ++i)
    check(std::abs(before.curl[i] - after.curl[i]) < 1e-4, "wrist filtering changed finger bend");
  check(f.right.wy == 0, "held wrist retained angular prediction velocity");
  // Articulation changes while the wrist remains inside its angular deadband.
  moved = hand(data, 55, "right");
  turn(moved, .7);
  f = frame(moved, 3);
  filter.apply(f, cfg, f.timestamp_ns, 0);
  check(em::measure_hand(f.right, .3).curl[3] < .4, "wrist filter froze little-finger extension");
  moved = base;
  turn(moved, 5);
  f = frame(moved, 4);
  filter.apply(f, cfg, f.timestamp_ns, 0);
  check(std::abs(yaw(f.right) - 3) < 1e-3,
        "angular deadband did not follow a deliberate movement continuously");
  f = frame(moved, 4);
  filter.apply(f, cfg, f.timestamp_ns, 0);
  check(std::abs(yaw(f.right) - 3) < 1e-3, "duplicate optical samples advanced the filter");
  f = frame(moved, 5);
  filter.apply(f, cfg, f.timestamp_ns, 1);
  check(std::abs(yaw(f.right) - 5) < 1e-3, "origin reset reused an old angular anchor");
  f = frame(moved, 6);
  const auto original = f.right;
  filter.apply(f, cfg, f.timestamp_ns, 1, false, false);
  check(!std::memcmp(&original, &f.right, sizeof(original)), "physical/blocked side was filtered");
  cfg.wrist_filter_enabled = false;
  f = frame(moved, 7);
  filter.apply(f, cfg, f.timestamp_ns, 1);
  check(!std::memcmp(&original, &f.right, sizeof(original)),
        "disabled wrist filter changed the hand");
  cfg.wrist_filter_enabled = true;
  f = frame(moved, 8);
  filter.apply(f, cfg, f.timestamp_ns + 121000000, 1);
  check(!std::memcmp(&original, &f.right, sizeof(original)), "stale hand was filtered");
  f = frame(base, 9);
  filter.apply(f, cfg, f.timestamp_ns, 1);
  check(std::abs(yaw(f.right)) < 1e-4, "stale input did not reset the anchor");
}
uint64_t buttons(const Json& data, const em::Config& cfg, int phase, const char* name) {
  em::Controller controller(cfg);
  RuntimeControllerStateFrameV1 output{};
  for (uint64_t i = 1; i <= 20; ++i) {
    auto f = frame(hand(data, phase, name), i);
    controller.apply(output, &f, f.timestamp_ns);
  }
  return std::string(name) == "left" ? output.left.buttons : output.right.buttons;
}
void flexion_channels_are_independent(const Json& data) {
  // Recorded poses: fist, index only, little only, index+little. Middle/ring
  // stay curled. The two activation directions are independently selectable.
  const int phases[] = {50, 51, 45, 55};
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto profile = data.at("profile");
    profile["trigger_on_curl"] = bool(mode & 1);
    profile["grip_on_curl"] = bool(mode & 2);
    const auto cfg = em::config_from_json(profile);
    check(cfg.trigger_on_curl == bool(mode & 1) && cfg.grip_on_curl == bool(mode & 2),
          "activation directions were not loaded from JSON");
    for (unsigned pose = 0; pose < 4; ++pose)
      check((buttons(data, cfg, phases[pose], "right") & 3) == (pose ^ mode),
            "Trigger/Grip activation truth table failed");
  }
  auto cfg = em::config_from_json(data.at("profile"));
  cfg.trigger_on_curl = cfg.grip_on_curl = true;
  em::Controller controller(cfg);
  RuntimeControllerStateFrameV1 output{};
  for (uint64_t i = 1; i <= 20; ++i) {
    auto f = frame(hand(data, 50, "right"), i);
    controller.apply(output, &f, f.timestamp_ns);
  }
  check((output.right.buttons & 3) == 3 && output.right.trigger >= .55 && output.right.grip >= .55,
        "folded fingers did not publish held analog buttons");
  auto lost = frame(hand(data, 50, "right"), 21);
  lost.right.joints[20].flags = 0;
  controller.apply(output, &lost, lost.timestamp_ns);
  check((output.right.buttons & 3) == 1 && output.right.grip == 0,
        "missing little finger was inverted into a Grip press");
  lost = frame(hand(data, 50, "right"), 22);
  lost.right.status = 0;
  controller.apply(output, &lost, lost.timestamp_ns);
  check((output.right.buttons & 3) == 0 && output.right.trigger == 0 && output.right.grip == 0,
        "lost hand retained flexion buttons");
  output = {};
  output.right.source_mask = RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
  output.right.flags = RUNTIME_CONTROLLER_CONNECTED | RUNTIME_CONTROLLER_INPUT_VALID;
  output.right.buttons = CONTROLLER_BUTTON_TRIGGER;
  output.right.trigger = .8f;
  controller.apply(output, &lost, lost.timestamp_ns, 0, true);
  check(output.right.buttons == CONTROLLER_BUTTON_TRIGGER && output.right.trigger == .8f &&
            output.right.grip == 0,
        "gesture inversion changed blocked physical controller input");
}
void flexion_neutral_and_specials(const Json& data) {
  auto cfg = em::config_from_json(data.at("profile"));
  const auto old_cfg = cfg;
  cfg.trigger_on_curl = cfg.grip_on_curl = true;
  for (const auto item : {std::pair<int, uint64_t>{26, CONTROLLER_BUTTON_A},
                          {28, CONTROLLER_BUTTON_B},
                          {30, CONTROLLER_BUTTON_THUMBSTICK},
                          {31, CONTROLLER_BUTTON_THUMBSTICK}})
    check(buttons(data, cfg, item.first, "left") == item.second,
          "flexion buttons conflicted with A/B/V");
  em::Controller controller(cfg);
  RuntimeControllerStateFrameV1 output{};
  uint64_t sequence = 0;
  const auto hold = [&](int phase, const char* side) {
    for (unsigned i = 0; i < 20; ++i) {
      auto f = frame(hand(data, phase, side), ++sequence);
      controller.apply(output, &f, f.timestamp_ns);
    }
  };
  hold(26, "left");
  hold(15, "left");
  check((output.left.buttons & 3) == 0, "fist rearmed folded buttons after A");
  hold(24, "left");
  check((output.left.buttons & 3) == 0, "released fingers were not a neutral in flexion mode");
  hold(15, "left");
  check((output.left.buttons & 3) == 3, "released pose did not rearm folded buttons after A");
  em::Controller old_dpad(old_cfg), new_dpad(cfg);
  RuntimeControllerStateFrameV1 before{}, after{};
  for (uint64_t i = 1; i <= 20; ++i) {
    auto f = frame(hand(data, 55, "right"), i);
    old_dpad.apply(before, &f, f.timestamp_ns);
    new_dpad.apply(after, &f, f.timestamp_ns);
  }
  check((before.right.buttons & 3) == 3 && (after.right.buttons & 3) == 0 &&
            before.right.thumbstick_x == after.right.thumbstick_x &&
            before.right.thumbstick_y == after.right.thumbstick_y,
        "finger inversion changed independent calibrated D-pad axes");
  cfg.right.calibrated = false;
  cfg.right.directional_calibration = false;
  em::Controller automatic(cfg);
  auto f = frame(hand(data, 55, "right"));
  after = {};
  automatic.apply(after, &f, f.timestamp_ns);
  check((after.right.touches & CONTROLLER_BUTTON_THUMBSTICK) && after.right.thumbstick_x == 0 &&
            after.right.thumbstick_y == 0,
        "automatic D-pad neutral did not accept released fingers in flexion mode");
  em::Controller menu(cfg);
  for (uint64_t i = 1; i <= 20; ++i) {
    f = frame(hand(data, 65, "left"), i);
    f.right = hand(data, 65, "right");
    menu.apply(after, &f, f.timestamp_ns);
  }
  check(after.left.buttons == CONTROLLER_BUTTON_MENU && after.right.buttons == 0,
        "flexion buttons conflicted with two-hand Menu");
  for (uint64_t i = 21; i <= 40; ++i) {
    f = frame(hand(data, 24, "left"), i);
    f.right = hand(data, 55, "right");
    menu.apply(after, &f, f.timestamp_ns);
  }
  check((after.left.buttons & 3) == 0 && (after.right.buttons & 3) == 0,
        "released pose after Menu pressed folded buttons");
  for (uint64_t i = 41; i <= 60; ++i) {
    f = frame(hand(data, 15, "left"), i);
    f.right = hand(data, 50, "right");
    menu.apply(after, &f, f.timestamp_ns);
  }
  check((after.left.buttons & 3) == 3 && (after.right.buttons & 3) == 3,
        "folded buttons did not rearm after two-hand Menu");

  cfg.left.grip_calibrated = true;
  cfg.left.grip_open_curl = 0;
  cfg.left.grip_closed_curl = .4;
  check((buttons(data, cfg, 24, "left") & 3) == 2, "folded Grip ignored its separate calibration");
  em::Controller calibrated(cfg);
  for (uint64_t i = 1; i <= 20; ++i) {
    f = frame(hand(data, 26, "left"), i);
    calibrated.apply(after, &f, f.timestamp_ns);
  }
  for (uint64_t i = 21; i <= 40; ++i) {
    f = frame(hand(data, 24, "left"), i);
    calibrated.apply(after, &f, f.timestamp_ns);
  }
  check((after.left.buttons & 3) == 0,
        "actuated calibrated Grip was incorrectly accepted as a released neutral after A");
}
void grip_calibration_is_independent(const Json& data) {
  auto cfg = em::config_from_json(data.at("profile"));
  cfg.little_extension_on = .7;
  check((buttons(data, cfg, 24, "left") & 3) == 1,
        "partial little-finger fixture unexpectedly activated Grip");
  const auto before = cfg;
  cfg.left.grip_calibrated = true;
  cfg.left.grip_open_curl = .3;
  cfg.left.grip_closed_curl = 1;
  check((buttons(data, cfg, 24, "left") & 3) == 3,
        "separate Grip calibration did not admit an extended hooked little finger");
  for (int phase : {26, 28, 30, 31})
    check(buttons(data, cfg, phase, "left") == buttons(data, before, phase, "left"),
          "Grip calibration changed A/B/V");
  check(buttons(data, cfg, 15, "left") == 0, "Grip calibration activated on a tracked fist");
  em::Controller menu(cfg);
  RuntimeControllerStateFrameV1 output{};
  for (uint64_t i = 1; i <= 20; ++i) {
    auto f = frame(hand(data, 65, "left"), i);
    f.right = hand(data, 65, "right");
    menu.apply(output, &f, f.timestamp_ns);
  }
  check(output.left.buttons == CONTROLLER_BUTTON_MENU && output.right.buttons == 0,
        "Grip calibration changed two-hand Menu");
}
void closed_knuckle_is_not_a_pinch(const Json& data) {
  const auto cfg = em::config_from_json(data.at("profile"));
  const auto fist = hand(data, 1005, "right");
  const auto measured = em::measure_hand(fist, .3);
  check(measured.index_chain_curl < .65 && measured.pinch_ratio < cfg.pinch_on_ratio,
        "recorded fist no longer reproduces the old pinch ambiguity");
  check(measured.index_knuckle_curl == 1, "recorded fist lost its folded index knuckle");
  em::Controller controller(cfg);
  RuntimeControllerStateFrameV1 output{};
  for (uint64_t i = 1; i <= 20; ++i) {
    auto f = frame(fist, i);
    controller.apply(output, &f, f.timestamp_ns);
    check((output.right.buttons & (CONTROLLER_BUTTON_A | CONTROLLER_BUTTON_B)) == 0,
          "closed knuckle was mistaken for a pinch button");
  }
  for (uint64_t i = 21; i <= 40; ++i) {
    auto f = frame(hand(data, 55, "right"), i);
    controller.apply(output, &f, f.timestamp_ns);
  }
  check((output.right.buttons & 3) == 3,
        "false pinch from the fist blocked the following Trigger/Grip combination");
  check(buttons(data, cfg, 26, "left") == CONTROLLER_BUTTON_A,
        "real recorded A was rejected by the knuckle guard");
  check(buttons(data, cfg, 28, "left") == CONTROLLER_BUTTON_B,
        "real recorded B was rejected by the knuckle guard");
}
void final_yaw_output(const Json& data) {
  auto profile = data.at("profile");
  check(em::config_from_json(profile).pose_yaw_offset_degrees == 0,
        "old profiles acquired an unsolicited yaw offset");
  profile["pose_yaw_offset_degrees"] = 45;
  auto cfg = em::config_from_json(profile);
  HandTrackingFrameF32V2 raw{};
  raw.left = hand(data, 24, "left");
  raw.right = hand(data, 55, "right");
  for (auto* h : {&raw.left, &raw.right}) {
    turn(*h, 13, {1, 2, 3});
    h->vx = .1f;
    h->vy = .2f;
    h->vz = .3f;
    h->wx = 1;
    h->wy = 2;
    h->wz = 3;
    // A pre-existing local Z mounting offset must remain before the new world Y yaw.
    const double a = 13 * pi / 360, b = 30 * pi / 360;
    h->controller_qw = h->palm_qw = h->wrist_qw = std::cos(a) * std::cos(b);
    h->controller_qx = h->palm_qx = h->wrist_qx = std::sin(a) * std::sin(b);
    h->controller_qy = h->palm_qy = h->wrist_qy = std::sin(a) * std::cos(b);
    h->controller_qz = h->palm_qz = h->wrist_qz = std::cos(a) * std::sin(b);
  }
  auto visual = raw;
  em::apply_pose_yaw_offset(visual, cfg);
  for (bool left : {false, true}) {
    const auto& before = left ? raw.left : raw.right;
    const auto& after = left ? visual.left : visual.right;
    const double degrees = left ? 45 : -45;
    const double a = (13 + degrees) * pi / 360, b = 30 * pi / 360;
    const em::Vec3 pivot{before.controller_px, before.controller_py, before.controller_pz};
    const double c = std::cos(degrees * pi / 180), s = std::sin(degrees * pi / 180);
    check(std::abs(after.controller_qw - std::cos(a) * std::cos(b)) < 1e-6 &&
              std::abs(after.controller_qx - std::sin(a) * std::sin(b)) < 1e-6 &&
              std::abs(after.controller_qy - std::sin(a) * std::cos(b)) < 1e-6 &&
              std::abs(after.controller_qz - std::cos(a) * std::sin(b)) < 1e-6,
          "final yaw used the wrong axis, sign or multiplication order");
    check(after.controller_px == before.controller_px &&
              after.controller_py == before.controller_py &&
              after.controller_pz == before.controller_pz && after.vx == before.vx &&
              after.vy == before.vy && after.vz == before.vz,
          "presentation yaw moved the controller pivot or linear velocity");
    for (size_t i = 0; i < 21; ++i) {
      const auto& j = before.joints[i];
      const auto& k = after.joints[i];
      check(std::abs(k.px - pivot[0] - c * (j.px - pivot[0]) - s * (j.pz - pivot[2])) < 1e-5 &&
                std::abs(k.py - j.py) < 1e-5 &&
                std::abs(k.pz - pivot[2] + s * (j.px - pivot[0]) - c * (j.pz - pivot[2])) < 1e-5,
            "final yaw broke skeleton rigidity or used the world origin as pivot");
    }
    const auto m0 = em::measure_hand(before, .3), m1 = em::measure_hand(after, .3);
    const em::Basis basis{{em::Vec3{1, 0, 0}, em::Vec3{0, 1, 0}, em::Vec3{0, 0, 1}}};
    const auto w0 = em::wrist_feature(m0, basis), w1 = em::wrist_feature(m1, basis);
    for (size_t i = 0; i < 4; ++i)
      check(std::abs(m0.curl[i] - m1.curl[i]) < 1e-4, "presentation yaw changed finger curl");
    for (size_t i = 0; i < 3; ++i)
      check(std::abs(w0[i] - w1[i]) < 1e-4 &&
                std::abs(m0.thumb_feature[i] - m1.thumb_feature[i]) < 1e-4,
            "presentation yaw changed wrist/thumb calibration features");
    check(std::abs(after.wx - (c + 3 * s)) < 1e-6 && after.wy == 2 &&
              std::abs(after.wz - (-s + 3 * c)) < 1e-6,
          "presentation yaw left angular velocity in the wrong frame");
    RuntimeControllerStateFrameV1 state{};
    auto& side = left ? state.left : state.right;
    side.flags = RUNTIME_CONTROLLER_POSE_VALID | RUNTIME_CONTROLLER_CONNECTED;
    side.source_mask =
        RUNTIME_CONTROLLER_SOURCE_HAND_ORIENTATION | RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;
    side.orientation_xyzw[0] = before.controller_qx;
    side.orientation_xyzw[1] = before.controller_qy;
    side.orientation_xyzw[2] = before.controller_qz;
    side.orientation_xyzw[3] = before.controller_qw;
    side.angular_velocity[0] = 1;
    side.angular_velocity[1] = 2;
    side.angular_velocity[2] = 3;
    side.buttons = 3;
    side.trigger = .75;
    side.thumbstick_y = .9;
    em::apply_pose_yaw_offset(state, cfg);
    check(side.orientation_xyzw[0] == after.controller_qx &&
              side.orientation_xyzw[1] == after.controller_qy &&
              side.orientation_xyzw[2] == after.controller_qz &&
              side.orientation_xyzw[3] == after.controller_qw &&
              side.angular_velocity[0] == after.wx && side.angular_velocity[2] == after.wz &&
              side.buttons == 3 && side.trigger == .75 && side.thumbstick_y == .9f,
          "controller output diverged from the visual hand or changed input");
    side.source_mask |= RUNTIME_CONTROLLER_SOURCE_CONTROLLER_IMU_ORIENTATION;
    const auto imu = state;
    em::apply_pose_yaw_offset(state, cfg);
    check(std::memcmp(&imu, &state, sizeof(state)) == 0,
          "emulation yaw changed a physical IMU pose");
  }
  cfg.pose_yaw_offset_degrees = 0;
  auto unchanged = raw;
  em::apply_pose_yaw_offset(unchanged, cfg);
  check(std::memcmp(&raw, &unchanged, sizeof(raw)) == 0, "zero yaw did not preserve exact output");
  cfg.pose_yaw_offset_degrees = 45;
  cfg.left.enabled = false;
  unchanged = raw;
  em::apply_pose_yaw_offset(unchanged, cfg, true, false);
  check(std::memcmp(&raw, &unchanged, sizeof(raw)) == 0, "disabled/IMU hands acquired an offset");

  cfg.left.enabled = true;
  HandTrackingFrameF64V1 legacy{};
  auto& h = legacy.left;
  h.status = 1;
  h.flags = HAND_POSE_VALID | HAND_JOINTS_VALID;
  h.palm_px = 1;
  h.palm_py = 2;
  h.palm_pz = 3;
  h.palm_qw = h.wrist_qw = 1;
  h.wrist_px = 1.1;
  h.wrist_py = 2;
  h.wrist_pz = 3;
  h.joint_count = HAND_JOINT_COUNT_V1;
  for (auto& j : h.joints) {
    j.flags = 1;
    j.px = 1.1;
    j.py = 2;
    j.pz = 3;
    j.qw = 1;
  }
  em::apply_pose_yaw_offset(legacy, cfg);
  check(h.palm_px == 1 && h.palm_py == 2 && h.palm_pz == 3 &&
            std::abs(h.wrist_px - 1 - .1 / std::sqrt(2)) < 1e-10 &&
            std::abs(h.joints[25].pz - 3 + .1 / std::sqrt(2)) < 1e-10,
        "legacy hand output used the wrong pivot or omitted a joint");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("fixture path required");
    std::ifstream in(argv[1]);
    Json data;
    in >> data;
    flexion_channels_are_independent(data);
    flexion_neutral_and_specials(data);
    whole_hand_filter(data);
    final_yaw_output(data);
    grip_calibration_is_independent(data);
    closed_knuckle_is_not_a_pinch(data);
    std::cout << "Whole-hand stability, Grip calibration and fist/pinch separation passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
