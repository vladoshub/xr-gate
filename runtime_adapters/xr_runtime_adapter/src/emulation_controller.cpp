#include "emulation_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xr_runtime_adapter::emulation {
namespace {
using namespace xr_runtime;
constexpr double pi = 3.14159265358979323846;
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(const Vec3& p) { return std::sqrt(dot(p, p)); }
bool finite(const Vec3& p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
bool unit(Vec3& p) {
  const double n = length(p);
  if (!std::isfinite(n) || n < 1e-5) return false;
  p = mul(p, 1 / n);
  return true;
}
void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error("emulation controller: " + message);
}
void range(double v, double low, double high, const char* name) {
  require(std::isfinite(v) && v >= low && v <= high, std::string(name) + " out of range");
}
double forward_axis(double y, double scale) { return y > 0 ? std::min(y / scale, 1.0) : y; }
bool position(const HandSideF32V2& h, uint32_t i, double confidence, Vec3& p) {
  if (i >= std::min(uint32_t(h.joint_count), HAND_JOINT_COUNT_V2)) return false;
  const auto& j = h.joints[i];
  p = {j.px, j.py, j.pz};
  return j.joint_id == i && j.flags != 0 && finite(p) && std::isfinite(j.confidence) &&
         j.confidence >= confidence;
}
void validate_axes(const HandConfig& c) {
  require(finite(c.neutral) && finite(c.axis_x) && finite(c.axis_y),
          "non-finite wrist calibration");
  const double xx = dot(c.axis_x, c.axis_x), yy = dot(c.axis_y, c.axis_y),
               xy = dot(c.axis_x, c.axis_y);
  require(xx >= 0.0004 && yy >= 0.0004 && xx * yy - xy * xy > xx * yy * 0.04,
          "wrist directions too small or collinear; record wrist tilt in both axes");
  if (c.directional_calibration) {
    for (const auto& direction : c.directions)
      require(finite(direction) && dot(direction, direction) >= .0004,
              "calibrated wrist direction too small");
    for (const auto pair : {std::array<size_t, 2>{1, 2}, {2, 0}, {0, 3}, {3, 1}}) {
      const auto& a = c.directions[pair[0]];
      const auto& b = c.directions[pair[1]];
      const double aa = dot(a, a), bb = dot(b, b), ab = dot(a, b);
      require(aa * bb - ab * ab > aa * bb * .04,
              "adjacent wrist directions too small or collinear");
    }
  }
}
void parse_hand(const nlohmann::json& j, HandConfig& c) {
  require(j.is_object(), "hand profile must be an object");
  c.enabled = j.value("enabled", c.enabled);
  c.invert_x = j.value("invert_x", c.invert_x);
  c.invert_y = j.value("invert_y", c.invert_y);
  c.deadzone = j.value("deadzone", c.deadzone);
  if (c.deadzone != -1) range(c.deadzone, 0, .7, "hand.deadzone");
  if (j.contains("calibration")) {
    const auto& a = j.at("calibration");
    require(a.value("feature", "palm_gravity_v1") == "palm_gravity_v1",
            "unsupported wrist calibration feature");
    c.neutral = a.at("neutral").get<Vec3>();
    c.axis_x = a.at("axis_x").get<Vec3>();
    c.axis_y = a.at("axis_y").get<Vec3>();
    c.calibrated = true;
    if (a.contains("directions")) {
      const auto& d = a.at("directions");
      const char* names[] = {"left", "right", "up", "down"};
      for (size_t i = 0; i < 4; ++i) c.directions[i] = d.at(names[i]).get<Vec3>();
      c.directional_calibration = true;
    }
    validate_axes(c);
  }
  c.open_curl = j.value("open_curl", c.open_curl);
  c.closed_curl = j.value("closed_curl", c.closed_curl);
  if (j.contains("grip_calibration")) {
    const auto& g = j.at("grip_calibration");
    c.grip_open_curl = g.at("open_curl").get<double>();
    c.grip_closed_curl = g.at("closed_curl").get<double>();
    c.grip_calibration_finger = g.value("finger", std::string("little"));
    require(c.grip_calibration_finger == "little" || c.grip_calibration_finger == "middle",
            "invalid grip_calibration.finger");
    range(c.grip_open_curl, 0, 1, "grip_calibration.open_curl");
    range(c.grip_closed_curl, 0, 1, "grip_calibration.closed_curl");
    require(c.grip_closed_curl - c.grip_open_curl >= .15, "Grip calibration range too small");
    c.grip_calibrated = true;
  }
  if (j.contains("trigger_calibration")) {
    const auto& g = j.at("trigger_calibration");
    c.trigger_open_curl = g.at("open_curl").get<double>();
    c.trigger_closed_curl = g.at("closed_curl").get<double>();
    range(c.trigger_open_curl, 0, 1, "trigger_calibration.open_curl");
    range(c.trigger_closed_curl, 0, 1, "trigger_calibration.closed_curl");
    require(c.trigger_closed_curl - c.trigger_open_curl >= .15,
            "Trigger calibration range too small");
    c.trigger_calibrated = true;
  }
  if (j.contains("thumbstick_click_calibration")) {
    const auto& g = j.at("thumbstick_click_calibration");
    require(g.value("finger", std::string("little")) == "little",
            "thumbstick_click_calibration must use little finger");
    c.click_open_curl = g.at("open_curl").get<double>();
    c.click_closed_curl = g.at("closed_curl").get<double>();
    range(c.click_open_curl, 0, 1, "thumbstick_click_calibration.open_curl");
    range(c.click_closed_curl, 0, 1, "thumbstick_click_calibration.closed_curl");
    require(c.click_closed_curl - c.click_open_curl >= .15, "Click calibration range too small");
    c.click_calibrated = true;
  }
  if (j.contains("thumb_calibration")) {
    const auto& t = j.at("thumb_calibration");
    require(t.at("feature") == "thumb_pose_v1", "unsupported thumb calibration feature");
    auto& out = c.thumb;
    out.neutral = t.at("neutral").get<Vec3>();
    out.axis_x = t.at("axis_x").get<Vec3>();
    out.axis_y = t.at("axis_y").get<Vec3>();
    const char* names[] = {"left", "right", "up", "down"};
    for (size_t i = 0; i < 4; ++i) out.directions[i] = t.at("directions").at(names[i]).get<Vec3>();
    out.deadzone = t.value("deadzone", out.deadzone);
    out.enable_clearance = t.at("enable_clearance").get<double>();
    out.disable_clearance = t.at("disable_clearance").get<double>();
    range(out.deadzone, 0, .65, "thumb_calibration.deadzone");
    range(out.enable_clearance, .01, 3, "thumb_calibration.enable_clearance");
    range(out.disable_clearance, 0, out.enable_clearance - .01,
          "thumb_calibration.disable_clearance");
    HandConfig axes;
    axes.neutral = out.neutral;
    axes.axis_x = out.axis_x;
    axes.axis_y = out.axis_y;
    axes.directions = out.directions;
    axes.directional_calibration = true;
    validate_axes(axes);
    out.calibrated = true;
  }
  for (size_t i = 0; i < 4; ++i) {
    range(c.open_curl[i], 0, 1, "open_curl");
    range(c.closed_curl[i], 0, 1, "closed_curl");
    require(c.closed_curl[i] - c.open_curl[i] >= 0.15, "finger calibration range too small");
  }
}
}  // namespace
Config config_from_json(const nlohmann::json& j) {
  require(j.is_object(), "profile must be an object");
  const int version = j.value("version", 3);
  const auto has_calibration = [&](const char* side) {
    return j.contains(side) && j.at(side).is_object() && j.at(side).contains("calibration");
  };
  require(version == 3 || (version == 2 && !has_calibration("left") && !has_calibration("right")),
          "use profile version 3; earlier wrist calibrations use a different coordinate feature");
  for (const auto* obsolete :
       {"finger_mode", "thumb_mode", "wrist_weight", "pinch_min_extended_fingers"})
    require(!j.contains(obsolete), std::string("obsolete option ") + obsolete +
                                       "; use emulation_controller.json version 3");
  Config c;
  c.dpad_mode = j.value("dpad_mode", c.dpad_mode);
  c.dpad_source = j.value("dpad_source", c.dpad_source);
  c.thumb_dpad_mode = j.value("thumb_dpad_mode", c.thumb_dpad_mode);
  c.button_layout = j.value("button_layout", c.button_layout);
  c.thumbstick_click_gesture = j.value("thumbstick_click_gesture", c.thumbstick_click_gesture);
  c.thumbstick_click_on_curl = j.value("thumbstick_click_on_curl", c.thumbstick_click_on_curl);
  c.wrist_filter_enabled = j.value("wrist_filter_enabled", c.wrist_filter_enabled);
  c.trigger_on_curl = j.value("trigger_on_curl", c.trigger_on_curl);
  c.grip_on_curl = j.value("grip_on_curl", c.grip_on_curl);
  c.wrist_filter_degrees = j.value("wrist_filter_degrees", c.wrist_filter_degrees);
  c.pose_yaw_offset_degrees = j.value("pose_yaw_offset_degrees", c.pose_yaw_offset_degrees);
  c.range_degrees = j.value("range_degrees", c.range_degrees);
  c.deadzone = j.value("deadzone", c.deadzone);
  c.smoothing_ms = j.value("smoothing_ms", c.smoothing_ms);
  c.forward_range_scale = j.value("forward_range_scale", c.forward_range_scale);
  c.thumb_back_on = j.value("thumb_back_on", c.thumb_back_on);
  c.thumb_back_off = j.value("thumb_back_off", c.thumb_back_off);
  c.min_confidence = j.value("min_confidence", c.min_confidence);
  c.max_age_ms = j.value("max_age_ms", c.max_age_ms);
  c.extension_on = j.value("extension_on", c.extension_on);
  c.extension_off = j.value("extension_off", c.extension_off);
  c.little_extension_on = j.value("little_extension_on", c.little_extension_on);
  c.little_extension_off = j.value("little_extension_off", c.little_extension_off);
  c.curl_on = j.value("curl_on", c.curl_on);
  c.curl_off = j.value("curl_off", c.curl_off);
  c.button_confirm_ms = j.value("button_confirm_ms", c.button_confirm_ms);
  c.pose_confirm_ms = j.value("pose_confirm_ms", c.pose_confirm_ms);
  c.menu_hold_ms = j.value("menu_hold_ms", c.menu_hold_ms);
  c.pinch_on_ratio = j.value("pinch_on_ratio", c.pinch_on_ratio);
  c.pinch_off_ratio = j.value("pinch_off_ratio", c.pinch_off_ratio);
  c.thumb_enable_clearance = j.value("thumb_enable_clearance", c.thumb_enable_clearance);
  c.thumb_disable_clearance = j.value("thumb_disable_clearance", c.thumb_disable_clearance);
  c.thumb_extension_on = j.value("thumb_extension_on", c.thumb_extension_on);
  c.thumb_extension_off = j.value("thumb_extension_off", c.thumb_extension_off);
  require(c.dpad_mode == "off" || c.dpad_mode == "four_way" || c.dpad_mode == "eight_way",
          "invalid dpad_mode");
  require(c.dpad_source == "wrist" || c.dpad_source == "thumb", "invalid dpad_source");
  require(c.button_layout == "legacy" || c.button_layout == "middle_grip", "invalid button_layout");
  require(c.thumbstick_click_gesture == "layout_default" ||
              c.thumbstick_click_gesture == "three_fingers",
          "invalid thumbstick_click_gesture");
  require(c.thumbstick_click_gesture != "three_fingers" || c.button_layout == "middle_grip",
          "three_fingers click requires button_layout=middle_grip");
  require(c.thumb_dpad_mode == "off" || c.thumb_dpad_mode == "four_way" ||
              c.thumb_dpad_mode == "eight_way",
          "invalid thumb_dpad_mode");
  range(c.range_degrees, 5, 80, "range_degrees");
  range(c.deadzone, 0, 0.9, "deadzone");
  range(c.smoothing_ms, 0, 1000, "smoothing_ms");
  range(c.forward_range_scale, .25, 2, "forward_range_scale");
  range(c.thumb_back_on, .65, 1.5, "thumb_back_on");
  range(c.thumb_back_off, .45, c.thumb_back_on - .01, "thumb_back_off");
  range(c.wrist_filter_degrees, 0, 20, "wrist_filter_degrees");
  range(c.pose_yaw_offset_degrees, -180, 180, "pose_yaw_offset_degrees");
  range(c.min_confidence, 0, 1, "min_confidence");
  range(c.max_age_ms, 10, 1000, "max_age_ms");
  range(c.extension_on, 0.55, 1, "extension_on");
  range(c.extension_off, 0.1, c.extension_on - 0.01, "extension_off");
  range(c.little_extension_on, .5, 1, "little_extension_on");
  range(c.little_extension_off, .1, c.little_extension_on - .01, "little_extension_off");
  range(c.curl_on, 0.55, 1, "curl_on");
  range(c.curl_off, 0.1, c.curl_on - 0.01, "curl_off");
  range(c.button_confirm_ms, 0, 500, "button_confirm_ms");
  range(c.pose_confirm_ms, 0, 1000, "pose_confirm_ms");
  range(c.menu_hold_ms, 0, 2000, "menu_hold_ms");
  range(c.pinch_on_ratio, 0.01, 1, "pinch_on_ratio");
  range(c.pinch_off_ratio, c.pinch_on_ratio + 0.01, 2, "pinch_off_ratio");
  range(c.thumb_enable_clearance, 0.01, 3, "thumb_enable_clearance");
  range(c.thumb_disable_clearance, 0, c.thumb_enable_clearance - 0.01, "thumb_disable_clearance");
  range(c.thumb_extension_on, .5, 1, "thumb_extension_on");
  range(c.thumb_extension_off, .1, c.thumb_extension_on - .01, "thumb_extension_off");
  if (j.contains("left")) parse_hand(j.at("left"), c.left);
  if (j.contains("right")) parse_hand(j.at("right"), c.right);
  if (c.dpad_source == "thumb") {
    require(
        !c.left.enabled || c.left.thumb.calibrated,
        "left thumb D-pad needs thumb_calibration; record --controls and run calibrate-controls");
    require(
        !c.right.enabled || c.right.thumb.calibrated,
        "right thumb D-pad needs thumb_calibration; record --controls and run calibrate-controls");
  }
  return c;
}
Config load_config(const std::string& path) {
  if (path.empty()) return Config{};
  std::ifstream in(path);
  require(bool(in), "cannot open " + path);
  nlohmann::json j;
  in >> j;
  auto config = config_from_json(j);
  config.profile_path = path;
  return config;
}
HandMeasurement measure_hand(const HandSideF32V2& h, double confidence) {
  HandMeasurement out;
  for (uint32_t f = 0; f < 4; ++f) {
    std::array<Vec3, 4> p{};
    bool valid = true;
    for (uint32_t k = 0; k < 4; ++k) valid &= position(h, 5 + f * 4 + k, confidence, p[k]);
    if (!valid) continue;
    double chain = 0;
    std::array<Vec3, 3> bones{};
    for (size_t k = 0; k < 3; ++k) {
      bones[k] = sub(p[k + 1], p[k]);
      chain += length(bones[k]);
      valid &= unit(bones[k]);
    }
    if (!valid || chain < 0.015 || chain > 0.20) continue;
    const double chord_curl = std::clamp((0.95 - length(sub(p[3], p[0])) / chain) / 0.40, 0.0, 1.0);
    if (f == 0) out.index_chain_curl = chord_curl;
    // A partly closed finger may still have a long MCP-to-tip chord. Joint
    // angles detect that bend without depending on finger size or world pose.
    // PIP flexion determines extension; a hooked fingertip alone is not a fist.
    // The recording contains a straight little finger with ~90 degree DIP
    // flexion, which must not cancel an otherwise extended Grip finger.
    // A hooked little fingertip is already represented by chord shortening.
    // Counting its DIP angle again loses Grip even with an unfolded MCP/PIP.
    const double dip_weight = f == 3 ? 0 : .35;
    const double bend = 2 * std::acos(std::clamp(dot(bones[0], bones[1]), -1.0, 1.0)) +
                        dip_weight * std::acos(std::clamp(dot(bones[1], bones[2]), -1.0, 1.0));
    // Tiny, noisy phalanges can produce large joint angles while the whole
    // finger remains straight. Require actual shortening of the finger chord.
    const double angle_support = std::clamp((1 - length(sub(p[3], p[0])) / chain) / .1, 0.0, 1.0);
    const double angle_curl = std::clamp((bend * 180 / pi - 20) / 80, 0.0, 1.0) * angle_support;
    out.curl[f] = std::max(chord_curl, angle_curl);
  }
  Vec3 wrist{}, index{}, middle{}, little{}, tip{}, index_tip{};
  if (!position(h, 0, confidence, wrist) || !position(h, 5, confidence, index) ||
      !position(h, 9, confidence, middle) || !position(h, 17, confidence, little))
    return out;
  Vec3 across = sub(index, little), forward = sub(middle, wrist);
  const double width = length(across);
  if (width < 0.025 || width > 0.14 || !unit(forward)) return out;
  // Mirror the left anatomical axis to give both wrists consistent tilt signs.
  if (h.handedness == 1) across = mul(across, -1);
  across = sub(across, mul(forward, dot(across, forward)));
  if (!unit(across)) return out;
  Vec3 normal = cross(across, forward);
  if (!unit(normal)) return out;
  out.palm_basis = {across, forward, normal};
  out.palm_valid = true;
  for (uint32_t f = 0; f < 4; ++f) {
    if (out.curl[f] < 0) continue;
    Vec3 base{}, pip{};
    if (!position(h, 5 + 4 * f, confidence, base) || !position(h, 6 + 4 * f, confidence, pip))
      continue;
    Vec3 metacarpal = sub(base, wrist);
    metacarpal = sub(metacarpal, mul(normal, dot(metacarpal, normal)));
    if (!unit(metacarpal)) continue;
    const Vec3 proximal = sub(pip, base);
    // Also recognize flexion at the knuckle with otherwise straight phalanges.
    // Project into the flexion plane so spreading an open little finger is not
    // mistaken for curling it and does not prevent Grip or the two-hand Menu.
    const double bend = std::atan2(std::abs(dot(proximal, normal)), dot(proximal, metacarpal));
    // Real recordings show straight phalanges with 45-70 degree MCP flexion.
    // Count only a deeply folded knuckle as curl; otherwise index/little
    // extension disappears even though the finger itself is visibly straight.
    const double knuckle_curl = std::clamp((bend * 180 / pi - 65) / 20, 0.0, 1.0);
    if (f == 0) out.index_knuckle_curl = knuckle_curl;
    out.curl[f] = std::max(out.curl[f], knuckle_curl);
  }
  if (!position(h, 4, confidence, tip)) return out;
  std::array<Vec3, 3> thumb{};
  for (uint32_t i = 0; i < 3; ++i)
    if (!position(h, i + 1, confidence, thumb[i])) return out;
  const double chain = length(sub(thumb[1], thumb[0])) + length(sub(thumb[2], thumb[1])) +
                       length(sub(tip, thumb[2]));
  if (chain < .015 || chain > .20) return out;
  out.thumb_straightness = length(sub(tip, thumb[0])) / chain;
  // A folded thumb can remain far from the wrist/middle centreline because of
  // the width of its own base. Measure clearance from the index knuckle instead.
  out.thumb_clearance = length(sub(tip, index)) / width;
  out.thumb_valid = true;
  // Thumb steering is relative to the palm. The proximal direction measures
  // lateral sweep; signed IP flexion separates curling the tip from arching it
  // backwards. No world/wrist orientation or inferred nail visibility is used.
  Vec3 proximal = sub(thumb[2], thumb[1]), distal = sub(tip, thumb[2]);
  if (unit(proximal) && unit(distal)) {
    Vec3 flexion_axis = cross(normal, proximal);
    if (unit(flexion_axis)) {
      out.thumb_feature = {dot(proximal, across),
                           std::atan2(dot(cross(proximal, distal), flexion_axis),
                                      std::clamp(dot(proximal, distal), -1.0, 1.0)),
                           dot(proximal, forward)};
      // The IP stays away from the index knuckle while only the tip curls.
      // Using tip clearance here would confuse forward with a hidden thumb.
      out.thumb_pad_clearance = length(sub(thumb[2], index)) / width;
      out.thumb_pad_valid = finite(out.thumb_feature);
    }
  }
  if (position(h, 8, confidence, index_tip)) out.pinch_ratio = length(sub(tip, index_tip)) / width;
  return out;
}
Vec3 wrist_feature(const HandMeasurement& h, const Basis& reference) {
  (void)reference;
  return {h.palm_basis[0][1], h.palm_basis[1][1], h.palm_basis[2][1]};
}
void calibrate_axes(HandConfig& c, const Vec3& neutral, const Vec3& left, const Vec3& right,
                    const Vec3& up, const Vec3& down) {
  c.neutral = neutral;
  c.axis_x = mul(sub(right, left), 0.5);
  c.axis_y = mul(sub(up, down), 0.5);
  c.directions = {sub(left, neutral), sub(right, neutral), sub(up, neutral), sub(down, neutral)};
  c.directional_calibration = true;
  c.calibrated = true;
  validate_axes(c);
}
Vec3 project_wrist(const HandConfig& c, const Vec3& feature) {
  const Vec3 delta = sub(feature, c.neutral);
  const double xx = dot(c.axis_x, c.axis_x), yy = dot(c.axis_y, c.axis_y),
               xy = dot(c.axis_x, c.axis_y), det = xx * yy - xy * xy;
  double x = (dot(delta, c.axis_x) * yy - dot(delta, c.axis_y) * xy) / det;
  double y = (dot(delta, c.axis_y) * xx - dot(delta, c.axis_x) * xy) / det;
  if (c.directional_calibration) {
    const Vec3 stick[] = {Vec3{-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    double best = std::numeric_limits<double>::infinity();
    // Adjacent measured directions define four continuous interpolation sectors.
    // Each recorded cardinal maps to its own axis, even with asymmetric motion.
    for (const auto pair : {std::array<size_t, 2>{1, 2}, {2, 0}, {0, 3}, {3, 1}}) {
      const auto& a = c.directions[pair[0]];
      const auto& b = c.directions[pair[1]];
      const double aa = dot(a, a), bb = dot(b, b), ab = dot(a, b), determinant = aa * bb - ab * ab;
      double u = (dot(delta, a) * bb - dot(delta, b) * ab) / determinant;
      double v = (dot(delta, b) * aa - dot(delta, a) * ab) / determinant;
      if (u < 0) {
        u = 0;
        v = dot(delta, b) / bb;
      }
      if (v < 0) {
        v = 0;
        u = dot(delta, a) / aa;
      }
      u = std::max(0.0, u);
      v = std::max(0.0, v);
      const Vec3 residual = sub(sub(delta, mul(a, u)), mul(b, v));
      const double error = dot(residual, residual);
      if (error < best) {
        best = error;
        x = u * stick[pair[0]][0] + v * stick[pair[1]][0];
        y = u * stick[pair[0]][1] + v * stick[pair[1]][1];
      }
    }
  }
  if (c.invert_x) x = -x;
  if (c.invert_y) y = -y;
  return {x, y, 0};
}
Controller::Controller(Config c) : config_(std::move(c)) {}
Vec3 project_thumb(const ThumbCalibration& c, const Vec3& feature) {
  HandConfig axes;
  axes.neutral = c.neutral;
  axes.axis_x = c.axis_x;
  axes.axis_y = c.axis_y;
  axes.directions = c.directions;
  axes.directional_calibration = true;
  return project_wrist(axes, feature);
}
void Controller::clear_input(SideState& s) {
  s.buttons = s.touches = s.dpad = 0;
  s.x = s.y = 0;
  s.trigger = s.grip = 0;
  s.trigger_channel = {};
  s.grip_channel = {};
  s.click_channel = {};
  s.grip_finger_active = false;
  s.grip_extension = 0;
  s.trigger_finger_active = s.trigger_released = false;
  s.trigger_extension = 0;
  s.click_finger_active = s.click_released = false;
  s.click_extension = 0;
  s.thumb_back_active = false;
}
bool Controller::is_released_pose(const SideState& s) const {
  if (config_.button_layout == "middle_grip") {
    const bool grip_released =
        !s.grip_finger_active && (config_.grip_on_curl ? s.grip_extension > 1 - config_.curl_off
                                                       : s.grip_extension < config_.extension_off);
    return s.fingers[2] == Finger::Curled && s.trigger_released && grip_released &&
           (config_.thumbstick_click_gesture == "three_fingers" || s.click_released);
  }
  // Grip-only calibration may differ from the finger classification used by
  // A/B/V. A still-actuated calibrated Grip must not rearm after a special pose.
  const bool grip_released = config_.grip_on_curl
                                 ? !s.grip_finger_active && s.grip_extension > 1 - config_.curl_off
                                 : s.fingers[3] == Finger::Curled;
  return s.fingers[1] == Finger::Curled && s.fingers[2] == Finger::Curled && s.trigger_released &&
         grip_released;
}
void Controller::update_buttons(const HandMeasurement& h, SideState& s, uint64_t stamp) {
  const auto all = [&](Finger state) {
    return std::all_of(s.fingers.begin(), s.fingers.end(),
                       [state](Finger f) { return f == state; });
  };
  const bool neutral = is_released_pose(s);
  const bool was_pinch = s.pinch_active;
  // A fist can have nearly straight index phalanges and nearby thumb/index
  // tips while its knuckle is fully folded. This is not a pinch: accepting it
  // as A also leaves the following Grip waiting to rearm.
  s.pinch_active =
      h.index_chain_curl < .65 && h.index_knuckle_curl < 1 && h.thumb_valid &&
      h.thumb_clearance >= config_.thumb_disable_clearance && h.pinch_ratio >= 0 &&
      h.pinch_ratio <= (s.pinch_active ? config_.pinch_off_ratio : config_.pinch_on_ratio);
  s.menu_open = all(Finger::Extended) && h.thumb_valid &&
                (config_.button_layout == "legacy" || !s.pinch_active) &&
                h.thumb_clearance >= config_.thumb_enable_clearance &&
                h.thumb_straightness >= config_.thumb_extension_on;
  Pose pose = Pose::None;
  const bool separate_click = config_.thumbstick_click_gesture == "three_fingers";
  // Legacy V remains available only in the legacy layout. In middle_grip
  // this finger pose is ordinary input; A/B use the two disjoint ring poses.
  if (separate_click && h.thumb_valid && h.thumb_clearance <= config_.thumb_disable_clearance &&
      h.pinch_ratio > config_.pinch_off_ratio &&
      !s.pinch_active && s.fingers[0] == Finger::Extended &&
      s.fingers[1] == Finger::Extended && s.fingers[2] == Finger::Extended) {
    pose = Pose::PadClick;
  } else if (config_.button_layout == "legacy" && s.fingers[0] == Finger::Extended &&
      s.fingers[1] == Finger::Extended && s.fingers[2] == Finger::Curled &&
      s.fingers[3] == Finger::Curled) {
    pose = Pose::PadClick;
  } else if (s.pinch_active) {
    pose = Pose::Pinch;
    if (std::all_of(s.fingers.begin() + 1, s.fingers.end(),
                    [](Finger f) { return f == Finger::Curled; }))
      pose = config_.button_layout == "middle_grip" ? Pose::B : Pose::A;
    if (std::all_of(s.fingers.begin() + 1, s.fingers.end(),
                    [](Finger f) { return f == Finger::Extended; }))
      pose = config_.button_layout == "middle_grip" ? Pose::A : Pose::B;
  }
  // Even an unconfirmed three-finger candidate must return through the
  // released operating pose. Do not turn its transition into A/B or gameplay.
  if (separate_click && s.candidate == Pose::PadClick && pose != Pose::PadClick) {
    s.click_neutral_required = true;
    s.wait_for_neutral = true;
  }
  if ((s.active != Pose::None && pose != s.active) || (was_pinch && !s.pinch_active)) {
    s.wait_for_neutral = true;
    s.active = Pose::None;
  }
  if (s.wait_for_neutral && neutral && !s.pinch_active) {
    s.wait_for_neutral = false;
    s.click_neutral_required = false;
    s.candidate = Pose::None;
  }
  if (pose != s.candidate) {
    s.candidate = pose;
    s.pose_since_ns = stamp;
  }
  if (s.wait_for_neutral || pose != Pose::None) {
    s.trigger_channel = {};
    s.grip_channel = {};
    s.click_channel = {};
    // Rearming blocks gameplay channels, but must not lock A->B transitions:
    // an intermediate confirmed ring pose may occur while extending the others.
    const bool click_blocked = separate_click &&
        (s.click_neutral_required || (pose == Pose::PadClick && s.wait_for_neutral));
    if (!click_blocked && pose != Pose::None && pose != Pose::Pinch &&
        double(stamp - s.pose_since_ns) / 1e6 >= config_.pose_confirm_ms) {
      s.active = pose;
      if (pose == Pose::A) s.buttons = CONTROLLER_BUTTON_A;
      if (pose == Pose::B) s.buttons = CONTROLLER_BUTTON_B;
      if (pose == Pose::PadClick) s.buttons = CONTROLLER_BUTTON_THUMBSTICK;
      s.touches = s.buttons;
    }
    return;
  }
  // Legacy uses middle/ring as the operating pose. The new layout requires
  // only the ring finger, leaving all three button fingers independent.
  const bool operating = s.fingers[2] == Finger::Curled &&
                         (config_.button_layout == "middle_grip" || s.fingers[1] == Finger::Curled);
  const auto channel = [&](Channel& c, bool actuated, double strength, uint64_t bit, float& value) {
    const bool raw = operating && actuated;
    if (!raw) {
      c = {};
      return;
    }
    if (!c.candidate) {
      c.candidate = true;
      c.since_ns = stamp;
    }
    if (double(stamp - c.since_ns) / 1e6 >= config_.button_confirm_ms) c.active = true;
    if (c.active) {
      s.buttons |= bit;
      s.touches |= bit;
      value = std::max(0.55f, float(strength));
    }
  };
  channel(s.trigger_channel, s.trigger_finger_active,
          config_.trigger_on_curl ? 1 - s.trigger_extension : s.trigger_extension,
          CONTROLLER_BUTTON_TRIGGER, s.trigger);
  channel(s.grip_channel, s.grip_finger_active,
          config_.grip_on_curl ? 1 - s.grip_extension : s.grip_extension, CONTROLLER_BUTTON_GRIP,
          s.grip);
  if (config_.button_layout == "middle_grip" && !separate_click) {
    float ignored = 0;
    channel(s.click_channel, s.click_finger_active,
            config_.thumbstick_click_on_curl ? 1 - s.click_extension : s.click_extension,
            CONTROLLER_BUTTON_THUMBSTICK, ignored);
  }
}
void Controller::update_pad(const HandMeasurement& h, const HandConfig& c, SideState& s, double dt,
                            bool blocked) {
  const bool thumb_mode = config_.dpad_source == "thumb";
  if (thumb_mode) {
    s.thumb_active = !blocked && h.palm_valid && h.thumb_pad_valid && c.thumb.calibrated &&
                     h.thumb_pad_clearance >=
                         (s.thumb_active ? c.thumb.disable_clearance : c.thumb.enable_clearance);
  } else {
    s.thumb_active = !blocked && h.palm_valid && h.thumb_valid &&
                     h.thumb_clearance >= (s.thumb_active ? config_.thumb_disable_clearance
                                                          : config_.thumb_enable_clearance) &&
                     h.thumb_straightness >= (s.thumb_active ? config_.thumb_extension_off
                                                             : config_.thumb_extension_on);
  }
  if (!s.thumb_active) {
    s.x = s.y = 0;
    s.dpad = 0;
    s.thumb_back_active = false;
    return;
  }
  if (!thumb_mode && !s.anchored) {
    // Automatic neutral needs the released operating pose for the selected
    // Trigger/Grip activation directions. A saved calibration
    // already supplies the neutral and must work on startup with Trigger/Grip
    // held, including after source resets.
    if (!c.calibrated && !is_released_pose(s)) {
      s.thumb_active = false;
      return;
    }
    s.reference = h.palm_basis;
    s.neutral = c.calibrated ? c.neutral : wrist_feature(h, s.reference);
    const double limit = config_.range_degrees * pi / 180;
    Vec3 side = {-s.neutral[2], 0, s.neutral[0]};
    if (!c.calibrated && !unit(side)) {
      s.thumb_active = false;
      return;  // Wrist forward exactly vertical: roll is not observable yet.
    }
    s.axis_x = c.calibrated ? c.axis_x : mul(side, std::sin(limit));
    Vec3 forward_tilt = cross(s.neutral, side);
    if (!c.calibrated) unit(forward_tilt);
    s.axis_y = c.calibrated ? c.axis_y : mul(forward_tilt, std::sin(limit));
    s.anchored = true;
  }
  HandConfig projection = c;
  projection.neutral = s.neutral;
  projection.axis_x = s.axis_x;
  projection.axis_y = s.axis_y;
  const Vec3 axes = thumb_mode ? project_thumb(c.thumb, h.thumb_feature)
                               : project_wrist(projection, wrist_feature(h, s.reference));
  double x = axes[0], y = axes[1];
  if (thumb_mode) {
    // Back is close to the user's relaxed thumb pose. Gate the analog axis as
    // well as its button, using calibrated raw Y before radial normalization.
    // This also permits a full back diagonal (both components near 1).
    s.thumb_back_active =
        -y >= (s.thumb_back_active ? config_.thumb_back_off : config_.thumb_back_on);
    if (y < 0 && !s.thumb_back_active) y = 0;
  }
  const double deadzone = thumb_mode        ? c.thumb.deadzone
                          : c.deadzone >= 0 ? c.deadzone
                                            : config_.deadzone;
  const double radius = std::hypot(x, y),
               mapped = std::clamp((radius - deadzone) / (1 - deadzone), 0.0, 1.0);
  if (radius > 1e-8) {
    x *= mapped / radius;
    y *= mapped / radius;
  }
  const double alpha =
      config_.smoothing_ms > 0 && dt > 0 ? 1 - std::exp(-dt / config_.smoothing_ms) : 1;
  s.x += alpha * (x - s.x);
  s.y += alpha * (y - s.y);
  if (thumb_mode && !s.thumb_back_active && s.y < 0) s.y = 0;
  if (std::hypot(s.x, s.y) < 0.001) s.x = s.y = 0;
  // Scale only the final axis, after the calibrated neutral region, smoothing
  // and tiny-output cutoff. Preserve resting jitter suppression and filter state.
  const double output_y = forward_axis(s.y, thumb_mode ? 1 : config_.forward_range_scale);
  const auto& mode = thumb_mode ? config_.thumb_dpad_mode : config_.dpad_mode;
  if (mode != "off") {
    const auto direction = [&](double v, uint64_t positive, uint64_t negative) {
      const uint64_t bit = v >= 0 ? positive : negative;
      return std::abs(v) >= ((s.dpad & bit) ? 0.45 : 0.65) ? bit : uint64_t(0);
    };
    const bool horizontal = std::abs(s.x) >= std::abs(output_y);
    uint64_t directions = 0;
    if (mode == "eight_way" || horizontal)
      directions |= direction(s.x, CONTROLLER_BUTTON_DPAD_RIGHT, CONTROLLER_BUTTON_DPAD_LEFT);
    if (mode == "eight_way" || !horizontal)
      directions |= direction(output_y, CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_DOWN);
    s.dpad = directions;
  }
  s.buttons |= s.dpad;
  s.touches |= CONTROLLER_BUTTON_THUMBSTICK;
}
void Controller::update_side(const HandSideF32V2* hand, const HandConfig& c, SideState& s,
                             const HandTrackingFrameF32V2* frame, uint64_t now, uint64_t origin) {
  if (!c.enabled) return;
  const uint64_t stamp =
      frame ? (frame->source_timestamp_ns ? frame->source_timestamp_ns : frame->timestamp_ns) : 0;
  s.fresh = false;
  s.reset_event =
      frame && (frame->reset_counter != s.reset_counter || origin != s.origin_counter ||
                (s.have_sample && (stamp < s.sample_ns || frame->sequence < s.sequence)));
  if (s.reset_event) {
    clear_input(s);
    s.anchored = s.have_sample = false;
    s.fingers = {};
    s.pinch_active = s.wait_for_neutral = false;
    s.click_neutral_required = false;
    s.candidate = s.active = Pose::None;
  }
  if (frame) {
    s.reset_counter = frame->reset_counter;
    s.origin_counter = origin;
  }
  // Sub-millisecond clock/snapshot skew must not release a held button. Larger
  // future timestamps remain invalid, just as expired optical samples do.
  const bool current_time =
      stamp <= now ? double(now - stamp) / 1e6 <= config_.max_age_ms : stamp - now <= 2000000;
  s.valid = frame && frame->sequence != 0 && stamp != 0 && current_time && hand &&
            hand->status == 1 &&
            (hand->flags & (HAND_POSE_VALID | HAND_JOINTS_VALID)) ==
                (HAND_POSE_VALID | HAND_JOINTS_VALID) &&
            std::isfinite(hand->confidence) && hand->confidence >= config_.min_confidence;
  if (!s.valid) {
    clear_input(s);
    s.have_sample = s.menu_open = false;
    s.fingers = {};
    s.pinch_active = s.wait_for_neutral = false;
    s.click_neutral_required = false;
    s.candidate = s.active = Pose::None;
    return;
  }
  if (s.have_sample && stamp == s.sample_ns) return;
  const double dt = s.have_sample ? double(stamp - s.sample_ns) / 1e6 : 0;
  s.have_sample = s.fresh = true;
  s.sequence = frame->sequence;
  s.sample_ns = stamp;
  const auto h = measure_hand(*hand, config_.min_confidence);
  s.buttons = s.touches = 0;
  s.trigger = s.grip = 0;
  for (size_t i = 0; i < 4; ++i) {
    if (h.curl[i] < 0) {
      s.fingers[i] = Finger::Unknown;
      s.extension[i] = 0;
      continue;
    }
    const double curl =
        std::clamp((h.curl[i] - c.open_curl[i]) / (c.closed_curl[i] - c.open_curl[i]), 0.0, 1.0);
    s.extension[i] = 1 - curl;
    const double extension_on = i == 3 ? config_.little_extension_on : config_.extension_on;
    const double extension_off = i == 3 ? config_.little_extension_off : config_.extension_off;
    if (s.extension[i] >= (s.fingers[i] == Finger::Extended ? extension_off : extension_on))
      s.fingers[i] = Finger::Extended;
    else if (curl >= (s.fingers[i] == Finger::Curled ? config_.curl_off : config_.curl_on))
      s.fingers[i] = Finger::Curled;
    else
      s.fingers[i] = Finger::Unknown;
  }
  const bool middle_layout = config_.button_layout == "middle_grip";
  const size_t grip_finger = middle_layout ? 1 : 3;
  const bool grip_calibrated =
      c.grip_calibrated && c.grip_calibration_finger == (middle_layout ? "middle" : "little");
  s.grip_extension = h.curl[grip_finger] < 0 ? 0
                     : grip_calibrated ? 1 - std::clamp((h.curl[grip_finger] - c.grip_open_curl) /
                                                            (c.grip_closed_curl - c.grip_open_curl),
                                                        0.0, 1.0)
                                       : s.extension[grip_finger];
  s.trigger_extension = h.curl[0] < 0 ? 0
                        : c.trigger_calibrated
                            ? 1 - std::clamp((h.curl[0] - c.trigger_open_curl) /
                                                 (c.trigger_closed_curl - c.trigger_open_curl),
                                             0.0, 1.0)
                            : s.extension[0];
  const double trigger_strength =
      config_.trigger_on_curl ? 1 - s.trigger_extension : s.trigger_extension;
  const double trigger_on = config_.trigger_on_curl ? config_.curl_on : config_.extension_on;
  const double trigger_off = config_.trigger_on_curl ? config_.curl_off : config_.extension_off;
  s.trigger_finger_active =
      c.trigger_calibrated
          ? h.curl[0] >= 0 &&
                trigger_strength >= (s.trigger_finger_active ? trigger_off : trigger_on)
          : s.fingers[0] == (config_.trigger_on_curl ? Finger::Curled : Finger::Extended);
  s.trigger_released =
      c.trigger_calibrated
          ? h.curl[0] >= 0 && !s.trigger_finger_active &&
                (config_.trigger_on_curl ? s.trigger_extension > 1 - config_.curl_off
                                         : s.trigger_extension < config_.extension_off)
          : s.fingers[0] == (config_.trigger_on_curl ? Finger::Extended : Finger::Curled);
  const double grip_strength = config_.grip_on_curl ? 1 - s.grip_extension : s.grip_extension;
  const double grip_on = config_.grip_on_curl ? config_.curl_on
                         : middle_layout      ? config_.extension_on
                                              : config_.little_extension_on;
  const double grip_off = config_.grip_on_curl ? config_.curl_off
                          : middle_layout      ? config_.extension_off
                                               : config_.little_extension_off;
  s.grip_finger_active =
      h.curl[grip_finger] >= 0 && grip_strength >= (s.grip_finger_active ? grip_off : grip_on);
  if (config_.thumbstick_click_gesture == "three_fingers") {
    s.click_extension = 0;
    s.click_finger_active = false;
    s.click_released = true;
  } else {
    s.click_extension = h.curl[3] < 0 ? 0
                        : c.click_calibrated
                            ? 1 - std::clamp((h.curl[3] - c.click_open_curl) /
                                                 (c.click_closed_curl - c.click_open_curl),
                                             0.0, 1.0)
                            : s.extension[3];
    const double click_strength =
        config_.thumbstick_click_on_curl ? 1 - s.click_extension : s.click_extension;
    s.click_finger_active =
        h.curl[3] >= 0 &&
        click_strength >= (s.click_finger_active
                               ? (config_.thumbstick_click_on_curl ? config_.curl_off
                                                                   : config_.little_extension_off)
                               : (config_.thumbstick_click_on_curl ? config_.curl_on
                                                                   : config_.little_extension_on));
    s.click_released =
        h.curl[3] >= 0 && !s.click_finger_active &&
        (config_.thumbstick_click_on_curl ? s.click_extension > 1 - config_.curl_off
                                          : s.click_extension < config_.little_extension_off);
  }
  update_buttons(h, s, stamp);
  update_pad(h, c, s, dt, s.wait_for_neutral || s.candidate != Pose::None);
}
void Controller::update_menu() {
  const bool both = config_.left.enabled && config_.right.enabled && left_.valid && right_.valid &&
                    left_.menu_open && right_.menu_open &&
                    (config_.thumbstick_click_gesture != "three_fingers" ||
                     (!left_.click_neutral_required && !right_.click_neutral_required));
  if (!both || left_.reset_event || right_.reset_event) {
    if (menu_candidate_)
      for (auto* s : {&left_, &right_}) {
        clear_input(*s);
        s->wait_for_neutral = true;
      }
    menu_candidate_ = menu_active_ = false;
    menu_since_ns_ = menu_sample_ns_ = 0;
    return;
  }
  const uint64_t sample = std::min(left_.sample_ns, right_.sample_ns);
  if (!menu_candidate_) {
    menu_candidate_ = true;
    menu_since_ns_ = sample;
  }
  if (sample != menu_sample_ns_) {
    menu_sample_ns_ = sample;
    if (double(sample - menu_since_ns_) / 1e6 >= config_.menu_hold_ms) menu_active_ = true;
  }
  for (auto* s : {&left_, &right_}) {
    clear_input(*s);
    s->thumb_active = false;
  }
  if (menu_active_) {
    left_.buttons = CONTROLLER_BUTTON_MENU;
    left_.touches = CONTROLLER_BUTTON_MENU;
  }
}
void Controller::publish_side(RuntimeControllerSideStateV1& out, const HandConfig& c,
                              SideState& s) {
  if (!c.enabled) return;
  // The adapter has already removed legacy gestures from enabled sides. Retain
  // only input explicitly supplied by a fresh physical controller, never the
  // previous hand gesture fields.
  const bool physical = (out.source_mask & RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT) != 0;
  const bool physical_valid = physical && (out.flags & RUNTIME_CONTROLLER_INPUT_VALID) != 0;
  const bool comparable =
      physical && s.physical_present && out.source_device_hash == s.physical_device_hash;
  constexpr uint64_t directions = CONTROLLER_BUTTON_DPAD_UP | CONTROLLER_BUTTON_DPAD_DOWN |
                                  CONTROLLER_BUTTON_DPAD_LEFT | CONTROLLER_BUTTON_DPAD_RIGHT;
  const bool physical_pad =
      physical && (std::abs(out.thumbstick_x) > 0.0001f || std::abs(out.thumbstick_y) > 0.0001f ||
                   ((out.buttons | out.touches) &
                    (CONTROLLER_BUTTON_THUMBSTICK | CONTROLLER_BUTTON_DPAD_MASK)));
  // A moving/touched physical pad owns the entire pair of axes and directions,
  // including its centre. Never mix opposite physical and wrist directions.
  const uint64_t emulated = physical_pad ? s.buttons & ~directions : s.buttons;
  out.buttons = emulated | (physical ? out.buttons : 0);
  out.touches = s.touches | (physical ? out.touches : 0);
  out.trigger = std::max(s.trigger, physical ? out.trigger : 0.0f);
  out.grip = std::max(s.grip, physical ? out.grip : 0.0f);
  if (!physical_pad) {
    out.thumbstick_x = float(s.x);
    out.thumbstick_y =
        float(forward_axis(s.y, config_.dpad_source == "thumb" ? 1 : config_.forward_range_scale));
  }
  out.changed_buttons = out.buttons ^ s.previous_buttons;
  for (size_t i = 0; i < 32; ++i) {
    const uint64_t bit = uint64_t(1) << i;
    uint32_t pressed = 0, released = 0;
    if (comparable && ((emulated | s.previous_emulated_buttons) & bit) == 0) {
      // Preserve physical taps between adapter ticks. Historical counters on
      // first connection/reset are baselines; an overlapping held gesture must
      // not manufacture additional presses or releases of the merged button.
      pressed = uint32_t(out.press_counters[i] - s.physical_presses[i]);
      released = uint32_t(out.release_counters[i] - s.physical_releases[i]);
      if (pressed >= 0x80000000u || released >= 0x80000000u) pressed = released = 0;
    }
    s.physical_presses[i] = physical ? out.press_counters[i] : 0;
    s.physical_releases[i] = physical ? out.release_counters[i] : 0;
    if (out.changed_buttons & bit) {
      if (out.buttons & bit)
        pressed = std::max(pressed, uint32_t(1));
      else
        released = std::max(released, uint32_t(1));
    }
    if (pressed || released) out.changed_buttons |= bit;
    s.presses[i] += pressed;
    s.releases[i] += released;
    out.press_counters[i] = s.presses[i];
    out.release_counters[i] = s.releases[i];
  }
  s.physical_present = physical;
  s.physical_device_hash = physical ? out.source_device_hash : 0;
  s.previous_emulated_buttons = emulated;
  s.previous_buttons = out.buttons;
  out.flags &= ~(RUNTIME_CONTROLLER_INPUT_VALID | RUNTIME_CONTROLLER_INPUT_STALE);
  out.source_mask |= RUNTIME_CONTROLLER_SOURCE_HAND_GESTURES;
  if (s.valid || physical_valid) {
    out.flags |= RUNTIME_CONTROLLER_CONNECTED | RUNTIME_CONTROLLER_INPUT_VALID;
    if (s.valid) out.last_input_ns = std::max(out.last_input_ns, s.sample_ns);
  }
  std::snprintf(out.debug_source, sizeof(out.debug_source), "%s",
                physical ? "emulation_controller+override_controller" : "emulation_controller");
}
void Controller::apply(RuntimeControllerStateFrameV1& out, const HandTrackingFrameF32V2* hands,
                       uint64_t now, uint64_t origin, bool gestures_blocked) {
  // Use the adapter's existing stream/latch gate. Blocked recognition releases
  // emulated input and confirmation candidates, but physical input still passes
  // through publish_side. Optical controller poses are composed independently.
  update_side(hands && !gestures_blocked ? &hands->left : nullptr, config_.left, left_, hands, now,
              origin);
  update_side(hands && !gestures_blocked ? &hands->right : nullptr, config_.right, right_, hands,
              now, origin);
  update_menu();
  publish_side(out.left, config_.left, left_);
  publish_side(out.right, config_.right, right_);
  const auto flag = [&](bool enabled, const RuntimeControllerSideStateV1& side, uint32_t side_bit,
                        uint32_t frame_bit) {
    if (enabled) {
      out.flags &= ~frame_bit;
      if (side.flags & side_bit) out.flags |= frame_bit;
    }
  };
  flag(config_.left.enabled, out.left, RUNTIME_CONTROLLER_CONNECTED,
       RUNTIME_CONTROLLER_FRAME_LEFT_CONNECTED);
  flag(config_.right.enabled, out.right, RUNTIME_CONTROLLER_CONNECTED,
       RUNTIME_CONTROLLER_FRAME_RIGHT_CONNECTED);
  flag(config_.left.enabled, out.left, RUNTIME_CONTROLLER_INPUT_VALID,
       RUNTIME_CONTROLLER_FRAME_LEFT_INPUT_VALID);
  flag(config_.right.enabled, out.right, RUNTIME_CONTROLLER_INPUT_VALID,
       RUNTIME_CONTROLLER_FRAME_RIGHT_INPUT_VALID);
}
}  // namespace xr_runtime_adapter::emulation
