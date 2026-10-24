#include "emulation_controller.hpp"
#include "emulation_wrist_filter.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
namespace em = xr_runtime_adapter::emulation;
using Json = nlohmann::json;
uint64_t number(const Json& j) {
  return j.is_string() ? std::stoull(j.get<std::string>()) : j.get<uint64_t>();
}
xr_runtime::HandSideF32V2 side(const Json& j, uint32_t handedness) {
  xr_runtime::HandSideF32V2 out{};
  out.handedness = handedness;
  out.status = j.value("status", 0u); out.flags = j.value("flags", 0u);
  out.confidence = j.value("confidence", 0.0f);
  const auto pose = [&](const char* name, float& x, float& y, float& z,
                         float& w, float& qx, float& qy, float& qz) {
    if (!j.contains(name)) return;
    const auto& p = j.at(name);
    const auto xyz = p.at("position").get<std::array<float,3>>();
    const auto q = p.at("orientation_xyzw").get<std::array<float,4>>();
    x=xyz[0]; y=xyz[1]; z=xyz[2]; w=q[3]; qx=q[0]; qy=q[1]; qz=q[2];
  };
  pose("controller",out.controller_px,out.controller_py,out.controller_pz,out.controller_qw,out.controller_qx,out.controller_qy,out.controller_qz);
  pose("palm",out.palm_px,out.palm_py,out.palm_pz,out.palm_qw,out.palm_qx,out.palm_qy,out.palm_qz);
  pose("wrist",out.wrist_px,out.wrist_py,out.wrist_pz,out.wrist_qw,out.wrist_qx,out.wrist_qy,out.wrist_qz);
  if (!j.contains("joints")) return out;
  std::array<bool, 21> seen{};
  for (const auto& joint : j.at("joints")) {
    const auto id = joint.at("joint_id").get<uint32_t>();
    if (id >= 21 || seen[id]) throw std::runtime_error("invalid/duplicate joint_id");
    seen[id] = true;
    const auto p = joint.at("position").get<em::Vec3>();
    auto& o = out.joints[id];
    o.joint_id = id; o.flags = joint.value("flags", 0u);
    o.confidence = joint.value("confidence", 0.0f);
    o.px = float(p[0]); o.py = float(p[1]); o.pz = float(p[2]);
    if (joint.contains("orientation_xyzw")) {
      const auto q=joint.at("orientation_xyzw").get<std::array<float,4>>();
      o.qx=q[0]; o.qy=q[1]; o.qz=q[2]; o.qw=q[3];
    }
    out.joint_count = std::max(out.joint_count, id+1);
  }
  return out;
}
xr_runtime::HandTrackingFrameF32V2 hands(const Json& frame) {
  xr_runtime::HandTrackingFrameF32V2 out{};
  if (!frame.contains("hands") || frame.at("hands").is_null()) return out;
  const auto& j = frame.at("hands");
  out.version = 2; out.size_bytes = sizeof(out);
  out.sequence = number(j.at("sequence"));
  out.timestamp_ns = number(j.at("timestamp_ns"));
  out.source_timestamp_ns = number(j.at("source_timestamp_ns"));
  out.reset_counter = number(j.at("reset_counter"));
  out.left = side(j.at("left"), 1); out.right = side(j.at("right"), 2);
  return out;
}
bool valid(const xr_runtime::HandSideF32V2& h, const em::Config& c) {
  return h.status == 1 && (h.flags & 9u) == 9u && std::isfinite(h.confidence) && h.confidence >= c.min_confidence;
}
em::Vec3 median_feature(const std::vector<em::Vec3>& samples, const std::string& label) {
  if (samples.size() < 10) throw std::runtime_error("need at least 10 valid unique frames for " + label);
  em::Vec3 out{};
  for (size_t i = 0; i < 3; ++i) {
    std::vector<double> values;
    for (const auto& v : samples) values.push_back(v[i]);
    std::sort(values.begin(), values.end());
    out[i] = values[values.size()/2];
  }
  return out;
}
Json calibration(const std::map<std::string, std::vector<em::HandMeasurement>>& groups,
                 const em::Config& c, em::HandConfig& hc, const std::string& name) {
  const auto neutral_it = groups.find("neutral");
  if (neutral_it == groups.end() || neutral_it->second.empty()) throw std::runtime_error("no neutral frames for " + name);
  const auto first = std::find_if(neutral_it->second.begin(), neutral_it->second.end(), [](const auto& h) {
    return h.palm_valid;
  });
  if (first == neutral_it->second.end()) throw std::runtime_error("no valid neutral palm for " + name);
  const auto reference = first->palm_basis;
  std::map<std::string, em::Vec3> features;
  for (const auto* label : {"neutral", "left", "right", "up", "down"}) {
    std::vector<em::Vec3> samples;
    const auto found = groups.find(label);
    if (found != groups.end()) for (const auto& h : found->second)
      // These labels explicitly identify wrist positions. Thumb clearance is
      // an independently calibrated activation switch, not a wrist-quality
      // check; filtering it biases directions and can remove a whole pose.
      if (h.palm_valid)
        samples.push_back(em::wrist_feature(h, reference));
    features[label] = median_feature(samples, name + "." + label);
  }
  em::calibrate_axes(hc, features["neutral"], features["left"], features["right"], features["up"], features["down"]);
  Json directions;
  const char* names[] = {"left", "right", "up", "down"};
  for (size_t i = 0; i < 4; ++i) directions[names[i]] = hc.directions[i];
  std::vector<double> neutral_radii;
  for (const auto& h : neutral_it->second) {
    if (!h.palm_valid) continue;
    const auto projected = em::project_wrist(hc, em::wrist_feature(h, reference));
    neutral_radii.push_back(std::hypot(projected[0], projected[1]));
  }
  std::sort(neutral_radii.begin(), neutral_radii.end());
  hc.deadzone = std::clamp(neutral_radii[neutral_radii.size()*95/100]+.03, c.deadzone, .65);
  Json out{{"feature", "palm_gravity_v1"}, {"neutral", hc.neutral},
           {"axis_x", hc.axis_x}, {"axis_y", hc.axis_y}, {"directions", directions}};
  return out;
}
Json controller_json(const xr_runtime::RuntimeControllerSideStateV1& c, const em::HandMeasurement& m,
                     const em::HandConfig& config) {
  std::array<double, 4> extension{};
  for (size_t i = 0; i < 4; ++i)
    extension[i] = m.curl[i] < 0 ? -1 : 1 - std::clamp(
        (m.curl[i] - config.open_curl[i]) / (config.closed_curl[i] - config.open_curl[i]), 0.0, 1.0);
  return {{"buttons", std::to_string(c.buttons)}, {"touches", std::to_string(c.touches)},
          {"changed_buttons", std::to_string(c.changed_buttons)}, {"flags", c.flags},
          {"trigger", c.trigger}, {"grip", c.grip}, {"thumbstick", {c.thumbstick_x, c.thumbstick_y}},
          {"thumb_valid", m.thumb_valid}, {"thumb_clearance", m.thumb_clearance},
          {"thumb_straightness", m.thumb_straightness},
          {"pinch_ratio", m.pinch_ratio}, {"palm_valid", m.palm_valid},
          {"finger_order", {"index", "middle", "ring", "little"}},
          {"finger_curl", m.curl}, {"finger_extension", extension},
          {"grip_extension", m.curl[3] < 0 ? -1 : config.grip_calibrated
              ? 1-std::clamp((m.curl[3]-config.grip_open_curl) /
                  (config.grip_closed_curl-config.grip_open_curl),0.0,1.0) : extension[3]}};
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 4 || (std::string(argv[1]) != "replay" && std::string(argv[1]) != "calibrate" &&
                     std::string(argv[1]) != "calibrate-grip") ||
        (argc != 4 && argc != 6) || (argc == 6 && std::string(argv[4]) != "--config")) {
      std::cerr << "Usage: xr_emulation_tool replay|calibrate|calibrate-grip INPUT.jsonl OUTPUT.json[l] [--config PROFILE.json]\n";
      return 2;
    }
    Json profile = Json::object();
    if (argc == 6) { std::ifstream cfg(argv[5]); if (!cfg) throw std::runtime_error("cannot open profile"); cfg >> profile; }
    const auto config = em::config_from_json(profile);
    std::ifstream input(argv[2]);
    if (!input) throw std::runtime_error("cannot open input");
    { std::ifstream existing(argv[3]); if (existing.good()) throw std::runtime_error("output already exists; choose a new filename"); }
    const bool grip_only = std::string(argv[1]) == "calibrate-grip";
    if (grip_only && argc != 6) throw std::runtime_error("calibrate-grip requires --config CURRENT.json to preserve the other calibration");
    const bool calibrate = std::string(argv[1]) != "replay";
    em::Controller controller(config);
    em::WristPoseFilter wrist_filter;
    std::array<std::map<std::string, std::vector<em::HandMeasurement>>, 2> groups;
    std::array<uint64_t, 2> previous_stamp{};
    std::ofstream output;
    if (!calibrate) { output.open(argv[3]); if (!output) throw std::runtime_error("cannot open output"); }
    std::string line;
    size_t frames = 0, line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      if (line.empty()) continue;
      try {
        const auto frame = Json::parse(line);
        if (frame.value("type", "") != "frame") continue;
        auto h = hands(frame);
        if (!calibrate) wrist_filter.apply(h,config,number(frame.at("read_timestamp_ns")),0);
        std::array<em::HandMeasurement, 2> measurements{{
            em::measure_hand(h.left, config.min_confidence), em::measure_hand(h.right, config.min_confidence)}};
        if (calibrate) {
          const auto selected = frame.value("record_hand", "both");
          const auto label = frame.value("label", "free");
          const std::array<std::string, 2> names{{"left", "right"}};
          for (size_t i = 0; i < 2; ++i) {
            if ((selected != "both" && selected != names[i]) || h.source_timestamp_ns == previous_stamp[i] ||
                !valid(i == 0 ? h.left : h.right, config)) continue;
            previous_stamp[i] = h.source_timestamp_ns;
            groups[i][label].push_back(measurements[i]);
          }
        } else {
          xr_runtime::RuntimeControllerStateFrameV1 result{};
          controller.apply(result, h.sequence ? &h : nullptr, number(frame.at("read_timestamp_ns")));
          output << Json{{"type", "emulation"}, {"label", frame.value("label", "free")},
              {"source_timestamp_ns", std::to_string(h.source_timestamp_ns)},
              {"left", controller_json(result.left, measurements[0], config.left)},
              {"right", controller_json(result.right, measurements[1], config.right)}}.dump() << '\n';
        }
        ++frames;
      } catch (const std::exception& e) {
        throw std::runtime_error("line " + std::to_string(line_number) + ": " + e.what());
      }
    }
    if (frames == 0) throw std::runtime_error("no frame records found");
    if (calibrate) {
      if (grip_only) {
        bool any = false;
        for (size_t i=0;i<2;++i) {
          const auto* name = i == 0 ? "left" : "right";
          const auto& group = groups[i];
          if (!group.count("grip_open") && !group.count("grip_closed")) continue;
          std::array<double,2> ends{};
          for (size_t k=0;k<2;++k) {
            const auto* label = k == 0 ? "grip_open" : "grip_closed";
            std::vector<double> values;
            const auto found = group.find(label);
            if (found != group.end()) for (const auto& h : found->second)
              if (h.palm_valid && h.curl[3] >= 0) values.push_back(h.curl[3]);
            if (values.size() < 10) throw std::runtime_error(std::string("need 10 valid unique frames for ")+name+"."+label);
            std::sort(values.begin(),values.end());
            // Admit normal variations of an extended little finger, while a
            // genuine fist must still release. Overlapping poses are rejected.
            ends[k] = values[k == 0 ? values.size()*9/10 : values.size()/10];
          }
          if (ends[1]-ends[0] < .15) throw std::runtime_error(std::string(name)+
              ": Grip open/closed overlap in the tracked joints; keep the little finger visible and record again");
          profile[name]["grip_calibration"] = {{"open_curl",ends[0]},{"closed_curl",ends[1]}};
          any = true;
        }
        if (!any) throw std::runtime_error("no labelled grip_open/grip_closed samples");
        (void)em::config_from_json(profile);
        output.open(argv[3]);
        if (!output) throw std::runtime_error("cannot open output");
        output << profile.dump(2) << '\n';
      } else {
      profile["version"] = 3;
      for (size_t i = 0; i < 2; ++i) {
        const auto* name = i == 0 ? "left" : "right";
        if (groups[i].find("neutral") == groups[i].end()) {
          if (!profile.contains(name)) profile[name]["enabled"] = false;
          continue;
        }
        profile[name]["enabled"] = true;
        auto hc = i == 0 ? config.left : config.right;
        profile[name]["calibration"] = calibration(groups[i], config, hc, name);
        profile[name]["deadzone"] = hc.deadzone;
        // Open/closed endpoints normalize each finger independently when present.
        if (groups[i].count("open") && groups[i].count("closed")) {
          std::array<double, 4> open{}, closed{};
          for (size_t f = 0; f < 4; ++f) for (const auto* label : {"open", "closed"}) {
            size_t count = 0;
            std::vector<double> values;
            const std::vector<std::string> candidates = std::string(label) == "open"
                ? std::vector<std::string>{"open", "menu", f == 0 ? "trigger" : f == 3 ? "grip" : "button_b", "combo"}
                : std::vector<std::string>{"closed", "neutral"};
            for (const auto& candidate : candidates) {
              if (candidate == "combo" && f != 0 && f != 3) continue;
              const auto found = groups[i].find(candidate);
              if (found != groups[i].end()) for (const auto& m : found->second)
                if (m.curl[f] >= 0) values.push_back(m.curl[f]);
            }
            std::sort(values.begin(), values.end());
            count = values.size();
            if (count < 10) throw std::runtime_error("need 10 valid frames for each open/closed finger");
            (std::string(label) == "open" ? open : closed)[f] =
                values[std::string(label) == "open" ? values.size()/10 : values.size()*9/10];
          }
          profile[name]["open_curl"] = open; profile[name]["closed_curl"] = closed;
        }
      }
      if (!groups[0].count("neutral") && !groups[1].count("neutral")) throw std::runtime_error("no labelled neutral poses");
      // Validate before creating the final profile; no partial calibration files.
      (void)em::config_from_json(profile);
      output.open(argv[3]);
      if (!output) throw std::runtime_error("cannot open output");
      output << profile.dump(2) << '\n';
      }
    }
    output.flush();
    if (!output) throw std::runtime_error("output write failed");
    std::cerr << "Processed " << frames << " frames\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "xr_emulation_tool: " << e.what() << '\n';
    return 1;
  }
}
