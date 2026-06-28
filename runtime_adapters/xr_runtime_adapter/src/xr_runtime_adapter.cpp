#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_runtime/net/tracking_net_v1.hpp>
#include <xr_runtime/net/runtime_output_udp.hpp>
#include <xr_runtime/contracts/controller_input_contract.hpp>
#include <xr_runtime/platform/udp_socket.hpp>
#include <xr_runtime/registry/runtime_paths.hpp>
#include <xr_runtime/publishers/runtime_pose_shm_publisher.hpp>
#include <xr_runtime/publishers/runtime_controller_state_shm_publisher.hpp>
#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>
#include <xr_tracking/contracts/hand_skeleton26_contract.hpp>
#include <xr_tracking/contracts/body_tracker_set_contract.hpp>
#include <xr_video/contracts/stereo_video_contract.hpp>
#include <xr_video/shm/stereo_video_shm_reader.hpp>
#include <xr_video/shm/stereo_video_shm_publisher.hpp>
#include <xr_video/net/stereo_video_tcp.hpp>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>
#include <xr_spatial/publishers/runtime_spatial_proxy_mesh_shm_publisher.hpp>
#include <chrono>

#include "gestures.hpp"
#include "override_controller.hpp"
#include "coordinate_util.hpp"
#include "hand_pose_stability_filter.hpp"
#include "hmd_pose_stability_filter.hpp"
#include "jitter_filter.hpp"
#include "body_tracker_stability_filter.hpp"
#include "platform/runtime_platform.hpp"

namespace {

namespace gestures = ::xr_runtime_adapter::gestures;
namespace override_controller = ::xr_runtime_adapter::override_controller;
namespace hand_filter = ::xr_runtime_adapter::hand_filter;
namespace hmd_filter = ::xr_runtime_adapter::hmd_filter;
namespace jitter_filter = ::xr_runtime_adapter::jitter_filter;
namespace body_tracker_filter = ::xr_runtime_adapter::body_tracker_filter;

using ::xr_runtime_adapter::body_tracker_filter::BodyTrackerStabilityFilter;
using ::xr_runtime_adapter::body_tracker_filter::BodyTrackerStabilityGateConfig;
using ::xr_runtime_adapter::body_tracker_filter::parse_body_tracker_predicted_status;
using ::xr_runtime_adapter::coordinate_util::Qd;
using ::xr_runtime_adapter::coordinate_util::RuntimeOriginSnapshot;
using ::xr_runtime_adapter::coordinate_util::RuntimeOriginState;
using ::xr_runtime_adapter::coordinate_util::StreamTransformConfig;
using ::xr_runtime_adapter::coordinate_util::TrackingTransformConfig;
using ::xr_runtime_adapter::coordinate_util::V3d;
using ::xr_runtime_adapter::coordinate_util::apply_body_tracker_frame_transform;
using ::xr_runtime_adapter::coordinate_util::apply_body_tracker_origin_transform;
using ::xr_runtime_adapter::coordinate_util::apply_hand_frame_transform;
using ::xr_runtime_adapter::coordinate_util::apply_hmd_pose_transform;
using ::xr_runtime_adapter::coordinate_util::apply_stream_position_transform;
using ::xr_runtime_adapter::coordinate_util::apply_stream_vector_transform;
using ::xr_runtime_adapter::coordinate_util::load_tracking_transform_config;
using ::xr_runtime_adapter::coordinate_util::log_stream_transform;
using ::xr_runtime_adapter::coordinate_util::normalize_q;
using ::xr_runtime_adapter::coordinate_util::q_conj;
using ::xr_runtime_adapter::coordinate_util::q_rotate;

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }


struct ControllerOverrideFileConfig {
  bool present = false;

  bool has_mode = false;
  std::string mode;

  bool has_lost_hand_pose_fallback = false;
  std::string lost_hand_pose_fallback;

  bool has_publish_runtime_controller_state_shm = false;
  bool publish_runtime_controller_state_shm = false;

  bool has_runtime_controller_state_registry = false;
  std::string runtime_controller_state_registry;
  bool has_runtime_controller_state_stream = false;
  std::string runtime_controller_state_stream;
  bool has_runtime_controller_state_shm_name = false;
  std::string runtime_controller_state_shm_name;
  bool has_runtime_controller_state_slots = false;
  uint32_t runtime_controller_state_slots = 0;

  bool has_left_hmd_relative_offset = false;
  std::array<float, 3> left_hmd_relative_offset_m{};
  bool has_right_hmd_relative_offset = false;
  std::array<float, 3> right_hmd_relative_offset_m{};

  bool has_left_static_orientation = false;
  std::array<float, 4> left_static_orientation_xyzw{0.0f, 0.0f, 0.0f, 1.0f};
  bool has_right_static_orientation = false;
  std::array<float, 4> right_static_orientation_xyzw{0.0f, 0.0f, 0.0f, 1.0f};

  bool has_left_hand_gestures_enabled = false;
  bool left_hand_gestures_enabled = true;
  bool has_right_hand_gestures_enabled = false;
  bool right_hand_gestures_enabled = true;
};

std::optional<std::array<float, 3>> json_vec3f_optional(const nlohmann::json& j,
                                                        const char* context) {
  if (j.is_null()) return std::nullopt;
  std::array<float, 3> out{0.0f, 0.0f, 0.0f};
  if (j.is_array()) {
    if (j.size() != 3) {
      throw std::runtime_error(std::string(context) + " must be a 3-element array or {x,y,z} object");
    }
    for (size_t i = 0; i < 3; ++i) out[i] = j.at(i).get<float>();
    return out;
  }
  if (j.is_object()) {
    out[0] = j.value("x", out[0]);
    out[1] = j.value("y", out[1]);
    out[2] = j.value("z", out[2]);
    return out;
  }
  throw std::runtime_error(std::string(context) + " must be a 3-element array or {x,y,z} object");
}

std::optional<std::array<float, 3>> json_euler_deg_optional(const nlohmann::json& j,
                                                              const char* context) {
  if (j.is_null()) return std::nullopt;
  std::array<float, 3> out{0.0f, 0.0f, 0.0f};
  if (j.is_array()) {
    if (j.size() != 3) {
      throw std::runtime_error(std::string(context) +
                               " must be [roll_deg,pitch_deg,yaw_deg] or an object");
    }
    for (size_t i = 0; i < 3; ++i) out[i] = j.at(i).get<float>();
    return out;
  }
  if (j.is_object()) {
    // Prefer explicit roll/pitch/yaw names; accept x/y/z aliases for compact configs.
    out[0] = j.value("roll", j.value("x", out[0]));
    out[1] = j.value("pitch", j.value("y", out[1]));
    out[2] = j.value("yaw", j.value("z", out[2]));
    return out;
  }
  throw std::runtime_error(std::string(context) +
                           " must be [roll_deg,pitch_deg,yaw_deg] or an object");
}

std::array<float, 4> normalize_quat_xyzw(std::array<float, 4> q) {
  const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!std::isfinite(n) || n <= 0.0f) return {0.0f, 0.0f, 0.0f, 1.0f};
  for (float& v : q) v /= n;
  return q;
}

std::optional<std::array<float, 4>> json_quat_xyzw_optional(const nlohmann::json& j,
                                                            const char* context) {
  if (j.is_null()) return std::nullopt;
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
  if (j.is_array()) {
    if (j.size() != 4) {
      throw std::runtime_error(std::string(context) +
                               " must be [x,y,z,w] or an object");
    }
    for (size_t i = 0; i < 4; ++i) out[i] = j.at(i).get<float>();
    return normalize_quat_xyzw(out);
  }
  if (j.is_object()) {
    out[0] = j.value("x", out[0]);
    out[1] = j.value("y", out[1]);
    out[2] = j.value("z", out[2]);
    out[3] = j.value("w", out[3]);
    return normalize_quat_xyzw(out);
  }
  throw std::runtime_error(std::string(context) +
                           " must be [x,y,z,w] or an object");
}

std::array<float, 4> euler_deg_to_quat_xyzw(const std::array<float, 3>& euler_deg) {
  // roll/pitch/yaw are rotations around X/Y/Z in degrees, composed as Z * Y * X.
  constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
  const double roll = static_cast<double>(euler_deg[0]) * kDegToRad;
  const double pitch = static_cast<double>(euler_deg[1]) * kDegToRad;
  const double yaw = static_cast<double>(euler_deg[2]) * kDegToRad;

  const double cr = std::cos(roll * 0.5);
  const double sr = std::sin(roll * 0.5);
  const double cp = std::cos(pitch * 0.5);
  const double sp = std::sin(pitch * 0.5);
  const double cy = std::cos(yaw * 0.5);
  const double sy = std::sin(yaw * 0.5);

  std::array<float, 4> out{
      static_cast<float>(cy * cp * sr - sy * sp * cr),
      static_cast<float>(sy * cp * sr + cy * sp * cr),
      static_cast<float>(sy * cp * cr - cy * sp * sr),
      static_cast<float>(cy * cp * cr + sy * sp * sr),
  };
  return normalize_quat_xyzw(out);
}

void set_static_orientation(ControllerOverrideFileConfig& out,
                            bool left,
                            const std::array<float, 4>& q_xyzw) {
  if (left) {
    out.left_static_orientation_xyzw = q_xyzw;
    out.has_left_static_orientation = true;
  } else {
    out.right_static_orientation_xyzw = q_xyzw;
    out.has_right_static_orientation = true;
  }
}

void parse_controller_override_offsets(const nlohmann::json& j,
                                       ControllerOverrideFileConfig& out,
                                       const char* context) {
  if (!j.is_object()) return;
  if (j.contains("left_offset_m")) {
    const auto v = json_vec3f_optional(j.at("left_offset_m"), (std::string(context) + ".left_offset_m").c_str());
    if (v) {
      out.left_hmd_relative_offset_m = *v;
      out.has_left_hmd_relative_offset = true;
    }
  }
  if (j.contains("right_offset_m")) {
    const auto v = json_vec3f_optional(j.at("right_offset_m"), (std::string(context) + ".right_offset_m").c_str());
    if (v) {
      out.right_hmd_relative_offset_m = *v;
      out.has_right_hmd_relative_offset = true;
    }
  }
}

void parse_static_orientation_side(const nlohmann::json& j,
                                       ControllerOverrideFileConfig& out,
                                       bool left,
                                       const std::string& context) {
  if (!j.is_object()) return;

  const char* euler_keys[] = {
      "euler_deg",
      "orientation_euler_deg",
      "static_euler_deg",
      "controller_euler_deg",
  };
  for (const char* key : euler_keys) {
    if (j.contains(key)) {
      const auto euler = json_euler_deg_optional(j.at(key), (context + "." + key).c_str());
      if (euler) set_static_orientation(out, left, euler_deg_to_quat_xyzw(*euler));
      return;
    }
  }

  const char* quat_keys[] = {
      "quat_xyzw",
      "quaternion_xyzw",
      "orientation_xyzw",
      "static_quat_xyzw",
  };
  for (const char* key : quat_keys) {
    if (j.contains(key)) {
      const auto q = json_quat_xyzw_optional(j.at(key), (context + "." + key).c_str());
      if (q) set_static_orientation(out, left, *q);
      return;
    }
  }
}

void parse_controller_override_static_orientation(const nlohmann::json& j,
                                                  ControllerOverrideFileConfig& out,
                                                  const char* context) {
  if (!j.is_object()) return;

  auto parse_euler_key = [&](const char* key, bool left) {
    if (!j.contains(key)) return false;
    const auto euler = json_euler_deg_optional(j.at(key), (std::string(context) + "." + key).c_str());
    if (euler) set_static_orientation(out, left, euler_deg_to_quat_xyzw(*euler));
    return true;
  };

  auto parse_quat_key = [&](const char* key, bool left) {
    if (!j.contains(key)) return false;
    const auto q = json_quat_xyzw_optional(j.at(key), (std::string(context) + "." + key).c_str());
    if (q) set_static_orientation(out, left, *q);
    return true;
  };

  parse_euler_key("left_euler_deg", true);
  parse_euler_key("right_euler_deg", false);
  parse_euler_key("left_orientation_euler_deg", true);
  parse_euler_key("right_orientation_euler_deg", false);
  parse_euler_key("left_static_euler_deg", true);
  parse_euler_key("right_static_euler_deg", false);

  parse_quat_key("left_quat_xyzw", true);
  parse_quat_key("right_quat_xyzw", false);
  parse_quat_key("left_orientation_xyzw", true);
  parse_quat_key("right_orientation_xyzw", false);
  parse_quat_key("left_static_quat_xyzw", true);
  parse_quat_key("right_static_quat_xyzw", false);

  if (j.contains("left")) {
    parse_static_orientation_side(j.at("left"), out, true, std::string(context) + ".left");
  }
  if (j.contains("right")) {
    parse_static_orientation_side(j.at("right"), out, false, std::string(context) + ".right");
  }
}

void parse_controller_override_hand_gestures(const nlohmann::json& j,
                                             ControllerOverrideFileConfig& out) {
  if (!j.is_object()) return;

  auto parse_bool = [&](const char* key, bool left) {
    if (!j.contains(key)) return;
    if (left) {
      out.left_hand_gestures_enabled = j.at(key).get<bool>();
      out.has_left_hand_gestures_enabled = true;
    } else {
      out.right_hand_gestures_enabled = j.at(key).get<bool>();
      out.has_right_hand_gestures_enabled = true;
    }
  };

  parse_bool("left_enabled", true);
  parse_bool("right_enabled", false);
  parse_bool("left", true);
  parse_bool("right", false);
  parse_bool("left_hand", true);
  parse_bool("right_hand", false);
  parse_bool("left_hand_gestures", true);
  parse_bool("right_hand_gestures", false);
  parse_bool("left_hand_gestures_enabled", true);
  parse_bool("right_hand_gestures_enabled", false);
}

ControllerOverrideFileConfig load_controller_override_file_config(const std::string& path) {
  ControllerOverrideFileConfig cfg;
  if (path.empty()) return cfg;

  std::ifstream in(path);
  if (!in) {
    // load_tracking_transform_config() will report the same path with its own
    // existing error message.  Do not introduce a second failure path here.
    return cfg;
  }

  nlohmann::json root;
  in >> root;
  if (!root.is_object() || !root.contains("controller_override")) return cfg;

  const auto& j = root.at("controller_override");
  if (!j.is_object()) {
    throw std::runtime_error("controller_override config block must be an object");
  }
  cfg.present = true;

  if (j.contains("mode")) {
    cfg.mode = j.at("mode").get<std::string>();
    cfg.has_mode = true;
  } else if (j.contains("runtime_controller_mode")) {
    cfg.mode = j.at("runtime_controller_mode").get<std::string>();
    cfg.has_mode = true;
  }

  if (j.contains("lost_hand_pose_fallback")) {
    cfg.lost_hand_pose_fallback = j.at("lost_hand_pose_fallback").get<std::string>();
    cfg.has_lost_hand_pose_fallback = true;
  } else if (j.contains("runtime_controller_lost_hand_pose_fallback")) {
    cfg.lost_hand_pose_fallback = j.at("runtime_controller_lost_hand_pose_fallback").get<std::string>();
    cfg.has_lost_hand_pose_fallback = true;
  }

  // Per-side visual hand gesture gates. They control only hand-tracking-derived
  // gestures; external ControllerInputV2 from override_controller is unaffected.
  parse_controller_override_hand_gestures(j, cfg);
  if (j.contains("hand_gestures")) {
    parse_controller_override_hand_gestures(j.at("hand_gestures"), cfg);
  }
  if (j.contains("visual_hand_gestures")) {
    parse_controller_override_hand_gestures(j.at("visual_hand_gestures"), cfg);
  }
  if (j.contains("gestures")) {
    parse_controller_override_hand_gestures(j.at("gestures"), cfg);
  }

  // Static controller orientation used by hand_position_controller_buttons_static_orientation
  // and as the base HMD-relative controller orientation. Values are Euler degrees
  // [roll,pitch,yaw] / {roll,pitch,yaw}; quaternion xyzw is also accepted for exact tuning.
  parse_controller_override_static_orientation(j, cfg, "controller_override");
  if (j.contains("static_orientation")) {
    parse_controller_override_static_orientation(j.at("static_orientation"), cfg,
                                                 "controller_override.static_orientation");
  }
  if (j.contains("static_hand_orientation")) {
    parse_controller_override_static_orientation(j.at("static_hand_orientation"), cfg,
                                                 "controller_override.static_hand_orientation");
  }
  if (j.contains("hand_position_static_orientation")) {
    parse_controller_override_static_orientation(j.at("hand_position_static_orientation"), cfg,
                                                 "controller_override.hand_position_static_orientation");
  }

  if (j.contains("publish_runtime_controller_state_shm")) {
    cfg.publish_runtime_controller_state_shm = j.at("publish_runtime_controller_state_shm").get<bool>();
    cfg.has_publish_runtime_controller_state_shm = true;
  }

  if (j.contains("runtime_controller_state") && j.at("runtime_controller_state").is_object()) {
    const auto& r = j.at("runtime_controller_state");
    if (r.contains("publish_shm")) {
      cfg.publish_runtime_controller_state_shm = r.at("publish_shm").get<bool>();
      cfg.has_publish_runtime_controller_state_shm = true;
    } else if (r.contains("enabled")) {
      cfg.publish_runtime_controller_state_shm = r.at("enabled").get<bool>();
      cfg.has_publish_runtime_controller_state_shm = true;
    }
    if (r.contains("registry")) {
      cfg.runtime_controller_state_registry = r.at("registry").get<std::string>();
      cfg.has_runtime_controller_state_registry = true;
    } else if (r.contains("registry_path")) {
      cfg.runtime_controller_state_registry = r.at("registry_path").get<std::string>();
      cfg.has_runtime_controller_state_registry = true;
    }
    if (r.contains("stream")) {
      cfg.runtime_controller_state_stream = r.at("stream").get<std::string>();
      cfg.has_runtime_controller_state_stream = true;
    } else if (r.contains("stream_id")) {
      cfg.runtime_controller_state_stream = r.at("stream_id").get<std::string>();
      cfg.has_runtime_controller_state_stream = true;
    }
    if (r.contains("shm_name")) {
      cfg.runtime_controller_state_shm_name = r.at("shm_name").get<std::string>();
      cfg.has_runtime_controller_state_shm_name = true;
    }
    if (r.contains("slots")) {
      cfg.runtime_controller_state_slots = r.at("slots").get<uint32_t>();
      cfg.has_runtime_controller_state_slots = true;
    } else if (r.contains("slot_count")) {
      cfg.runtime_controller_state_slots = r.at("slot_count").get<uint32_t>();
      cfg.has_runtime_controller_state_slots = true;
    }
  }

  if (j.contains("synthetic_hmd_relative_pose")) {
    parse_controller_override_offsets(j.at("synthetic_hmd_relative_pose"), cfg,
                                      "controller_override.synthetic_hmd_relative_pose");
  } else if (j.contains("hmd_relative_pose")) {
    parse_controller_override_offsets(j.at("hmd_relative_pose"), cfg,
                                      "controller_override.hmd_relative_pose");
  } else if (j.contains("hmd_relative")) {
    parse_controller_override_offsets(j.at("hmd_relative"), cfg,
                                      "controller_override.hmd_relative");
  }

  // Short aliases are accepted for compact configs.
  parse_controller_override_offsets(j, cfg, "controller_override");
  return cfg;
}

xr_runtime::HandSideF64V1 hand_side_from_summary(
    const xr_runtime::tracking_net_v1::HandSideSummaryF64V1& src) {
  xr_runtime::HandSideF64V1 out {};
  out.handedness = src.handedness;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;

  out.palm_px = src.palm_px;
  out.palm_py = src.palm_py;
  out.palm_pz = src.palm_pz;
  out.palm_qw = src.palm_qw;
  out.palm_qx = src.palm_qx;
  out.palm_qy = src.palm_qy;
  out.palm_qz = src.palm_qz;

  out.wrist_px = src.wrist_px;
  out.wrist_py = src.wrist_py;
  out.wrist_pz = src.wrist_pz;
  out.wrist_qw = src.wrist_qw;
  out.wrist_qx = src.wrist_qx;
  out.wrist_qy = src.wrist_qy;
  out.wrist_qz = src.wrist_qz;

  out.vx = src.vx;
  out.vy = src.vy;
  out.vz = src.vz;
  out.wx = src.wx;
  out.wy = src.wy;
  out.wz = src.wz;

  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;

  out.joint_count = 0;
  out.reserved0 = 0;
  return out;
}

xr_runtime::HandTrackingFrameF64V1 hand_tracking_from_summary(
    const xr_runtime::tracking_net_v1::HandSummaryF64V1& src) {
  xr_runtime::HandTrackingFrameF64V1 out {};
  out.version = 1;
  out.size_bytes = sizeof(xr_runtime::HandTrackingFrameF64V1);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = hand_side_from_summary(src.left);
  out.right = hand_side_from_summary(src.right);
  return out;
}


xr_runtime::HandSideF32V2 hand_side_v2_from_summary(
    const xr_runtime::tracking_net_v1::HandSideSummaryF32V2& src) {
  xr_runtime::HandSideF32V2 out {};
  out.handedness = src.handedness;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.controller_px = src.controller_px; out.controller_py = src.controller_py; out.controller_pz = src.controller_pz;
  out.controller_qw = src.controller_qw; out.controller_qx = src.controller_qx; out.controller_qy = src.controller_qy; out.controller_qz = src.controller_qz;
  out.palm_px = src.palm_px; out.palm_py = src.palm_py; out.palm_pz = src.palm_pz;
  out.palm_qw = src.palm_qw; out.palm_qx = src.palm_qx; out.palm_qy = src.palm_qy; out.palm_qz = src.palm_qz;
  out.wrist_px = src.wrist_px; out.wrist_py = src.wrist_py; out.wrist_pz = src.wrist_pz;
  out.wrist_qw = src.wrist_qw; out.wrist_qx = src.wrist_qx; out.wrist_qy = src.wrist_qy; out.wrist_qz = src.wrist_qz;
  out.vx = src.vx; out.vy = src.vy; out.vz = src.vz;
  out.wx = src.wx; out.wy = src.wy; out.wz = src.wz;
  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;
  out.joint_count = src.joint_count;
  out.reserved0 = src.reserved0;
  return out;
}

xr_runtime::HandTrackingFrameF32V2 hand_tracking_v2_from_summary(
    const xr_runtime::tracking_net_v1::HandSummaryF32V2& src) {
  xr_runtime::HandTrackingFrameF32V2 out {};
  out.version = 2;
  out.size_bytes = sizeof(xr_runtime::HandTrackingFrameF32V2);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = hand_side_v2_from_summary(src.left);
  out.right = hand_side_v2_from_summary(src.right);
  return out;
}

void apply_hand_joints_v2(xr_runtime::HandTrackingFrameF32V2& dst,
                          const xr_runtime::tracking_net_v1::HandJointsF32V2& src) {
  xr_runtime::HandSideF32V2* side = nullptr;
  if (src.handedness == 1) {
    side = &dst.left;
  } else if (src.handedness == 2) {
    side = &dst.right;
  } else {
    return;
  }
  side->handedness = src.handedness;
  side->status = src.status;
  side->flags = src.flags;
  side->confidence = src.confidence;
  side->joint_count = std::min<uint32_t>(src.joint_count, xr_runtime::HAND_JOINT_COUNT_V2);
  for (uint32_t i = 0; i < xr_runtime::HAND_JOINT_COUNT_V2; ++i) {
    side->joints[i] = src.joints[i];
  }
}

class UdpTrackingInput {
 public:
  UdpTrackingInput(const std::string& bind_host, uint16_t bind_port)
      : receiver_(bind_host,
                  bind_port,
                  xr_runtime::platform::UdpReceiveMode::NonBlocking) {}

  UdpTrackingInput(const UdpTrackingInput&) = delete;
  UdpTrackingInput& operator=(const UdpTrackingInput&) = delete;

  void pump(size_t max_packets) {
    for (size_t i = 0; i < max_packets; ++i) {
      alignas(8) uint8_t buffer[4096];
      const auto n_opt = receiver_.receive(buffer, sizeof(buffer));
      if (!n_opt) {
        return;
      }
      const size_t n = *n_opt;

      ++received_packets_;

      try {
        if (n < sizeof(xr_runtime::tracking_net_v1::PacketHeader)) {
          throw std::runtime_error("short packet");
        }

        const auto header = xr_runtime::tracking_net_v1::decode_header(buffer, n);
        xr_runtime::tracking_net_v1::validate_common_header(header, n);

        observe_transport_sequence(header.transport_sequence);

        if (header.packet_type ==
            static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HmdPoseF64)) {
          xr_runtime::tracking_net_v1::validate_hmd_pose_packet_header(header, n);

          const auto hmd = xr_runtime::tracking_net_v1::decode_hmd_pose_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));
          latest_hmd_ = hmd;
          latest_hmd_receive_timestamp_ns_ = xr_runtime::now_ns();
          ++valid_hmd_packets_;
        } else if (header.packet_type ==
                   static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandSummaryF64)) {
          xr_runtime::tracking_net_v1::validate_hand_summary_packet_header(header, n);

          const auto summary = xr_runtime::tracking_net_v1::decode_hand_summary_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));
          latest_hand_ = hand_tracking_from_summary(summary);
          latest_hand_receive_timestamp_ns_ = xr_runtime::now_ns();
          ++valid_hand_packets_;
        } else if (header.packet_type ==
                   static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandSummaryF32V2)) {
          xr_runtime::tracking_net_v1::validate_hand_summary_v2_packet_header(header, n);

          const auto summary = xr_runtime::tracking_net_v1::decode_hand_summary_f32_v2_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));
          latest_hand_v2_ = hand_tracking_v2_from_summary(summary);
          latest_hand_v2_receive_timestamp_ns_ = xr_runtime::now_ns();
          ++valid_hand_v2_packets_;
        } else if (header.packet_type ==
                   static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandJointsF32V2)) {
          xr_runtime::tracking_net_v1::validate_hand_joints_v2_packet_header(header, n);

          const auto joints = xr_runtime::tracking_net_v1::decode_hand_joints_f32_v2_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));
          if (!latest_hand_v2_ || latest_hand_v2_->sequence != joints.sequence) {
            xr_runtime::HandTrackingFrameF32V2 frame {};
            frame.version = 2;
            frame.size_bytes = sizeof(xr_runtime::HandTrackingFrameF32V2);
            frame.sequence = joints.sequence;
            frame.timestamp_ns = joints.timestamp_ns;
            frame.source_timestamp_ns = joints.source_timestamp_ns;
            frame.reset_counter = joints.reset_counter;
            latest_hand_v2_ = frame;
          }
          apply_hand_joints_v2(*latest_hand_v2_, joints);
          latest_hand_v2_receive_timestamp_ns_ = xr_runtime::now_ns();
          ++valid_hand_joints_v2_packets_;
        } else if (header.packet_type ==
                   static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandFullF32V2)) {
          xr_runtime::tracking_net_v1::validate_hand_full_v2_packet_header(header, n);

          latest_hand_v2_ = xr_runtime::tracking_net_v1::decode_hand_full_f32_v2_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));
          latest_hand_v2_receive_timestamp_ns_ = xr_runtime::now_ns();
          ++valid_hand_v2_packets_;
        } else {
          throw std::runtime_error("unknown packet_type " + std::to_string(header.packet_type));
        }
      } catch (const std::exception&) {
        ++invalid_packets_;
      }
    }
  }

  const std::optional<xr_runtime::HmdPoseF64V1>& latest_hmd() const { return latest_hmd_; }
  const std::optional<xr_runtime::HandTrackingFrameF64V1>& latest_hand() const { return latest_hand_; }
  const std::optional<xr_runtime::HandTrackingFrameF32V2>& latest_hand_v2() const { return latest_hand_v2_; }

  int64_t latest_hmd_receive_timestamp_ns() const { return latest_hmd_receive_timestamp_ns_; }
  int64_t latest_hand_receive_timestamp_ns() const { return latest_hand_receive_timestamp_ns_; }
  int64_t latest_hand_v2_receive_timestamp_ns() const { return latest_hand_v2_receive_timestamp_ns_; }

  uint64_t received_packets() const { return received_packets_; }
  uint64_t valid_hmd_packets() const { return valid_hmd_packets_; }
  uint64_t valid_hand_packets() const { return valid_hand_packets_; }
  uint64_t valid_hand_v2_packets() const { return valid_hand_v2_packets_; }
  uint64_t valid_hand_joints_v2_packets() const { return valid_hand_joints_v2_packets_; }
  uint64_t invalid_packets() const { return invalid_packets_; }
  uint64_t estimated_transport_loss() const { return estimated_transport_loss_; }
  uint64_t duplicate_or_reordered() const { return duplicate_or_reordered_; }
  uint64_t last_transport_seq() const { return last_transport_seq_; }

 private:
  void observe_transport_sequence(uint64_t seq) {
    if (last_transport_seq_ == 0 || seq > last_transport_seq_) {
      if (last_transport_seq_ != 0 && seq > last_transport_seq_ + 1) {
        estimated_transport_loss_ += seq - last_transport_seq_ - 1;
      }
      last_transport_seq_ = seq;
    } else {
      ++duplicate_or_reordered_;
    }
  }

  xr_runtime::platform::UdpReceiver receiver_;

  std::optional<xr_runtime::HmdPoseF64V1> latest_hmd_;
  std::optional<xr_runtime::HandTrackingFrameF64V1> latest_hand_;
  std::optional<xr_runtime::HandTrackingFrameF32V2> latest_hand_v2_;
  int64_t latest_hmd_receive_timestamp_ns_ = 0;
  int64_t latest_hand_receive_timestamp_ns_ = 0;
  int64_t latest_hand_v2_receive_timestamp_ns_ = 0;

  uint64_t received_packets_ = 0;
  uint64_t valid_hmd_packets_ = 0;
  uint64_t valid_hand_packets_ = 0;
  uint64_t valid_hand_v2_packets_ = 0;
  uint64_t valid_hand_joints_v2_packets_ = 0;
  uint64_t invalid_packets_ = 0;
  uint64_t estimated_transport_loss_ = 0;
  uint64_t duplicate_or_reordered_ = 0;
  uint64_t last_transport_seq_ = 0;
};

bool is_age_stale(int64_t now_ns_value, int64_t sample_ns, double max_age_ms) {
  if (max_age_ms <= 0.0) return false;
  if (sample_ns <= 0) return true;
  const double age_ms = xr_runtime::ns_to_ms(now_ns_value - sample_ns);
  return !std::isfinite(age_ms) || age_ms > max_age_ms;
}

int64_t ms_to_ns(double ms) {
  if (ms <= 0.0) return 0;
  return static_cast<int64_t>(ms * 1000000.0);
}

enum class TrackingStalePolicy {
  Lost,
  HoldLast,
  HoldThenLost,
};

TrackingStalePolicy parse_tracking_stale_policy(const std::string& s, const char* option_name) {
  if (s == "lost") return TrackingStalePolicy::Lost;
  if (s == "hold_last") return TrackingStalePolicy::HoldLast;
  if (s == "hold_then_lost") return TrackingStalePolicy::HoldThenLost;
  throw std::runtime_error(std::string("unknown ") + option_name + ": " + s +
                           "; expected lost, hold_last, hold_then_lost");
}

const char* tracking_stale_policy_name(TrackingStalePolicy p) {
  switch (p) {
    case TrackingStalePolicy::Lost: return "lost";
    case TrackingStalePolicy::HoldLast: return "hold_last";
    case TrackingStalePolicy::HoldThenLost: return "hold_then_lost";
  }
  return "unknown";
}

struct ReattachState {
  int64_t stale_since_ns = 0;
  int64_t last_attempt_ns = 0;
  uint64_t attempts = 0;
  uint64_t successes = 0;
  uint64_t failures = 0;
  std::string last_error;
};

