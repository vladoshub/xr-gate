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
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("fixture path required");
    std::ifstream in(argv[1]);
    Json data;
    in >> data;
    whole_hand_filter(data);
    grip_calibration_is_independent(data);
    std::cout << "Whole-hand angular stability and independent Grip calibration passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