void update_stale_since(ReattachState& s, bool stale_or_missing, int64_t now_ns_value) {
  if (stale_or_missing) {
    if (s.stale_since_ns == 0) s.stale_since_ns = now_ns_value;
  } else {
    s.stale_since_ns = 0;
  }
}

bool reattach_due(ReattachState& s, bool stale_or_missing, int64_t now_ns_value, double interval_ms) {
  update_stale_since(s, stale_or_missing, now_ns_value);
  if (!stale_or_missing || interval_ms <= 0.0) return false;
  const int64_t interval_ns = ms_to_ns(interval_ms);
  return s.last_attempt_ns == 0 || now_ns_value - s.last_attempt_ns >= interval_ns;
}

bool hold_allowed(TrackingStalePolicy policy,
                  int64_t stale_since_ns,
                  int64_t now_ns_value,
                  double hold_last_max_ms) {
  if (policy == TrackingStalePolicy::Lost) return false;
  if (policy == TrackingStalePolicy::HoldLast) return true;
  if (policy == TrackingStalePolicy::HoldThenLost) {
    if (stale_since_ns == 0) return true;
    if (hold_last_max_ms <= 0.0) return false;
    return now_ns_value - stale_since_ns <= ms_to_ns(hold_last_max_ms);
  }
  return false;
}


enum class HmdPoseSourceKind {
  Primary6Dof,
  Priority3Dof,
  Udp,
};

const char* hmd_pose_source_name(HmdPoseSourceKind kind) {
  switch (kind) {
    case HmdPoseSourceKind::Primary6Dof: return "hmd";
    case HmdPoseSourceKind::Priority3Dof: return "hmd_3dof";
    case HmdPoseSourceKind::Udp: return "udp";
  }
  return "unknown";
}

xr_runtime::HmdPoseF64V1 make_held_hmd_pose(xr_runtime::HmdPoseF64V1 hmd,
                                            int64_t now_ns_value) {
  hmd.timestamp_ns = static_cast<uint64_t>(now_ns_value);
  hmd.vx = 0.0;
  hmd.vy = 0.0;
  hmd.vz = 0.0;
  hmd.wx = 0.0;
  hmd.wy = 0.0;
  hmd.wz = 0.0;
  hmd.flags &= ~xr_runtime::HMD_FLAG_LINEAR_VELOCITY_VALID;
  hmd.flags &= ~xr_runtime::HMD_FLAG_ANGULAR_VELOCITY_VALID;
  return hmd;
}

xr_runtime::HandTrackingFrameF64V1 make_held_hand_frame(xr_runtime::HandTrackingFrameF64V1 hand,
                                                        int64_t now_ns_value) {
  hand.timestamp_ns = static_cast<uint64_t>(now_ns_value);
  return hand;
}

xr_runtime::HandTrackingFrameF32V2 make_held_hand_frame(xr_runtime::HandTrackingFrameF32V2 hand,
                                                        int64_t now_ns_value) {
  hand.timestamp_ns = static_cast<uint64_t>(now_ns_value);
  return hand;
}



xr_tracking::HandJointF32V2 runtime_hand_joint_v2_from_runtime(
    const xr_runtime::HandJointF32V2& src) {
  xr_tracking::HandJointF32V2 out {};
  out.joint_id = src.joint_id;
  out.flags = src.flags;
  out.px = src.px;
  out.py = src.py;
  out.pz = src.pz;
  out.qw = src.qw;
  out.qx = src.qx;
  out.qy = src.qy;
  out.qz = src.qz;
  out.radius_m = src.radius_m;
  out.confidence = src.confidence;
  return out;
}

xr_tracking::HandSideF32V2 runtime_hand_side_v2_from_runtime(
    const xr_runtime::HandSideF32V2& src) {
  xr_tracking::HandSideF32V2 out {};
  out.handedness = src.handedness;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.controller_px = src.controller_px;
  out.controller_py = src.controller_py;
  out.controller_pz = src.controller_pz;
  out.controller_qw = src.controller_qw;
  out.controller_qx = src.controller_qx;
  out.controller_qy = src.controller_qy;
  out.controller_qz = src.controller_qz;
  out.palm_px = src.palm_px;
  out.palm_py = src.palm_py;
  out.palm_pz = src.palm_pz;
  out.palm_qw = src.palm_qw;
  out.palm_qx = src.palm_qx;
  out.palm_qy = src.palm_qy;
  out.palm_qz = src.palm_qz;
  out.wrist_px = src.wrist_px;
  out.wrist_py = src.wrist_py;
  out.wrist_pz = src.wrist_pz;
  out.wrist_qw = src.wrist_qw;
  out.wrist_qx = src.wrist_qx;
  out.wrist_qy = src.wrist_qy;
  out.wrist_qz = src.wrist_qz;
  out.vx = src.vx;
  out.vy = src.vy;
  out.vz = src.vz;
  out.wx = src.wx;
  out.wy = src.wy;
  out.wz = src.wz;
  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;
  out.joint_count = std::min<uint32_t>(src.joint_count, xr_tracking::HAND_JOINT_COUNT_V2);
  out.reserved0 = src.reserved0;
  for (uint32_t i = 0; i < xr_tracking::HAND_JOINT_COUNT_V2; ++i) {
    out.joints[i] = runtime_hand_joint_v2_from_runtime(src.joints[i]);
  }
  return out;
}

xr_tracking::HandTrackingFrameF32V2 runtime_hand_frame_v2_from_runtime(
    const xr_runtime::HandTrackingFrameF32V2& src,
    int64_t publish_timestamp_ns) {
  xr_tracking::HandTrackingFrameF32V2 out {};
  out.version = xr_tracking::HAND_TRACKING_FORMAT_VERSION_V2;
  out.size_bytes = sizeof(xr_tracking::HandTrackingFrameF32V2);
  out.sequence = src.sequence;
  out.timestamp_ns = static_cast<uint64_t>(publish_timestamp_ns);
  out.source_timestamp_ns = src.timestamp_ns != 0 ? src.timestamp_ns : src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = runtime_hand_side_v2_from_runtime(src.left);
  out.right = runtime_hand_side_v2_from_runtime(src.right);
  return out;
}

xr_tracking::HandSideF32V2 runtime_hand_side_v2_from_v1(
    const xr_runtime::HandSideF64V1& src,
    bool left) {
  xr_tracking::HandSideF32V2 out {};
  out.handedness = left ? 1u : 2u;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.controller_px = static_cast<float>(src.palm_px);
  out.controller_py = static_cast<float>(src.palm_py);
  out.controller_pz = static_cast<float>(src.palm_pz);
  out.controller_qw = static_cast<float>(src.palm_qw);
  out.controller_qx = static_cast<float>(src.palm_qx);
  out.controller_qy = static_cast<float>(src.palm_qy);
  out.controller_qz = static_cast<float>(src.palm_qz);
  out.palm_px = static_cast<float>(src.palm_px);
  out.palm_py = static_cast<float>(src.palm_py);
  out.palm_pz = static_cast<float>(src.palm_pz);
  out.palm_qw = static_cast<float>(src.palm_qw);
  out.palm_qx = static_cast<float>(src.palm_qx);
  out.palm_qy = static_cast<float>(src.palm_qy);
  out.palm_qz = static_cast<float>(src.palm_qz);
  out.wrist_px = static_cast<float>(src.wrist_px);
  out.wrist_py = static_cast<float>(src.wrist_py);
  out.wrist_pz = static_cast<float>(src.wrist_pz);
  out.wrist_qw = static_cast<float>(src.wrist_qw);
  out.wrist_qx = static_cast<float>(src.wrist_qx);
  out.wrist_qy = static_cast<float>(src.wrist_qy);
  out.wrist_qz = static_cast<float>(src.wrist_qz);
  out.vx = static_cast<float>(src.vx);
  out.vy = static_cast<float>(src.vy);
  out.vz = static_cast<float>(src.vz);
  out.wx = static_cast<float>(src.wx);
  out.wy = static_cast<float>(src.wy);
  out.wz = static_cast<float>(src.wz);
  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;
  out.joint_count = 0;
  out.reserved0 = 0;
  return out;
}

xr_tracking::HandTrackingFrameF32V2 runtime_hand_frame_v2_from_v1(
    const xr_runtime::HandTrackingFrameF64V1& src,
    int64_t publish_timestamp_ns) {
  xr_tracking::HandTrackingFrameF32V2 out =
      xr_tracking::make_no_hands_frame_v2(src.timestamp_ns != 0 ?
                                          static_cast<int64_t>(src.timestamp_ns) :
                                          static_cast<int64_t>(src.source_timestamp_ns),
                                          src.reset_counter);
  out.sequence = src.sequence;
  out.timestamp_ns = static_cast<uint64_t>(publish_timestamp_ns);
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = runtime_hand_side_v2_from_v1(src.left, true);
  out.right = runtime_hand_side_v2_from_v1(src.right, false);
  return out;
}

xr_tracking::HandTrackingFrameF32V2 runtime_no_hands_frame_v2(int64_t now_ns_value,
                                                              uint64_t reset_counter = 0) {
  auto out = xr_tracking::make_no_hands_frame_v2(now_ns_value, reset_counter);
  out.timestamp_ns = static_cast<uint64_t>(now_ns_value);
  out.source_timestamp_ns = static_cast<uint64_t>(now_ns_value);
  return out;
}


// Coordinate and orientation transform helpers live in coordinate_util.cpp.





int xr_spatial_proxy_axis_permutation_sign(const std::array<int, 3>& axis_map) {
  // Permutation parity for the 3x3 axis remap matrix. +1 for even, -1 for odd.
  int inv = 0;
  for (int i = 0; i < 3; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if (axis_map[i] > axis_map[j]) ++inv;
    }
  }
  return (inv % 2) == 0 ? 1 : -1;
}

bool xr_spatial_proxy_transform_flips_handedness(const StreamTransformConfig& transform) {
  const auto& ct = transform.coordinate_transform;
  if (!ct.enabled) return false;

  // Rotation has det +1.  Axis remap/sign/negative uniform scale can flip handedness.
  double det_sign = static_cast<double>(xr_spatial_proxy_axis_permutation_sign(ct.axis_map));
  det_sign *= ct.sign[0] * ct.sign[1] * ct.sign[2];
  if (ct.scale < 0.0) det_sign *= -1.0; // uniform scale det sign is sign(scale)^3.
  return std::isfinite(det_sign) && det_sign < 0.0;
}


static double xr_spatial_proxy_deg_to_rad(double deg) {
  return deg * 3.14159265358979323846 / 180.0;
}

static bool xr_spatial_proxy_has_extra_rotation(double rx, double ry, double rz) {
  return std::abs(rx) > 1e-9 || std::abs(ry) > 1e-9 || std::abs(rz) > 1e-9;
}

static bool xr_spatial_proxy_has_extra_offset(double ox, double oy, double oz) {
  return std::abs(ox) > 1e-9 || std::abs(oy) > 1e-9 || std::abs(oz) > 1e-9;
}

static V3d xr_spatial_proxy_rotate_extra_xyz(const V3d& in, double rx, double ry, double rz) {
  V3d out = in;
  if (rx != 0.0) {
    const double c = std::cos(rx), s = std::sin(rx);
    out = {out.x, out.y * c - out.z * s, out.y * s + out.z * c};
  }
  if (ry != 0.0) {
    const double c = std::cos(ry), s = std::sin(ry);
    out = {out.x * c + out.z * s, out.y, -out.x * s + out.z * c};
  }
  if (rz != 0.0) {
    const double c = std::cos(rz), s = std::sin(rz);
    out = {out.x * c - out.y * s, out.x * s + out.y * c, out.z};
  }
  return out;
}

static V3d xr_spatial_proxy_apply_extra_rotation_deg(const V3d& in, double rx_deg, double ry_deg, double rz_deg) {
  if (!xr_spatial_proxy_has_extra_rotation(rx_deg, ry_deg, rz_deg)) return in;
  return xr_spatial_proxy_rotate_extra_xyz(in,
                                           xr_spatial_proxy_deg_to_rad(rx_deg),
                                           xr_spatial_proxy_deg_to_rad(ry_deg),
                                           xr_spatial_proxy_deg_to_rad(rz_deg));
}

struct SpatialProxyMeshInputConfig {
  std::string input = "none"; // none, shm, udp
  std::string registry = "/tmp/runtime_tracking_streams.json";
  std::string stream = "spatial_proxy_mesh";
  std::string udp_bind_host = "0.0.0.0";
  int udp_bind_port = 45740;
  double reattach_on_stale_ms = 1000.0;
  double max_source_age_ms = 1000.0;
  bool publish_runtime_shm = false;
  std::string runtime_registry = "/tmp/runtime_tracking_streams.json";
  std::string runtime_stream = "runtime_spatial_proxy_mesh";
  std::string runtime_shm_name = "runtime_spatial_proxy_mesh";
  std::string runtime_frame = "runtime_local";
  uint32_t runtime_slots = 8;
  bool runtime_unlink_existing = true;
  StreamTransformConfig transform{};
  // auto: derive from coordinate transform determinant; keep/swap: explicit debug override.
  std::string triangle_winding = "auto"; // auto, keep, swap
  // Additional runtime-space mesh rotation for fast calibration/debug. Prefer the main
  // tracking transform config for final profiles; keep this at 0 unless tuning overlay geometry.
  double rotate_deg_x = 0.0;
  double rotate_deg_y = 0.0;
  double rotate_deg_z = 0.0;
  double offset_m_x = 0.0;
  double offset_m_y = 0.0;
  double offset_m_z = 0.0;
};

struct RuntimeHmdPoseSnapshotForSpatialMesh {
  bool valid = false;
  V3d p{};
  Qd q{};
  uint64_t timestamp_ns = 0;
};

struct SpatialProxyMeshInputSnapshot {
  bool enabled = false;
  bool connected = false;
  bool runtime_output_enabled = false;
  uint64_t frames = 0;
  uint64_t runtime_published = 0;
  uint64_t udp_packets = 0;
  uint64_t udp_complete_meshes = 0;
  uint64_t udp_invalid_packets = 0;
  uint64_t last_sequence = 0;
  double age_ms = 0.0;
  uint32_t vertices = 0;
  uint32_t triangles = 0;
  bool triangle_winding_flipped = false;
  bool camera_relative_runtime = false;
  bool camera_relative_hmd = false;
  double camera_relative_hmd_age_ms = 0.0;
  std::string last_error;
};

class SpatialProxyMeshInputThread {
 public:
  explicit SpatialProxyMeshInputThread(SpatialProxyMeshInputConfig cfg) : cfg_(std::move(cfg)) {}
  ~SpatialProxyMeshInputThread() { stop(); }
  SpatialProxyMeshInputThread(const SpatialProxyMeshInputThread&) = delete;
  SpatialProxyMeshInputThread& operator=(const SpatialProxyMeshInputThread&) = delete;

  void start() {
    if (cfg_.input == "none") return;
    stop_ = false;
    { std::lock_guard<std::mutex> lock(mu_); snapshot_ = {}; snapshot_.enabled = true; snapshot_.runtime_output_enabled = cfg_.publish_runtime_shm; }
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  SpatialProxyMeshInputSnapshot snapshot() const { std::lock_guard<std::mutex> lock(mu_); return snapshot_; }

  void set_origin_snapshot(RuntimeOriginSnapshot origin) {
    std::lock_guard<std::mutex> lock(mu_);
    origin_snapshot_ = std::move(origin);
  }

  void set_runtime_hmd_snapshot(RuntimeHmdPoseSnapshotForSpatialMesh hmd) {
    std::lock_guard<std::mutex> lock(mu_);
    runtime_hmd_snapshot_ = std::move(hmd);
  }

 private:
  struct UdpAssembly {
    uint64_t sequence = 0;
    xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1 header{};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> received;
    uint16_t received_count = 0;

    void reset(const xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1& h) {
      sequence = h.mesh_sequence;
      header = h;
      payload.assign(h.full_payload_size_bytes, 0);
      received.assign(h.chunk_count, 0);
      received_count = 0;
    }

    bool add(const xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1& h, const uint8_t* data, size_t n) {
      if (sequence != h.mesh_sequence || received.empty()) reset(h);
      if (h.chunk_count == 0 || h.chunk_index >= received.size()) return false;
      if (h.payload_offset_bytes + h.payload_size > payload.size()) return false;
      if (n < h.payload_size) return false;
      std::memcpy(payload.data() + h.payload_offset_bytes, data, h.payload_size);
      if (!received[h.chunk_index]) { received[h.chunk_index] = 1; ++received_count; }
      header = h;
      return received_count == received.size();
    }
  };

  void run() {
    uint64_t last_wait_log_ns = 0;
    while (!stop_) {
      try {
        if (cfg_.input == "shm") {
          run_shm_once();
        } else if (cfg_.input == "udp") {
          run_udp_once();
        } else {
          throw std::runtime_error("unsupported spatial proxy mesh input: " + cfg_.input);
        }
      } catch (const std::exception& e) {
        set_error(e.what());
        const uint64_t now = xr_runtime::now_ns();
        if (last_wait_log_ns == 0 || xr_runtime::ns_to_ms(static_cast<int64_t>(now - last_wait_log_ns)) >= 2000.0) {
          if (cfg_.input == "udp") {
            std::cout << "[xr_runtime_adapter] waiting for spatial proxy mesh UDP input "
                      << cfg_.udp_bind_host << ":" << cfg_.udp_bind_port << ": " << e.what() << "\n";
          } else {
            std::cout << "[xr_runtime_adapter] waiting for spatial proxy mesh SHM input "
                      << cfg_.registry << ":" << cfg_.stream << ": " << e.what() << "\n";
          }
          last_wait_log_ns = now;
        }
        sleep_ms(500);
      }
    }
  }

  void run_shm_once() {
    if (cfg_.input != "shm") throw std::runtime_error("unsupported spatial proxy mesh input: " + cfg_.input);
    auto info = xr_runtime::stream_info_from_registry(cfg_.registry, cfg_.stream);
    xr_runtime::TrackingRingReader<xr_spatial::RuntimeSpatialProxyMeshF32V1> reader(
        std::move(info), xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME);
    ensure_publisher();
    set_connected(true, "");
    uint64_t last_seq = 0;
    uint64_t last_frame_ns = xr_runtime::now_ns();
    while (!stop_) {
      auto mesh = reader.latest();
      const uint64_t now = xr_runtime::now_ns();
      if (mesh && mesh->sequence != 0 && mesh->sequence != last_seq) {
        last_seq = mesh->sequence;
        last_frame_ns = now;
        observe(*mesh, now);
      } else {
        if (cfg_.reattach_on_stale_ms > 0.0 &&
            xr_runtime::ns_to_ms(static_cast<int64_t>(now - last_frame_ns)) >= cfg_.reattach_on_stale_ms) {
          throw std::runtime_error("spatial proxy mesh SHM stream stale; reattaching");
        }
        sleep_ms(2);
      }
    }
  }

  void run_udp_once() {
    xr_runtime::platform::UdpReceiver receiver(
        cfg_.udp_bind_host,
        static_cast<uint16_t>(cfg_.udp_bind_port),
        xr_runtime::platform::UdpReceiveMode::NonBlocking);
    ensure_publisher();
    set_connected(true, "");
    std::cout << "[xr_runtime_adapter] spatial proxy mesh UDP receiver listening on "
              << cfg_.udp_bind_host << ":" << cfg_.udp_bind_port << "\n";
    UdpAssembly asmbl;
    alignas(8) uint8_t buffer[2048];
    while (!stop_) {
      const auto n_opt = receiver.receive(buffer, sizeof(buffer));
      if (!n_opt) {
        sleep_ms(1);
        continue;
      }
      const size_t n = *n_opt;
      {
        std::lock_guard<std::mutex> lock(mu_);
        ++snapshot_.udp_packets;
      }
      if (n < sizeof(xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1)) {
        note_invalid_udp_packet("short XRPM packet");
        continue;
      }
      xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1 h{};
      std::memcpy(&h, buffer, sizeof(h));
      if (!xr_spatial::proxy_mesh_udp_magic_ok(h) || h.version != 2 || h.header_size != sizeof(h) ||
          h.chunk_count == 0 || h.chunk_index >= h.chunk_count ||
          n < sizeof(h) + h.payload_size) {
        note_invalid_udp_packet("invalid XRPM header");
        continue;
      }
      if (!asmbl.add(h, buffer + sizeof(h), n - sizeof(h))) {
        continue;
      }
      try {
        auto mesh = xr_spatial::proxy_mesh_from_payload(asmbl.header, asmbl.payload);
        {
          std::lock_guard<std::mutex> lock(mu_);
          ++snapshot_.udp_complete_meshes;
        }
        observe(std::move(mesh), xr_runtime::now_ns());
      } catch (const std::exception& e) {
        note_invalid_udp_packet(e.what());
      }
      asmbl.received.clear();
    }
  }

  void ensure_publisher() {
    if (!cfg_.publish_runtime_shm || publisher_) return;
    xr_spatial::RuntimeSpatialProxyMeshShmPublisherConfig pcfg;
    pcfg.registry_path = cfg_.runtime_registry;
    pcfg.stream_id = cfg_.runtime_stream;
    pcfg.shm_name = cfg_.runtime_shm_name;
    pcfg.frame_id = cfg_.runtime_frame;
    pcfg.slot_count = cfg_.runtime_slots;
    pcfg.unlink_existing = cfg_.runtime_unlink_existing;
    pcfg.created_by = "xr_runtime_adapter";
    publisher_ = std::make_unique<xr_spatial::RuntimeSpatialProxyMeshShmPublisher>(std::move(pcfg));
  }

  void observe(xr_spatial::RuntimeSpatialProxyMeshF32V1 mesh, uint64_t now) {
    const double age_ms = mesh.timestamp_ns != 0 ? xr_runtime::ns_to_ms(static_cast<int64_t>(now - mesh.timestamp_ns)) : 0.0;
    // SHM uses the same monotonic clock domain. UDP may cross process/VM/host boundaries,
    // so do not reject UDP meshes based on sender monotonic timestamps.
    if (cfg_.input == "shm" && cfg_.max_source_age_ms > 0.0 && (!std::isfinite(age_ms) || age_ms > cfg_.max_source_age_ms)) {
      return;
    }
    if (!transform_mesh(mesh, now)) {
      return;
    }
    mesh.timestamp_ns = now;
    if (publisher_) {
      publisher_->publish(mesh);
      std::lock_guard<std::mutex> lock(mu_);
      ++snapshot_.runtime_published;
    }
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = true;
    snapshot_.runtime_output_enabled = cfg_.publish_runtime_shm;
    snapshot_.last_error.clear();
    snapshot_.last_sequence = mesh.sequence;
    snapshot_.age_ms = age_ms;
    snapshot_.vertices = mesh.vertex_count;
    snapshot_.triangles = mesh.triangle_count;
    snapshot_.triangle_winding_flipped = should_swap_triangle_winding();
    snapshot_.camera_relative_runtime = cfg_.transform.spatial_mesh.camera_relative_runtime.enabled;
    snapshot_.camera_relative_hmd = last_camera_relative_hmd_used_;
    snapshot_.camera_relative_hmd_age_ms = last_camera_relative_hmd_age_ms_;
    ++snapshot_.frames;
  }

  bool should_swap_triangle_winding() const {
    if (cfg_.triangle_winding == "keep") return false;
    if (cfg_.triangle_winding == "swap") return true;
    return xr_spatial_proxy_transform_flips_handedness(cfg_.transform);
  }

  V3d apply_runtime_origin_to_point(const V3d& p, const RuntimeOriginSnapshot& origin) const {
    const auto& mesh_rt = cfg_.transform.spatial_mesh;
    if (!origin.enabled || !origin.ready ||
        (!mesh_rt.apply_runtime_origin_position && !mesh_rt.apply_runtime_origin_orientation)) {
      return p;
    }

    const Qd inv_origin_q = q_conj(normalize_q(origin.origin_q));
    V3d out = p;
    if (mesh_rt.apply_runtime_origin_position) {
      out.x -= origin.origin_p.x;
      out.y -= origin.origin_p.y;
      out.z -= origin.origin_p.z;
    }
    if (mesh_rt.apply_runtime_origin_orientation) {
      out = q_rotate(inv_origin_q, out);
    }
    return out;
  }

  bool transform_mesh(xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh, uint64_t now) {
    const uint32_t vc = std::min<uint32_t>(mesh.vertex_count, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
    const uint32_t tc = std::min<uint32_t>(mesh.triangle_count, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
    const bool flip_triangle_winding = should_swap_triangle_winding();
    const bool extra_rotation = xr_spatial_proxy_has_extra_rotation(cfg_.rotate_deg_x, cfg_.rotate_deg_y, cfg_.rotate_deg_z);
    const bool extra_offset = xr_spatial_proxy_has_extra_offset(cfg_.offset_m_x, cfg_.offset_m_y, cfg_.offset_m_z);
    RuntimeOriginSnapshot origin;
    RuntimeHmdPoseSnapshotForSpatialMesh runtime_hmd;
    {
      std::lock_guard<std::mutex> lock(mu_);
      origin = origin_snapshot_;
      runtime_hmd = runtime_hmd_snapshot_;
    }
    const auto& mesh_rt = cfg_.transform.spatial_mesh;
    const auto& camera_rt = mesh_rt.camera_relative_runtime;
    bool camera_relative_hmd_used = false;
    double camera_relative_hmd_age_ms = 0.0;
    if (camera_rt.enabled) {
      camera_relative_hmd_age_ms = runtime_hmd.timestamp_ns != 0
          ? xr_runtime::ns_to_ms(static_cast<int64_t>(now - runtime_hmd.timestamp_ns))
          : std::numeric_limits<double>::infinity();
      const bool hmd_fresh = runtime_hmd.valid &&
          (camera_rt.max_hmd_age_ms <= 0.0 ||
           (std::isfinite(camera_relative_hmd_age_ms) && camera_relative_hmd_age_ms <= camera_rt.max_hmd_age_ms));
      if (!hmd_fresh && camera_rt.require_hmd) {
        set_error("spatial camera_relative_runtime waiting for fresh runtime HMD pose");
        last_camera_relative_hmd_used_ = false;
        last_camera_relative_hmd_age_ms_ = camera_relative_hmd_age_ms;
        return false;
      }
      camera_relative_hmd_used = hmd_fresh;
    }
    bool have_bbox = false;
    float minx = 0, miny = 0, minz = 0, maxx = 0, maxy = 0, maxz = 0;
    uint32_t runtime_valid_points = 0;
    for (uint32_t i = 0; i < vc; ++i) {
      V3d p{mesh.vertices[i].x, mesh.vertices[i].y, mesh.vertices[i].z};
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        continue; // organized-grid invalid cell; preserve NaN/invalid marker.
      }
      V3d q{};
      if (camera_rt.enabled) {
        q = apply_stream_vector_transform(cfg_.transform, p);
        q = xr_spatial_proxy_apply_extra_rotation_deg(q, cfg_.rotate_deg_x, cfg_.rotate_deg_y, cfg_.rotate_deg_z);
        q.x += cfg_.offset_m_x + camera_rt.offset_m.x;
        q.y += cfg_.offset_m_y + camera_rt.offset_m.y;
        q.z += cfg_.offset_m_z + camera_rt.offset_m.z;
        if (camera_relative_hmd_used && camera_rt.apply_hmd_orientation) {
          q = q_rotate(runtime_hmd.q, q);
        }
        if (camera_relative_hmd_used && camera_rt.apply_hmd_position) {
          q.x += runtime_hmd.p.x;
          q.y += runtime_hmd.p.y;
          q.z += runtime_hmd.p.z;
        }
      } else {
        q = apply_stream_position_transform(cfg_.transform, p, nullptr, nullptr);
        q = xr_spatial_proxy_apply_extra_rotation_deg(q, cfg_.rotate_deg_x, cfg_.rotate_deg_y, cfg_.rotate_deg_z);
        if (extra_offset) { q.x += cfg_.offset_m_x; q.y += cfg_.offset_m_y; q.z += cfg_.offset_m_z; }
        q = apply_runtime_origin_to_point(q, origin);
      }
      if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
        mesh.vertices[i].x = std::numeric_limits<float>::quiet_NaN();
        mesh.vertices[i].y = std::numeric_limits<float>::quiet_NaN();
        mesh.vertices[i].z = std::numeric_limits<float>::quiet_NaN();
        continue;
      }
      mesh.vertices[i].x = static_cast<float>(q.x);
      mesh.vertices[i].y = static_cast<float>(q.y);
      mesh.vertices[i].z = static_cast<float>(q.z);
      ++runtime_valid_points;
      if (!have_bbox) { minx = maxx = mesh.vertices[i].x; miny = maxy = mesh.vertices[i].y; minz = maxz = mesh.vertices[i].z; have_bbox = true; }
      else { minx = std::min(minx, mesh.vertices[i].x); maxx = std::max(maxx, mesh.vertices[i].x);
             miny = std::min(miny, mesh.vertices[i].y); maxy = std::max(maxy, mesh.vertices[i].y);
             minz = std::min(minz, mesh.vertices[i].z); maxz = std::max(maxz, mesh.vertices[i].z); }
    }
    if (mesh.mesh_kind == 2) mesh.valid_point_count = runtime_valid_points;
    if (have_bbox) {
      mesh.bbox_min_x = minx; mesh.bbox_min_y = miny; mesh.bbox_min_z = minz;
      mesh.bbox_max_x = maxx; mesh.bbox_max_y = maxy; mesh.bbox_max_z = maxz;
    }

    // If xr_runtime_adapter coordinate transform changes handedness, triangle winding
    // must be inverted too.  Otherwise filled/normal-aware consumers see inside-out
    // faces after runtime normalization.  Wireframe overlay is less sensitive, but the
    // runtime stream should still be geometrically correct for future consumers.
    const bool origin_policy_non_default =
        !mesh_rt.apply_runtime_origin_position || !mesh_rt.apply_runtime_origin_orientation;
    const bool camera_relative_enabled = camera_rt.enabled;
    if ((extra_rotation || extra_offset || origin_policy_non_default || camera_relative_enabled) && !triangle_winding_logged_) {
      std::cout << "[xr_runtime_adapter] spatial_proxy_mesh mesh_runtime"
                << " triangle_winding=" << cfg_.triangle_winding
                << " extra_rotation_deg=(" << cfg_.rotate_deg_x << ","
                << cfg_.rotate_deg_y << "," << cfg_.rotate_deg_z << ")"
                << " extra_offset_m=(" << cfg_.offset_m_x << ","
                << cfg_.offset_m_y << "," << cfg_.offset_m_z << ")"
                << " apply_origin_position="
                << (mesh_rt.apply_runtime_origin_position ? "true" : "false")
                << " apply_origin_orientation="
                << (mesh_rt.apply_runtime_origin_orientation ? "true" : "false")
                << " camera_relative_runtime="
                << (camera_rt.enabled ? "true" : "false")
                << " camera_relative_require_hmd="
                << (camera_rt.require_hmd ? "true" : "false")
                << " camera_relative_hmd_used="
                << (camera_relative_hmd_used ? "true" : "false")
                << " camera_relative_hmd_age_ms=" << camera_relative_hmd_age_ms
                << " camera_relative_offset_m=(" << camera_rt.offset_m.x << ","
                << camera_rt.offset_m.y << "," << camera_rt.offset_m.z << ")"
                << "\n";
      triangle_winding_logged_ = true;
    }

    if (flip_triangle_winding) {
      for (uint32_t i = 0; i < tc; ++i) std::swap(mesh.triangles[i].i1, mesh.triangles[i].i2);
      if (!triangle_winding_logged_) {
        std::cout << "[xr_runtime_adapter] spatial_proxy_mesh triangle_winding=" << cfg_.triangle_winding
                  << " action=swap extra_rotation_deg=(" << cfg_.rotate_deg_x << ","
                  << cfg_.rotate_deg_y << "," << cfg_.rotate_deg_z << ")\n";
        triangle_winding_logged_ = true;
      }
    }
    last_camera_relative_hmd_used_ = camera_relative_hmd_used;
    last_camera_relative_hmd_age_ms_ = camera_relative_hmd_age_ms;
    return true;
  }

  void note_invalid_udp_packet(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.udp_invalid_packets;
    snapshot_.last_error = error;
  }

  void set_connected(bool connected, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true; snapshot_.connected = connected; snapshot_.last_error = error;
  }
  void set_error(const std::string& error) { set_connected(false, error); }
  void sleep_ms(int ms) const { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

  SpatialProxyMeshInputConfig cfg_;
  std::atomic_bool stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  SpatialProxyMeshInputSnapshot snapshot_;
  RuntimeOriginSnapshot origin_snapshot_;
  RuntimeHmdPoseSnapshotForSpatialMesh runtime_hmd_snapshot_;
  std::unique_ptr<xr_spatial::RuntimeSpatialProxyMeshShmPublisher> publisher_;
  bool triangle_winding_logged_ = false;
  bool last_camera_relative_hmd_used_ = false;
  double last_camera_relative_hmd_age_ms_ = 0.0;
};

struct VideoInputHealthConfig {
  std::string input = "none";
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45700;
  std::string registry = "/tmp/xr_video_streams.json";
  std::string stream = "stereo_video";
  double reattach_on_stale_ms = 1000.0;

  bool publish_runtime_video_shm = false;
  std::string runtime_registry = "/tmp/runtime_video_streams.json";
  std::string runtime_stream = "runtime_stereo_video";
  std::string runtime_shm_name = "runtime_stereo_video";
  std::string runtime_frame = "runtime_video";
  uint32_t runtime_slots = 8;
  bool runtime_unlink_existing = true;
};

struct VideoInputHealthSnapshot {
  bool enabled = false;
  bool connected = false;
  bool runtime_output_enabled = false;
  uint64_t frames = 0;
  uint64_t runtime_video_published = 0;
  uint64_t last_sequence = 0;
  uint64_t sequence_gaps = 0;
  uint64_t dropped_estimate = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  double runtime_rate_hz = 0.0;
  double camera_rate_hz = 0.0;
  double age_ms = 0.0;
  double stereo_delta_ms = 0.0;
  uint64_t reattach_attempts = 0;
  uint64_t reattach_successes = 0;
  uint64_t reattach_failures = 0;
  std::string last_error;
  std::string runtime_output_error;
};

class VideoInputHealthThread {
 public:
  explicit VideoInputHealthThread(VideoInputHealthConfig cfg) : cfg_(std::move(cfg)) {}
  ~VideoInputHealthThread() { stop(); }

  VideoInputHealthThread(const VideoInputHealthThread&) = delete;
  VideoInputHealthThread& operator=(const VideoInputHealthThread&) = delete;

  void start() {
    if (cfg_.input == "none") return;
    stop_ = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot_ = {};
      snapshot_.enabled = true;
      snapshot_.runtime_output_enabled = cfg_.publish_runtime_video_shm;
    }
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  VideoInputHealthSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot_;
  }

 private:
  void run() {
    while (!stop_) {
      try {
        note_reattach_attempt();
        if (cfg_.input == "tcp") {
          run_tcp_once();
        } else if (cfg_.input == "shm") {
          run_shm_once();
        } else {
          set_error("unknown video input: " + cfg_.input);
          return;
        }
      } catch (const std::exception& e) {
        note_reattach_failure(e.what());
        set_error(e.what());
        sleep_ms(500);
      }
    }
  }

  void run_tcp_once() {
    xr_video::StereoVideoTcpClient client(cfg_.tcp_host, cfg_.tcp_port);
    note_reattach_success();
    set_connected(true, "");
    while (!stop_) {
      auto frame = client.read_next();
      observe(std::move(frame));
    }
  }

  void run_shm_once() {
    auto info = xr_video::stereo_video_stream_from_registry(cfg_.registry, cfg_.stream);
    xr_video::StereoVideoShmReader reader(std::move(info));
    note_reattach_success();
    set_connected(true, "");
    uint64_t last_seq = 0;
    uint64_t last_frame_ns = xr_video::monotonic_now_ns();
    while (!stop_) {
      auto frame = reader.latest();
      if (frame && frame->header.sequence > last_seq) {
        last_seq = frame->header.sequence;
        last_frame_ns = xr_video::monotonic_now_ns();
        observe(std::move(*frame));
      } else {
        const uint64_t now_ns = xr_video::monotonic_now_ns();
        if (cfg_.reattach_on_stale_ms > 0.0 &&
            xr_video::ns_to_ms(static_cast<int64_t>(now_ns - last_frame_ns)) >= cfg_.reattach_on_stale_ms) {
          throw std::runtime_error("XR video SHM stream stale; reattaching");
        }
        sleep_ms(1);
      }
    }
  }

  void observe(xr_video::StereoVideoFrame f) {
    const uint64_t now_ns = xr_video::monotonic_now_ns();
    if (cfg_.publish_runtime_video_shm) {
      publish_runtime_video(f, now_ns);
    }

    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = true;
    snapshot_.runtime_output_enabled = cfg_.publish_runtime_video_shm;
    snapshot_.last_error.clear();
    if (snapshot_.frames == 0) {
      first_observe_ns_ = now_ns;
      first_source_ts_ns_ = static_cast<int64_t>(f.header.source_timestamp_ns);
    }
    if (snapshot_.last_sequence != 0 && f.header.sequence > snapshot_.last_sequence + 1) {
      ++snapshot_.sequence_gaps;
      snapshot_.dropped_estimate += f.header.sequence - snapshot_.last_sequence - 1;
    }
    snapshot_.last_sequence = f.header.sequence;
    ++snapshot_.frames;
    snapshot_.width = f.header.width;
    snapshot_.height = f.header.height;
    last_source_ts_ns_ = static_cast<int64_t>(f.header.source_timestamp_ns);
    snapshot_.age_ms = xr_video::ns_to_ms(static_cast<int64_t>(now_ns - f.header.publish_timestamp_ns));
    snapshot_.stereo_delta_ms = xr_video::ns_to_ms(f.header.left_timestamp_ns - f.header.right_timestamp_ns);

    const double runtime_s = first_observe_ns_ != 0 && now_ns > first_observe_ns_
        ? static_cast<double>(now_ns - first_observe_ns_) / 1e9
        : 0.0;
    snapshot_.runtime_rate_hz = runtime_s > 0.0
        ? static_cast<double>(std::max<uint64_t>(1, snapshot_.frames - 1)) / runtime_s
        : 0.0;

    const double camera_s = first_source_ts_ns_ != 0 && last_source_ts_ns_ > first_source_ts_ns_
        ? static_cast<double>(last_source_ts_ns_ - first_source_ts_ns_) / 1e9
        : 0.0;
    snapshot_.camera_rate_hz = camera_s > 0.0
        ? static_cast<double>(std::max<uint64_t>(1, snapshot_.frames - 1)) / camera_s
        : 0.0;
  }

  void publish_runtime_video(xr_video::StereoVideoFrame frame, uint64_t now_ns) {
    if (!runtime_video_publisher_ ||
        runtime_video_width_ != frame.header.width ||
        runtime_video_height_ != frame.header.height ||
        runtime_video_pixel_format_ != frame.header.pixel_format) {
      xr_video::StereoVideoShmPublisherConfig cfg;
      cfg.registry_path = cfg_.runtime_registry;
      cfg.stream_id = cfg_.runtime_stream;
      cfg.shm_name = cfg_.runtime_shm_name;
      cfg.frame_id = cfg_.runtime_frame;
      cfg.slot_count = cfg_.runtime_slots;
      cfg.width = frame.header.width;
      cfg.height = frame.header.height;
      cfg.pixel_format = static_cast<xr_video::StereoVideoPixelFormat>(frame.header.pixel_format);
      cfg.unlink_existing = cfg_.runtime_unlink_existing;
      cfg.created_by = "xr_runtime_adapter";
      runtime_video_publisher_ = std::make_unique<xr_video::StereoVideoShmPublisher>(std::move(cfg));
      runtime_video_width_ = frame.header.width;
      runtime_video_height_ = frame.header.height;
      runtime_video_pixel_format_ = frame.header.pixel_format;
      std::cout << "[xr_runtime_adapter] publishing runtime video stream "
                << cfg_.runtime_stream << " -> " << cfg_.runtime_shm_name
                << " frame=" << cfg_.runtime_frame
                << " size=" << runtime_video_width_ << "x" << runtime_video_height_ << "\n";
    }

    frame.header.timestamp_ns = now_ns;
    frame.header.publish_timestamp_ns = now_ns;
    runtime_video_publisher_->publish(std::move(frame));
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++snapshot_.runtime_video_published;
      snapshot_.runtime_output_error.clear();
    }
  }

  void set_connected(bool connected, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.runtime_output_enabled = cfg_.publish_runtime_video_shm;
    snapshot_.connected = connected;
    snapshot_.last_error = error;
  }

  void set_error(const std::string& error) { set_connected(false, error); }

  void note_reattach_attempt() {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_attempts;
  }

  void note_reattach_success() {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_successes;
  }

  void note_reattach_failure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_failures;
    snapshot_.last_error = error;
  }

  void sleep_ms(int ms) const {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_ && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  VideoInputHealthConfig cfg_;
  std::atomic_bool stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  VideoInputHealthSnapshot snapshot_;
  uint64_t first_observe_ns_ = 0;
  int64_t first_source_ts_ns_ = 0;
  int64_t last_source_ts_ns_ = 0;

  std::unique_ptr<xr_video::StereoVideoShmPublisher> runtime_video_publisher_;
  uint32_t runtime_video_width_ = 0;
  uint32_t runtime_video_height_ = 0;
  uint32_t runtime_video_pixel_format_ = 0;

};

struct HandSkeleton26InputConfig {
  std::string input = "none";  // none, shm, tcp
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45674;
  std::string registry = xr_runtime::default_tracking_registry_path();
  std::string stream = "hand_skeleton26";
  double reattach_on_stale_ms = 1000.0;
};

struct HandSkeleton26InputSnapshot {
  bool enabled = false;
  bool connected = false;
  uint64_t frames = 0;
  uint64_t last_sequence = 0;
  uint64_t sequence_gaps = 0;
  uint64_t dropped_estimate = 0;
  uint64_t latest_receive_timestamp_ns = 0;
  double age_ms = 0.0;
  uint64_t reattach_attempts = 0;
  uint64_t reattach_successes = 0;
  uint64_t reattach_failures = 0;
  std::string last_error;
  std::optional<xr_tracking::HandSkeleton26FrameF32V1> latest;
};

class HandSkeleton26InputThread {
 public:
  explicit HandSkeleton26InputThread(HandSkeleton26InputConfig cfg) : cfg_(std::move(cfg)) {}
  ~HandSkeleton26InputThread() { stop(); }

  HandSkeleton26InputThread(const HandSkeleton26InputThread&) = delete;
  HandSkeleton26InputThread& operator=(const HandSkeleton26InputThread&) = delete;

  void start() {
    if (cfg_.input == "none") return;
    stop_ = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot_ = {};
      snapshot_.enabled = true;
    }
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  HandSkeleton26InputSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot_;
  }

 private:
  void run() {
    while (!stop_) {
      try {
        note_reattach_attempt();
        if (cfg_.input == "tcp") {
          run_tcp_once();
        } else if (cfg_.input == "shm") {
          run_shm_once();
        } else {
          set_error("unknown hand_skeleton26 input: " + cfg_.input);
          return;
        }
      } catch (const std::exception& e) {
        note_reattach_failure(e.what());
        set_error(e.what());
        sleep_ms(500);
      }
    }
  }

  void run_tcp_once() {
    xr_tracking::HandSkeleton26TcpClient client(cfg_.tcp_host, cfg_.tcp_port);
    note_reattach_success();
    set_connected(true, "");
    while (!stop_) {
      auto frame = client.read_next();
      observe(std::move(frame));
    }
  }

  void run_shm_once() {
    auto info = xr_runtime::stream_info_from_registry(cfg_.registry, cfg_.stream);
    xr_runtime::TrackingRingReader<xr_tracking::HandSkeleton26FrameF32V1> reader(
        std::move(info), xr_tracking::HAND_SKELETON26_FORMAT_NAME);
    note_reattach_success();
    set_connected(true, "");
    uint64_t last_seq = 0;
    uint64_t last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
    while (!stop_) {
      auto frame = reader.latest();
      if (frame && frame->sequence > last_seq) {
        last_seq = frame->sequence;
        last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        observe(std::move(*frame));
      } else {
        const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        if (cfg_.reattach_on_stale_ms > 0.0 &&
            xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - last_frame_ns)) >= cfg_.reattach_on_stale_ms) {
          throw std::runtime_error("hand_skeleton26 SHM stream stale; reattaching");
        }
        sleep_ms(1);
      }
    }
  }

  void observe(xr_tracking::HandSkeleton26FrameF32V1 frame) {
    const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = true;
    snapshot_.last_error.clear();
    if (snapshot_.last_sequence != 0 && frame.sequence > snapshot_.last_sequence + 1) {
      ++snapshot_.sequence_gaps;
      snapshot_.dropped_estimate += frame.sequence - snapshot_.last_sequence - 1;
    }
    snapshot_.last_sequence = frame.sequence;
    snapshot_.latest_receive_timestamp_ns = now_ns;
    snapshot_.age_ms = xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - frame.timestamp_ns));
    snapshot_.latest = frame;
    ++snapshot_.frames;
  }

  void set_connected(bool connected, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = connected;
    snapshot_.last_error = error;
  }

  void set_error(const std::string& error) { set_connected(false, error); }

  void note_reattach_attempt() {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_attempts;
  }

  void note_reattach_success() {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_successes;
  }

  void note_reattach_failure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_failures;
    snapshot_.last_error = error;
  }

  void sleep_ms(int ms) const {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_ && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  HandSkeleton26InputConfig cfg_;
  std::atomic_bool stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  HandSkeleton26InputSnapshot snapshot_;
};

struct ControllerInputThreadConfig {
  xr_runtime::ControllerInputRuntimeConfig input;
};

struct ControllerInputThreadSnapshot {
  bool enabled = false;
  bool connected = false;
  uint64_t frames = 0;
  uint64_t last_sequence = 0;
  uint64_t latest_receive_timestamp_ns = 0;
  double age_ms = 0.0;
  uint64_t reattach_attempts = 0;
  uint64_t reattach_successes = 0;
  uint64_t reattach_failures = 0;
  std::string last_error;
  std::optional<xr_runtime::ControllerInputV2> latest;
};

class ControllerInputThread {
 public:
  explicit ControllerInputThread(ControllerInputThreadConfig cfg) : cfg_(std::move(cfg)) {}
  ~ControllerInputThread() { stop(); }

  ControllerInputThread(const ControllerInputThread&) = delete;
  ControllerInputThread& operator=(const ControllerInputThread&) = delete;

  void start() {
    const auto transport = xr_runtime::parse_controller_input_transport(cfg_.input.transport);
    if (transport == xr_runtime::ControllerInputTransport::NONE) return;
    stop_ = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot_ = {};
      snapshot_.enabled = true;
    }
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  ControllerInputThreadSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot_;
  }

 private:
  void run() {
    while (!stop_) {
      try {
        note_reattach_attempt();
        const auto transport = xr_runtime::parse_controller_input_transport(cfg_.input.transport);
        if (transport == xr_runtime::ControllerInputTransport::TCP) {
          run_tcp_once();
        } else if (transport == xr_runtime::ControllerInputTransport::SHM) {
          run_shm_once();
        } else {
          set_error("controller input transport not implemented in this build: " + cfg_.input.transport);
          return;
        }
      } catch (const std::exception& e) {
        note_reattach_failure(e.what());
        set_error(e.what());
        sleep_ms(500);
      }
    }
  }

  void run_tcp_once() {
    xr_runtime::ControllerInputTcpClient client(cfg_.input.host, cfg_.input.port);
    note_reattach_success();
    set_connected(true, "");
    while (!stop_) {
      auto frame = client.read_next();
      observe(std::move(frame));
    }
  }

  void run_shm_once() {
    auto info = xr_runtime::stream_info_from_registry(cfg_.input.registry, cfg_.input.stream);
    if (info.format_name == xr_runtime::CONTROLLER_INPUT_FORMAT_NAME) {
      run_shm_v1_once(std::move(info));
      return;
    }
    run_shm_v2_once(std::move(info));
  }

  void run_shm_v2_once(xr_runtime::StreamInfo info) {
    xr_runtime::TrackingRingReader<xr_runtime::ControllerInputV2> reader(
        std::move(info), xr_runtime::CONTROLLER_INPUT_V2_FORMAT_NAME);
    note_reattach_success();
    set_connected(true, "");
    uint64_t last_seq = 0;
    uint64_t last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
    while (!stop_) {
      auto frame = reader.latest();
      if (frame && frame->sequence > last_seq) {
        last_seq = frame->sequence;
        last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        observe(std::move(*frame));
      } else {
        const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        if (cfg_.input.max_age_ms > 0 &&
            xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - last_frame_ns)) >= cfg_.input.max_age_ms) {
          throw std::runtime_error("controller input SHM stream stale; reattaching");
        }
        sleep_ms(1);
      }
    }
  }

  void run_shm_v1_once(xr_runtime::StreamInfo info) {
    xr_runtime::TrackingRingReader<xr_runtime::ControllerInputV1> reader(
        std::move(info), xr_runtime::CONTROLLER_INPUT_FORMAT_NAME);
    note_reattach_success();
    set_connected(true, "");
    uint64_t last_seq = 0;
    uint64_t last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
    while (!stop_) {
      auto frame = reader.latest();
      if (frame && frame->sequence > last_seq) {
        last_seq = frame->sequence;
        last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        observe(xr_runtime::controller_input_v2_from_v1(*frame));
      } else {
        const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        if (cfg_.input.max_age_ms > 0 &&
            xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - last_frame_ns)) >= cfg_.input.max_age_ms) {
          throw std::runtime_error("controller input SHM stream stale; reattaching");
        }
        sleep_ms(1);
      }
    }
  }

  void observe(xr_runtime::ControllerInputV2 frame) {
    const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = true;
    snapshot_.last_error.clear();
    snapshot_.last_sequence = frame.sequence;
    snapshot_.latest_receive_timestamp_ns = now_ns;
    snapshot_.age_ms = xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - frame.timestamp_ns));
    snapshot_.latest = frame;
    ++snapshot_.frames;
  }

  void set_connected(bool connected, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = connected;
    snapshot_.last_error = error;
  }
  void set_error(const std::string& error) { set_connected(false, error); }
  void note_reattach_attempt() { std::lock_guard<std::mutex> lock(mu_); ++snapshot_.reattach_attempts; }
  void note_reattach_success() { std::lock_guard<std::mutex> lock(mu_); ++snapshot_.reattach_successes; }
  void note_reattach_failure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_failures;
    snapshot_.last_error = error;
  }
  void sleep_ms(int ms) const {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_ && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  ControllerInputThreadConfig cfg_;
  std::atomic_bool stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  ControllerInputThreadSnapshot snapshot_;

};

struct BodyTrackerInputConfig {
  std::string input = "none";  // none, shm, udp
  std::string registry = xr_runtime::default_tracking_registry_path();
  std::string stream = "body_trackers";
  std::string udp_bind_host = "0.0.0.0";
  int udp_bind_port = 45676;
  double reattach_on_stale_ms = 1000.0;

  bool publish_runtime_shm = false;
  std::string runtime_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string runtime_stream = "runtime_body_trackers";
  std::string runtime_shm_name = "runtime_body_trackers";
  std::string runtime_frame = "runtime_local";
  uint32_t runtime_slots = 1024;
  bool runtime_unlink_existing = true;
  StreamTransformConfig transform{};
  jitter_filter::RuntimeJitterFilterConfig jitter_filter{};
  BodyTrackerStabilityGateConfig stability_gate{};
};

struct BodyTrackerInputSnapshot {
  bool enabled = false;
  bool connected = false;
  bool runtime_output_enabled = false;
  uint64_t frames = 0;
  uint64_t runtime_published = 0;
  uint64_t last_sequence = 0;
  uint64_t sequence_gaps = 0;
  uint64_t dropped_estimate = 0;
  uint64_t latest_receive_timestamp_ns = 0;
  uint32_t tracker_count = 0;
  double age_ms = 0.0;
  uint64_t reattach_attempts = 0;
  uint64_t reattach_successes = 0;
  uint64_t reattach_failures = 0;
  uint64_t invalid_packets = 0;
  std::string last_error;
  std::optional<xr_tracking::BodyTrackerSetFrameF32V1> latest;
};

class BodyTrackerInputThread {
 public:
  explicit BodyTrackerInputThread(BodyTrackerInputConfig cfg) : cfg_(std::move(cfg)) {}
  ~BodyTrackerInputThread() { stop(); }

  BodyTrackerInputThread(const BodyTrackerInputThread&) = delete;
  BodyTrackerInputThread& operator=(const BodyTrackerInputThread&) = delete;

  void start() {
    if (cfg_.input == "none") return;
    stop_ = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      snapshot_ = {};
      snapshot_.enabled = true;
      snapshot_.runtime_output_enabled = cfg_.publish_runtime_shm;
    }
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  BodyTrackerInputSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot_;
  }

  void set_origin_snapshot(RuntimeOriginSnapshot origin) {
    std::lock_guard<std::mutex> lock(mu_);
    origin_snapshot_ = std::move(origin);
  }

 private:
  void run() {
    while (!stop_) {
      try {
        note_reattach_attempt();
        if (cfg_.input == "shm") {
          run_shm_once();
        } else if (cfg_.input == "udp") {
          run_udp_once();
        } else {
          set_error("unknown body_trackers input: " + cfg_.input);
          return;
        }
      } catch (const std::exception& e) {
        note_reattach_failure(e.what());
        set_error(e.what());
        sleep_ms(500);
      }
    }
  }

  void run_shm_once() {
    auto info = xr_runtime::stream_info_from_registry(cfg_.registry, cfg_.stream);
    xr_runtime::TrackingRingReader<xr_tracking::BodyTrackerSetFrameF32V1> reader(
        std::move(info), xr_tracking::BODY_TRACKER_SET_FORMAT_NAME);
    note_reattach_success();
    set_connected(true, "");
    uint64_t last_seq = 0;
    uint64_t last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
    while (!stop_) {
      auto frame = reader.latest();
      if (frame && frame->sequence > last_seq) {
        last_seq = frame->sequence;
        last_frame_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        observe(std::move(*frame));
      } else {
        const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        maybe_publish_synthetic(now_ns);
        if (cfg_.reattach_on_stale_ms > 0.0 &&
            xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - last_frame_ns)) >= cfg_.reattach_on_stale_ms) {
          throw std::runtime_error("body_trackers SHM stream stale; reattaching");
        }
        sleep_ms(1);
      }
    }
  }

  void run_udp_once() {
    xr_runtime::platform::UdpReceiver receiver(
        cfg_.udp_bind_host,
        static_cast<uint16_t>(cfg_.udp_bind_port),
        xr_runtime::platform::UdpReceiveMode::NonBlocking);
    note_reattach_success();
    set_connected(true, "");
    while (!stop_) {
      alignas(8) uint8_t buffer[sizeof(xr_tracking::BodyTrackerSetUdpHeaderV1) +
                                sizeof(xr_tracking::BodyTrackerSetFrameF32V1)];
      const auto n_opt = receiver.receive(buffer, sizeof(buffer));
      if (!n_opt) {
        const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());
        maybe_publish_synthetic(now_ns);
        sleep_ms(1);
        continue;
      }
      try {
        auto frame = xr_tracking::decode_body_tracker_set_udp_packet(buffer, *n_opt);
        observe(std::move(frame));
      } catch (const std::exception& e) {
        note_invalid_packet(e.what());
      }
    }
  }

  void observe(xr_tracking::BodyTrackerSetFrameF32V1 frame) {
    const uint64_t now_ns = static_cast<uint64_t>(xr_runtime::now_ns());

    // Body trackers are expected to be in a world/source frame, not HMD-local.
    // First apply their configured basis transform, then apply the same runtime
    // origin/recenter as HMD so FBT and HMD share one runtime coordinate frame.
    apply_body_tracker_frame_transform(frame, cfg_.transform, nullptr);
    RuntimeOriginSnapshot origin;
    {
      std::lock_guard<std::mutex> lock(mu_);
      origin = origin_snapshot_;
    }
    apply_body_tracker_origin_transform(frame, origin);
    stability_filter_.configure(cfg_.stability_gate);
    frame = stability_filter_.filter_observed(std::move(frame), now_ns);
    runtime_jitter_filter_.configure(cfg_.jitter_filter);
    runtime_jitter_filter_.filter_body_trackers(frame);

    if (cfg_.publish_runtime_shm) {
      publish_runtime(frame, now_ns);
    }

    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.connected = true;
    snapshot_.runtime_output_enabled = cfg_.publish_runtime_shm;
    snapshot_.last_error.clear();
    if (snapshot_.last_sequence != 0 && frame.sequence > snapshot_.last_sequence + 1) {
      ++snapshot_.sequence_gaps;
      snapshot_.dropped_estimate += frame.sequence - snapshot_.last_sequence - 1;
    }
    snapshot_.last_sequence = frame.sequence;
    snapshot_.latest_receive_timestamp_ns = now_ns;
    snapshot_.tracker_count = frame.tracker_count;
    snapshot_.age_ms = xr_runtime::ns_to_ms(static_cast<int64_t>(now_ns - frame.timestamp_ns));
    snapshot_.latest = frame;
    ++snapshot_.frames;
  }


  void maybe_publish_synthetic(uint64_t now_ns) {
    if (!cfg_.publish_runtime_shm || !cfg_.stability_gate.enabled) return;
    stability_filter_.configure(cfg_.stability_gate);
    auto frame = stability_filter_.predicted_frame(now_ns);
    if (!frame) return;
    runtime_jitter_filter_.configure(cfg_.jitter_filter);
    runtime_jitter_filter_.filter_body_trackers(*frame);
    publish_runtime(std::move(*frame), now_ns);
  }

  void publish_runtime(xr_tracking::BodyTrackerSetFrameF32V1 frame, uint64_t now_ns) {
    if (!runtime_publisher_) {
      xr_tracking::BodyTrackerSetShmPublisherConfig cfg;
      cfg.registry_path = cfg_.runtime_registry;
      cfg.stream_id = cfg_.runtime_stream;
      cfg.shm_name = cfg_.runtime_shm_name;
      cfg.frame_id = cfg_.runtime_frame;
      cfg.slot_count = cfg_.runtime_slots;
      cfg.unlink_existing = cfg_.runtime_unlink_existing;
      cfg.created_by = "xr_runtime_adapter";
      cfg.space = xr_tracking::BODY_TRACKER_SPACE_RUNTIME_HMD;
      cfg.source = frame.source;
      runtime_publisher_ = std::make_unique<xr_tracking::BodyTrackerSetShmPublisher>(std::move(cfg));
      std::cout << "[xr_runtime_adapter] publishing runtime body trackers stream "
                << cfg_.runtime_stream << " -> " << cfg_.runtime_shm_name
                << " frame=" << cfg_.runtime_frame << "\n";
    }

    frame.timestamp_ns = now_ns;
    if (frame.source_timestamp_ns == 0) frame.source_timestamp_ns = now_ns;
    runtime_publisher_->publish(std::move(frame));
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++snapshot_.runtime_published;
    }
  }

  void set_connected(bool connected, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot_.enabled = true;
    snapshot_.runtime_output_enabled = cfg_.publish_runtime_shm;
    snapshot_.connected = connected;
    snapshot_.last_error = error;
  }
  void set_error(const std::string& error) { set_connected(false, error); }
  void note_reattach_attempt() { std::lock_guard<std::mutex> lock(mu_); ++snapshot_.reattach_attempts; }
  void note_reattach_success() { std::lock_guard<std::mutex> lock(mu_); ++snapshot_.reattach_successes; }
  void note_reattach_failure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.reattach_failures;
    snapshot_.last_error = error;
  }
  void note_invalid_packet(const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    ++snapshot_.invalid_packets;
    snapshot_.last_error = error;
  }
  void sleep_ms(int ms) const {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_ && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  BodyTrackerInputConfig cfg_;
  std::atomic_bool stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  BodyTrackerInputSnapshot snapshot_;
  RuntimeOriginSnapshot origin_snapshot_;
  jitter_filter::RuntimeJitterFilter runtime_jitter_filter_{};
  BodyTrackerStabilityFilter stability_filter_{};
  std::unique_ptr<xr_tracking::BodyTrackerSetShmPublisher> runtime_publisher_;
};


}  // namespace

int main(int argc, char** argv) {
  std::string registry_path = xr_runtime::default_tracking_registry_path();
  std::string hmd_stream = "hmd_pose";
  std::string hand_stream = "hand_tracking";
  bool hmd_3dof_priority = false;
  std::string hmd_3dof_registry = xr_runtime::default_tracking_registry_path();
  std::string hmd_3dof_stream = "hmd_pose_3dof";
  double hmd_3dof_reattach_on_stale_ms = 500.0;
  std::string adapter_type = "logging";
  std::string mode = "event";
  std::string input = xr_runtime_adapter::platform::default_tracking_input_transport();
  std::string udp_bind_host = "127.0.0.1";
  uint16_t udp_bind_port = 45670;
  std::string udp_frame_id = "tracking_world";
  size_t udp_pump_limit = 256;

  double duration_s = 0.0;
  int print_every = 30;
  double poll_ms = 1.0;
  double tick_rate_hz = 90.0;
  double prediction_ms = 15.0;
  double max_prediction_ms = 50.0;
  double max_hmd_age_ms = 250.0;
  double max_hand_age_ms = 250.0;
  bool require_hmd = false;
  bool require_hands = false;
  bool no_prediction = false;
  bool consume_stale_as_lost = false;
  std::string hmd_stale_policy_name = "lost";
  std::string hand_stale_policy_name = "lost";
  double hmd_reattach_on_stale_ms = 0.0;
  double hand_reattach_on_stale_ms = 0.0;
  double hmd_hold_last_max_ms = 0.0;
  double hand_hold_last_max_ms = 300.0;

  bool runtime_hmd_pose_stability_filter = false;
  double runtime_hmd_pose_stability_window_ms = 250.0;
  double runtime_hmd_pose_stability_max_distance_cm = 50.0;

  std::string origin_mode = "none";

  std::string runtime_frame = "runtime_local";
  std::string tracking_transform_config_path;

  bool recenter_on_reset_counter = true;

  bool no_recenter_on_reset_counter = false;

  std::string controller_input_mode = "hand_tracking_only";

  std::string controller_input_transport = "none";

  std::string left_controller_id = "auto";

  std::string right_controller_id = "auto";

  int max_controller_age_ms = 250;

  xr_runtime::ControllerInputRuntimeConfig controller_input_config;

  bool publish_runtime_pose_shm = false;
  bool publish_runtime_pose_udp = false;
  std::string runtime_pose_udp_host = "127.0.0.1";
  int runtime_pose_udp_port = 45800;
  bool no_runtime_pose_unlink = false;
  std::string runtime_pose_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string runtime_pose_stream = "runtime_hmd_pose";
  std::string runtime_pose_shm_name = "runtime_hmd_pose";
  uint32_t runtime_pose_slots = 1024;

  bool publish_runtime_hand_shm = false;
  bool publish_runtime_hand_udp = false;
  std::string runtime_hand_udp_host = "127.0.0.1";
  int runtime_hand_udp_port = 45801;
  bool no_runtime_hand_unlink = false;
  std::string runtime_hand_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string runtime_hand_stream = "runtime_hand_tracking";
  std::string runtime_hand_shm_name = "runtime_hand_tracking";
  uint32_t runtime_hand_slots = 1024;

  std::string runtime_controller_mode = "hand_tracking_with_button_priority";
  std::string runtime_controller_lost_hand_pose_fallback = "pose_invalid";
  bool publish_runtime_controller_state_shm = false;
  bool publish_runtime_controller_state_udp = false;
  std::string runtime_controller_state_udp_host = "127.0.0.1";
  int runtime_controller_state_udp_port = 45802;
  bool no_runtime_controller_state_unlink = false;
  std::string runtime_controller_state_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string runtime_controller_state_stream = "runtime_controller_state";
  std::string runtime_controller_state_shm_name = "runtime_controller_state";
  uint32_t runtime_controller_state_slots = 1024;
  float runtime_controller_left_offset_x = -0.22f;
  float runtime_controller_left_offset_y = -0.22f;
  float runtime_controller_left_offset_z = -0.35f;
  float runtime_controller_right_offset_x = 0.22f;
  float runtime_controller_right_offset_y = -0.22f;
  float runtime_controller_right_offset_z = -0.35f;
  std::array<float, 4> runtime_controller_left_static_orientation_xyzw{0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 4> runtime_controller_right_static_orientation_xyzw{0.0f, 0.0f, 0.0f, 1.0f};

  std::string video_input = "none";  // none, tcp, shm
  std::string video_tcp_host = "127.0.0.1";
  int video_tcp_port = 45700;
  std::string video_registry = "/tmp/xr_video_streams.json";
  std::string video_stream = "stereo_video";
  double video_reattach_on_stale_ms = 1000.0;

  bool publish_runtime_video_shm = false;
  bool no_runtime_video_unlink = false;
  std::string runtime_video_registry = "/tmp/runtime_video_streams.json";
  std::string runtime_video_stream = "runtime_stereo_video";
  std::string runtime_video_shm_name = "runtime_stereo_video";
  std::string runtime_video_frame = "runtime_video";
  uint32_t runtime_video_slots = 8;

  std::string body_trackers_input = "none";  // none, shm, udp
  std::string spatial_proxy_mesh_input = "none"; // none, shm, udp
  std::string spatial_proxy_mesh_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string spatial_proxy_mesh_stream = "spatial_proxy_mesh";
  std::string spatial_proxy_mesh_udp_bind_host = "0.0.0.0";
  int spatial_proxy_mesh_udp_bind_port = 45740;
  double spatial_proxy_mesh_reattach_on_stale_ms = 1000.0;
  double spatial_proxy_mesh_max_source_age_ms = 1000.0;
  std::string spatial_proxy_mesh_triangle_winding = "auto"; // auto, keep, swap
  double spatial_proxy_mesh_rotate_deg_x = 0.0;
  double spatial_proxy_mesh_rotate_deg_y = 0.0;
  double spatial_proxy_mesh_rotate_deg_z = 0.0;
  bool publish_runtime_spatial_proxy_mesh_shm = false;
  bool no_runtime_spatial_proxy_mesh_unlink = false;
  std::string runtime_spatial_proxy_mesh_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string runtime_spatial_proxy_mesh_stream = "runtime_spatial_proxy_mesh";
  std::string runtime_spatial_proxy_mesh_shm_name = "runtime_spatial_proxy_mesh";
  uint32_t runtime_spatial_proxy_mesh_slots = 8;
  std::string body_trackers_registry = xr_runtime::default_tracking_registry_path();
  std::string body_trackers_stream = "body_trackers";
  std::string body_trackers_udp_bind_host = "0.0.0.0";
  int body_trackers_udp_bind_port = 45676;
  double body_trackers_reattach_on_stale_ms = 1000.0;

  bool publish_runtime_body_trackers_shm = false;
  bool no_runtime_body_trackers_unlink = false;
  std::string runtime_body_trackers_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string runtime_body_trackers_stream = "runtime_body_trackers";
  std::string runtime_body_trackers_shm_name = "runtime_body_trackers";
  uint32_t runtime_body_trackers_slots = 1024;
  bool runtime_body_tracker_stability_gate = false;
  double runtime_body_tracker_hold_lost_ms = 150.0;
  double runtime_body_tracker_predict_lost_ms = 350.0;
  double runtime_body_tracker_max_prediction_velocity_mps = 0.8;
  double runtime_body_tracker_max_prediction_acceleration_mps2 = 0.0;
  double runtime_body_tracker_prediction_damping = 0.35;
  double runtime_body_tracker_prediction_publish_hz = 90.0;
  std::string runtime_body_tracker_predicted_status = "tracking";

  std::string hand_skeleton26_input = "none";  // none, shm, tcp
  std::string hand_skeleton26_registry = xr_runtime::default_tracking_registry_path();
  std::string hand_skeleton26_stream = "hand_skeleton26";
  std::string hand_skeleton26_tcp_host = "127.0.0.1";
  int hand_skeleton26_tcp_port = 45674;
  double hand_skeleton26_reattach_on_stale_ms = 1000.0;
  bool hand_skeleton26_derive_gestures = true;
  bool runtime_derive_hand_gestures = true;
  bool runtime_derive_hand_gestures_with_controller_input = false;
  bool runtime_derived_gestures_require_fresh_tracking = true;
  double runtime_derived_gesture_latch_ms = 120.0;
  bool runtime_ignore_backend_hand_gestures = false;
  bool runtime_left_hand_gestures_enabled = true;
  bool runtime_right_hand_gestures_enabled = true;
  bool runtime_derive_extra_gesture_buttons = true;
  bool runtime_derive_extra_gesture_buttons_with_controller_input = false;
  bool override_controller_block_gestures_while_stream_present = true;
  double override_controller_gesture_block_latch_ms = 2000.0;

  bool runtime_hand_stability_gate = false;
  double runtime_hand_gate_max_jump_m = 0.08;
  int runtime_hand_gate_confirm_frames = 4;
  double runtime_hand_gate_confirm_max_step_m = 0.03;
  double runtime_hand_gate_hold_lost_ms = 180.0;
  double runtime_hand_gate_max_continuity_velocity_mps = 1.25;
  double runtime_hand_gate_predict_lost_ms = 0.0;
  double runtime_hand_gate_max_prediction_velocity_mps = 2.0;
  double runtime_hand_gate_prediction_damping = 0.5;
  double runtime_hand_gate_reacquire_blend_ms = 0.0;
  std::string runtime_hand_gate_debug_csv;

  bool runtime_jitter_filter_enabled = false;
  double runtime_jitter_filter_hmd_cm = 0.20;
  double runtime_jitter_filter_tracker_cm = 0.50;
  double runtime_jitter_filter_hmd_deg = 0.15;
  double runtime_jitter_filter_tracker_deg = 0.75;

  std::string derived_thumbs_up_button = "a";
  std::string derived_index_point_button = "b";
  float derived_pinch_active_threshold = 0.70f;
  float derived_grab_active_threshold = 0.70f;
  float derived_pinch_deactive_threshold = 0.35f;
  float derived_grab_deactive_threshold = 0.35f;
  float derived_pinch_response_start = 0.80f;
  float derived_grab_response_start = 0.80f;
  float derived_thumbs_up_active_threshold = 0.80f;
  float derived_index_point_active_threshold = 0.80f;
  float derived_thumbs_up_deactive_threshold = 0.45f;
  float derived_index_point_deactive_threshold = 0.45f;
  float derived_extra_gesture_response_start = 0.65f;
  double derived_extra_gesture_hold_ms = 120.0;

  float controller_trigger_pinch_threshold = 0.55f;
  float controller_grip_grab_threshold = 0.55f;

  CLI::App app{"Runtime-neutral XR adapter scaffold for hmd_pose + hand_tracking streams"};
  app.add_option("--registry", registry_path, "tracking stream registry");
  app.add_option("--hmd-stream", hmd_stream, "HMD pose stream id for SHM input");
  app.add_option("--hand-stream", hand_stream, "hand tracking stream id for SHM input");
  app.add_flag("--hmd-3dof-priority", hmd_3dof_priority,
               "SHM input: prefer a fresh 3DoF HMD pose stream over the primary HMD stream when available");
  app.add_option("--hmd-3dof-registry", hmd_3dof_registry,
                 "SHM registry containing the priority 3DoF HMD pose stream");
  app.add_option("--hmd-3dof-stream", hmd_3dof_stream,
                 "Priority 3DoF HMD pose stream id");
  app.add_option("--hmd-3dof-reattach-on-stale-ms", hmd_3dof_reattach_on_stale_ms,
                 "SHM 3DoF HMD reader reattach interval while stale/missing; <=0 disables");
  app.add_option("--adapter", adapter_type, "adapter: logging, monado, steamvr");
  app.add_option("--mode", mode, "consume mode: event or tick");
  app.add_option("--input", input, "tracking input: shm or udp");
  app.add_option("--udp-bind-host", udp_bind_host, "UDP input bind host");
  app.add_option("--udp-bind-port", udp_bind_port, "UDP input bind port");
  app.add_option("--udp-frame-id", udp_frame_id, "frame id assigned to UDP tracking packets");
  app.add_option("--udp-pump-limit", udp_pump_limit, "maximum UDP packets to drain per loop");
  app.add_option("--duration", duration_s, "run duration in seconds; 0 means until Ctrl+C");
  app.add_option("--print-every", print_every, "print every N consumed frames");
  app.add_option("--poll-ms", poll_ms, "event mode poll interval in milliseconds");
  app.add_option("--tick-rate", tick_rate_hz, "tick mode runtime rate in Hz");
  app.add_option("--prediction-ms", prediction_ms, "tick mode prediction horizon in milliseconds");
  app.add_option("--max-prediction-ms", max_prediction_ms,
                 "maximum absolute HMD prediction interval in milliseconds");
  app.add_option("--max-hmd-age-ms", max_hmd_age_ms,
                 "maximum local/source HMD age before treating pose as stale/lost; <=0 disables");
  app.add_option("--max-hand-age-ms", max_hand_age_ms,
                 "maximum local/source hand age before treating hands as stale/lost; <=0 disables");
  app.add_flag("--require-hmd", require_hmd, "SHM input: fail startup if hmd stream is absent");
  app.add_flag("--require-hands", require_hands, "SHM input: fail startup if hand stream is absent");
  app.add_flag("--no-prediction", no_prediction, "tick mode: use latest raw HMD pose without prediction");
  app.add_flag("--consume-stale-as-lost", consume_stale_as_lost,
               "tick mode: keep consuming runtime ticks after tracking becomes stale, with hmd/hand marked lost/invalid");
  app.add_option("--hmd-stale-policy", hmd_stale_policy_name,
                 "SHM HMD stale policy: lost, hold_last, hold_then_lost");
  app.add_option("--hand-stale-policy", hand_stale_policy_name,
                 "SHM hand stale policy: lost, hold_last, hold_then_lost");
  app.add_option("--hmd-reattach-on-stale-ms", hmd_reattach_on_stale_ms,
                 "SHM HMD reader reattach interval while stale/missing; <=0 disables");
  app.add_option("--hand-reattach-on-stale-ms", hand_reattach_on_stale_ms,
                 "SHM hand reader reattach interval while stale/missing; <=0 disables");
  app.add_option("--hmd-hold-last-max-ms", hmd_hold_last_max_ms,
                 "Maximum HMD hold duration for hold_then_lost; <=0 means no hold_then_lost hold. hold_last ignores this");
  app.add_option("--hand-hold-last-max-ms", hand_hold_last_max_ms,
                 "Maximum hand hold duration for hold_then_lost; <=0 means no hold_then_lost hold. hold_last ignores this");

  app.add_flag("--runtime-hmd-pose-stability-filter", runtime_hmd_pose_stability_filter,
               "Enable runtime-side HMD pose speed/jump stability filter before stale hold-last and runtime transforms");
  app.add_option("--runtime-hmd-pose-stability-window-ms", runtime_hmd_pose_stability_window_ms,
                 "Runtime HMD pose stability filter time window in milliseconds");
  app.add_option("--runtime-hmd-pose-stability-max-distance-cm", runtime_hmd_pose_stability_max_distance_cm,
                 "Runtime HMD pose stability filter max allowed travel distance in centimeters during the configured window");

  app.add_option("--origin-mode", origin_mode,

                 "Runtime origin mode: none, start_pose, yaw_only");

  app.add_option("--runtime-frame", runtime_frame,

                 "Output/runtime frame name used after origin transform");

  app.add_option("--tracking-transform-config", tracking_transform_config_path,
                 "Optional JSON config with per-stream axis_map/invert/offset/hmd_relative transforms");

  app.add_flag("--recenter-on-reset-counter", recenter_on_reset_counter,

               "Re-capture runtime origin when HMD reset_counter changes");

  app.add_flag("--no-recenter-on-reset-counter", no_recenter_on_reset_counter,

               "Do not re-capture runtime origin on HMD reset_counter changes");


  app.add_option("--controller-input-mode", controller_input_mode,


                 "Input composition mode: hand_tracking_only, hand_plus_controller, controller_buttons_only");


  app.add_option("--controller-input-transport", controller_input_transport,


                 "External controller input transport: none, shm, udp, tcp, named_pipe");


  app.add_option("--left-controller-id", left_controller_id,


                 "Left controller selector: auto, evdev path, Bluetooth MAC, or stable device id");


  app.add_option("--right-controller-id", right_controller_id,


                 "Right controller selector: auto, evdev path, Bluetooth MAC, or stable device id");


  app.add_option("--max-controller-age-ms", max_controller_age_ms,


                 "Maximum external controller input age before considered stale");



  app.add_option("--controller-input-stream", controller_input_config.stream,



                 "Controller input stream name in tracking registry for shm transport");



  app.add_option("--controller-input-registry", controller_input_config.registry,



                 "Tracking registry path for controller input shm transport");



  app.add_option("--controller-input-host", controller_input_config.host,



                 "Controller input host for udp/tcp transport");



  app.add_option("--controller-input-port", controller_input_config.port,



                 "Controller input port for udp/tcp transport");



  app.add_option("--controller-input-named-pipe", controller_input_config.named_pipe,



                 "Controller input named pipe path for Windows local transport");



  app.add_option("--controller-input-conflict-policy", controller_input_config.conflict_policy,



                 "Controller/hand button conflict policy: controller_override, additive, hand_override");



  app.add_option("--controller-input-stale-policy", controller_input_config.stale_policy,



                 "Controller stale policy: zero_on_stale, hold_last");

  app.add_option("--override-controller-block-gestures-while-stream-present",
                 override_controller_block_gestures_while_stream_present,
                 "In controller_buttons_runtime_only, block hand gesture input while override_controller stream is fresh/recently seen; hand_plus_controller is unaffected");

  app.add_option("--override-controller-gesture-block-latch-ms",
                 override_controller_gesture_block_latch_ms,
                 "In controller_buttons_runtime_only, keep hand gestures blocked this long after the last fresh override_controller frame; 0 blocks only the current fresh frame");

  app.add_flag("--publish-runtime-pose-shm", publish_runtime_pose_shm,
               "Publish RUNTIME_HMD_POSE_V1 output for Monado/OpenVR runtime drivers");
  app.add_flag("--publish-runtime-pose-udp", publish_runtime_pose_udp,
               "Publish RUNTIME_HMD_POSE_V1 over UDP for native Windows/OpenVR drivers");
  app.add_option("--runtime-pose-udp-host", runtime_pose_udp_host,
                 "UDP host for --publish-runtime-pose-udp");
  app.add_option("--runtime-pose-udp-port", runtime_pose_udp_port,
                 "UDP port for --publish-runtime-pose-udp");
  app.add_option("--runtime-pose-registry", runtime_pose_registry,
                 "runtime pose output registry path");
  app.add_option("--runtime-pose-stream", runtime_pose_stream,
                 "runtime pose output stream id");
  app.add_option("--runtime-pose-shm-name", runtime_pose_shm_name,
                 "runtime pose output POSIX SHM name");
  app.add_option("--runtime-pose-slots", runtime_pose_slots,
                 "runtime pose output ring slot count");
  app.add_flag("--no-runtime-pose-unlink", no_runtime_pose_unlink,
               "Do not unlink existing runtime pose SHM before publishing");

  app.add_flag("--publish-runtime-hand-shm", publish_runtime_hand_shm,
               "Publish 21-joint hand-tracking output for Monado/OpenVR runtime drivers");
  app.add_flag("--publish-runtime-hand-udp", publish_runtime_hand_udp,
               "Publish 21-joint hand-tracking output over UDP for native Windows/OpenVR drivers");
  app.add_option("--runtime-hand-udp-host", runtime_hand_udp_host,
                 "UDP host for --publish-runtime-hand-udp");
  app.add_option("--runtime-hand-udp-port", runtime_hand_udp_port,
                 "UDP port for --publish-runtime-hand-udp");
  app.add_option("--runtime-hand-registry", runtime_hand_registry,
                 "runtime hand output registry path");
  app.add_option("--runtime-hand-stream", runtime_hand_stream,
                 "runtime hand output stream id");
  app.add_option("--runtime-hand-shm-name", runtime_hand_shm_name,
                 "runtime hand output POSIX SHM name");
  app.add_option("--runtime-hand-slots", runtime_hand_slots,
                 "runtime hand output ring slot count");
  app.add_flag("--no-runtime-hand-unlink", no_runtime_hand_unlink,
               "Do not unlink existing runtime hand SHM before publishing");

  CLI::Option* runtime_controller_mode_option =
      app.add_option("--runtime-controller-mode", runtime_controller_mode,
                     "Runtime controller synthesis mode. Normally set this in config controller_override.mode; CLI overrides config.");
  CLI::Option* runtime_controller_lost_hand_pose_fallback_option =
      app.add_option("--runtime-controller-lost-hand-pose-fallback", runtime_controller_lost_hand_pose_fallback,
                     "Hand-tracking controller modes: fallback pose when a hand is lost: pose_invalid, hmd_relative_with_input, hmd_relative_with_controller_present, or hmd_relative. hmd_relative_with_input keeps held override-controller hands body/HMD-relative only while physical buttons/axes are active.");
  CLI::Option* publish_runtime_controller_state_shm_option =
      app.add_flag("--publish-runtime-controller-state-shm", publish_runtime_controller_state_shm,
                   "Publish RUNTIME_CONTROLLER_STATE_V1 output for OpenVR/Monado controller drivers");
  app.add_flag("--publish-runtime-controller-state-udp", publish_runtime_controller_state_udp,
               "Publish RUNTIME_CONTROLLER_STATE_V1 over UDP for native Windows/OpenVR drivers");
  app.add_option("--runtime-controller-state-udp-host", runtime_controller_state_udp_host,
                 "UDP host for --publish-runtime-controller-state-udp");
  app.add_option("--runtime-controller-state-udp-port", runtime_controller_state_udp_port,
                 "UDP port for --publish-runtime-controller-state-udp");
  CLI::Option* runtime_controller_state_registry_option =
      app.add_option("--runtime-controller-state-registry", runtime_controller_state_registry,
                     "runtime controller state output registry path");
  CLI::Option* runtime_controller_state_stream_option =
      app.add_option("--runtime-controller-state-stream", runtime_controller_state_stream,
                     "runtime controller state output stream id");
  CLI::Option* runtime_controller_state_shm_name_option =
      app.add_option("--runtime-controller-state-shm-name", runtime_controller_state_shm_name,
                     "runtime controller state output POSIX SHM name");
  CLI::Option* runtime_controller_state_slots_option =
      app.add_option("--runtime-controller-state-slots", runtime_controller_state_slots,
                     "runtime controller state output ring slot count");
  app.add_flag("--no-runtime-controller-state-unlink", no_runtime_controller_state_unlink,
               "Do not unlink existing runtime controller state SHM before publishing");
  CLI::Option* runtime_controller_left_offset_x_option =
      app.add_option("--runtime-controller-left-offset-x", runtime_controller_left_offset_x,
                     "controller_only_hmd_relative_pose: left controller HMD-relative X offset in metres; normally set in config controller_override.synthetic_hmd_relative_pose.left_offset_m");
  CLI::Option* runtime_controller_left_offset_y_option =
      app.add_option("--runtime-controller-left-offset-y", runtime_controller_left_offset_y,
                     "controller_only_hmd_relative_pose: left controller HMD-relative Y offset in metres; normally set in config controller_override.synthetic_hmd_relative_pose.left_offset_m");
  CLI::Option* runtime_controller_left_offset_z_option =
      app.add_option("--runtime-controller-left-offset-z", runtime_controller_left_offset_z,
                     "controller_only_hmd_relative_pose: left controller HMD-relative Z offset in metres; normally set in config controller_override.synthetic_hmd_relative_pose.left_offset_m");
  CLI::Option* runtime_controller_right_offset_x_option =
      app.add_option("--runtime-controller-right-offset-x", runtime_controller_right_offset_x,
                     "controller_only_hmd_relative_pose: right controller HMD-relative X offset in metres; normally set in config controller_override.synthetic_hmd_relative_pose.right_offset_m");
  CLI::Option* runtime_controller_right_offset_y_option =
      app.add_option("--runtime-controller-right-offset-y", runtime_controller_right_offset_y,
                     "controller_only_hmd_relative_pose: right controller HMD-relative Y offset in metres; normally set in config controller_override.synthetic_hmd_relative_pose.right_offset_m");
  CLI::Option* runtime_controller_right_offset_z_option =
      app.add_option("--runtime-controller-right-offset-z", runtime_controller_right_offset_z,
                     "controller_only_hmd_relative_pose: right controller HMD-relative Z offset in metres; normally set in config controller_override.synthetic_hmd_relative_pose.right_offset_m");

  app.add_option("--video-input", video_input,
                 "Optional stereo video input side-channel: none, tcp, or shm");
  app.add_option("--video-tcp-host", video_tcp_host,
                 "XR stereo video TCP host used with --video-input tcp");
  app.add_option("--video-tcp-port", video_tcp_port,
                 "XR stereo video TCP port used with --video-input tcp");
  app.add_option("--video-registry", video_registry,
                 "XR stereo video registry path used with --video-input shm");
  app.add_option("--video-stream", video_stream,
                 "XR stereo video stream id used with --video-input shm");
  app.add_option("--video-reattach-on-stale-ms", video_reattach_on_stale_ms,
                 "XR stereo video SHM reader reattach interval while stale/missing; <=0 disables");

  app.add_flag("--publish-runtime-video-shm", publish_runtime_video_shm,
               "Publish runtime-owned XR_STEREO_VIDEO_V1 output");
  app.add_option("--runtime-video-registry", runtime_video_registry,
                 "runtime video output registry path");
  app.add_option("--runtime-video-stream", runtime_video_stream,
                 "runtime video output stream id");
  app.add_option("--runtime-video-shm-name", runtime_video_shm_name,
                 "runtime video output POSIX SHM name");
  app.add_option("--runtime-video-frame", runtime_video_frame,
                 "runtime video output frame id");
  app.add_option("--runtime-video-slots", runtime_video_slots,
                 "runtime video output ring slot count");
  app.add_flag("--no-runtime-video-unlink", no_runtime_video_unlink,
               "Do not unlink existing runtime video SHM before publishing");

  app.add_option("--body-trackers-input", body_trackers_input,
                 "Optional external body tracker set input: none, shm, or udp");
  app.add_option("--spatial-proxy-mesh-input", spatial_proxy_mesh_input,
                 "Optional spatial live-depth-grid/proxy-mesh input: none, shm, or udp");
  app.add_option("--spatial-proxy-mesh-registry", spatial_proxy_mesh_registry,
                 "Registry path used with --spatial-proxy-mesh-input shm");
  app.add_option("--spatial-proxy-mesh-stream", spatial_proxy_mesh_stream,
                 "Spatial proxy mesh input stream id");
  app.add_option("--spatial-proxy-mesh-udp-bind-host", spatial_proxy_mesh_udp_bind_host,
                 "UDP bind host used with --spatial-proxy-mesh-input udp");
  app.add_option("--spatial-proxy-mesh-udp-bind-port", spatial_proxy_mesh_udp_bind_port,
                 "UDP bind port used with --spatial-proxy-mesh-input udp");
  app.add_option("--spatial-proxy-mesh-reattach-on-stale-ms", spatial_proxy_mesh_reattach_on_stale_ms,
                 "spatial_proxy_mesh SHM reader reattach interval while stale/missing; <=0 disables");
  app.add_option("--spatial-proxy-mesh-max-source-age-ms", spatial_proxy_mesh_max_source_age_ms,
                 "Max source age before dropping proxy mesh frames; <=0 disables");
  app.add_option("--spatial-proxy-mesh-triangle-winding", spatial_proxy_mesh_triangle_winding,
                 "Triangle winding for transformed runtime mesh: auto, keep, or swap");
  app.add_option("--spatial-proxy-mesh-rotate-deg-x", spatial_proxy_mesh_rotate_deg_x,
                 "Extra runtime-space spatial mesh rotation around X axis in degrees; debug/calibration override");
  app.add_option("--spatial-proxy-mesh-rotate-deg-y", spatial_proxy_mesh_rotate_deg_y,
                 "Extra runtime-space spatial mesh rotation around Y axis in degrees; debug/calibration override");
  app.add_option("--spatial-proxy-mesh-rotate-deg-z", spatial_proxy_mesh_rotate_deg_z,
                 "Extra runtime-space spatial mesh rotation around Z axis in degrees; debug/calibration override");
  app.add_flag("--publish-runtime-spatial-proxy-mesh-shm", publish_runtime_spatial_proxy_mesh_shm,
               "Publish transformed runtime_spatial_proxy_mesh SHM output");
  app.add_option("--runtime-spatial-proxy-mesh-registry", runtime_spatial_proxy_mesh_registry,
                 "runtime spatial proxy mesh output registry path");
  app.add_option("--runtime-spatial-proxy-mesh-stream", runtime_spatial_proxy_mesh_stream,
                 "runtime spatial proxy mesh output stream id");
  app.add_option("--runtime-spatial-proxy-mesh-shm-name", runtime_spatial_proxy_mesh_shm_name,
                 "runtime spatial proxy mesh output POSIX SHM name");
  app.add_option("--runtime-spatial-proxy-mesh-slots", runtime_spatial_proxy_mesh_slots,
                 "runtime spatial proxy mesh output ring slot count");
  app.add_flag("--no-runtime-spatial-proxy-mesh-unlink", no_runtime_spatial_proxy_mesh_unlink,
               "Do not unlink existing runtime spatial proxy mesh SHM before publishing");

  app.add_option("--body-trackers-registry", body_trackers_registry,
                 "Tracking registry path used with --body-trackers-input shm");
  app.add_option("--body-trackers-stream", body_trackers_stream,
                 "Stream id used with --body-trackers-input shm");
  app.add_option("--body-trackers-udp-bind-host", body_trackers_udp_bind_host,
                 "UDP bind host used with --body-trackers-input udp");
  app.add_option("--body-trackers-udp-bind-port", body_trackers_udp_bind_port,
                 "UDP bind port used with --body-trackers-input udp");
  app.add_option("--body-trackers-reattach-on-stale-ms", body_trackers_reattach_on_stale_ms,
                 "body_trackers SHM reader reattach interval while stale/missing; <=0 disables");

  app.add_flag("--publish-runtime-body-trackers-shm", publish_runtime_body_trackers_shm,
               "Publish BODY_TRACKER_SET_F32_V1 output for Monado/OpenVR runtime drivers");
  app.add_option("--runtime-body-trackers-registry", runtime_body_trackers_registry,
                 "runtime body trackers output registry path");
  app.add_option("--runtime-body-trackers-stream", runtime_body_trackers_stream,
                 "runtime body trackers output stream id");
  app.add_option("--runtime-body-trackers-shm-name", runtime_body_trackers_shm_name,
                 "runtime body trackers output POSIX SHM name");
  app.add_option("--runtime-body-trackers-slots", runtime_body_trackers_slots,
                 "runtime body trackers output ring slot count");
  app.add_flag("--no-runtime-body-trackers-unlink", no_runtime_body_trackers_unlink,
               "Do not unlink existing runtime body trackers SHM before publishing");

  app.add_flag("--runtime-body-tracker-stability-gate", runtime_body_tracker_stability_gate,
               "Enable runtime-side body tracker hold/prediction gate; disabled by default");
  app.add_option("--runtime-body-tracker-hold-lost-ms", runtime_body_tracker_hold_lost_ms,
                 "Hold last valid body tracker pose for this many ms after tracker loss");
  app.add_option("--runtime-body-tracker-predict-lost-ms", runtime_body_tracker_predict_lost_ms,
                 "Predict body tracker pose for this many ms after hold-lost phase");
  app.add_option("--runtime-body-tracker-max-prediction-velocity-mps", runtime_body_tracker_max_prediction_velocity_mps,
                 "Cap body tracker prediction velocity in metres per second; <=0 disables velocity clamp");
  app.add_option("--runtime-body-tracker-max-prediction-acceleration-mps2", runtime_body_tracker_max_prediction_acceleration_mps2,
                 "Cap body tracker prediction velocity change in metres per second squared; <=0 disables acceleration clamp");
  app.add_option("--runtime-body-tracker-prediction-damping", runtime_body_tracker_prediction_damping,
                 "Scale predicted body tracker velocity during lost-tracker prediction; 0 freezes, 1 uses full velocity");
  app.add_option("--runtime-body-tracker-prediction-publish-hz", runtime_body_tracker_prediction_publish_hz,
                 "Synthetic runtime body tracker publish rate while source stream is stale; <=0 publishes every poll");
  app.add_option("--runtime-body-tracker-predicted-status", runtime_body_tracker_predicted_status,
                 "Status to publish for predicted body trackers: tracking, stale, or lost");

  app.add_option("--hand-skeleton26-input", hand_skeleton26_input,
                 "Optional generic OpenXR/Ultraleap-style 26-joint hand skeleton input: none, shm, or tcp");
  app.add_option("--hand-skeleton26-registry", hand_skeleton26_registry,
                 "Tracking registry path used with --hand-skeleton26-input shm");
  app.add_option("--hand-skeleton26-stream", hand_skeleton26_stream,
                 "Stream id used with --hand-skeleton26-input shm");
  app.add_option("--hand-skeleton26-tcp-host", hand_skeleton26_tcp_host,
                 "TCP host used with --hand-skeleton26-input tcp");
  app.add_option("--hand-skeleton26-tcp-port", hand_skeleton26_tcp_port,
                 "TCP port used with --hand-skeleton26-input tcp");
  app.add_option("--hand-skeleton26-reattach-on-stale-ms", hand_skeleton26_reattach_on_stale_ms,
                 "hand_skeleton26 SHM reader reattach interval while stale/missing; <=0 disables");
  app.add_option("--hand-skeleton26-derive-gestures", hand_skeleton26_derive_gestures,
                 "Derive fallback pinch/grab from 26-joint geometry when source does not provide gestures");
  app.add_option("--runtime-derive-hand-gestures", runtime_derive_hand_gestures,
                 "Derive fallback pinch/grab from runtime hand joints when source does not provide gestures");
  app.add_option("--runtime-derive-hand-gestures-with-controller-input", runtime_derive_hand_gestures_with_controller_input,
                 "Allow runtime visual gesture derivation even on frames with fresh controller input; normally false so controller buttons stay authoritative");
  app.add_option("--runtime-derived-gestures-require-fresh-tracking", runtime_derived_gestures_require_fresh_tracking,
                 "If true, runtime-derived visual gestures are not recalculated from held/stale/lost hand poses");
  app.add_option("--runtime-derived-gesture-latch-ms", runtime_derived_gesture_latch_ms,
                 "How long to keep the last fresh derived visual pinch/grab in ms after hand tracking stops being fresh; 0 clears immediately");
  app.add_option("--runtime-ignore-backend-hand-gestures", runtime_ignore_backend_hand_gestures,
                 "Ignore pinch/grab/button gesture values received from hand_tracking input before applying controller override or runtime-derived gestures");
  CLI::Option* runtime_left_hand_gestures_enabled_option =
      app.add_option("--runtime-left-hand-gestures-enabled", runtime_left_hand_gestures_enabled,
                     "Enable visual hand-tracking gestures for the left hand. ControllerInputV2 override buttons are unaffected. Normally set in config controller_override.hand_gestures.left_enabled");
  CLI::Option* runtime_right_hand_gestures_enabled_option =
      app.add_option("--runtime-right-hand-gestures-enabled", runtime_right_hand_gestures_enabled,
                     "Enable visual hand-tracking gestures for the right hand. ControllerInputV2 override buttons are unaffected. Normally set in config controller_override.hand_gestures.right_enabled");
  app.add_option("--runtime-derive-extra-gesture-buttons", runtime_derive_extra_gesture_buttons,
                 "Derive extra visual hand gestures such as thumbs_up/index_point and map them to runtime button bits");
  app.add_option("--runtime-derive-extra-gesture-buttons-with-controller-input", runtime_derive_extra_gesture_buttons_with_controller_input,
                 "Allow extra visual gesture buttons even on frames with fresh controller input; normally false so physical buttons stay authoritative");

  app.add_flag("--runtime-hand-stability-gate", runtime_hand_stability_gate,
               "Enable runtime-side hand pose stability gate after hand input read and before transforms/gestures/controller override");
  app.add_option("--runtime-hand-gate-max-jump-m", runtime_hand_gate_max_jump_m,
                 "Runtime hand gate: max accepted controller-pose jump in meters before confirmation is required");
  app.add_option("--runtime-hand-gate-confirm-frames", runtime_hand_gate_confirm_frames,
                 "Runtime hand gate: consecutive stable frames required to accept a far reacquire candidate");
  app.add_option("--runtime-hand-gate-confirm-max-step-m", runtime_hand_gate_confirm_max_step_m,
                 "Runtime hand gate: max step in meters between pending reacquire confirmation frames");
  app.add_option("--runtime-hand-gate-hold-lost-ms", runtime_hand_gate_hold_lost_ms,
                 "Runtime hand gate: how long to hold the last good hand pose after tracking loss/rejected jump");
  app.add_option("--runtime-hand-gate-max-continuity-velocity-mps", runtime_hand_gate_max_continuity_velocity_mps,
                 "Runtime hand gate: reject continuous hand tracking updates faster than this velocity in meters/second; <=0 disables");
  app.add_option("--runtime-hand-gate-predict-lost-ms", runtime_hand_gate_predict_lost_ms,
                 "Runtime hand gate: lost-hand prediction window after hold_lost in milliseconds; 0 disables");
  app.add_option("--runtime-hand-gate-max-prediction-velocity-mps", runtime_hand_gate_max_prediction_velocity_mps,
                 "Runtime hand gate: clamp predicted lost-hand linear velocity in meters/second");
  app.add_option("--runtime-hand-gate-prediction-damping", runtime_hand_gate_prediction_damping,
                 "Runtime hand gate: prediction damping factor in 0..1 before applying lost-hand velocity");
  app.add_option("--runtime-hand-gate-reacquire-blend-ms", runtime_hand_gate_reacquire_blend_ms,
                 "Runtime hand gate: blend duration after confirmed far reacquire; 0 disables");
  app.add_option("--runtime-hand-gate-debug-csv", runtime_hand_gate_debug_csv,
                 "Runtime hand gate: optional CSV path for gate decisions");

  app.add_flag("--runtime-jitter-filter", runtime_jitter_filter_enabled,
               "Enable runtime-side position/orientation deadband jitter filter for HMD, hand trackers, and body trackers");
  app.add_option("--runtime-jitter-filter-hmd-cm", runtime_jitter_filter_hmd_cm,
                 "Runtime jitter filter HMD position deadband in centimeters; <=0 disables HMD position deadband");
  app.add_option("--runtime-jitter-filter-tracker-cm", runtime_jitter_filter_tracker_cm,
                 "Runtime jitter filter hand/body tracker position deadband in centimeters; <=0 disables tracker position deadband");
  app.add_option("--runtime-jitter-filter-hmd-deg", runtime_jitter_filter_hmd_deg,
                 "Runtime jitter filter HMD orientation deadband in degrees; <=0 disables HMD orientation deadband");
  app.add_option("--runtime-jitter-filter-tracker-deg", runtime_jitter_filter_tracker_deg,
                 "Runtime jitter filter hand/body tracker orientation deadband in degrees; <=0 disables tracker orientation deadband");

  app.add_option("--derived-thumbs-up-button", derived_thumbs_up_button,
                 "Runtime button mapped from visually-derived thumbs_up: none, a, b, menu, thumbstick, trigger, grip");
  app.add_option("--derived-index-point-button,--derived-victory-button", derived_index_point_button,
                 "Runtime button mapped from visually-derived one-finger index-point gesture: none, a, b, menu, thumbstick, trigger, grip. --derived-victory-button is a deprecated alias");
  app.add_option("--derived-thumbs-up-active-threshold", derived_thumbs_up_active_threshold,
                 "Activation threshold for visually-derived thumbs_up button [0..1]");
  app.add_option("--derived-index-point-active-threshold,--derived-victory-active-threshold", derived_index_point_active_threshold,
                 "Activation threshold for visually-derived one-finger index-point button [0..1]. --derived-victory-active-threshold is a deprecated alias");
  app.add_option("--derived-thumbs-up-deactive-threshold", derived_thumbs_up_deactive_threshold,
                 "Deactivation threshold for visually-derived thumbs_up button hysteresis [0..active]");
  app.add_option("--derived-index-point-deactive-threshold,--derived-victory-deactive-threshold", derived_index_point_deactive_threshold,
                 "Deactivation threshold for visually-derived one-finger index-point button hysteresis [0..active]. --derived-victory-deactive-threshold is a deprecated alias");
  app.add_option("--derived-extra-gesture-response-start", derived_extra_gesture_response_start,
                 "Raw extra gesture strength below this value is mapped to 0 before button activation [0..0.99]");
  app.add_option("--derived-extra-gesture-hold-ms", derived_extra_gesture_hold_ms,
                 "Minimum hold time in ms for derived extra gesture button clicks after activation; 0 disables hold");
  app.add_option("--derived-pinch-active-threshold", derived_pinch_active_threshold,
                 "Activation threshold for visually-derived pinch from hand joints; higher means less sensitive");
  app.add_option("--derived-grab-active-threshold", derived_grab_active_threshold,
                 "Activation threshold for visually-derived grab/fist from hand joints; higher means less sensitive");
  app.add_option("--derived-pinch-deactive-threshold", derived_pinch_deactive_threshold,
                 "Deactivation threshold for visually-derived pinch active/click hysteresis [0..active]");
  app.add_option("--derived-grab-deactive-threshold", derived_grab_deactive_threshold,
                 "Deactivation threshold for visually-derived grab active/click hysteresis [0..active]");
  app.add_option("--derived-pinch-response-start", derived_pinch_response_start,
                 "Raw visual pinch strength below this value is mapped to 0 before publishing [0..0.99]");
  app.add_option("--derived-grab-response-start", derived_grab_response_start,
                 "Raw visual grab strength below this value is mapped to 0 before publishing [0..0.99]");

  app.add_option("--controller-trigger-pinch-threshold", controller_trigger_pinch_threshold,
                 "Controller trigger threshold used to override hand pinch gesture");
  app.add_option("--controller-grip-grab-threshold", controller_grip_grab_threshold,
                 "Controller grip threshold used to override hand grab gesture");


  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  if (no_recenter_on_reset_counter) recenter_on_reset_counter = false;

  const uint32_t runtime_body_tracker_predicted_status_value =
      parse_body_tracker_predicted_status(runtime_body_tracker_predicted_status);

  const ControllerOverrideFileConfig controller_override_file_config =
      load_controller_override_file_config(tracking_transform_config_path);
  if (controller_override_file_config.present) {
    if (controller_override_file_config.has_mode && runtime_controller_mode_option->count() == 0) {
      runtime_controller_mode = controller_override_file_config.mode;
    }
    if (controller_override_file_config.has_lost_hand_pose_fallback &&
        runtime_controller_lost_hand_pose_fallback_option->count() == 0) {
      runtime_controller_lost_hand_pose_fallback = controller_override_file_config.lost_hand_pose_fallback;
    }
    if (controller_override_file_config.has_publish_runtime_controller_state_shm &&
        publish_runtime_controller_state_shm_option->count() == 0) {
      publish_runtime_controller_state_shm = controller_override_file_config.publish_runtime_controller_state_shm;
    }
    if (controller_override_file_config.has_runtime_controller_state_registry &&
        runtime_controller_state_registry_option->count() == 0) {
      runtime_controller_state_registry = controller_override_file_config.runtime_controller_state_registry;
    }
    if (controller_override_file_config.has_runtime_controller_state_stream &&
        runtime_controller_state_stream_option->count() == 0) {
      runtime_controller_state_stream = controller_override_file_config.runtime_controller_state_stream;
    }
    if (controller_override_file_config.has_runtime_controller_state_shm_name &&
        runtime_controller_state_shm_name_option->count() == 0) {
      runtime_controller_state_shm_name = controller_override_file_config.runtime_controller_state_shm_name;
    }
    if (controller_override_file_config.has_runtime_controller_state_slots &&
        runtime_controller_state_slots_option->count() == 0) {
      runtime_controller_state_slots = controller_override_file_config.runtime_controller_state_slots;
    }
    if (controller_override_file_config.has_left_hmd_relative_offset) {
      if (runtime_controller_left_offset_x_option->count() == 0) runtime_controller_left_offset_x = controller_override_file_config.left_hmd_relative_offset_m[0];
      if (runtime_controller_left_offset_y_option->count() == 0) runtime_controller_left_offset_y = controller_override_file_config.left_hmd_relative_offset_m[1];
      if (runtime_controller_left_offset_z_option->count() == 0) runtime_controller_left_offset_z = controller_override_file_config.left_hmd_relative_offset_m[2];
    }
    if (controller_override_file_config.has_right_hmd_relative_offset) {
      if (runtime_controller_right_offset_x_option->count() == 0) runtime_controller_right_offset_x = controller_override_file_config.right_hmd_relative_offset_m[0];
      if (runtime_controller_right_offset_y_option->count() == 0) runtime_controller_right_offset_y = controller_override_file_config.right_hmd_relative_offset_m[1];
      if (runtime_controller_right_offset_z_option->count() == 0) runtime_controller_right_offset_z = controller_override_file_config.right_hmd_relative_offset_m[2];
    }
    if (controller_override_file_config.has_left_static_orientation) {
      runtime_controller_left_static_orientation_xyzw = controller_override_file_config.left_static_orientation_xyzw;
    }
    if (controller_override_file_config.has_right_static_orientation) {
      runtime_controller_right_static_orientation_xyzw = controller_override_file_config.right_static_orientation_xyzw;
    }
    if (controller_override_file_config.has_left_hand_gestures_enabled &&
        runtime_left_hand_gestures_enabled_option->count() == 0) {
      runtime_left_hand_gestures_enabled = controller_override_file_config.left_hand_gestures_enabled;
    }
    if (controller_override_file_config.has_right_hand_gestures_enabled &&
        runtime_right_hand_gestures_enabled_option->count() == 0) {
      runtime_right_hand_gestures_enabled = controller_override_file_config.right_hand_gestures_enabled;
    }
  }

  const bool controller_buttons_runtime_only_mode =
      controller_input_mode == "controller_buttons_runtime_only";
  const std::string effective_controller_input_mode =
      controller_buttons_runtime_only_mode ? "hand_plus_controller" : controller_input_mode;

  controller_input_config.mode = effective_controller_input_mode;
  controller_input_config.transport = controller_input_transport;
  controller_input_config.left_controller_id = left_controller_id;
  controller_input_config.right_controller_id = right_controller_id;
  controller_input_config.max_age_ms = max_controller_age_ms;

  auto valid_controller_mode = [](const std::string& v) {
    return v == "hand_tracking_only" ||
           v == "hand_plus_controller" ||
           v == "controller_buttons_only" ||
           v == "controller_buttons_runtime_only";
  };
  auto valid_controller_source = [](const std::string& v) {
    return v == "none" || v == "shm" || v == "udp" || v == "tcp" || v == "named_pipe";
  };

  if (!valid_controller_mode(controller_input_mode)) {
    throw std::runtime_error("--controller-input-mode must be one of: hand_tracking_only, hand_plus_controller, controller_buttons_only, controller_buttons_runtime_only");
  }
  if (!valid_controller_source(controller_input_transport)) {
    throw std::runtime_error("--controller-input-transport must be one of: none, shm, udp, tcp, named_pipe");
  }
  if (max_controller_age_ms <= 0) {
    throw std::runtime_error("--max-controller-age-ms must be positive");
  }
  if (override_controller_gesture_block_latch_ms < 0.0) {
    throw std::runtime_error("--override-controller-gesture-block-latch-ms must be >= 0");
  }
  if (runtime_hand_gate_confirm_frames < 1) {
    throw std::runtime_error("--runtime-hand-gate-confirm-frames must be >= 1");
  }
  if (runtime_hand_gate_max_continuity_velocity_mps < 0.0) {
    throw std::runtime_error("--runtime-hand-gate-max-continuity-velocity-mps must be >= 0; use 0 to disable");
  }

  const TrackingStalePolicy hmd_stale_policy =
      parse_tracking_stale_policy(hmd_stale_policy_name, "--hmd-stale-policy");
  const TrackingStalePolicy hand_stale_policy =
      parse_tracking_stale_policy(hand_stale_policy_name, "--hand-stale-policy");

  xr_runtime::validate_controller_input_runtime_config(controller_input_config);
  const xr_runtime::RuntimeControllerMode runtime_controller_mode_value =
      xr_runtime::parse_runtime_controller_mode(runtime_controller_mode);
  const override_controller::LostHandPoseFallbackMode runtime_controller_lost_hand_pose_fallback_value =
      override_controller::parse_lost_hand_pose_fallback_mode(
          runtime_controller_lost_hand_pose_fallback,
          "--runtime-controller-lost-hand-pose-fallback");

  // In controller-buttons-only modes the hand pose is still used for controller
  // pose, but visual/backend hand gestures must not become controller inputs.
  // This covers both the legacy controller_input.mode value and the newer
  // runtime_controller_mode value.
  const bool runtime_controller_buttons_only_mode =
      controller_input_config.mode == "controller_buttons_only" ||
      runtime_controller_mode_value == xr_runtime::RuntimeControllerMode::HAND_TRACKING_CONTROLLER_BUTTONS_ONLY;
  const bool effective_runtime_left_hand_gestures_enabled =
      runtime_left_hand_gestures_enabled && !runtime_controller_buttons_only_mode;
  const bool effective_runtime_right_hand_gestures_enabled =
      runtime_right_hand_gestures_enabled && !runtime_controller_buttons_only_mode;

  const uint32_t derived_thumbs_up_button_mask =
      override_controller::parse_runtime_button_target(derived_thumbs_up_button, "--derived-thumbs-up-button");
  const uint32_t derived_index_point_button_mask =
      override_controller::parse_runtime_button_target(derived_index_point_button, "--derived-index-point-button");

  const TrackingTransformConfig tracking_transform_config =
      load_tracking_transform_config(tracking_transform_config_path);

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    if (mode != "event" && mode != "tick") {
      throw std::runtime_error("--mode must be event or tick");
    }
    if (input != "shm" && input != "udp") {
      throw std::runtime_error("--input must be shm or udp");
    }
    xr_runtime_adapter::platform::require_transport_available(input, "--input");
    if (tick_rate_hz <= 0.0) {
      throw std::runtime_error("--tick-rate must be > 0");
    }
    if (udp_pump_limit == 0) {
      throw std::runtime_error("--udp-pump-limit must be > 0");
    }
    if (runtime_pose_slots == 0) {
      throw std::runtime_error("--runtime-pose-slots must be > 0");
    }
    if (runtime_hand_slots == 0) {
      throw std::runtime_error("--runtime-hand-slots must be > 0");
    }
    if (runtime_controller_state_slots == 0) {
      throw std::runtime_error("--runtime-controller-state-slots must be > 0");
    }
    if (runtime_video_slots == 0) {
      throw std::runtime_error("--runtime-video-slots must be > 0");
    }
    if (video_input != "none" && video_input != "tcp" && video_input != "shm") {
      throw std::runtime_error("--video-input must be one of: none, tcp, shm");
    }
    xr_runtime_adapter::platform::require_transport_available(video_input, "--video-input");
    if (body_trackers_input != "none" && body_trackers_input != "shm" && body_trackers_input != "udp") {
      throw std::runtime_error("--body-trackers-input must be one of: none, shm, udp");
    }
    xr_runtime_adapter::platform::require_transport_available(body_trackers_input, "--body-trackers-input");
    if (spatial_proxy_mesh_input != "none" && spatial_proxy_mesh_input != "shm" && spatial_proxy_mesh_input != "udp") {
      throw std::runtime_error("--spatial-proxy-mesh-input must be one of: none, shm, udp");
    }
    xr_runtime_adapter::platform::require_transport_available(spatial_proxy_mesh_input, "--spatial-proxy-mesh-input");
    if (spatial_proxy_mesh_udp_bind_port <= 0 || spatial_proxy_mesh_udp_bind_port > 65535) {
      throw std::runtime_error("--spatial-proxy-mesh-udp-bind-port must be in 1..65535");
    }
    if (spatial_proxy_mesh_triangle_winding != "auto" &&
        spatial_proxy_mesh_triangle_winding != "keep" &&
        spatial_proxy_mesh_triangle_winding != "swap") {
      throw std::runtime_error("--spatial-proxy-mesh-triangle-winding must be one of: auto, keep, swap");
    }
    if (body_trackers_udp_bind_port <= 0 || body_trackers_udp_bind_port > 65535) {
      throw std::runtime_error("--body-trackers-udp-bind-port must be in 1..65535");
    }
    if (runtime_body_trackers_slots == 0) {
      throw std::runtime_error("--runtime-body-trackers-slots must be > 0");
    }
    if (hand_skeleton26_input != "none" && hand_skeleton26_input != "tcp" && hand_skeleton26_input != "shm") {
      throw std::runtime_error("--hand-skeleton26-input must be one of: none, tcp, shm");
    }
    xr_runtime_adapter::platform::require_transport_available(hand_skeleton26_input, "--hand-skeleton26-input");
    if (hand_skeleton26_tcp_port <= 0 || hand_skeleton26_tcp_port > 65535) {
      throw std::runtime_error("--hand-skeleton26-tcp-port must be in 1..65535");
    }
    auto validate_udp_port = [](int port, const char* name) {
      if (port <= 0 || port > 65535) {
        throw std::runtime_error(std::string(name) + " must be in 1..65535");
      }
    };
    validate_udp_port(runtime_pose_udp_port, "--runtime-pose-udp-port");
    validate_udp_port(runtime_hand_udp_port, "--runtime-hand-udp-port");
    validate_udp_port(runtime_controller_state_udp_port, "--runtime-controller-state-udp-port");
    xr_runtime_adapter::platform::require_shm_flag_available(publish_runtime_pose_shm, "--publish-runtime-pose-shm");
    xr_runtime_adapter::platform::require_shm_flag_available(publish_runtime_hand_shm, "--publish-runtime-hand-shm");
    xr_runtime_adapter::platform::require_shm_flag_available(publish_runtime_controller_state_shm,
                                                             "--publish-runtime-controller-state-shm");
    xr_runtime_adapter::platform::require_shm_flag_available(publish_runtime_video_shm, "--publish-runtime-video-shm");
    xr_runtime_adapter::platform::require_shm_flag_available(publish_runtime_spatial_proxy_mesh_shm,
                                                             "--publish-runtime-spatial-proxy-mesh-shm");
    xr_runtime_adapter::platform::require_shm_flag_available(publish_runtime_body_trackers_shm,
                                                             "--publish-runtime-body-trackers-shm");
    if (runtime_body_tracker_stability_gate && !publish_runtime_body_trackers_shm) {
      std::cout << "[xr_runtime_adapter] warning: runtime body tracker stability gate is enabled but "
                << "--publish-runtime-body-trackers-shm is disabled; gate has no runtime output to publish\n";
    }
    if (runtime_body_tracker_prediction_publish_hz < 0.0) {
      throw std::runtime_error("--runtime-body-tracker-prediction-publish-hz must be >= 0");
    }
    if (derived_pinch_active_threshold < 0.0f || derived_pinch_active_threshold > 1.0f ||
        derived_grab_active_threshold < 0.0f || derived_grab_active_threshold > 1.0f) {
      throw std::runtime_error("derived gesture thresholds must be in 0..1");
    }
    if (derived_pinch_deactive_threshold < 0.0f || derived_pinch_deactive_threshold > derived_pinch_active_threshold ||
        derived_grab_deactive_threshold < 0.0f || derived_grab_deactive_threshold > derived_grab_active_threshold) {
      throw std::runtime_error("derived gesture deactivation thresholds must be in 0..active_threshold");
    }
    if (derived_pinch_response_start < 0.0f || derived_pinch_response_start >= 1.0f ||
        derived_grab_response_start < 0.0f || derived_grab_response_start >= 1.0f) {
      throw std::runtime_error("derived gesture response-start thresholds must be in [0,1)");
    }
    if (derived_thumbs_up_active_threshold < 0.0f || derived_thumbs_up_active_threshold > 1.0f ||
        derived_index_point_active_threshold < 0.0f || derived_index_point_active_threshold > 1.0f) {
      throw std::runtime_error("derived extra gesture active thresholds must be in 0..1");
    }
    if (derived_thumbs_up_deactive_threshold < 0.0f || derived_thumbs_up_deactive_threshold > derived_thumbs_up_active_threshold ||
        derived_index_point_deactive_threshold < 0.0f || derived_index_point_deactive_threshold > derived_index_point_active_threshold) {
      throw std::runtime_error("derived extra gesture deactivation thresholds must be in 0..active_threshold");
    }
    if (derived_extra_gesture_response_start < 0.0f || derived_extra_gesture_response_start >= 1.0f) {
      throw std::runtime_error("derived extra gesture response-start threshold must be in [0,1)");
    }
    if (derived_extra_gesture_hold_ms < 0.0) {
      throw std::runtime_error("--derived-extra-gesture-hold-ms must be >= 0");
    }
    if (runtime_derived_gesture_latch_ms < 0.0) {
      throw std::runtime_error("--runtime-derived-gesture-latch-ms must be >= 0");
    }
    if (controller_trigger_pinch_threshold < 0.0f || controller_trigger_pinch_threshold > 1.0f ||
        controller_grip_grab_threshold < 0.0f || controller_grip_grab_threshold > 1.0f) {
      throw std::runtime_error("controller gesture thresholds must be in 0..1");
    }

    std::cout << "== xr_runtime_adapter scaffold ==\n";
    std::cout << "registry: " << registry_path << "\n";
    std::cout << "adapter: " << adapter_type << "\n";
    std::cout << "mode: " << mode << "\n";
    std::cout << "input: " << input << "\n";
    std::cout << "video_input: " << video_input << "\n";
    std::cout << "body_trackers_input: " << body_trackers_input << "\n";
    std::cout << "spatial_proxy_mesh_input: " << spatial_proxy_mesh_input << "\n";
    if (spatial_proxy_mesh_input == "shm") {
      std::cout << "spatial_proxy_mesh_registry: " << spatial_proxy_mesh_registry
                << " spatial_proxy_mesh_stream: " << spatial_proxy_mesh_stream << "\n";
    } else if (spatial_proxy_mesh_input == "udp") {
      std::cout << "spatial_proxy_mesh_udp_bind: " << spatial_proxy_mesh_udp_bind_host
                << ":" << spatial_proxy_mesh_udp_bind_port << "\n";
    }
    std::cout << "publish_runtime_spatial_proxy_mesh_shm: "
              << (publish_runtime_spatial_proxy_mesh_shm ? "true" : "false") << "\n";
    if (publish_runtime_spatial_proxy_mesh_shm) {
      std::cout << "runtime_spatial_proxy_mesh_registry: " << runtime_spatial_proxy_mesh_registry << "\n";
      std::cout << "runtime_spatial_proxy_mesh_stream: " << runtime_spatial_proxy_mesh_stream << "\n";
      std::cout << "runtime_spatial_proxy_mesh_shm_name: " << runtime_spatial_proxy_mesh_shm_name << "\n";
      std::cout << "runtime_spatial_proxy_mesh_slots: " << runtime_spatial_proxy_mesh_slots << "\n";
    }
    if (spatial_proxy_mesh_input != "none") {
      std::cout << "spatial_proxy_mesh_triangle_winding: " << spatial_proxy_mesh_triangle_winding
                << " rotate_deg=(" << spatial_proxy_mesh_rotate_deg_x << ","
                << spatial_proxy_mesh_rotate_deg_y << "," << spatial_proxy_mesh_rotate_deg_z << ")\n";
    }
    if (body_trackers_input == "udp") {
      std::cout << "body_trackers_udp_bind: " << body_trackers_udp_bind_host << ":" << body_trackers_udp_bind_port << "\n";
    } else if (body_trackers_input == "shm") {
      std::cout << "body_trackers_registry: " << body_trackers_registry
                << " body_trackers_stream: " << body_trackers_stream << "\n";
    }
    std::cout << "publish_runtime_body_trackers_shm: " << (publish_runtime_body_trackers_shm ? "true" : "false") << "\n";
    if (publish_runtime_body_trackers_shm) {
      std::cout << "runtime_body_trackers_registry: " << runtime_body_trackers_registry << "\n";
      std::cout << "runtime_body_trackers_stream: " << runtime_body_trackers_stream << "\n";
      std::cout << "runtime_body_trackers_shm_name: " << runtime_body_trackers_shm_name << "\n";
      std::cout << "runtime_body_trackers_slots: " << runtime_body_trackers_slots << "\n";
      std::cout << "runtime_body_tracker_stability_gate: " << (runtime_body_tracker_stability_gate ? "true" : "false") << "\n";
      if (runtime_body_tracker_stability_gate) {
        std::cout << "runtime_body_tracker_hold_lost_ms: " << runtime_body_tracker_hold_lost_ms << "\n";
        std::cout << "runtime_body_tracker_predict_lost_ms: " << runtime_body_tracker_predict_lost_ms << "\n";
        std::cout << "runtime_body_tracker_max_prediction_velocity_mps: " << runtime_body_tracker_max_prediction_velocity_mps << "\n";
        std::cout << "runtime_body_tracker_max_prediction_acceleration_mps2: " << runtime_body_tracker_max_prediction_acceleration_mps2 << "\n";
        std::cout << "runtime_body_tracker_prediction_damping: " << runtime_body_tracker_prediction_damping << "\n";
        std::cout << "runtime_body_tracker_prediction_publish_hz: " << runtime_body_tracker_prediction_publish_hz << "\n";
        std::cout << "runtime_body_tracker_predicted_status: " << runtime_body_tracker_predicted_status << "\n";
      }
    }
    std::cout << "hand_skeleton26_input: " << hand_skeleton26_input << "\n";
    if (hand_skeleton26_input == "tcp") {
      std::cout << "hand_skeleton26_tcp: " << hand_skeleton26_tcp_host << ":" << hand_skeleton26_tcp_port << "\n";
    } else if (hand_skeleton26_input == "shm") {
      std::cout << "hand_skeleton26_registry: " << hand_skeleton26_registry
                << " hand_skeleton26_stream: " << hand_skeleton26_stream << "\n";
    }
    std::cout << "hand_skeleton26_derive_gestures: " << (hand_skeleton26_derive_gestures ? "true" : "false") << "\n";
    std::cout << "runtime_derive_hand_gestures: " << (runtime_derive_hand_gestures ? "true" : "false") << "\n";
    std::cout << "runtime_derive_hand_gestures_with_controller_input: "
              << (runtime_derive_hand_gestures_with_controller_input ? "true" : "false") << "\n";
    std::cout << "runtime_derived_gestures_require_fresh_tracking: "
              << (runtime_derived_gestures_require_fresh_tracking ? "true" : "false") << "\n";
    std::cout << "runtime_derived_gesture_latch_ms: " << runtime_derived_gesture_latch_ms << "\n";
    std::cout << "runtime_ignore_backend_hand_gestures: "
              << (runtime_ignore_backend_hand_gestures ? "true" : "false") << "\n";
    std::cout << "runtime_left_hand_gestures_enabled: "
              << (runtime_left_hand_gestures_enabled ? "true" : "false") << "\n";
    std::cout << "runtime_right_hand_gestures_enabled: "
              << (runtime_right_hand_gestures_enabled ? "true" : "false") << "\n";
    std::cout << "runtime_controller_buttons_only_mode: "
              << (runtime_controller_buttons_only_mode ? "true" : "false") << "\n";
    std::cout << "controller_buttons_runtime_only_mode: "
              << (controller_buttons_runtime_only_mode ? "true" : "false") << "\n";
    std::cout << "runtime_controller_lost_hand_pose_fallback: "
              << override_controller::lost_hand_pose_fallback_mode_name(
                     runtime_controller_lost_hand_pose_fallback_value)
              << "\n";
    std::cout << "effective_runtime_left_hand_gestures_enabled: "
              << (effective_runtime_left_hand_gestures_enabled ? "true" : "false") << "\n";
    std::cout << "effective_runtime_right_hand_gestures_enabled: "
              << (effective_runtime_right_hand_gestures_enabled ? "true" : "false") << "\n";
    std::cout << "override_controller_block_gestures_while_stream_present: "
              << (override_controller_block_gestures_while_stream_present ? "true" : "false") << "\n";
    std::cout << "override_controller_gesture_block_latch_ms: "
              << override_controller_gesture_block_latch_ms << "\n";
    std::cout << "runtime_derive_extra_gesture_buttons: "
              << (runtime_derive_extra_gesture_buttons ? "true" : "false") << "\n";
    std::cout << "runtime_derive_extra_gesture_buttons_with_controller_input: "
              << (runtime_derive_extra_gesture_buttons_with_controller_input ? "true" : "false") << "\n";
    std::cout << "runtime_hand_stability_gate: " << (runtime_hand_stability_gate ? "true" : "false") << "\n";
    if (runtime_hand_stability_gate) {
      std::cout << "runtime_hand_gate_max_jump_m: " << runtime_hand_gate_max_jump_m << "\n";
      std::cout << "runtime_hand_gate_confirm_frames: " << runtime_hand_gate_confirm_frames << "\n";
      std::cout << "runtime_hand_gate_confirm_max_step_m: " << runtime_hand_gate_confirm_max_step_m << "\n";
      std::cout << "runtime_hand_gate_hold_lost_ms: " << runtime_hand_gate_hold_lost_ms << "\n";
      std::cout << "runtime_hand_gate_max_continuity_velocity_mps: " << runtime_hand_gate_max_continuity_velocity_mps << "\n";
      std::cout << "runtime_hand_gate_predict_lost_ms: " << runtime_hand_gate_predict_lost_ms << "\n";
      std::cout << "runtime_hand_gate_max_prediction_velocity_mps: " << runtime_hand_gate_max_prediction_velocity_mps << "\n";
      std::cout << "runtime_hand_gate_prediction_damping: " << runtime_hand_gate_prediction_damping << "\n";
      std::cout << "runtime_hand_gate_reacquire_blend_ms: " << runtime_hand_gate_reacquire_blend_ms << "\n";
      if (!runtime_hand_gate_debug_csv.empty()) {
        std::cout << "runtime_hand_gate_debug_csv: " << runtime_hand_gate_debug_csv << "\n";
      }
    }
    std::cout << "runtime_jitter_filter: " << (runtime_jitter_filter_enabled ? "true" : "false") << "\n";
    if (runtime_jitter_filter_enabled) {
      std::cout << "runtime_jitter_filter_hmd_cm: " << runtime_jitter_filter_hmd_cm << "\n";
      std::cout << "runtime_jitter_filter_tracker_cm: " << runtime_jitter_filter_tracker_cm << "\n";
      std::cout << "runtime_jitter_filter_hmd_deg: " << runtime_jitter_filter_hmd_deg << "\n";
      std::cout << "runtime_jitter_filter_tracker_deg: " << runtime_jitter_filter_tracker_deg << "\n";
    }
    std::cout << "derived_thumbs_up_button: " << derived_thumbs_up_button << "\n";
    std::cout << "derived_index_point_button: " << derived_index_point_button << "\n";
    std::cout << "derived_thumbs_up_active_threshold: " << derived_thumbs_up_active_threshold << "\n";
    std::cout << "derived_index_point_active_threshold: " << derived_index_point_active_threshold << "\n";
    std::cout << "derived_thumbs_up_deactive_threshold: " << derived_thumbs_up_deactive_threshold << "\n";
    std::cout << "derived_index_point_deactive_threshold: " << derived_index_point_deactive_threshold << "\n";
    std::cout << "derived_extra_gesture_response_start: " << derived_extra_gesture_response_start << "\n";
    std::cout << "derived_extra_gesture_hold_ms: " << derived_extra_gesture_hold_ms << "\n";
    std::cout << "derived_pinch_active_threshold: " << derived_pinch_active_threshold << "\n";
    std::cout << "derived_grab_active_threshold: " << derived_grab_active_threshold << "\n";
    std::cout << "derived_pinch_deactive_threshold: " << derived_pinch_deactive_threshold << "\n";
    std::cout << "derived_grab_deactive_threshold: " << derived_grab_deactive_threshold << "\n";
    std::cout << "derived_pinch_response_start: " << derived_pinch_response_start << "\n";
    std::cout << "derived_grab_response_start: " << derived_grab_response_start << "\n";
    if (video_input == "tcp") {
      std::cout << "video_tcp: " << video_tcp_host << ":" << video_tcp_port << "\n";
    } else if (video_input == "shm") {
      std::cout << "video_registry: " << video_registry << " video_stream: " << video_stream << "\n";
    }
    std::cout << "hmd_stream: " << hmd_stream << "\n";
    if (hmd_3dof_priority) {
      std::cout << "hmd_3dof_priority: true registry=" << hmd_3dof_registry
                << " stream=" << hmd_3dof_stream
                << " reattach_on_stale_ms=" << hmd_3dof_reattach_on_stale_ms << "\n";
    } else {
      std::cout << "hmd_3dof_priority: false\n";
    }
    std::cout << "hand_stream: " << hand_stream << "\n";
    std::cout << "max_hmd_age_ms: " << max_hmd_age_ms << "\n";
    std::cout << "max_hand_age_ms: " << max_hand_age_ms << "\n";
    std::cout << "consume_stale_as_lost: " << (consume_stale_as_lost ? "true" : "false") << "\n";
    std::cout << "hmd_stale_policy: " << tracking_stale_policy_name(hmd_stale_policy) << "\n";
    std::cout << "hand_stale_policy: " << tracking_stale_policy_name(hand_stale_policy) << "\n";
    std::cout << "hmd_reattach_on_stale_ms: " << hmd_reattach_on_stale_ms << "\n";
    std::cout << "hand_reattach_on_stale_ms: " << hand_reattach_on_stale_ms << "\n";
    std::cout << "runtime_hmd_pose_stability_filter: " << (runtime_hmd_pose_stability_filter ? "true" : "false") << "\n";
    if (runtime_hmd_pose_stability_filter) {
      std::cout << "runtime_hmd_pose_stability_window_ms: " << runtime_hmd_pose_stability_window_ms << "\n";
      std::cout << "runtime_hmd_pose_stability_max_distance_cm: " << runtime_hmd_pose_stability_max_distance_cm << "\n";
    }
    std::cout << "hmd_hold_last_max_ms: " << hmd_hold_last_max_ms << "\n";
    std::cout << "hand_hold_last_max_ms: " << hand_hold_last_max_ms << "\n";
    std::cout << "publish_runtime_pose_shm: " << (publish_runtime_pose_shm ? "true" : "false") << "\n";
    if (publish_runtime_pose_shm) {
      std::cout << "runtime_pose_registry: " << runtime_pose_registry << "\n";
      std::cout << "runtime_pose_stream: " << runtime_pose_stream << "\n";
      std::cout << "runtime_pose_shm_name: " << runtime_pose_shm_name << "\n";
      std::cout << "runtime_pose_slots: " << runtime_pose_slots << "\n";
    }
    std::cout << "publish_runtime_hand_shm: " << (publish_runtime_hand_shm ? "true" : "false") << "\n";
    if (publish_runtime_hand_shm) {
      std::cout << "runtime_hand_registry: " << runtime_hand_registry << "\n";
      std::cout << "runtime_hand_stream: " << runtime_hand_stream << "\n";
      std::cout << "runtime_hand_shm_name: " << runtime_hand_shm_name << "\n";
      std::cout << "runtime_hand_slots: " << runtime_hand_slots << "\n";
    }
    if (input == "udp") {
      std::cout << "udp_bind: " << udp_bind_host << ":" << udp_bind_port << "\n";
      std::cout << "udp_frame_id: " << udp_frame_id << "\n";
    }
    if (mode == "tick") {
      std::cout << "tick_rate_hz: " << tick_rate_hz << "\n";
      std::cout << "prediction_ms: " << prediction_ms << "\n";
      std::cout << "max_prediction_ms: " << max_prediction_ms << "\n";
      std::cout << "prediction_enabled: " << (no_prediction ? "false" : "true") << "\n";
      std::cout << "origin_mode: " << origin_mode << "\n";
      std::cout << "runtime_frame: " << runtime_frame << "\n";
      std::cout << "tracking_transform_config: "
                << (tracking_transform_config.path.empty() ? std::string("<none>") : tracking_transform_config.path)
                << " enabled=" << (tracking_transform_config.enabled ? "true" : "false") << "\n";
      if (tracking_transform_config.enabled) {
        log_stream_transform("hmd", tracking_transform_config.hmd);
        log_stream_transform("hmd_3dof", tracking_transform_config.hmd_3dof);
        log_stream_transform("hand_tracking_21_joint", tracking_transform_config.hand_tracking_21_joint);
        log_stream_transform("hand_skeleton26", tracking_transform_config.hand_skeleton26);
        log_stream_transform("body_trackers", tracking_transform_config.body_trackers);
        log_stream_transform("spatial_proxy_mesh", tracking_transform_config.spatial_proxy_mesh, true);
      }
      std::cout << "recenter_on_reset_counter: "
                << (recenter_on_reset_counter ? "true" : "false") << "\n";
  std::cout << "controller_input_mode: " << controller_input_mode << "\n";
  if (effective_controller_input_mode != controller_input_mode) {
    std::cout << "effective_controller_input_mode: " << effective_controller_input_mode << "\n";
  }
  std::cout << "controller_buttons_runtime_only_mode: "
            << (controller_buttons_runtime_only_mode ? "true" : "false") << "\n";
  std::cout << "controller_input_transport: " << controller_input_transport << "\n";
  std::cout << "left_controller_id: " << left_controller_id << "\n";
  std::cout << "right_controller_id: " << right_controller_id << "\n";
  std::cout << "max_controller_age_ms: " << max_controller_age_ms << "\n";
  std::cout << "controller_override_config: "
            << (controller_override_file_config.present ? "present" : "absent") << "\n";
  std::cout << "runtime_controller_mode: " << runtime_controller_mode << "\n";
  std::cout << "runtime_controller_lost_hand_pose_fallback: "
            << override_controller::lost_hand_pose_fallback_mode_name(
                   runtime_controller_lost_hand_pose_fallback_value)
            << "\n";
  std::cout << "publish_runtime_controller_state_shm: "
            << (publish_runtime_controller_state_shm ? "true" : "false") << "\n";
  std::cout << "runtime_controller_left_offset_m: ["
            << runtime_controller_left_offset_x << ", "
            << runtime_controller_left_offset_y << ", "
            << runtime_controller_left_offset_z << "]\n";
  std::cout << "runtime_controller_right_offset_m: ["
            << runtime_controller_right_offset_x << ", "
            << runtime_controller_right_offset_y << ", "
            << runtime_controller_right_offset_z << "]\n";
  std::cout << "runtime_controller_left_static_orientation_xyzw: ["
            << runtime_controller_left_static_orientation_xyzw[0] << ", "
            << runtime_controller_left_static_orientation_xyzw[1] << ", "
            << runtime_controller_left_static_orientation_xyzw[2] << ", "
            << runtime_controller_left_static_orientation_xyzw[3] << "]\n";
  std::cout << "runtime_controller_right_static_orientation_xyzw: ["
            << runtime_controller_right_static_orientation_xyzw[0] << ", "
            << runtime_controller_right_static_orientation_xyzw[1] << ", "
            << runtime_controller_right_static_orientation_xyzw[2] << ", "
            << runtime_controller_right_static_orientation_xyzw[3] << "]\n";
  if (controller_input_transport != "none") {
    std::cout << "[xr_runtime_adapter] controller input reader requested: "
              << controller_input_transport << " stream=" << controller_input_config.stream
              << " conflict_policy=" << controller_input_config.conflict_policy << "\n";
  }
    }

    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>> hmd_reader;
    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>> hmd_3dof_reader;
    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF64V1>> hand_reader;
    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF32V2>> hand_reader_v2;
    std::unique_ptr<UdpTrackingInput> udp_input;

    ReattachState hmd_reattach_state;
    ReattachState hmd_3dof_reattach_state;
    ReattachState hand_reattach_state;

    auto attach_hmd_stream = [&](const std::string& reason, bool required) -> bool {
      hmd_reattach_state.last_attempt_ns = xr_runtime::now_ns();
      ++hmd_reattach_state.attempts;
      try {
        auto hmd_info = xr_runtime::stream_info_from_registry(registry_path, hmd_stream);
        auto next_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>>(
            std::move(hmd_info), "HMD_POSE_F64_LE");
        std::cout << "[xr_runtime_adapter] hmd stream attached"
                  << " reason=" << reason
                  << " stream=" << hmd_stream
                  << " frame=" << next_reader->info().frame_id
                  << " shm=" << next_reader->info().shm_name << "\n";
        hmd_reader = std::move(next_reader);
        ++hmd_reattach_state.successes;
        hmd_reattach_state.last_error.clear();
        return true;
      } catch (const std::exception& e) {
        ++hmd_reattach_state.failures;
        hmd_reattach_state.last_error = e.what();
        if (required) throw;
        std::cout << "[xr_runtime_adapter] hmd stream unavailable"
                  << " reason=" << reason
                  << " error=" << e.what() << "\n";
        hmd_reader.reset();
        return false;
      }
    };

    auto attach_hmd_3dof_stream = [&](const std::string& reason, bool required) -> bool {
      hmd_3dof_reattach_state.last_attempt_ns = xr_runtime::now_ns();
      ++hmd_3dof_reattach_state.attempts;
      try {
        auto hmd_info = xr_runtime::stream_info_from_registry(hmd_3dof_registry, hmd_3dof_stream);
        auto next_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>>(
            std::move(hmd_info), "HMD_POSE_F64_LE");
        std::cout << "[xr_runtime_adapter] hmd_3dof priority stream attached"
                  << " reason=" << reason
                  << " registry=" << hmd_3dof_registry
                  << " stream=" << hmd_3dof_stream
                  << " frame=" << next_reader->info().frame_id
                  << " shm=" << next_reader->info().shm_name << "\n";
        hmd_3dof_reader = std::move(next_reader);
        ++hmd_3dof_reattach_state.successes;
        hmd_3dof_reattach_state.last_error.clear();
        return true;
      } catch (const std::exception& e) {
        ++hmd_3dof_reattach_state.failures;
        hmd_3dof_reattach_state.last_error = e.what();
        if (required) throw;
        if (reason == "startup") {
          std::cout << "[xr_runtime_adapter] hmd_3dof priority stream unavailable"
                    << " reason=" << reason
                    << " error=" << e.what() << "\n";
        }
        hmd_3dof_reader.reset();
        return false;
      }
    };

    auto attach_hand_stream = [&](const std::string& reason, bool required) -> bool {
      hand_reattach_state.last_attempt_ns = xr_runtime::now_ns();
      ++hand_reattach_state.attempts;
      try {
        auto hand_info = xr_runtime::stream_info_from_registry(registry_path, hand_stream);
        if (hand_info.format_name == "HAND_TRACKING_21_JOINT_F32_V2" ||
            hand_info.format_name == "HAND_TRACKING_F32_V2" ||
            hand_info.format_name == "HAND_TRACKING_V2") {
          const std::string hand_format_name = hand_info.format_name;
          auto next_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF32V2>>(
              std::move(hand_info), hand_format_name);
          std::cout << "[xr_runtime_adapter] 21-joint hand stream attached"
                    << " reason=" << reason
                    << " stream=" << hand_stream
                    << " format=" << hand_format_name
                    << " frame=" << next_reader->info().frame_id
                    << " shm=" << next_reader->info().shm_name
                    << " payload_size=" << next_reader->info().payload_size << "\n";
          hand_reader.reset();
          hand_reader_v2 = std::move(next_reader);
        } else {
          auto next_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF64V1>>(
              std::move(hand_info), "HAND_TRACKING_V1");
          std::cout << "[xr_runtime_adapter] hand stream attached"
                    << " reason=" << reason
                    << " stream=" << hand_stream
                    << " format=HAND_TRACKING_V1"
                    << " frame=" << next_reader->info().frame_id
                    << " shm=" << next_reader->info().shm_name << "\n";
          hand_reader = std::move(next_reader);
          hand_reader_v2.reset();
        }
        ++hand_reattach_state.successes;
        hand_reattach_state.last_error.clear();
        return true;
      } catch (const std::exception& e) {
        ++hand_reattach_state.failures;
        hand_reattach_state.last_error = e.what();
        if (required) throw;
        std::cout << "[xr_runtime_adapter] hand stream unavailable"
                  << " reason=" << reason
                  << " error=" << e.what() << "\n";
        hand_reader.reset();
        hand_reader_v2.reset();
        return false;
      }
    };

    if (input == "shm") {
      attach_hmd_stream("startup", require_hmd);
      if (hmd_3dof_priority) {
        attach_hmd_3dof_stream("startup", false);
      }
      attach_hand_stream("startup", require_hands);

      if (!hmd_reader && !hmd_3dof_reader && !hand_reader && !hand_reader_v2) {
        std::cout << "[xr_runtime_adapter] no tracking streams attached at startup; "
                  << "will keep running and reattach while streams are unavailable\n";
      }
    } else {
      udp_input = std::make_unique<UdpTrackingInput>(udp_bind_host, udp_bind_port);
      std::cout << "[xr_runtime_adapter] UDP tracking input attached: "
                << udp_bind_host << ":" << udp_bind_port << "\n";
      std::cout << "[xr_runtime_adapter] expected tracking_net_v1 packets: "
                << "HMD_POSE_F64 + HAND_SUMMARY_F64/HAND_SUMMARY_F32_V2/HAND_JOINTS_F32_V2\n";
    }

    auto adapter = xr_runtime::make_adapter(adapter_type);
    adapter->start();

    std::unique_ptr<VideoInputHealthThread> video_health_thread;
    if (video_input != "none") {
      VideoInputHealthConfig cfg;
      cfg.input = video_input;
      cfg.tcp_host = video_tcp_host;
      cfg.tcp_port = video_tcp_port;
      cfg.registry = video_registry;
      cfg.stream = video_stream;
      cfg.reattach_on_stale_ms = video_reattach_on_stale_ms;
      cfg.publish_runtime_video_shm = publish_runtime_video_shm;
      cfg.runtime_registry = runtime_video_registry;
      cfg.runtime_stream = runtime_video_stream;
      cfg.runtime_shm_name = runtime_video_shm_name;
      cfg.runtime_frame = runtime_video_frame;
      cfg.runtime_slots = runtime_video_slots;
      cfg.runtime_unlink_existing = !no_runtime_video_unlink;
      video_health_thread = std::make_unique<VideoInputHealthThread>(std::move(cfg));
      video_health_thread->start();
      std::cout << "[xr_runtime_adapter] video input health thread started: " << video_input << "\n";
    }

    std::unique_ptr<BodyTrackerInputThread> body_tracker_thread;
    std::unique_ptr<SpatialProxyMeshInputThread> spatial_proxy_mesh_thread;
    if (spatial_proxy_mesh_input != "none") {
      SpatialProxyMeshInputConfig cfg;
      cfg.input = spatial_proxy_mesh_input;
      cfg.registry = spatial_proxy_mesh_registry;
      cfg.stream = spatial_proxy_mesh_stream;
      cfg.udp_bind_host = spatial_proxy_mesh_udp_bind_host;
      cfg.udp_bind_port = spatial_proxy_mesh_udp_bind_port;
      cfg.reattach_on_stale_ms = spatial_proxy_mesh_reattach_on_stale_ms;
      cfg.max_source_age_ms = spatial_proxy_mesh_max_source_age_ms;
      cfg.publish_runtime_shm = publish_runtime_spatial_proxy_mesh_shm;
      cfg.runtime_registry = runtime_spatial_proxy_mesh_registry;
      cfg.runtime_stream = runtime_spatial_proxy_mesh_stream;
      cfg.runtime_shm_name = runtime_spatial_proxy_mesh_shm_name;
      cfg.runtime_frame = runtime_frame;
      cfg.runtime_slots = runtime_spatial_proxy_mesh_slots;
      cfg.runtime_unlink_existing = !no_runtime_spatial_proxy_mesh_unlink;
      cfg.transform = tracking_transform_config.spatial_proxy_mesh;
      // Source of truth: tracking transform JSON, streams.spatial_proxy_mesh.mesh_runtime.
      // The CLI/env values remain only as legacy fallback if the JSON block is absent.
      cfg.triangle_winding = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.triangle_winding.empty()
          ? spatial_proxy_mesh_triangle_winding
          : tracking_transform_config.spatial_proxy_mesh.spatial_mesh.triangle_winding;
      cfg.rotate_deg_x = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.extra_rotation_deg.x;
      cfg.rotate_deg_y = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.extra_rotation_deg.y;
      cfg.rotate_deg_z = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.extra_rotation_deg.z;
      cfg.offset_m_x = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.extra_offset_m.x;
      cfg.offset_m_y = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.extra_offset_m.y;
      cfg.offset_m_z = tracking_transform_config.spatial_proxy_mesh.spatial_mesh.extra_offset_m.z;
      spatial_proxy_mesh_thread = std::make_unique<SpatialProxyMeshInputThread>(std::move(cfg));
      spatial_proxy_mesh_thread->start();
      std::cout << "[xr_runtime_adapter] spatial proxy mesh input thread started: "
                << spatial_proxy_mesh_input << "\n";
    }

    if (body_trackers_input != "none") {
      BodyTrackerInputConfig cfg;
      cfg.input = body_trackers_input;
      cfg.registry = body_trackers_registry;
      cfg.stream = body_trackers_stream;
      cfg.udp_bind_host = body_trackers_udp_bind_host;
      cfg.udp_bind_port = body_trackers_udp_bind_port;
      cfg.reattach_on_stale_ms = body_trackers_reattach_on_stale_ms;
      cfg.publish_runtime_shm = publish_runtime_body_trackers_shm;
      cfg.runtime_registry = runtime_body_trackers_registry;
      cfg.runtime_stream = runtime_body_trackers_stream;
      cfg.runtime_shm_name = runtime_body_trackers_shm_name;
      cfg.runtime_frame = runtime_frame;
      cfg.runtime_slots = runtime_body_trackers_slots;
      cfg.runtime_unlink_existing = !no_runtime_body_trackers_unlink;
      cfg.transform = tracking_transform_config.body_trackers;
      cfg.jitter_filter.enabled = runtime_jitter_filter_enabled;
      cfg.jitter_filter.hmd_threshold_m = runtime_jitter_filter_hmd_cm / 100.0;
      cfg.jitter_filter.tracker_threshold_m = runtime_jitter_filter_tracker_cm / 100.0;
      cfg.jitter_filter.hmd_angle_threshold_rad = runtime_jitter_filter_hmd_deg * 3.14159265358979323846 / 180.0;
      cfg.jitter_filter.tracker_angle_threshold_rad = runtime_jitter_filter_tracker_deg * 3.14159265358979323846 / 180.0;
      cfg.stability_gate.enabled = runtime_body_tracker_stability_gate;
      cfg.stability_gate.hold_lost_ms = runtime_body_tracker_hold_lost_ms;
      cfg.stability_gate.predict_lost_ms = runtime_body_tracker_predict_lost_ms;
      cfg.stability_gate.max_prediction_velocity_mps = runtime_body_tracker_max_prediction_velocity_mps;
      cfg.stability_gate.max_prediction_acceleration_mps2 = runtime_body_tracker_max_prediction_acceleration_mps2;
      cfg.stability_gate.prediction_damping = runtime_body_tracker_prediction_damping;
      cfg.stability_gate.synthetic_publish_hz = runtime_body_tracker_prediction_publish_hz;
      cfg.stability_gate.predicted_status = runtime_body_tracker_predicted_status_value;
      body_tracker_thread = std::make_unique<BodyTrackerInputThread>(std::move(cfg));
      body_tracker_thread->start();
      std::cout << "[xr_runtime_adapter] body_trackers input thread started: "
                << body_trackers_input << "\n";
    }

    std::unique_ptr<HandSkeleton26InputThread> hand_skeleton26_thread;
    if (hand_skeleton26_input != "none") {
      HandSkeleton26InputConfig cfg;
      cfg.input = hand_skeleton26_input;
      cfg.tcp_host = hand_skeleton26_tcp_host;
      cfg.tcp_port = hand_skeleton26_tcp_port;
      cfg.registry = hand_skeleton26_registry;
      cfg.stream = hand_skeleton26_stream;
      cfg.reattach_on_stale_ms = hand_skeleton26_reattach_on_stale_ms;
      hand_skeleton26_thread = std::make_unique<HandSkeleton26InputThread>(std::move(cfg));
      hand_skeleton26_thread->start();
      std::cout << "[xr_runtime_adapter] hand_skeleton26 input thread started: "
                << hand_skeleton26_input << "\n";
    }

    std::unique_ptr<ControllerInputThread> controller_input_thread;
    if (controller_input_config.transport != "none") {
      ControllerInputThreadConfig cfg;
      cfg.input = controller_input_config;
      controller_input_thread = std::make_unique<ControllerInputThread>(std::move(cfg));
      controller_input_thread->start();
      std::cout << "[xr_runtime_adapter] controller input thread started: "
                << controller_input_config.transport << "\n";
    }

    std::unique_ptr<xr_runtime::RuntimePoseShmPublisher> runtime_pose_publisher;
    std::unique_ptr<xr_runtime::net::runtime_output_udp::RuntimeOutputUdpSender> runtime_pose_udp_publisher;
    if (publish_runtime_pose_udp) {
      runtime_pose_udp_publisher = std::make_unique<xr_runtime::net::runtime_output_udp::RuntimeOutputUdpSender>(
          runtime_pose_udp_host, static_cast<uint16_t>(runtime_pose_udp_port));
      std::cout << "[xr_runtime_adapter] publishing runtime pose UDP -> "
                << runtime_pose_udp_host << ":" << runtime_pose_udp_port << "\n";
    }
    if (publish_runtime_pose_shm) {
      xr_runtime::RuntimePoseShmPublisherConfig cfg;
      cfg.registry_path = runtime_pose_registry;
      cfg.stream_id = runtime_pose_stream;
      cfg.shm_name = runtime_pose_shm_name;
      cfg.frame_id = runtime_frame;
      cfg.slot_count = runtime_pose_slots;
      cfg.unlink_existing = !no_runtime_pose_unlink;
      cfg.created_by = "xr_runtime_adapter";
      runtime_pose_publisher = std::make_unique<xr_runtime::RuntimePoseShmPublisher>(std::move(cfg));
      std::cout << "[xr_runtime_adapter] publishing runtime pose stream "
                << runtime_pose_stream << " -> " << runtime_pose_shm_name
                << " frame=" << runtime_frame << "\n";
    }

    std::unique_ptr<xr_tracking::HandTrackingShmPublisherV2> runtime_hand_publisher;
    std::unique_ptr<xr_runtime::net::runtime_output_udp::RuntimeOutputUdpSender> runtime_hand_udp_publisher;
    if (publish_runtime_hand_udp) {
      runtime_hand_udp_publisher = std::make_unique<xr_runtime::net::runtime_output_udp::RuntimeOutputUdpSender>(
          runtime_hand_udp_host, static_cast<uint16_t>(runtime_hand_udp_port));
      std::cout << "[xr_runtime_adapter] publishing runtime hand UDP -> "
                << runtime_hand_udp_host << ":" << runtime_hand_udp_port << "\n";
    }
    if (publish_runtime_hand_shm) {
      xr_tracking::HandTrackingShmPublisherConfig cfg;
      cfg.registry_path = runtime_hand_registry;
      cfg.stream_id = runtime_hand_stream;
      cfg.shm_name = runtime_hand_shm_name;
      cfg.frame_id = runtime_frame;
      cfg.slot_count = runtime_hand_slots;
      cfg.unlink_existing = !no_runtime_hand_unlink;
      cfg.created_by = "xr_runtime_adapter";
      runtime_hand_publisher = std::make_unique<xr_tracking::HandTrackingShmPublisherV2>(std::move(cfg));
      std::cout << "[xr_runtime_adapter] publishing runtime hand stream "
                << runtime_hand_stream << " -> " << runtime_hand_shm_name
                << " frame=" << runtime_frame << "\n";
    }

    std::unique_ptr<xr_runtime::RuntimeControllerStateShmPublisher> runtime_controller_state_publisher;
    std::unique_ptr<xr_runtime::net::runtime_output_udp::RuntimeOutputUdpSender> runtime_controller_state_udp_publisher;
    if (publish_runtime_controller_state_udp) {
      runtime_controller_state_udp_publisher = std::make_unique<xr_runtime::net::runtime_output_udp::RuntimeOutputUdpSender>(
          runtime_controller_state_udp_host, static_cast<uint16_t>(runtime_controller_state_udp_port));
      std::cout << "[xr_runtime_adapter] publishing runtime controller state UDP -> "
                << runtime_controller_state_udp_host << ":" << runtime_controller_state_udp_port << "\n";
    }
    if (publish_runtime_controller_state_shm) {
      xr_runtime::RuntimeControllerStateShmPublisherConfig cfg;
      cfg.registry_path = runtime_controller_state_registry;
      cfg.stream_id = runtime_controller_state_stream;
      cfg.shm_name = runtime_controller_state_shm_name;
      cfg.frame_id = runtime_frame;
      cfg.slot_count = runtime_controller_state_slots;
      cfg.unlink_existing = !no_runtime_controller_state_unlink;
      cfg.created_by = "xr_runtime_adapter";
      runtime_controller_state_publisher =
          std::make_unique<xr_runtime::RuntimeControllerStateShmPublisher>(std::move(cfg));
      std::cout << "[xr_runtime_adapter] publishing runtime controller state stream "
                << runtime_controller_state_stream << " -> " << runtime_controller_state_shm_name
                << " frame=" << runtime_frame
                << " mode=" << xr_runtime::runtime_controller_mode_name(runtime_controller_mode_value)
                << "\n";
    }

    override_controller::RuntimeControllerSynthesisConfig runtime_controller_synthesis_cfg;
    runtime_controller_synthesis_cfg.mode = runtime_controller_mode_value;
    runtime_controller_synthesis_cfg.controller_trigger_threshold = controller_trigger_pinch_threshold;
    runtime_controller_synthesis_cfg.controller_grip_threshold = controller_grip_grab_threshold;
    runtime_controller_synthesis_cfg.left_hmd_relative_offset_m[0] = runtime_controller_left_offset_x;
    runtime_controller_synthesis_cfg.left_hmd_relative_offset_m[1] = runtime_controller_left_offset_y;
    runtime_controller_synthesis_cfg.left_hmd_relative_offset_m[2] = runtime_controller_left_offset_z;
    runtime_controller_synthesis_cfg.right_hmd_relative_offset_m[0] = runtime_controller_right_offset_x;
    runtime_controller_synthesis_cfg.right_hmd_relative_offset_m[1] = runtime_controller_right_offset_y;
    runtime_controller_synthesis_cfg.right_hmd_relative_offset_m[2] = runtime_controller_right_offset_z;
    for (size_t i = 0; i < 4; ++i) {
      runtime_controller_synthesis_cfg.left_static_orientation_xyzw[i] =
          runtime_controller_left_static_orientation_xyzw[i];
      runtime_controller_synthesis_cfg.right_static_orientation_xyzw[i] =
          runtime_controller_right_static_orientation_xyzw[i];
    }
    runtime_controller_synthesis_cfg.left_hand_gestures_enabled = effective_runtime_left_hand_gestures_enabled;
    runtime_controller_synthesis_cfg.right_hand_gestures_enabled = effective_runtime_right_hand_gestures_enabled;
    runtime_controller_synthesis_cfg.lost_hand_pose_fallback = runtime_controller_lost_hand_pose_fallback_value;

    const auto start = std::chrono::steady_clock::now();
    auto next_tick = start;
    const auto tick_period = std::chrono::duration<double>(1.0 / tick_rate_hz);

    uint64_t consumed = 0;
    uint64_t event_consumed = 0;
    uint64_t tick_consumed = 0;
  RuntimeOriginState origin_state(origin_mode, runtime_frame, recenter_on_reset_counter);

    uint64_t last_hmd_seq = 0;
    std::string last_hmd_source_for_count;
    uint64_t last_hmd_reset_counter = 0;
    uint64_t last_hand_seq = 0;
    uint64_t last_consumed_hmd_seq = 0;
    std::string last_consumed_hmd_source;
    uint64_t last_consumed_hand_seq = 0;
    uint64_t last_consumed_controller_seq = 0;
    uint64_t new_hmd_frames = 0;
    uint64_t new_hand_frames = 0;
    uint64_t stale_hmd_frames = 0;
    uint64_t stale_hand_frames = 0;
    uint64_t hmd_hold_last_frames = 0;
    uint64_t hand_hold_last_frames = 0;
    hmd_filter::HmdPoseStabilityFilter runtime_hmd_stability_filter;
    if (runtime_hmd_pose_stability_filter) {
      hmd_filter::HmdPoseStabilityFilterConfig hmd_gate_cfg;
      hmd_gate_cfg.enabled = true;
      hmd_gate_cfg.window_ms = runtime_hmd_pose_stability_window_ms;
      hmd_gate_cfg.max_distance_m = runtime_hmd_pose_stability_max_distance_cm / 100.0;
      runtime_hmd_stability_filter.configure(std::move(hmd_gate_cfg));
    }

    hand_filter::HandPoseStabilityFilter runtime_hand_stability_filter;
    if (runtime_hand_stability_gate) {
      hand_filter::HandPoseStabilityFilterConfig gate_cfg;
      gate_cfg.enabled = true;
      gate_cfg.max_reacquire_jump_m = runtime_hand_gate_max_jump_m;
      gate_cfg.confirm_frames = static_cast<uint32_t>(std::max(1, runtime_hand_gate_confirm_frames));
      gate_cfg.confirm_max_step_m = runtime_hand_gate_confirm_max_step_m;
      gate_cfg.hold_lost_ms = runtime_hand_gate_hold_lost_ms;
      gate_cfg.max_continuity_velocity_mps = runtime_hand_gate_max_continuity_velocity_mps;
      gate_cfg.predict_lost_ms = runtime_hand_gate_predict_lost_ms;
      gate_cfg.max_prediction_velocity_mps = runtime_hand_gate_max_prediction_velocity_mps;
      gate_cfg.prediction_damping = runtime_hand_gate_prediction_damping;
      gate_cfg.reacquire_blend_ms = runtime_hand_gate_reacquire_blend_ms;
      gate_cfg.debug_csv = runtime_hand_gate_debug_csv;
      runtime_hand_stability_filter.configure(std::move(gate_cfg));
    }

    jitter_filter::RuntimeJitterFilterConfig runtime_jitter_filter_cfg;
    runtime_jitter_filter_cfg.enabled = runtime_jitter_filter_enabled;
    runtime_jitter_filter_cfg.hmd_threshold_m = runtime_jitter_filter_hmd_cm / 100.0;
    runtime_jitter_filter_cfg.tracker_threshold_m = runtime_jitter_filter_tracker_cm / 100.0;
    runtime_jitter_filter_cfg.hmd_angle_threshold_rad = runtime_jitter_filter_hmd_deg * 3.14159265358979323846 / 180.0;
    runtime_jitter_filter_cfg.tracker_angle_threshold_rad = runtime_jitter_filter_tracker_deg * 3.14159265358979323846 / 180.0;
    jitter_filter::RuntimeJitterFilter runtime_jitter_filter(runtime_jitter_filter_cfg);

    uint64_t runtime_pose_published = 0;
    uint64_t runtime_hand_published = 0;
    uint64_t runtime_controller_state_published = 0;
    uint64_t runtime_hand_output_sequence = 0;
    uint64_t runtime_hand_clear_reset_counter = 0;
    bool runtime_last_hand_output_had_hands = false;
    uint64_t skeleton26_frames_used = 0;
    uint64_t controller_override_frames = 0;
    uint64_t override_controller_gesture_block_frames = 0;
    uint64_t override_controller_gesture_allow_frames = 0;
    int64_t override_controller_gesture_block_until_ns = 0;
    gestures::RuntimeGestureHysteresisState runtime_gesture_hysteresis;
    gestures::RuntimeHandGestureSnapshot last_fresh_runtime_hand_gestures;

    std::optional<xr_runtime::HmdPoseF64V1> last_good_hmd;
    std::string last_good_hmd_frame_id = "tracking_world";
    std::optional<xr_runtime::HandTrackingFrameF64V1> last_good_hand;
    std::optional<xr_runtime::HandTrackingFrameF32V2> last_good_hand_v2;
    std::string last_good_hand_frame_id = "tracking_world";

    while (!g_stop) {
      const auto loop_now = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(loop_now - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;

      xr_runtime::RuntimeFrame frame;
      frame.read_timestamp_ns = xr_runtime::now_ns();
      bool hmd_hold_last_active = false;
      bool hand_hold_last_active = false;
      bool runtime_visual_hand_tracking_fresh = false;
      HmdPoseSourceKind frame_hmd_source_kind = input == "udp" ? HmdPoseSourceKind::Udp : HmdPoseSourceKind::Primary6Dof;
      std::string frame_hmd_source_key;

      if (input == "udp" && udp_input) {
        udp_input->pump(udp_pump_limit);
      }

      if (input == "shm") {
        bool hmd_missing_or_stale = !hmd_reader;
        bool hmd_fresh = false;
        bool hmd_3dof_missing_or_stale = hmd_3dof_priority && !hmd_3dof_reader;
        bool hmd_3dof_fresh = false;

        xr_runtime::HmdPoseF64V1 selected_hmd{};
        std::string selected_hmd_frame_id;
        HmdPoseSourceKind selected_hmd_source = HmdPoseSourceKind::Primary6Dof;

        auto read_hmd_candidate = [&](xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>* reader,
                                      const char* source_name,
                                      HmdPoseSourceKind source_kind,
                                      bool* missing_or_stale,
                                      bool* fresh) -> bool {
          if (!reader) {
            *missing_or_stale = true;
            *fresh = false;
            return false;
          }
          const auto hmd = reader->latest();
          if (!hmd) {
            *missing_or_stale = true;
            *fresh = false;
            return false;
          }
          const bool stale = is_age_stale(frame.read_timestamp_ns,
                                          static_cast<int64_t>(hmd->timestamp_ns),
                                          max_hmd_age_ms);
          *missing_or_stale = stale;
          *fresh = hmd->sequence != 0 && !stale;
          if (stale && hmd->sequence != 0) {
            ++stale_hmd_frames;
          }
          if (hmd->sequence != 0 &&
              (hmd->sequence != last_hmd_seq || last_hmd_source_for_count != source_name)) {
            ++new_hmd_frames;
            last_hmd_seq = hmd->sequence;
            last_hmd_source_for_count = source_name;
            last_hmd_reset_counter = hmd->reset_counter;
          }
          if (*fresh) {
            selected_hmd = *hmd;
            selected_hmd_frame_id = reader->info().frame_id;
            selected_hmd_source = source_kind;
            frame_hmd_source_key = source_name;
            return true;
          }
          return false;
        };

        if (hmd_3dof_priority) {
          hmd_3dof_fresh = read_hmd_candidate(hmd_3dof_reader.get(),
                                              "hmd_3dof",
                                              HmdPoseSourceKind::Priority3Dof,
                                              &hmd_3dof_missing_or_stale,
                                              &hmd_3dof_fresh);
        }

        if (!hmd_3dof_fresh) {
          hmd_fresh = read_hmd_candidate(hmd_reader.get(),
                                         "hmd",
                                         HmdPoseSourceKind::Primary6Dof,
                                         &hmd_missing_or_stale,
                                         &hmd_fresh);
        }

        if (hmd_3dof_fresh || hmd_fresh) {
          frame.raw_hmd = selected_hmd;
          frame.hmd = selected_hmd;
          frame.hmd_valid = true;
          frame.hmd_frame_id = selected_hmd_frame_id;
          frame_hmd_source_kind = selected_hmd_source;

          if (runtime_hmd_pose_stability_filter) {
            hmd_filter::HmdPoseStabilityFilterDecision hmd_filter_decision;
            frame.hmd = runtime_hmd_stability_filter.filter(
                frame.raw_hmd, frame.read_timestamp_ns, &hmd_filter_decision);
            frame.raw_hmd = frame.hmd;
            frame.hmd_valid = frame.hmd.sequence != 0;
          }
          if (frame.hmd_valid) {
            last_good_hmd = frame.raw_hmd;
            last_good_hmd_frame_id = frame.hmd_frame_id;
          }
        } else if (hmd_reader) {
          const auto hmd = hmd_reader->latest();
          if (hmd && hmd->sequence != 0) {
            frame.raw_hmd = *hmd;
            frame.hmd = *hmd;
            frame.hmd.tracking_status = 4;  // lost
            frame.hmd_valid = false;
            frame.hmd_frame_id = hmd_reader->info().frame_id;
          }
        } else if (hmd_3dof_reader) {
          const auto hmd = hmd_3dof_reader->latest();
          if (hmd && hmd->sequence != 0) {
            frame.raw_hmd = *hmd;
            frame.hmd = *hmd;
            frame.hmd.tracking_status = 4;  // lost
            frame.hmd_valid = false;
            frame.hmd_frame_id = hmd_3dof_reader->info().frame_id;
            frame_hmd_source_kind = HmdPoseSourceKind::Priority3Dof;
            frame_hmd_source_key = "hmd_3dof";
          }
        }

        update_stale_since(hmd_reattach_state, hmd_missing_or_stale, frame.read_timestamp_ns);
        if (hmd_3dof_priority) {
          update_stale_since(hmd_3dof_reattach_state, hmd_3dof_missing_or_stale, frame.read_timestamp_ns);
        }
        if (!(hmd_3dof_fresh || hmd_fresh) && last_good_hmd &&
            hold_allowed(hmd_stale_policy, hmd_reattach_state.stale_since_ns,
                         frame.read_timestamp_ns, hmd_hold_last_max_ms)) {
          frame.raw_hmd = *last_good_hmd;
          frame.hmd = make_held_hmd_pose(*last_good_hmd, frame.read_timestamp_ns);
          frame.hmd_valid = true;
          frame.hmd_predicted = false;
          frame.hmd_frame_id = last_good_hmd_frame_id;
          hmd_hold_last_active = true;
          ++hmd_hold_last_frames;
          frame_hmd_source_key = last_good_hmd_frame_id;
        }
        if (reattach_due(hmd_reattach_state, hmd_missing_or_stale,
                         frame.read_timestamp_ns, hmd_reattach_on_stale_ms)) {
          attach_hmd_stream(hmd_reader ? "stale" : "missing", false);
        }
        if (hmd_3dof_priority &&
            reattach_due(hmd_3dof_reattach_state, hmd_3dof_missing_or_stale,
                         frame.read_timestamp_ns, hmd_3dof_reattach_on_stale_ms)) {
          attach_hmd_3dof_stream(hmd_3dof_reader ? "stale" : "missing", false);
        }

        bool hand_missing_or_stale = !hand_reader && !hand_reader_v2;
        bool hand_fresh = false;
        if (hand_reader) {
          auto hand = hand_reader->latest();
          if (hand) {
            frame.hand = *hand;
            const bool hand_stale = is_age_stale(frame.read_timestamp_ns,
                                                 static_cast<int64_t>(hand->timestamp_ns),
                                                 max_hand_age_ms);
            hand_missing_or_stale = hand_stale;
            frame.hand_valid = hand->sequence != 0 && !hand_stale;
            if (hand_stale && hand->sequence != 0) {
              ++stale_hand_frames;
              frame.hand.tracking_status = 2;  // lost
            }
            frame.hand_frame_id = hand_reader->info().frame_id;
            if (hand->sequence != 0 && hand->sequence != last_hand_seq) {
              ++new_hand_frames;
              last_hand_seq = hand->sequence;
            }
            if (frame.hand_valid) {
              hand_fresh = true;
              runtime_visual_hand_tracking_fresh = true;
              last_good_hand = frame.hand;
              last_good_hand_v2.reset();
              last_good_hand_frame_id = frame.hand_frame_id;
            }
          } else {
            hand_missing_or_stale = true;
          }
        }

        if (hand_reader_v2) {
          auto hand_v2 = hand_reader_v2->latest();
          if (hand_v2) {
            frame.hand_v2 = *hand_v2;
            const bool hand_v2_stale = is_age_stale(frame.read_timestamp_ns,
                                                    static_cast<int64_t>(hand_v2->timestamp_ns),
                                                    max_hand_age_ms);
            hand_missing_or_stale = hand_v2_stale;
            frame.hand_v2_valid = hand_v2->sequence != 0 && !hand_v2_stale;
            if (hand_v2_stale && hand_v2->sequence != 0) {
              ++stale_hand_frames;
              frame.hand_v2.tracking_status = 2;  // lost
            }
            frame.hand_frame_id = hand_reader_v2->info().frame_id;
            if (hand_v2->sequence != 0 && hand_v2->sequence != last_hand_seq) {
              ++new_hand_frames;
              last_hand_seq = hand_v2->sequence;
            }
            if (frame.hand_v2_valid) {
              hand_fresh = true;
              runtime_visual_hand_tracking_fresh = true;
              last_good_hand_v2 = frame.hand_v2;
              last_good_hand.reset();
              last_good_hand_frame_id = frame.hand_frame_id;
            }
          } else {
            hand_missing_or_stale = true;
          }
        }

        update_stale_since(hand_reattach_state, hand_missing_or_stale, frame.read_timestamp_ns);
        if (!hand_fresh &&
            hold_allowed(hand_stale_policy, hand_reattach_state.stale_since_ns,
                         frame.read_timestamp_ns, hand_hold_last_max_ms)) {
          if (last_good_hand_v2) {
            frame.hand_v2 = make_held_hand_frame(*last_good_hand_v2, frame.read_timestamp_ns);
            frame.hand_v2_valid = true;
            frame.hand_frame_id = last_good_hand_frame_id;
            hand_hold_last_active = true;
            ++hand_hold_last_frames;
          } else if (last_good_hand) {
            frame.hand = make_held_hand_frame(*last_good_hand, frame.read_timestamp_ns);
            frame.hand_valid = true;
            frame.hand_frame_id = last_good_hand_frame_id;
            hand_hold_last_active = true;
            ++hand_hold_last_frames;
          }
        }
        if (reattach_due(hand_reattach_state, hand_missing_or_stale,
                         frame.read_timestamp_ns, hand_reattach_on_stale_ms)) {
          attach_hand_stream((hand_reader || hand_reader_v2) ? "stale" : "missing", false);
        }
      } else {
        if (udp_input) {
          const auto& hmd = udp_input->latest_hmd();
          if (hmd) {
            frame.raw_hmd = *hmd;
            frame.hmd = *hmd;
            const bool hmd_stale = is_age_stale(frame.read_timestamp_ns,
                                                udp_input->latest_hmd_receive_timestamp_ns(),
                                                max_hmd_age_ms);
            frame.hmd_valid = hmd->sequence != 0 && !hmd_stale;
            if (hmd_stale && hmd->sequence != 0) {
              ++stale_hmd_frames;
              frame.hmd.tracking_status = 4;  // lost
            }
            frame.hmd_frame_id = udp_frame_id;
            frame_hmd_source_kind = HmdPoseSourceKind::Udp;
            frame_hmd_source_key = "udp";
            if (hmd->sequence != 0 &&
                (hmd->sequence != last_hmd_seq || last_hmd_source_for_count != "udp")) {
              ++new_hmd_frames;
              last_hmd_seq = hmd->sequence;
              last_hmd_source_for_count = "udp";
              last_hmd_reset_counter = hmd->reset_counter;
            }
            if (!hmd_stale && runtime_hmd_pose_stability_filter) {
              hmd_filter::HmdPoseStabilityFilterDecision hmd_filter_decision;
              frame.hmd = runtime_hmd_stability_filter.filter(
                  frame.raw_hmd, frame.read_timestamp_ns, &hmd_filter_decision);
              frame.raw_hmd = frame.hmd;
              frame.hmd_valid = frame.hmd.sequence != 0;
            }
            if (frame.hmd_valid) {
              last_good_hmd = frame.raw_hmd;
              last_good_hmd_frame_id = frame.hmd_frame_id;
            }
          }

          const auto& hand = udp_input->latest_hand();
          if (hand) {
            frame.hand = *hand;
            const bool hand_stale = is_age_stale(frame.read_timestamp_ns,
                                                 udp_input->latest_hand_receive_timestamp_ns(),
                                                 max_hand_age_ms);
            frame.hand_valid = hand->sequence != 0 && !hand_stale;
            if (hand_stale && hand->sequence != 0) {
              ++stale_hand_frames;
              frame.hand.tracking_status = 2;  // lost
            }
            frame.hand_frame_id = udp_frame_id;
            if (frame.hand_valid) {
              runtime_visual_hand_tracking_fresh = true;
            }
            if (hand->sequence != 0 && hand->sequence != last_hand_seq) {
              ++new_hand_frames;
              last_hand_seq = hand->sequence;
            }
          }

          const auto& hand_v2 = udp_input->latest_hand_v2();
          if (hand_v2) {
            frame.hand_v2 = *hand_v2;
            const bool hand_v2_stale = is_age_stale(frame.read_timestamp_ns,
                                                    udp_input->latest_hand_v2_receive_timestamp_ns(),
                                                    max_hand_age_ms);
            frame.hand_v2_valid = hand_v2->sequence != 0 && !hand_v2_stale;
            if (hand_v2_stale && hand_v2->sequence != 0) {
              ++stale_hand_frames;
              frame.hand_v2.tracking_status = 2;  // lost
            }
            frame.hand_frame_id = udp_frame_id;
            if (frame.hand_v2_valid) {
              runtime_visual_hand_tracking_fresh = true;
            }
            if (hand_v2->sequence != 0 && hand_v2->sequence != last_hand_seq) {
              ++new_hand_frames;
              last_hand_seq = hand_v2->sequence;
            }
          }
        }
      }

      bool skeleton26_input_used = false;
      if (hand_skeleton26_thread) {
        const auto skeleton_snapshot = hand_skeleton26_thread->snapshot();
        if (skeleton_snapshot.latest) {
          const bool skeleton_stale = is_age_stale(
              frame.read_timestamp_ns,
              static_cast<int64_t>(skeleton_snapshot.latest_receive_timestamp_ns),
              max_hand_age_ms);
          if (!skeleton_stale && skeleton_snapshot.latest->sequence != 0) {
            frame.hand_v2 = gestures::runtime_hand_v2_from_skeleton26(*skeleton_snapshot.latest,
                                                            hand_skeleton26_derive_gestures,
                                                            derived_pinch_active_threshold,
                                                            derived_grab_active_threshold,
                                                            derived_pinch_response_start,
                                                            derived_grab_response_start);
            frame.hand_v2_valid = frame.hand_v2.sequence != 0 && frame.hand_v2.hand_count > 0;
            frame.hand_valid = false;
            frame.hand_frame_id = "hand_skeleton26";
            if (frame.hand_v2.sequence != 0 && frame.hand_v2.sequence != last_hand_seq) {
              ++new_hand_frames;
              last_hand_seq = frame.hand_v2.sequence;
            }
            if (frame.hand_v2_valid) {
              runtime_visual_hand_tracking_fresh = true;
              last_good_hand_v2 = frame.hand_v2;
              last_good_hand.reset();
              last_good_hand_frame_id = frame.hand_frame_id;
            }
            skeleton26_input_used = true;
            ++skeleton26_frames_used;
          }
        }
      }

      if (runtime_hand_stability_gate && frame.hand_v2.sequence != 0) {
        frame.hand_v2 = runtime_hand_stability_filter.filter(frame.hand_v2);
        frame.hand_v2_valid = frame.hand_v2.sequence != 0 && frame.hand_v2.hand_count > 0;
        const bool hand_filter_degraded =
            frame.hand_v2.tracking_status == 2u ||
            frame.hand_v2.left.status == 2u ||
            frame.hand_v2.right.status == 2u;
        if (!frame.hand_v2_valid || hand_filter_degraded) {
          runtime_visual_hand_tracking_fresh = false;
        }
      }

      const bool clear_left_visual_gestures =
          runtime_ignore_backend_hand_gestures || !effective_runtime_left_hand_gestures_enabled;
      const bool clear_right_visual_gestures =
          runtime_ignore_backend_hand_gestures || !effective_runtime_right_hand_gestures_enabled;

      if ((clear_left_visual_gestures || clear_right_visual_gestures) && frame.hand_v2.sequence != 0) {
        gestures::clear_runtime_hand_v2_backend_gestures_by_side(
            frame.hand_v2, clear_left_visual_gestures, clear_right_visual_gestures);
      }
      if ((clear_left_visual_gestures || clear_right_visual_gestures) && frame.hand.sequence != 0) {
        auto clear_runtime_hand_v1_side_gestures = [](xr_runtime::HandSideF64V1& side) {
          side.pinch_strength = 0.0;
          side.grab_strength = 0.0;
          side.pinch_active = 0;
          side.grab_active = 0;
          side.flags &= ~(xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID);
        };
        if (clear_left_visual_gestures) clear_runtime_hand_v1_side_gestures(frame.hand.left);
        if (clear_right_visual_gestures) clear_runtime_hand_v1_side_gestures(frame.hand.right);
        if ((frame.hand.left.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) == 0u &&
            (frame.hand.right.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) == 0u) {
          frame.hand.flags &= ~xr_runtime::HAND_FLAG_GESTURES_VALID;
        }
      }

      bool controller_override_active = false;
      bool controller_stream_fresh = false;
      bool controller_stream_connected = false;
      bool controller_has_nonzero_input = false;
      bool controller_left_connected = false;
      bool controller_right_connected = false;
      uint64_t frame_controller_sequence = 0;
      std::optional<xr_runtime::ControllerInputV2> fresh_controller_input;
      if (controller_input_thread &&
          (controller_input_config.mode != "hand_tracking_only" || publish_runtime_controller_state_shm)) {
        const auto controller_snapshot = controller_input_thread->snapshot();
        if (controller_snapshot.latest) {
          const bool controller_receive_stale = is_age_stale(
              frame.read_timestamp_ns,
              static_cast<int64_t>(controller_snapshot.latest_receive_timestamp_ns),
              static_cast<double>(controller_input_config.max_age_ms));
          const bool controller_payload_stale = is_age_stale(
              frame.read_timestamp_ns,
              static_cast<int64_t>(controller_snapshot.latest->timestamp_ns),
              static_cast<double>(controller_input_config.max_age_ms));
          const bool controller_stale = controller_receive_stale || controller_payload_stale;
          if (!controller_stale && controller_snapshot.latest->sequence != 0) {
            controller_stream_fresh = true;
            fresh_controller_input = *controller_snapshot.latest;
            controller_stream_connected =
                override_controller::controller_input_has_present_controller(*controller_snapshot.latest);
            controller_has_nonzero_input =
                override_controller::controller_input_has_nonzero_input(*controller_snapshot.latest);
            controller_left_connected =
                override_controller::controller_device_is_present(controller_snapshot.latest->left);
            controller_right_connected =
                override_controller::controller_device_is_present(controller_snapshot.latest->right);
            // In controller_buttons_runtime_only, the override_controller service
            // being fresh is enough to own controller buttons. Do not key this off
            // non-zero input or even per-side present status: otherwise an idle
            // controller stream can let hand-tracking gestures leak into games for
            // a tick. If the stream becomes stale/missing, controller_override_active
            // becomes false and hand gestures are allowed again.
            controller_override_active = controller_buttons_runtime_only_mode
                ? controller_stream_fresh
                : controller_has_nonzero_input;
            frame_controller_sequence = controller_snapshot.latest->sequence;
            if (controller_buttons_runtime_only_mode &&
                override_controller_block_gestures_while_stream_present) {
              const int64_t block_until_ns = frame.read_timestamp_ns +
                  ms_to_ns(override_controller_gesture_block_latch_ms);
              override_controller_gesture_block_until_ns =
                  std::max(override_controller_gesture_block_until_ns, block_until_ns);
            }
            if (controller_override_active) {
              ++controller_override_frames;
            }
            if (!frame.hand_v2_valid && frame.hand_valid) {
              frame.hand_v2 = gestures::runtime_hand_v2_from_runtime_v1(frame.hand);
              frame.hand_v2_valid = true;
              frame.hand_valid = false;
            }
            const bool should_apply_controller_gesture_override =
                !runtime_controller_buttons_only_mode &&
                !controller_buttons_runtime_only_mode &&
                controller_has_nonzero_input &&
                (frame.hand_v2_valid || frame.hand_v2.sequence != 0);
            if (should_apply_controller_gesture_override) {
              const auto policy = xr_runtime::parse_controller_input_conflict_policy(
                  controller_input_config.conflict_policy);
              override_controller::apply_controller_gesture_override(frame.hand_v2,
                                                *controller_snapshot.latest,
                                                policy,
                                                controller_trigger_pinch_threshold,
                                                controller_grip_grab_threshold);
              frame.hand_v2_valid = frame.hand_v2.sequence != 0 &&
                                    (frame.hand_v2.hand_count > 0 ||
                                     frame.hand_v2.left.status == 1 ||
                                     frame.hand_v2.right.status == 1);
              // Legacy runtime hand path has been updated above.  The new runtime_controller_state
              // stream is composed later from the same fresh_controller_input snapshot.
            }
          }
        }
      }

      const bool runtime_only_gesture_input_blocked =
          controller_buttons_runtime_only_mode &&
          override_controller_block_gestures_while_stream_present &&
          (controller_stream_fresh || frame.read_timestamp_ns < override_controller_gesture_block_until_ns);
      if (controller_buttons_runtime_only_mode) {
        if (runtime_only_gesture_input_blocked) ++override_controller_gesture_block_frames;
        else ++override_controller_gesture_allow_frames;
      }

      const bool runtime_only_clear_left_visual_gestures = runtime_only_gesture_input_blocked;
      const bool runtime_only_clear_right_visual_gestures = runtime_only_gesture_input_blocked;
      if ((runtime_only_clear_left_visual_gestures || runtime_only_clear_right_visual_gestures) &&
          frame.hand_v2.sequence != 0) {
        gestures::clear_runtime_hand_v2_backend_gestures_by_side(
            frame.hand_v2,
            runtime_only_clear_left_visual_gestures,
            runtime_only_clear_right_visual_gestures);
      }
      if ((runtime_only_clear_left_visual_gestures || runtime_only_clear_right_visual_gestures) &&
          frame.hand.sequence != 0) {
        auto clear_runtime_hand_v1_side_gestures = [](xr_runtime::HandSideF64V1& side) {
          side.pinch_strength = 0.0;
          side.grab_strength = 0.0;
          side.pinch_active = 0;
          side.grab_active = 0;
          side.flags &= ~(xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID);
        };
        if (runtime_only_clear_left_visual_gestures) clear_runtime_hand_v1_side_gestures(frame.hand.left);
        if (runtime_only_clear_right_visual_gestures) clear_runtime_hand_v1_side_gestures(frame.hand.right);
        if ((frame.hand.left.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) == 0u &&
            (frame.hand.right.flags & (xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID)) == 0u) {
          frame.hand.flags &= ~xr_runtime::HAND_FLAG_GESTURES_VALID;
        }
      }

      const bool frame_hmd_stale_lost =
          frame.hmd.sequence != 0 && !frame.hmd_valid && frame.hmd.tracking_status == 4;
      const bool frame_hand_stale_lost =
          (frame.hand.sequence != 0 && !frame.hand_valid && frame.hand.tracking_status == 2) ||
          (frame.hand_v2.sequence != 0 && !frame.hand_v2_valid && frame.hand_v2.tracking_status == 2);
      const bool frame_any_hand_valid = frame.hand_valid || frame.hand_v2_valid;
      const uint64_t frame_hand_sequence = frame.hand_v2.sequence != 0 ? frame.hand_v2.sequence : frame.hand.sequence;

      bool should_consume = false;

      if (mode == "event") {
        should_consume =
            (frame.hmd_valid && frame.hmd.sequence != 0 &&
             (frame.hmd.sequence != last_consumed_hmd_seq || frame_hmd_source_key != last_consumed_hmd_source)) ||
            (frame_any_hand_valid && frame_hand_sequence != 0 && frame_hand_sequence != last_consumed_hand_seq) ||
            (controller_override_active && frame_controller_sequence != 0 &&
             frame_controller_sequence != last_consumed_controller_seq);
      } else {
        const auto now_for_tick = std::chrono::steady_clock::now();
        if (now_for_tick >= next_tick) {
          should_consume = frame.hmd_valid || frame_any_hand_valid || controller_override_active ||
                           (consume_stale_as_lost && (frame_hmd_stale_lost || frame_hand_stale_lost));

          frame.tick_frame = true;
          frame.target_timestamp_ns = frame.read_timestamp_ns +
                                      static_cast<int64_t>(prediction_ms * 1000000.0);
          frame.prediction_ms = prediction_ms;

          if (frame.hmd_valid && !no_prediction && !hmd_hold_last_active) {
            frame.hmd = xr_runtime::predict_hmd_pose(frame.raw_hmd,
                                                     frame.target_timestamp_ns,
                                                     max_prediction_ms);
            frame.hmd_predicted = true;
          }

          do {
            next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(tick_period);
          } while (next_tick <= now_for_tick);
        }
      }

      if (should_consume) {
        if (tracking_transform_config.enabled && frame.hmd.sequence != 0) {
          const StreamTransformConfig& hmd_transform =
              frame_hmd_source_kind == HmdPoseSourceKind::Priority3Dof
                  ? tracking_transform_config.hmd_3dof
                  : tracking_transform_config.hmd;
          apply_hmd_pose_transform(frame.hmd, hmd_transform);
          frame.raw_hmd = frame.hmd;
        }

        bool runtime_origin_applied = false;
        if (frame.hmd.sequence != 0 && origin_state.enabled()) {
          runtime_origin_applied = origin_state.apply(frame.hmd);
          if (runtime_origin_applied) {
            frame.hmd_frame_id = origin_state.output_frame();
          }
        }

        if (runtime_jitter_filter_enabled && frame.hmd.sequence != 0) {
          runtime_jitter_filter.filter_hmd(frame.hmd);
        }

        const RuntimeOriginSnapshot runtime_origin_snapshot = origin_state.snapshot();
        if (body_tracker_thread) {
          body_tracker_thread->set_origin_snapshot(runtime_origin_snapshot);
        }
        if (spatial_proxy_mesh_thread) {
          spatial_proxy_mesh_thread->set_origin_snapshot(runtime_origin_snapshot);
          RuntimeHmdPoseSnapshotForSpatialMesh runtime_hmd_for_spatial;
          if (frame.hmd.sequence != 0 && frame.hmd_valid) {
            runtime_hmd_for_spatial.valid = true;
            runtime_hmd_for_spatial.p = V3d{frame.hmd.px, frame.hmd.py, frame.hmd.pz};
            runtime_hmd_for_spatial.q = normalize_q({frame.hmd.qw, frame.hmd.qx, frame.hmd.qy, frame.hmd.qz});
            runtime_hmd_for_spatial.timestamp_ns = frame.read_timestamp_ns;
          }
          spatial_proxy_mesh_thread->set_runtime_hmd_snapshot(runtime_hmd_for_spatial);
        }

        if (tracking_transform_config.enabled) {
          std::optional<V3d> transformed_hmd_position;
          std::optional<Qd> transformed_hmd_orientation;
          if (frame.hmd.sequence != 0) {
            transformed_hmd_position = V3d{frame.hmd.px, frame.hmd.py, frame.hmd.pz};
            transformed_hmd_orientation = normalize_q({frame.hmd.qw, frame.hmd.qx, frame.hmd.qy, frame.hmd.qz});
          }
          const V3d* hmd_position_ptr = transformed_hmd_position ? &(*transformed_hmd_position) : nullptr;
          const Qd* hmd_orientation_ptr = transformed_hmd_orientation ? &(*transformed_hmd_orientation) : nullptr;
          const StreamTransformConfig& hand_transform = skeleton26_input_used
              ? tracking_transform_config.hand_skeleton26
              : tracking_transform_config.hand_tracking_21_joint;
          if (frame.hand_v2.sequence != 0) {
            apply_hand_frame_transform(frame.hand_v2, hand_transform, hmd_position_ptr, hmd_orientation_ptr);
          }
          if (frame.hand.sequence != 0) {
            apply_hand_frame_transform(frame.hand, tracking_transform_config.hand_tracking_21_joint, hmd_position_ptr, hmd_orientation_ptr);
          }
        }

        if (runtime_jitter_filter_enabled) {
          if (frame.hand_v2.sequence != 0) {
            runtime_jitter_filter.filter_hand(frame.hand_v2);
          }
          if (frame.hand.sequence != 0) {
            runtime_jitter_filter.filter_hand(frame.hand);
          }
        }

        const bool frame_runtime_left_hand_gestures_enabled =
            effective_runtime_left_hand_gestures_enabled && !runtime_only_gesture_input_blocked;
        const bool frame_runtime_right_hand_gestures_enabled =
            effective_runtime_right_hand_gestures_enabled && !runtime_only_gesture_input_blocked;

        const bool runtime_derived_gestures_have_fresh_tracking =
            !runtime_derived_gestures_require_fresh_tracking || runtime_visual_hand_tracking_fresh;

        if (frame.hand_v2.sequence != 0 && !runtime_derived_gestures_have_fresh_tracking) {
          gestures::apply_runtime_hand_v2_gesture_latch_or_clear(frame.hand_v2,
                                                                 last_fresh_runtime_hand_gestures,
                                                                 frame.read_timestamp_ns,
                                                                 runtime_derived_gesture_latch_ms,
                                                                 frame_runtime_left_hand_gestures_enabled,
                                                                 frame_runtime_right_hand_gestures_enabled);
          runtime_gesture_hysteresis.reset_all();
        } else {
          const bool should_derive_runtime_hand_gestures =
              runtime_derive_hand_gestures &&
              !runtime_only_gesture_input_blocked &&
              (!controller_override_active || runtime_derive_hand_gestures_with_controller_input);
          if (frame.hand_v2.sequence != 0 && should_derive_runtime_hand_gestures) {
            gestures::derive_missing_runtime_hand_v2_gestures(frame.hand_v2,
                                                    derived_pinch_active_threshold,
                                                    derived_grab_active_threshold,
                                                    derived_pinch_deactive_threshold,
                                                    derived_grab_deactive_threshold,
                                                    derived_pinch_response_start,
                                                    derived_grab_response_start,
                                                    runtime_gesture_hysteresis,
                                                    frame_runtime_left_hand_gestures_enabled,
                                                    frame_runtime_right_hand_gestures_enabled);
          }

          const bool should_derive_extra_gesture_buttons =
              runtime_derive_extra_gesture_buttons &&
              !runtime_only_gesture_input_blocked &&
              (!controller_override_active || runtime_derive_extra_gesture_buttons_with_controller_input);
          if (frame.hand_v2.sequence != 0 && should_derive_extra_gesture_buttons) {
            gestures::derive_runtime_hand_v2_extra_gesture_buttons(frame.hand_v2,
                                                         derived_thumbs_up_button_mask,
                                                         derived_index_point_button_mask,
                                                         derived_thumbs_up_active_threshold,
                                                         derived_index_point_active_threshold,
                                                         derived_thumbs_up_deactive_threshold,
                                                         derived_index_point_deactive_threshold,
                                                         derived_extra_gesture_response_start,
                                                         derived_extra_gesture_hold_ms,
                                                         frame.read_timestamp_ns,
                                                         runtime_gesture_hysteresis,
                                                         frame_runtime_left_hand_gestures_enabled,
                                                         frame_runtime_right_hand_gestures_enabled);
          }

          if (frame.hand_v2.sequence != 0 && runtime_visual_hand_tracking_fresh &&
              should_derive_runtime_hand_gestures) {
            gestures::capture_runtime_hand_v2_gesture_snapshot(last_fresh_runtime_hand_gestures,
                                                              frame.hand_v2,
                                                              frame.read_timestamp_ns,
                                                              frame_runtime_left_hand_gestures_enabled,
                                                              frame_runtime_right_hand_gestures_enabled);
          }
        }

        adapter->consume(frame);
        ++consumed;

        if ((runtime_pose_publisher || runtime_pose_udp_publisher) && frame.hmd.sequence != 0) {
          const uint64_t runtime_pose_seq = runtime_pose_publisher ? runtime_pose_publisher->next_sequence()
                                                                  : (runtime_pose_published + 1);
          auto runtime_pose = xr_runtime::runtime_hmd_pose_from_hmd_pose(
              frame.hmd,
              runtime_pose_seq,
              frame.read_timestamp_ns,
              frame.target_timestamp_ns,
              frame.hmd_valid,
              frame.hmd_predicted,
              runtime_origin_applied,
              frame_hmd_stale_lost);
          if (runtime_pose_publisher) runtime_pose_publisher->publish(runtime_pose);
          if (runtime_pose_udp_publisher) runtime_pose_udp_publisher->send_pose(runtime_pose);
          ++runtime_pose_published;
        }
        if (runtime_controller_state_publisher || runtime_controller_state_udp_publisher) {
          std::optional<xr_runtime::HandTrackingFrameF32V2> runtime_controller_hand;
          if (frame.hand_v2.sequence != 0) {
            runtime_controller_hand = frame.hand_v2;
          } else if (frame.hand.sequence != 0) {
            runtime_controller_hand = gestures::runtime_hand_v2_from_runtime_v1(frame.hand);
          }

          std::optional<xr_runtime::HmdPoseF64V1> runtime_controller_hmd;
          if (frame.hmd.sequence != 0) {
            runtime_controller_hmd = frame.hmd;
          }

          auto runtime_controller_synthesis_cfg_for_frame = runtime_controller_synthesis_cfg;
          if (runtime_only_gesture_input_blocked) {
            runtime_controller_synthesis_cfg_for_frame.left_hand_gestures_enabled = false;
            runtime_controller_synthesis_cfg_for_frame.right_hand_gestures_enabled = false;
          }

          auto runtime_controller_state = override_controller::compose_runtime_controller_state(
              runtime_controller_state_publisher ? runtime_controller_state_publisher->next_sequence()
                                                 : (runtime_controller_state_published + 1),
              static_cast<uint64_t>(frame.read_timestamp_ns),
              runtime_controller_synthesis_cfg_for_frame,
              runtime_controller_hand,
              fresh_controller_input,
              runtime_controller_hmd);
          if (runtime_controller_state_publisher) runtime_controller_state_publisher->publish(runtime_controller_state);
          if (runtime_controller_state_udp_publisher) runtime_controller_state_udp_publisher->send_controller_state(runtime_controller_state);
          ++runtime_controller_state_published;
        }
        if (runtime_hand_publisher || runtime_hand_udp_publisher) {
          xr_tracking::HandTrackingFrameF32V2 runtime_hand{};
          if (frame.hand_v2_valid) {
            runtime_hand = runtime_hand_frame_v2_from_runtime(frame.hand_v2, frame.read_timestamp_ns);
          } else if (frame.hand_valid) {
            runtime_hand = runtime_hand_frame_v2_from_v1(frame.hand, frame.read_timestamp_ns);
          } else {
            runtime_hand = runtime_no_hands_frame_v2(frame.read_timestamp_ns, frame.hand_v2.reset_counter != 0 ? frame.hand_v2.reset_counter : frame.hand.reset_counter);
          }

          const bool runtime_hand_has_hands =
              runtime_hand.hand_count > 0 ||
              runtime_hand.left.status == 1u ||
              runtime_hand.right.status == 1u;
          if (!runtime_hand_has_hands && runtime_last_hand_output_had_hands) {
            ++runtime_hand_clear_reset_counter;
          }
          runtime_last_hand_output_had_hands = runtime_hand_has_hands;

          // Runtime hand output must advance even when publishing explicit
          // no-hands/lost frames. Otherwise downstream drivers may keep the last
          // visible hand/controller pose after the hand-tracking backend stops.
          runtime_hand.sequence = ++runtime_hand_output_sequence;
          runtime_hand.reset_counter += runtime_hand_clear_reset_counter;

          if (runtime_hand_publisher) runtime_hand_publisher->publish(runtime_hand);
          if (runtime_hand_udp_publisher) runtime_hand_udp_publisher->send_hand(runtime_hand);
          ++runtime_hand_published;
        }
        if (mode == "event") {
          ++event_consumed;
        } else {
          ++tick_consumed;
        }

        if (frame.hmd_valid && frame.hmd.sequence != 0) {
          last_consumed_hmd_seq = frame.hmd.sequence;
          last_consumed_hmd_source = frame_hmd_source_key;
        }
        if (frame_any_hand_valid && frame_hand_sequence != 0) {
          last_consumed_hand_seq = frame_hand_sequence;
        }
        if (controller_override_active && frame_controller_sequence != 0) {
          last_consumed_controller_seq = frame_controller_sequence;
        }

        if (print_every > 0 && consumed % static_cast<uint64_t>(print_every) == 0) {
          const double rate = double(consumed) / std::max(1e-9, elapsed_s);
          const double hmd_source_age_ms =
              (frame.hmd_valid
                   ? xr_runtime::ns_to_ms(frame.read_timestamp_ns -
                                          static_cast<int64_t>(frame.raw_hmd.timestamp_ns))
                   : 0.0);
          const double hmd_target_delta_ms =
              (frame.hmd_valid && frame.target_timestamp_ns != 0
                   ? xr_runtime::ns_to_ms(frame.target_timestamp_ns - frame.read_timestamp_ns)
                   : 0.0);
          const bool log_hand_valid = frame.hand_valid || frame.hand_v2_valid;
          const uint64_t log_hand_timestamp_ns = frame.hand_v2_valid ? frame.hand_v2.timestamp_ns : frame.hand.timestamp_ns;
          const uint64_t log_hand_seq = frame.hand_v2.sequence != 0 ? frame.hand_v2.sequence : frame.hand.sequence;
          const uint32_t log_hand_status = frame.hand_v2.sequence != 0 ? frame.hand_v2.tracking_status : frame.hand.tracking_status;
          const uint32_t log_hand_count = frame.hand_v2.sequence != 0 ? frame.hand_v2.hand_count : frame.hand.hand_count;
          const uint32_t log_left_joint_count = frame.hand_v2.sequence != 0 ? frame.hand_v2.left.joint_count : frame.hand.left.joint_count;
          const uint32_t log_right_joint_count = frame.hand_v2.sequence != 0 ? frame.hand_v2.right.joint_count : frame.hand.right.joint_count;
          const double hand_age_ms =
              (log_hand_valid
                   ? xr_runtime::ns_to_ms(frame.read_timestamp_ns -
                                          static_cast<int64_t>(log_hand_timestamp_ns))
                   : 0.0);

          const auto video_health = video_health_thread ? video_health_thread->snapshot() : VideoInputHealthSnapshot{};
          const auto body_tracker_health = body_tracker_thread ? body_tracker_thread->snapshot() : BodyTrackerInputSnapshot{};
          const auto spatial_proxy_mesh_health =
              spatial_proxy_mesh_thread ? spatial_proxy_mesh_thread->snapshot() : SpatialProxyMeshInputSnapshot{};

          std::cout << "[xr_runtime_adapter] frame=" << consumed
                    << " rate=" << rate << "Hz"
                    << " mode=" << mode
                    << " input=" << input
                    << " hmd=" << (frame.hmd_valid ? "yes" : "no")
                    << " hmd_seq=" << frame.hmd.sequence
                    << " hmd_reset_counter=" << frame.hmd.reset_counter
                    << " origin_mode=" << origin_state.mode_name()
                    << " origin_ready=" << (origin_state.ready() ? "yes" : "no")
                    << " runtime_frame=" << origin_state.output_frame()
                    << " hmd_status=" << xr_runtime::hmd_status_name(frame.hmd.tracking_status)
                    << " hmd_frame=" << frame.hmd_frame_id
                    << " hmd_source_age_ms=" << hmd_source_age_ms
                    << " hmd_target_delta_ms=" << hmd_target_delta_ms
                    << " hmd_predicted=" << (frame.hmd_predicted ? "yes" : "no")
                    << " hmd_hold_last=" << (hmd_hold_last_active ? "yes" : "no")
                    << " hmd_reattach_attempts=" << hmd_reattach_state.attempts
                    << " hmd_reattach_successes=" << hmd_reattach_state.successes
                    << " hmd_p=(" << frame.hmd.px << "," << frame.hmd.py << "," << frame.hmd.pz << ")"
                    << " runtime_pose_published=" << runtime_pose_published
                    << " runtime_pose_output=" << (runtime_pose_publisher ? "yes" : "no")
                    << " runtime_hand_published=" << runtime_hand_published
                    << " runtime_hand_output=" << (runtime_hand_publisher ? "yes" : "no")
                    << " runtime_controller_state_published=" << runtime_controller_state_published
                    << " runtime_controller_state_output=" << (runtime_controller_state_publisher ? "yes" : "no")
                    << " runtime_controller_mode=" << xr_runtime::runtime_controller_mode_name(runtime_controller_mode_value)
                    << " runtime_controller_lost_hand_pose_fallback="
                    << override_controller::lost_hand_pose_fallback_mode_name(runtime_controller_lost_hand_pose_fallback_value)
                    << " hands=" << (log_hand_valid ? "yes" : "no")
                    << " hand_v2=" << (frame.hand_v2_valid ? "yes" : "no")
                    << " hand_seq=" << log_hand_seq
                    << " hand_status=" << xr_runtime::hand_status_name(log_hand_status)
                    << " hand_count=" << log_hand_count
                    << " hand_frame=" << frame.hand_frame_id
                    << " hand_age_ms=" << hand_age_ms
                    << " hand_left_joints=" << log_left_joint_count
                    << " hand_right_joints=" << log_right_joint_count
                    << " hand_hold_last=" << (hand_hold_last_active ? "yes" : "no")
                    << " hand_skeleton26_used=" << (skeleton26_input_used ? "yes" : "no")
                    << " hand_reattach_attempts=" << hand_reattach_state.attempts
                    << " hand_reattach_successes=" << hand_reattach_state.successes
                    << " controller_override=" << (controller_override_active ? "yes" : "no")
                    << " runtime_only_gesture_blocked=" << (runtime_only_gesture_input_blocked ? "yes" : "no")
                    << " override_gesture_block_frames=" << override_controller_gesture_block_frames
                    << " override_gesture_allow_frames=" << override_controller_gesture_allow_frames
                    << " controller_seq=" << frame_controller_sequence
                    << " controller_input_mode=" << controller_input_config.mode
                    << " controller_input_transport=" << controller_input_config.transport
                    << " body_trackers=" << (body_tracker_health.enabled && body_tracker_health.connected ? "yes" : "no")
                    << " body_trackers_input=" << body_trackers_input
                    << " body_trackers_seq=" << body_tracker_health.last_sequence
                    << " body_trackers_count=" << body_tracker_health.tracker_count
                    << " runtime_body_trackers_published=" << body_tracker_health.runtime_published
                    << " runtime_body_trackers_output=" << (body_tracker_health.runtime_output_enabled ? "yes" : "no")
                    << " body_trackers_reattach_attempts=" << body_tracker_health.reattach_attempts
                    << " body_trackers_reattach_successes=" << body_tracker_health.reattach_successes
                    << " body_trackers_age_ms=" << body_tracker_health.age_ms
                    << " body_trackers_gaps=" << body_tracker_health.sequence_gaps
                    << " body_trackers_error=" << body_tracker_health.last_error
                    << " spatial_proxy_mesh=" << (spatial_proxy_mesh_health.enabled && spatial_proxy_mesh_health.connected ? "yes" : "no")
                    << " spatial_proxy_mesh_input=" << spatial_proxy_mesh_input
                    << " spatial_proxy_mesh_seq=" << spatial_proxy_mesh_health.last_sequence
                    << " spatial_proxy_mesh_frames=" << spatial_proxy_mesh_health.frames
                    << " spatial_proxy_mesh_vertices=" << spatial_proxy_mesh_health.vertices
                    << " spatial_proxy_mesh_triangles=" << spatial_proxy_mesh_health.triangles
                    << " runtime_spatial_proxy_mesh_published=" << spatial_proxy_mesh_health.runtime_published
                    << " runtime_spatial_proxy_mesh_output=" << (spatial_proxy_mesh_health.runtime_output_enabled ? "yes" : "no")
                    << " spatial_proxy_mesh_age_ms=" << spatial_proxy_mesh_health.age_ms
                    << " spatial_proxy_mesh_udp_packets=" << spatial_proxy_mesh_health.udp_packets
                    << " spatial_proxy_mesh_udp_complete=" << spatial_proxy_mesh_health.udp_complete_meshes
                    << " spatial_proxy_mesh_udp_invalid=" << spatial_proxy_mesh_health.udp_invalid_packets
                    << " spatial_proxy_mesh_winding_flipped=" << (spatial_proxy_mesh_health.triangle_winding_flipped ? "yes" : "no")
                    << " spatial_proxy_mesh_camera_relative_runtime=" << (spatial_proxy_mesh_health.camera_relative_runtime ? "yes" : "no")
                    << " spatial_proxy_mesh_camera_relative_hmd=" << (spatial_proxy_mesh_health.camera_relative_hmd ? "yes" : "no")
                    << " spatial_proxy_mesh_camera_relative_hmd_age_ms=" << spatial_proxy_mesh_health.camera_relative_hmd_age_ms
                    << " spatial_proxy_mesh_error=" << spatial_proxy_mesh_health.last_error
                    << " video=" << (video_health.enabled && video_health.connected ? "yes" : "no")
                    << " video_input=" << video_input
                    << " video_seq=" << video_health.last_sequence
                    << " runtime_video_published=" << video_health.runtime_video_published
                    << " runtime_video_output=" << (video_health.runtime_output_enabled ? "yes" : "no")
                    << " video_reattach_attempts=" << video_health.reattach_attempts
                    << " video_reattach_successes=" << video_health.reattach_successes
                    << " video_rate_hz=" << video_health.runtime_rate_hz
                    << " video_camera_rate_hz=" << video_health.camera_rate_hz
                    << " video_age_ms=" << video_health.age_ms
                    << " video_size=" << video_health.width << "x" << video_health.height
                    << " video_gaps=" << video_health.sequence_gaps
                    << " video_error=" << video_health.last_error
                    << "\n";
        }
      }

      if (mode == "tick") {
        std::this_thread::sleep_until(next_tick);
      } else {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(poll_ms));
      }
    }

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    VideoInputHealthSnapshot final_video_health;
    if (video_health_thread) {
      final_video_health = video_health_thread->snapshot();
      video_health_thread->stop();
    }

    BodyTrackerInputSnapshot final_body_tracker_health;
    if (body_tracker_thread) {
      final_body_tracker_health = body_tracker_thread->snapshot();
      body_tracker_thread->stop();
    }

    HandSkeleton26InputSnapshot final_skeleton26_health;
    if (hand_skeleton26_thread) {
      final_skeleton26_health = hand_skeleton26_thread->snapshot();
      hand_skeleton26_thread->stop();
    }

    ControllerInputThreadSnapshot final_controller_health;
    if (controller_input_thread) {
      final_controller_health = controller_input_thread->snapshot();
      controller_input_thread->stop();
    }

    adapter->stop();

    std::cout << "=== xr_runtime_adapter summary ===\n";
    std::cout << "adapter: " << adapter->name() << "\n";
    std::cout << "mode: " << mode << "\n";
    std::cout << "input: " << input << "\n";
    std::cout << "video_input: " << video_input << "\n";
    std::cout << "hand_skeleton26_input: " << hand_skeleton26_input << "\n";
    if (hand_skeleton26_input != "none") {
      std::cout << "hand_skeleton26_connected: " << (final_skeleton26_health.connected ? "true" : "false") << "\n";
      std::cout << "hand_skeleton26_frames: " << final_skeleton26_health.frames << "\n";
      std::cout << "hand_skeleton26_last_seq: " << final_skeleton26_health.last_sequence << "\n";
      std::cout << "hand_skeleton26_used_frames: " << skeleton26_frames_used << "\n";
      std::cout << "hand_skeleton26_last_error: " << final_skeleton26_health.last_error << "\n";
    }
    if (spatial_proxy_mesh_input != "none") {
      const auto final_spatial_proxy_mesh_health =
          spatial_proxy_mesh_thread ? spatial_proxy_mesh_thread->snapshot() : SpatialProxyMeshInputSnapshot{};
      std::cout << "spatial_proxy_mesh_connected: " << (final_spatial_proxy_mesh_health.connected ? "true" : "false") << "\n";
      std::cout << "spatial_proxy_mesh_frames: " << final_spatial_proxy_mesh_health.frames << "\n";
      std::cout << "spatial_proxy_mesh_last_seq: " << final_spatial_proxy_mesh_health.last_sequence << "\n";
      std::cout << "spatial_proxy_mesh_vertices: " << final_spatial_proxy_mesh_health.vertices << "\n";
      std::cout << "spatial_proxy_mesh_triangles: " << final_spatial_proxy_mesh_health.triangles << "\n";
      std::cout << "spatial_proxy_mesh_age_ms: " << final_spatial_proxy_mesh_health.age_ms << "\n";
      std::cout << "spatial_proxy_mesh_udp_packets: " << final_spatial_proxy_mesh_health.udp_packets << "\n";
      std::cout << "spatial_proxy_mesh_udp_complete: " << final_spatial_proxy_mesh_health.udp_complete_meshes << "\n";
      std::cout << "spatial_proxy_mesh_udp_invalid: " << final_spatial_proxy_mesh_health.udp_invalid_packets << "\n";
      std::cout << "spatial_proxy_mesh_winding_flipped: " << (final_spatial_proxy_mesh_health.triangle_winding_flipped ? "true" : "false") << "\n";
      std::cout << "spatial_proxy_mesh_camera_relative_runtime: " << (final_spatial_proxy_mesh_health.camera_relative_runtime ? "true" : "false") << "\n";
      std::cout << "spatial_proxy_mesh_camera_relative_hmd: " << (final_spatial_proxy_mesh_health.camera_relative_hmd ? "true" : "false") << "\n";
      std::cout << "spatial_proxy_mesh_camera_relative_hmd_age_ms: " << final_spatial_proxy_mesh_health.camera_relative_hmd_age_ms << "\n";
      std::cout << "runtime_spatial_proxy_mesh_published: " << final_spatial_proxy_mesh_health.runtime_published << "\n";
      std::cout << "runtime_spatial_proxy_mesh_output: " << (final_spatial_proxy_mesh_health.runtime_output_enabled ? "enabled" : "disabled") << "\n";
      std::cout << "spatial_proxy_mesh_last_error: " << final_spatial_proxy_mesh_health.last_error << "\n";
    }
    if (body_trackers_input != "none") {
      std::cout << "body_trackers_connected: " << (final_body_tracker_health.connected ? "true" : "false") << "\n";
      std::cout << "body_trackers_frames: " << final_body_tracker_health.frames << "\n";
      std::cout << "body_trackers_last_seq: " << final_body_tracker_health.last_sequence << "\n";
      std::cout << "body_trackers_count: " << final_body_tracker_health.tracker_count << "\n";
      std::cout << "body_trackers_age_ms: " << final_body_tracker_health.age_ms << "\n";
      std::cout << "body_trackers_sequence_gaps: " << final_body_tracker_health.sequence_gaps << "\n";
      std::cout << "body_trackers_dropped_estimate: " << final_body_tracker_health.dropped_estimate << "\n";
      std::cout << "body_trackers_reattach_attempts: " << final_body_tracker_health.reattach_attempts << "\n";
      std::cout << "body_trackers_reattach_successes: " << final_body_tracker_health.reattach_successes << "\n";
      std::cout << "body_trackers_reattach_failures: " << final_body_tracker_health.reattach_failures << "\n";
      std::cout << "body_trackers_invalid_packets: " << final_body_tracker_health.invalid_packets << "\n";
      std::cout << "runtime_body_trackers_published: " << final_body_tracker_health.runtime_published << "\n";
      std::cout << "runtime_body_trackers_output: " << (final_body_tracker_health.runtime_output_enabled ? "enabled" : "disabled") << "\n";
      std::cout << "body_trackers_last_error: " << final_body_tracker_health.last_error << "\n";
    }
    if (video_input != "none") {
      std::cout << "video_connected: " << (final_video_health.connected ? "true" : "false") << "\n";
      std::cout << "video_frames: " << final_video_health.frames << "\n";
      std::cout << "video_last_seq: " << final_video_health.last_sequence << "\n";
      std::cout << "video_rate_hz: " << final_video_health.runtime_rate_hz << "\n";
      std::cout << "video_camera_rate_hz: " << final_video_health.camera_rate_hz << "\n";
      std::cout << "video_age_ms: " << final_video_health.age_ms << "\n";
      std::cout << "video_size: " << final_video_health.width << "x" << final_video_health.height << "\n";
      std::cout << "video_sequence_gaps: " << final_video_health.sequence_gaps << "\n";
      std::cout << "video_dropped_estimate: " << final_video_health.dropped_estimate << "\n";
      std::cout << "video_last_error: " << final_video_health.last_error << "\n";
    }
    std::cout << "runtime_s: " << elapsed_s << "\n";
    std::cout << "consumed_frames: " << consumed << "\n";
    std::cout << "consume_stale_as_lost: " << (consume_stale_as_lost ? "true" : "false") << "\n";
    std::cout << "consume_rate_hz: " << (double(consumed) / std::max(1e-9, elapsed_s)) << "\n";
    std::cout << "event_consumed: " << event_consumed << "\n";
    std::cout << "tick_consumed: " << tick_consumed << "\n";
    std::cout << "new_hmd_frames: " << new_hmd_frames << "\n";
    std::cout << "new_hand_frames: " << new_hand_frames << "\n";
    std::cout << "stale_hmd_frames: " << stale_hmd_frames << "\n";
    std::cout << "stale_hand_frames: " << stale_hand_frames << "\n";
    std::cout << "hmd_hold_last_frames: " << hmd_hold_last_frames << "\n";
    std::cout << "hand_hold_last_frames: " << hand_hold_last_frames << "\n";
    std::cout << "hmd_reattach_attempts: " << hmd_reattach_state.attempts << "\n";
    std::cout << "hmd_reattach_successes: " << hmd_reattach_state.successes << "\n";
    std::cout << "hmd_reattach_failures: " << hmd_reattach_state.failures << "\n";
    std::cout << "hmd_reattach_last_error: " << hmd_reattach_state.last_error << "\n";
    std::cout << "hand_reattach_attempts: " << hand_reattach_state.attempts << "\n";
    std::cout << "hand_reattach_successes: " << hand_reattach_state.successes << "\n";
    std::cout << "hand_reattach_failures: " << hand_reattach_state.failures << "\n";
    std::cout << "hand_reattach_last_error: " << hand_reattach_state.last_error << "\n";
    std::cout << runtime_hmd_stability_filter.summary_string();
    std::cout << "runtime_pose_published: " << runtime_pose_published << "\n";
    std::cout << "runtime_pose_output: " << (runtime_pose_publisher ? "enabled" : "disabled") << "\n";
    if (runtime_pose_publisher) {
      std::cout << "runtime_pose_registry: " << runtime_pose_registry << "\n";
      std::cout << "runtime_pose_stream: " << runtime_pose_stream << "\n";
      std::cout << "runtime_pose_frame: " << runtime_frame << "\n";
    }
    std::cout << "runtime_hand_published: " << runtime_hand_published << "\n";
    std::cout << "runtime_hand_output: " << (runtime_hand_publisher ? "enabled" : "disabled") << "\n";
    std::cout << "runtime_controller_state_published: " << runtime_controller_state_published << "\n";
    std::cout << "runtime_controller_state_output: " << (runtime_controller_state_publisher ? "enabled" : "disabled") << "\n";
    std::cout << "runtime_controller_mode: " << xr_runtime::runtime_controller_mode_name(runtime_controller_mode_value) << "\n";
    std::cout << "runtime_controller_lost_hand_pose_fallback: "
              << override_controller::lost_hand_pose_fallback_mode_name(runtime_controller_lost_hand_pose_fallback_value)
              << "\n";
    if (runtime_controller_state_publisher) {
      std::cout << "runtime_controller_state_registry: " << runtime_controller_state_registry << "\n";
      std::cout << "runtime_controller_state_stream: " << runtime_controller_state_stream << "\n";
      std::cout << "runtime_controller_state_frame: " << runtime_frame << "\n";
    }
    std::cout << runtime_hand_stability_filter.summary_string();
    if (runtime_hand_publisher) {
      std::cout << "runtime_hand_registry: " << runtime_hand_registry << "\n";
      std::cout << "runtime_hand_stream: " << runtime_hand_stream << "\n";
      std::cout << "runtime_hand_frame: " << runtime_frame << "\n";
    }
    std::cout << "last_hmd_seq: " << last_hmd_seq << "\n";
    std::cout << "last_hmd_reset_counter: " << last_hmd_reset_counter << "\n";
    std::cout << "origin_mode: " << origin_state.mode_name() << "\n";
    std::cout << "origin_ready: " << (origin_state.ready() ? "true" : "false") << "\n";
    std::cout << "origin_reset_counter: " << origin_state.origin_reset_counter() << "\n";
    std::cout << "runtime_frame: " << origin_state.output_frame() << "\n";
    std::cout << "controller_input_mode: " << controller_input_config.mode << "\n";
    std::cout << "controller_input_transport: " << controller_input_config.transport << "\n";
    std::cout << "controller_input_connected: " << (final_controller_health.connected ? "true" : "false") << "\n";
    std::cout << "controller_input_frames: " << final_controller_health.frames << "\n";
    std::cout << "controller_input_last_seq: " << final_controller_health.last_sequence << "\n";
    std::cout << "controller_override_frames: " << controller_override_frames << "\n";
    std::cout << "override_controller_gesture_block_frames: " << override_controller_gesture_block_frames << "\n";
    std::cout << "override_controller_gesture_allow_frames: " << override_controller_gesture_allow_frames << "\n";
    std::cout << "controller_input_last_error: " << final_controller_health.last_error << "\n";
    std::cout << "controller_input_stream: " << controller_input_config.stream << "\n";
    std::cout << "left_controller_id: " << controller_input_config.left_controller_id << "\n";
    std::cout << "right_controller_id: " << controller_input_config.right_controller_id << "\n";
    std::cout << "controller_input_conflict_policy: " << controller_input_config.conflict_policy << "\n";
    std::cout << "controller_input_stale_policy: " << controller_input_config.stale_policy << "\n";
    std::cout << "hand_skeleton26_derive_gestures: " << (hand_skeleton26_derive_gestures ? "true" : "false") << "\n";
    std::cout << "runtime_derive_hand_gestures: " << (runtime_derive_hand_gestures ? "true" : "false") << "\n";
    std::cout << "runtime_derive_hand_gestures_with_controller_input: "
              << (runtime_derive_hand_gestures_with_controller_input ? "true" : "false") << "\n";
    std::cout << "runtime_derived_gestures_require_fresh_tracking: "
              << (runtime_derived_gestures_require_fresh_tracking ? "true" : "false") << "\n";
    std::cout << "runtime_derived_gesture_latch_ms: " << runtime_derived_gesture_latch_ms << "\n";
    std::cout << "runtime_ignore_backend_hand_gestures: "
              << (runtime_ignore_backend_hand_gestures ? "true" : "false") << "\n";
    std::cout << "runtime_left_hand_gestures_enabled: "
              << (runtime_left_hand_gestures_enabled ? "true" : "false") << "\n";
    std::cout << "runtime_right_hand_gestures_enabled: "
              << (runtime_right_hand_gestures_enabled ? "true" : "false") << "\n";
    std::cout << "runtime_hand_stability_gate_enabled: "
              << (runtime_hand_stability_gate ? "true" : "false") << "\n";
    std::cout << "derived_pinch_active_threshold: " << derived_pinch_active_threshold << "\n";
    std::cout << "derived_grab_active_threshold: " << derived_grab_active_threshold << "\n";
    std::cout << "derived_pinch_deactive_threshold: " << derived_pinch_deactive_threshold << "\n";
    std::cout << "derived_grab_deactive_threshold: " << derived_grab_deactive_threshold << "\n";
    std::cout << "derived_pinch_response_start: " << derived_pinch_response_start << "\n";
    std::cout << "derived_grab_response_start: " << derived_grab_response_start << "\n";
    std::cout << "controller_input_mode: " << controller_input_mode << "\n";
    std::cout << "controller_input_transport: " << controller_input_transport << "\n";
    std::cout << "left_controller_id: " << left_controller_id << "\n";
    std::cout << "right_controller_id: " << right_controller_id << "\n";
    std::cout << "last_hand_seq: " << last_hand_seq << "\n";

    if (udp_input) {
      std::cout << "udp_received_packets: " << udp_input->received_packets() << "\n";
      std::cout << "udp_valid_hmd_packets: " << udp_input->valid_hmd_packets() << "\n";
      std::cout << "udp_valid_hand_packets: " << udp_input->valid_hand_packets() << "\n";
      std::cout << "udp_valid_hand_v2_packets: " << udp_input->valid_hand_v2_packets() << "\n";
      std::cout << "udp_valid_hand_joints_v2_packets: " << udp_input->valid_hand_joints_v2_packets() << "\n";
      std::cout << "udp_invalid_packets: " << udp_input->invalid_packets() << "\n";
      std::cout << "udp_last_transport_seq: " << udp_input->last_transport_seq() << "\n";
      std::cout << "udp_estimated_transport_loss: " << udp_input->estimated_transport_loss() << "\n";
      std::cout << "udp_duplicate_or_reordered: " << udp_input->duplicate_or_reordered() << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
