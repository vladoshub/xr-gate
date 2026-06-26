#include "coordinate_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace xr_runtime_adapter::coordinate_util {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

template <typename T>
void assign_v3(T& x, T& y, T& z, const V3d& v) {
  x = static_cast<T>(v.x);
  y = static_cast<T>(v.y);
  z = static_cast<T>(v.z);
}

template <typename T>
void assign_q(T& qw, T& qx, T& qy, T& qz, const Qd& q_raw) {
  const Qd q = normalize_q(q_raw);
  qw = static_cast<T>(q.w);
  qx = static_cast<T>(q.x);
  qy = static_cast<T>(q.y);
  qz = static_cast<T>(q.z);
}

int axis_index_from_name(const std::string& name) {
  if (name == "x" || name == "X" || name == "0") return 0;
  if (name == "y" || name == "Y" || name == "1") return 1;
  if (name == "z" || name == "Z" || name == "2") return 2;
  throw std::runtime_error("invalid axis name in tracking transform config: " + name);
}

V3d json_vec3(const nlohmann::json& j, const V3d& fallback = {}) {
  V3d out = fallback;
  if (j.is_null()) return out;
  if (j.is_array()) {
    if (j.size() > 0) out.x = j.at(0).get<double>();
    if (j.size() > 1) out.y = j.at(1).get<double>();
    if (j.size() > 2) out.z = j.at(2).get<double>();
    return out;
  }
  if (j.is_object()) {
    out.x = j.value("x", out.x);
    out.y = j.value("y", out.y);
    out.z = j.value("z", out.z);
    return out;
  }
  throw std::runtime_error("expected vector3 object or array in tracking transform config");
}

V3d json_basis_rotation_deg(const nlohmann::json& j, const V3d& fallback = {}) {
  V3d out = fallback;
  if (j.is_null()) return out;
  if (j.is_array()) return json_vec3(j, fallback);
  if (j.is_object()) {
    out.x = j.value("rx_deg", j.value("x", out.x));
    out.y = j.value("ry_deg", j.value("y", out.y));
    out.z = j.value("rz_deg", j.value("z", out.z));
    return out;
  }
  throw std::runtime_error("expected basis_rotation object or array in tracking transform config");
}

void parse_coordinate_transform(const nlohmann::json& j, CoordinateTransformConfig& out) {
  if (!j.is_object()) return;
  out.enabled = j.value("enabled", out.enabled);

  if (j.contains("axis_map")) {
    const auto& a = j.at("axis_map");
    if (!a.is_array() || a.size() != 3) {
      throw std::runtime_error("coordinate_transform.axis_map must be an array of 3 axes");
    }
    for (size_t i = 0; i < 3; ++i) {
      if (a.at(i).is_number_integer()) {
        const int idx = a.at(i).get<int>();
        if (idx < 0 || idx > 2) throw std::runtime_error("axis_map integer must be 0..2");
        out.axis_map[i] = idx;
      } else {
        out.axis_map[i] = axis_index_from_name(a.at(i).get<std::string>());
      }
    }
  }

  if (j.value("invert_x", false)) out.sign[0] = -out.sign[0];
  if (j.value("invert_y", false)) out.sign[1] = -out.sign[1];
  if (j.value("invert_z", false)) out.sign[2] = -out.sign[2];

  if (j.contains("sign")) {
    const auto& sign = j.at("sign");
    if (!sign.is_array() || sign.size() != 3) {
      throw std::runtime_error("coordinate_transform.sign must be an array of 3 numbers");
    }
    for (size_t i = 0; i < 3; ++i) out.sign[i] *= sign.at(i).get<double>();
  }

  if (j.contains("rotation_deg")) {
    out.rotation_deg = json_vec3(j.at("rotation_deg"), out.rotation_deg);
  } else if (j.contains("rotation_euler_deg")) {
    out.rotation_deg = json_vec3(j.at("rotation_euler_deg"), out.rotation_deg);
  }

  out.rotation_rad = {out.rotation_deg.x * kPi / 180.0,
                      out.rotation_deg.y * kPi / 180.0,
                      out.rotation_deg.z * kPi / 180.0};

  if (!std::isfinite(out.rotation_rad.x) ||
      !std::isfinite(out.rotation_rad.y) ||
      !std::isfinite(out.rotation_rad.z)) {
    throw std::runtime_error("coordinate_transform.rotation_deg must contain finite x/y/z values");
  }

  out.scale = j.value("scale", out.scale);
  if (!std::isfinite(out.scale) || out.scale == 0.0) {
    throw std::runtime_error("coordinate_transform.scale must be finite and non-zero");
  }
  if (j.contains("offset_m")) out.offset_m = json_vec3(j.at("offset_m"), out.offset_m);
}

void parse_orientation_transform(const nlohmann::json& j, OrientationTransformConfig& out) {
  if (!j.is_object()) return;
  out.enabled = j.value("enabled", out.enabled);

  if (j.contains("basis_rotation")) {
    out.basis_rotation_deg = json_basis_rotation_deg(j.at("basis_rotation"), out.basis_rotation_deg);
    if (!j.contains("enabled")) out.enabled = true;
  } else if (j.contains("basis_rotation_deg")) {
    out.basis_rotation_deg = json_basis_rotation_deg(j.at("basis_rotation_deg"), out.basis_rotation_deg);
    if (!j.contains("enabled")) out.enabled = true;
  }

  out.basis_rotation_rad = {out.basis_rotation_deg.x * kPi / 180.0,
                            out.basis_rotation_deg.y * kPi / 180.0,
                            out.basis_rotation_deg.z * kPi / 180.0};

  if (!std::isfinite(out.basis_rotation_rad.x) ||
      !std::isfinite(out.basis_rotation_rad.y) ||
      !std::isfinite(out.basis_rotation_rad.z)) {
    throw std::runtime_error("orientation_transform.basis_rotation must contain finite rx_deg/ry_deg/rz_deg values");
  }

  out.basis_q = q_from_euler_basis_xyz(out.basis_rotation_rad);
}

void parse_hmd_relative(const nlohmann::json& j, HmdRelativeConfig& out) {
  if (!j.is_object()) return;
  out.enabled = j.value("enabled", out.enabled);
  if (j.contains("offset_m")) out.offset_m = json_vec3(j.at("offset_m"), out.offset_m);
  out.rotate_with_hmd_orientation =
      j.value("rotate_with_hmd_orientation",
              j.value("rotate_with_hmd", out.rotate_with_hmd_orientation));
}

void parse_hand_side_orientation_offset(const nlohmann::json& j,
                                        HandSideOrientationOffsetConfig& out,
                                        bool parent_enabled) {
  if (!j.is_object()) return;
  out.enabled = j.value("enabled", parent_enabled);

  if (j.contains("rotation_deg")) {
    out.rotation_deg = json_basis_rotation_deg(j.at("rotation_deg"), out.rotation_deg);
  } else if (j.contains("rotation")) {
    out.rotation_deg = json_basis_rotation_deg(j.at("rotation"), out.rotation_deg);
  } else if (j.contains("basis_rotation")) {
    out.rotation_deg = json_basis_rotation_deg(j.at("basis_rotation"), out.rotation_deg);
  } else if (j.contains("rx_deg") || j.contains("ry_deg") || j.contains("rz_deg") ||
             j.contains("x") || j.contains("y") || j.contains("z")) {
    out.rotation_deg = json_basis_rotation_deg(j, out.rotation_deg);
  }

  out.rotation_rad = {out.rotation_deg.x * kPi / 180.0,
                      out.rotation_deg.y * kPi / 180.0,
                      out.rotation_deg.z * kPi / 180.0};
  if (!std::isfinite(out.rotation_rad.x) ||
      !std::isfinite(out.rotation_rad.y) ||
      !std::isfinite(out.rotation_rad.z)) {
    throw std::runtime_error("hand_orientation_offset rotation must contain finite rx_deg/ry_deg/rz_deg values");
  }
  out.q = q_from_euler_basis_xyz(out.rotation_rad);
}

void parse_hand_orientation_offset(const nlohmann::json& j, HandOrientationOffsetConfig& out) {
  if (!j.is_object()) return;
  out.enabled = j.value("enabled", out.enabled);
  out.apply_to_controller = j.value("apply_to_controller", out.apply_to_controller);
  out.apply_to_palm = j.value("apply_to_palm", out.apply_to_palm);
  out.apply_to_wrist = j.value("apply_to_wrist", out.apply_to_wrist);
  out.apply_to_joints = j.value("apply_to_joints", out.apply_to_joints);

  if (j.contains("apply_to") && j.at("apply_to").is_object()) {
    const auto& a = j.at("apply_to");
    out.apply_to_controller = a.value("controller", out.apply_to_controller);
    out.apply_to_palm = a.value("palm", out.apply_to_palm);
    out.apply_to_wrist = a.value("wrist", out.apply_to_wrist);
    out.apply_to_joints = a.value("joints", out.apply_to_joints);
  }

  const std::string multiply_order = j.value("multiply_order", std::string("post"));
  if (multiply_order == "pre" || multiply_order == "world") {
    out.pre_multiply = true;
  } else if (multiply_order == "post" || multiply_order == "local") {
    out.pre_multiply = false;
  } else {
    throw std::runtime_error("hand_orientation_offset.multiply_order must be 'post'/'local' or 'pre'/'world'");
  }

  if (j.contains("left")) parse_hand_side_orientation_offset(j.at("left"), out.left, out.enabled);
  if (j.contains("right")) parse_hand_side_orientation_offset(j.at("right"), out.right, out.enabled);
}

void parse_spatial_mesh_runtime_binding(const nlohmann::json& j, SpatialMeshRuntimeConfig& out) {
  if (!j.is_object()) return;

  // Preferred schema:
  //   mesh_runtime.runtime_binding.enabled
  //   mesh_runtime.runtime_binding.apply_origin_position
  //   mesh_runtime.runtime_binding.apply_origin_orientation
  if (j.contains("enabled")) {
    const bool enabled = j.at("enabled").get<bool>();
    out.apply_runtime_origin_position = enabled;
    out.apply_runtime_origin_orientation = enabled;
  }
  if (j.contains("apply_origin_position")) {
    out.apply_runtime_origin_position = j.at("apply_origin_position").get<bool>();
  }
  if (j.contains("apply_origin_orientation")) {
    out.apply_runtime_origin_orientation = j.at("apply_origin_orientation").get<bool>();
  }

  // Backward/CLI-style aliases for quick experiments.
  if (j.contains("apply_runtime_origin")) {
    const bool enabled = j.at("apply_runtime_origin").get<bool>();
    out.apply_runtime_origin_position = enabled;
    out.apply_runtime_origin_orientation = enabled;
  }
  if (j.contains("apply_runtime_origin_position")) {
    out.apply_runtime_origin_position = j.at("apply_runtime_origin_position").get<bool>();
  }
  if (j.contains("apply_runtime_origin_orientation")) {
    out.apply_runtime_origin_orientation = j.at("apply_runtime_origin_orientation").get<bool>();
  }
}

void parse_spatial_mesh_camera_relative_runtime(const nlohmann::json& j,
                                                SpatialMeshRuntimeConfig& out) {
  if (!j.is_object()) return;
  auto& cam = out.camera_relative_runtime;
  cam.enabled = j.value("enabled", cam.enabled);
  cam.require_hmd = j.value("require_hmd", cam.require_hmd);
  cam.apply_hmd_position = j.value("apply_hmd_position", cam.apply_hmd_position);
  cam.apply_hmd_orientation = j.value("apply_hmd_orientation", cam.apply_hmd_orientation);
  cam.max_hmd_age_ms = j.value("max_hmd_age_ms", cam.max_hmd_age_ms);
  if (j.contains("offset_m")) {
    cam.offset_m = json_vec3(j.at("offset_m"), cam.offset_m);
  }
}

void parse_spatial_mesh_runtime(const nlohmann::json& j, SpatialMeshRuntimeConfig& out) {
  if (!j.is_object()) return;
  if (j.contains("triangle_winding")) {
    out.triangle_winding = j.at("triangle_winding").get<std::string>();
    if (out.triangle_winding != "auto" && out.triangle_winding != "keep" && out.triangle_winding != "swap") {
      throw std::runtime_error("spatial mesh triangle_winding must be one of: auto, keep, swap");
    }
  }
  if (j.contains("extra_rotation_deg")) {
    out.extra_rotation_deg = json_vec3(j.at("extra_rotation_deg"), out.extra_rotation_deg);
  } else if (j.contains("rotation_deg")) {
    out.extra_rotation_deg = json_vec3(j.at("rotation_deg"), out.extra_rotation_deg);
  }
  if (j.contains("extra_offset_m")) {
    out.extra_offset_m = json_vec3(j.at("extra_offset_m"), out.extra_offset_m);
  } else if (j.contains("offset_m")) {
    out.extra_offset_m = json_vec3(j.at("offset_m"), out.extra_offset_m);
  }
  if (j.contains("runtime_binding")) {
    parse_spatial_mesh_runtime_binding(j.at("runtime_binding"), out);
  } else {
    // Accept the same keys directly inside mesh_runtime for compact/manual profiles.
    parse_spatial_mesh_runtime_binding(j, out);
  }
  if (j.contains("camera_relative_runtime")) {
    parse_spatial_mesh_camera_relative_runtime(j.at("camera_relative_runtime"), out);
  }
}

void parse_stream_transform(const nlohmann::json& j, StreamTransformConfig& out) {
  if (!j.is_object()) return;
  if (j.contains("coordinate_transform")) {
    parse_coordinate_transform(j.at("coordinate_transform"), out.coordinate_transform);
  }
  if (j.contains("orientation_transform")) {
    parse_orientation_transform(j.at("orientation_transform"), out.orientation_transform);
  }
  if (j.contains("hmd_relative")) {
    parse_hmd_relative(j.at("hmd_relative"), out.hmd_relative);
  }
  if (j.contains("hand_orientation_offset")) {
    parse_hand_orientation_offset(j.at("hand_orientation_offset"), out.hand_orientation_offset);
  }
  // Alias for controller-centric configs.
  if (j.contains("controller_orientation_offset")) {
    parse_hand_orientation_offset(j.at("controller_orientation_offset"), out.hand_orientation_offset);
  }
  if (j.contains("mesh_runtime")) {
    parse_spatial_mesh_runtime(j.at("mesh_runtime"), out.spatial_mesh);
  } else if (j.contains("spatial_mesh")) {
    parse_spatial_mesh_runtime(j.at("spatial_mesh"), out.spatial_mesh);
  } else if (j.contains("mesh")) {
    parse_spatial_mesh_runtime(j.at("mesh"), out.spatial_mesh);
  }
}

V3d rotate_x(const V3d& in, double radians) {
  if (radians == 0.0) return in;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  return {in.x, in.y * c - in.z * s, in.y * s + in.z * c};
}

V3d rotate_y(const V3d& in, double radians) {
  if (radians == 0.0) return in;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  return {in.x * c + in.z * s, in.y, -in.x * s + in.z * c};
}

V3d rotate_z(const V3d& in, double radians) {
  if (radians == 0.0) return in;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  return {in.x * c - in.y * s, in.x * s + in.y * c, in.z};
}

V3d apply_rotation_xyz(const V3d& in, const V3d& rotation_rad) {
  V3d out = in;
  out = rotate_x(out, rotation_rad.x);
  out = rotate_y(out, rotation_rad.y);
  out = rotate_z(out, rotation_rad.z);
  return out;
}

V3d apply_coordinate_transform(const CoordinateTransformConfig& cfg,
                               const V3d& in,
                               bool apply_offset) {
  if (!cfg.enabled) return in;
  const double v[3] = {in.x, in.y, in.z};
  V3d out;
  out.x = cfg.sign[0] * v[cfg.axis_map[0]];
  out.y = cfg.sign[1] * v[cfg.axis_map[1]];
  out.z = cfg.sign[2] * v[cfg.axis_map[2]];
  out = apply_rotation_xyz(out, cfg.rotation_rad);
  out.x = out.x * cfg.scale + (apply_offset ? cfg.offset_m.x : 0.0);
  out.y = out.y * cfg.scale + (apply_offset ? cfg.offset_m.y : 0.0);
  out.z = out.z * cfg.scale + (apply_offset ? cfg.offset_m.z : 0.0);
  return out;
}

V3d apply_hmd_relative(const HmdRelativeConfig& cfg,
                       const V3d& in,
                       const V3d* hmd_position,
                       const Qd* hmd_orientation) {
  if (!cfg.enabled) return in;

  // Two useful modes:
  // 1) rotate_with_hmd_orientation=false:
  //    position is interpreted in the already-runtime/world/start frame; add
  //    post-origin HMD position and offset, but do not yaw-rotate the hand when
  //    the user turns their head/body.
  // 2) rotate_with_hmd_orientation=true:
  //    position is interpreted as true HMD-local/camera-local coordinates.
  V3d local{in.x + cfg.offset_m.x,
            in.y + cfg.offset_m.y,
            in.z + cfg.offset_m.z};

  V3d out = (cfg.rotate_with_hmd_orientation && hmd_orientation)
      ? q_rotate(*hmd_orientation, local)
      : local;

  if (hmd_position) {
    out.x += hmd_position->x;
    out.y += hmd_position->y;
    out.z += hmd_position->z;
  }
  return out;
}

enum class HandOrientationOffsetTarget {
  Controller,
  Palm,
  Wrist,
  Joint,
};

bool hand_orientation_offset_target_enabled(const HandOrientationOffsetConfig& cfg,
                                            HandOrientationOffsetTarget target) {
  switch (target) {
    case HandOrientationOffsetTarget::Controller: return cfg.apply_to_controller;
    case HandOrientationOffsetTarget::Palm: return cfg.apply_to_palm;
    case HandOrientationOffsetTarget::Wrist: return cfg.apply_to_wrist;
    case HandOrientationOffsetTarget::Joint: return cfg.apply_to_joints;
  }
  return false;
}

Qd apply_hand_orientation_offset(const StreamTransformConfig& cfg,
                                 const Qd& in,
                                 bool is_left,
                                 HandOrientationOffsetTarget target) {
  const auto& offset_cfg = cfg.hand_orientation_offset;
  if (!offset_cfg.enabled || !hand_orientation_offset_target_enabled(offset_cfg, target)) {
    return normalize_q(in);
  }

  const auto& side_cfg = is_left ? offset_cfg.left : offset_cfg.right;
  if (!side_cfg.enabled) return normalize_q(in);

  const Qd base = normalize_q(in);
  const Qd offset = normalize_q(side_cfg.q);
  return normalize_q(offset_cfg.pre_multiply ? q_mul(offset, base) : q_mul(base, offset));
}

bool nonzero_q(const Qd& q) {
  if (!finite_q(q)) return false;
  const double n2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
  return std::isfinite(n2) && n2 > 1e-8;
}

bool hand_side_has_transformable_payload(const xr_runtime::HandSideF32V2& side) {
  const bool tracking = side.status == 1u || side.status == 2u;
  const bool has_pose = (side.flags & xr_runtime::HAND_POSE_VALID) != 0u;
  const bool has_joints = (side.flags & xr_runtime::HAND_JOINTS_VALID) != 0u && side.joint_count > 0u;
  return tracking && (has_pose || has_joints);
}

void apply_hand_joint_transform(xr_runtime::HandJointF32V2& joint,
                                const StreamTransformConfig& cfg,
                                const V3d* hmd_position,
                                const Qd* hmd_orientation,
                                bool is_left) {
  const V3d p = apply_stream_position_transform(cfg, {joint.px, joint.py, joint.pz}, hmd_position, hmd_orientation);
  assign_v3(joint.px, joint.py, joint.pz, p);

  const Qd in_q{joint.qw, joint.qx, joint.qy, joint.qz};
  if (nonzero_q(in_q)) {
    const Qd q = apply_stream_orientation_transform(cfg, in_q, hmd_orientation);
    assign_q(joint.qw, joint.qx, joint.qy, joint.qz,
             apply_hand_orientation_offset(cfg, q, is_left, HandOrientationOffsetTarget::Joint));
  }
}

void apply_hand_side_transform(xr_runtime::HandSideF32V2& side,
                               const StreamTransformConfig& cfg,
                               const V3d* hmd_position,
                               const Qd* hmd_orientation,
                               bool is_left) {
  if ((side.flags & xr_runtime::HAND_POSE_VALID) != 0u) {
    V3d p = apply_stream_position_transform(cfg, {side.controller_px, side.controller_py, side.controller_pz}, hmd_position, hmd_orientation);
    assign_v3(side.controller_px, side.controller_py, side.controller_pz, p);
    p = apply_stream_position_transform(cfg, {side.palm_px, side.palm_py, side.palm_pz}, hmd_position, hmd_orientation);
    assign_v3(side.palm_px, side.palm_py, side.palm_pz, p);
    p = apply_stream_position_transform(cfg, {side.wrist_px, side.wrist_py, side.wrist_pz}, hmd_position, hmd_orientation);
    assign_v3(side.wrist_px, side.wrist_py, side.wrist_pz, p);

    const Qd controller_q{side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz};
    if (nonzero_q(controller_q)) {
      Qd q = apply_stream_orientation_transform(cfg, controller_q, hmd_orientation);
      assign_q(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz,
               apply_hand_orientation_offset(cfg, q, is_left, HandOrientationOffsetTarget::Controller));
    }

    const Qd palm_q{side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz};
    if (nonzero_q(palm_q)) {
      Qd q = apply_stream_orientation_transform(cfg, palm_q, hmd_orientation);
      assign_q(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz,
               apply_hand_orientation_offset(cfg, q, is_left, HandOrientationOffsetTarget::Palm));
    }

    const Qd wrist_q{side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz};
    if (nonzero_q(wrist_q)) {
      Qd q = apply_stream_orientation_transform(cfg, wrist_q, hmd_orientation);
      assign_q(side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz,
               apply_hand_orientation_offset(cfg, q, is_left, HandOrientationOffsetTarget::Wrist));
    }

    V3d v = apply_stream_vector_transform(cfg, {side.vx, side.vy, side.vz});
    assign_v3(side.vx, side.vy, side.vz, v);
    V3d w = apply_stream_vector_transform(cfg, {side.wx, side.wy, side.wz});
    assign_v3(side.wx, side.wy, side.wz, w);
  }

  if ((side.flags & xr_runtime::HAND_JOINTS_VALID) != 0u) {
    const uint32_t n = std::min<uint32_t>(side.joint_count, xr_runtime::HAND_JOINT_COUNT_V2);
    for (uint32_t i = 0; i < n; ++i) {
      apply_hand_joint_transform(side.joints[i], cfg, hmd_position, hmd_orientation, is_left);
    }
  }
}

void apply_hand_side_transform(xr_runtime::HandSideF64V1& side,
                               const StreamTransformConfig& cfg,
                               const V3d* hmd_position,
                               const Qd* hmd_orientation,
                               bool is_left) {
  V3d p = apply_stream_position_transform(cfg, {side.palm_px, side.palm_py, side.palm_pz}, hmd_position, hmd_orientation);
  assign_v3(side.palm_px, side.palm_py, side.palm_pz, p);
  p = apply_stream_position_transform(cfg, {side.wrist_px, side.wrist_py, side.wrist_pz}, hmd_position, hmd_orientation);
  assign_v3(side.wrist_px, side.wrist_py, side.wrist_pz, p);
  Qd q = apply_stream_orientation_transform(cfg, {side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz}, hmd_orientation);
  assign_q(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz,
           apply_hand_orientation_offset(cfg, q, is_left, HandOrientationOffsetTarget::Palm));
  q = apply_stream_orientation_transform(cfg, {side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz}, hmd_orientation);
  assign_q(side.wrist_qw, side.wrist_qx, side.wrist_qy, side.wrist_qz,
           apply_hand_orientation_offset(cfg, q, is_left, HandOrientationOffsetTarget::Wrist));
  V3d v = apply_stream_vector_transform(cfg, {side.vx, side.vy, side.vz});
  assign_v3(side.vx, side.vy, side.vz, v);
  V3d w = apply_stream_vector_transform(cfg, {side.wx, side.wy, side.wz});
  assign_v3(side.wx, side.wy, side.wz, w);
  const uint32_t n = std::min<uint32_t>(side.joint_count, xr_runtime::HAND_JOINT_COUNT_V1);
  for (uint32_t i = 0; i < n; ++i) {
    const V3d jp = apply_stream_position_transform(cfg,
        {side.joints[i].px, side.joints[i].py, side.joints[i].pz}, hmd_position, hmd_orientation);
    assign_v3(side.joints[i].px, side.joints[i].py, side.joints[i].pz, jp);
    const Qd jq = apply_stream_orientation_transform(cfg, {side.joints[i].qw, side.joints[i].qx, side.joints[i].qy, side.joints[i].qz}, hmd_orientation);
    assign_q(side.joints[i].qw, side.joints[i].qx, side.joints[i].qy, side.joints[i].qz,
             apply_hand_orientation_offset(cfg, jq, is_left, HandOrientationOffsetTarget::Joint));
  }
}

}  // namespace

bool finite_q(const Qd& q) {
  return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

bool finite_v3(const V3d& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Qd normalize_q(Qd q) {
  const double n2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
  if (!std::isfinite(n2) || n2 <= 0.0) return {};
  const double inv_n = 1.0 / std::sqrt(n2);
  q.w *= inv_n;
  q.x *= inv_n;
  q.y *= inv_n;
  q.z *= inv_n;
  if (q.w < 0.0) {
    q.w = -q.w;
    q.x = -q.x;
    q.y = -q.y;
    q.z = -q.z;
  }
  return q;
}

Qd q_conj(const Qd& q) {
  return {q.w, -q.x, -q.y, -q.z};
}

Qd q_mul_raw(const Qd& a, const Qd& b) {
  return {
      a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
      a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
      a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
      a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w};
}

Qd q_mul(const Qd& a, const Qd& b) {
  return normalize_q(q_mul_raw(a, b));
}

V3d q_rotate(const Qd& q_raw, const V3d& v) {
  const Qd q = normalize_q(q_raw);
  const Qd p{0.0, v.x, v.y, v.z};
  const Qd r = q_mul_raw(q_mul_raw(q, p), q_conj(q));
  return {r.x, r.y, r.z};
}

double yaw_from_q_z_up(const Qd& q_raw) {
  const Qd q = normalize_q(q_raw);
  return std::atan2(2.0 * (q.w*q.z + q.x*q.y),
                    1.0 - 2.0 * (q.y*q.y + q.z*q.z));
}

Qd q_from_yaw_z_up(double yaw) {
  const double half = 0.5 * yaw;
  return normalize_q({std::cos(half), 0.0, 0.0, std::sin(half)});
}

Qd q_from_axis_angle(double ax, double ay, double az, double radians) {
  if (radians == 0.0) return {};
  const double half = 0.5 * radians;
  const double s = std::sin(half);
  return normalize_q({std::cos(half), ax * s, ay * s, az * s});
}

Qd q_from_euler_basis_xyz(const V3d& radians) {
  const Qd qx = q_from_axis_angle(1.0, 0.0, 0.0, radians.x);
  const Qd qy = q_from_axis_angle(0.0, 1.0, 0.0, radians.y);
  const Qd qz = q_from_axis_angle(0.0, 0.0, 1.0, radians.z);
  return q_mul(qz, q_mul(qy, qx));
}

Qd apply_basis_quat_transform(const Qd& basis_q, const Qd& in) {
  return q_mul(q_mul(basis_q, in), q_conj(basis_q));
}

TrackingTransformConfig load_tracking_transform_config(const std::string& path) {
  TrackingTransformConfig cfg;
  cfg.path = path;
  if (path.empty()) return cfg;

  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open tracking transform config: " + path);
  }
  nlohmann::json j;
  in >> j;
  cfg.enabled = j.value("enabled", true);
  if (j.contains("streams")) {
    const auto& streams = j.at("streams");
    if (streams.contains("hmd")) parse_stream_transform(streams.at("hmd"), cfg.hmd);
    if (streams.contains("hmd_3dof")) parse_stream_transform(streams.at("hmd_3dof"), cfg.hmd_3dof);
    if (streams.contains("hand_tracking_21_joint")) {
      parse_stream_transform(streams.at("hand_tracking_21_joint"), cfg.hand_tracking_21_joint);
    } else if (streams.contains("mercury_hand_tracker")) {
      // Backward-compatible config key from the old Mercury-specific runtime contract.
      parse_stream_transform(streams.at("mercury_hand_tracker"), cfg.hand_tracking_21_joint);
    }
    if (streams.contains("hand_skeleton26")) {
      parse_stream_transform(streams.at("hand_skeleton26"), cfg.hand_skeleton26);
    }
    if (streams.contains("body_trackers")) {
      parse_stream_transform(streams.at("body_trackers"), cfg.body_trackers);
    }
    if (streams.contains("spatial_proxy_mesh")) {
      parse_stream_transform(streams.at("spatial_proxy_mesh"), cfg.spatial_proxy_mesh);
    }
  }
  return cfg;
}

V3d apply_stream_position_transform(const StreamTransformConfig& cfg,
                                    const V3d& in,
                                    const V3d* hmd_position,
                                    const Qd* hmd_orientation) {
  return apply_hmd_relative(cfg.hmd_relative,
                            apply_coordinate_transform(cfg.coordinate_transform, in, true),
                            hmd_position,
                            hmd_orientation);
}

V3d apply_stream_vector_transform(const StreamTransformConfig& cfg, const V3d& in) {
  return apply_coordinate_transform(cfg.coordinate_transform, in, false);
}

Qd apply_stream_orientation_transform(const StreamTransformConfig& cfg,
                                      const Qd& in,
                                      const Qd* hmd_orientation) {
  Qd out = cfg.orientation_transform.enabled
      ? apply_basis_quat_transform(cfg.orientation_transform.basis_q, in)
      : normalize_q(in);

  // If this stream is true HMD/camera-local, positions are rotated by the
  // post-origin HMD orientation in apply_hmd_relative(). Rotate orientations by
  // the same HMD orientation too; otherwise controller/hand models can spin
  // around their own axis while their positions follow the HMD frame.
  if (cfg.hmd_relative.enabled &&
      cfg.hmd_relative.rotate_with_hmd_orientation &&
      hmd_orientation &&
      finite_q(*hmd_orientation)) {
    out = q_mul(normalize_q(*hmd_orientation), normalize_q(out));
  }

  return normalize_q(out);
}

void apply_hmd_pose_transform(xr_runtime::HmdPoseF64V1& hmd,
                              const StreamTransformConfig& cfg) {
  V3d p = apply_stream_position_transform(cfg, {hmd.px, hmd.py, hmd.pz});
  assign_v3(hmd.px, hmd.py, hmd.pz, p);
  assign_q(hmd.qw, hmd.qx, hmd.qy, hmd.qz,
           apply_stream_orientation_transform(cfg, {hmd.qw, hmd.qx, hmd.qy, hmd.qz}));
  V3d v = apply_stream_vector_transform(cfg, {hmd.vx, hmd.vy, hmd.vz});
  assign_v3(hmd.vx, hmd.vy, hmd.vz, v);
  V3d w = apply_stream_vector_transform(cfg, {hmd.wx, hmd.wy, hmd.wz});
  assign_v3(hmd.wx, hmd.wy, hmd.wz, w);
}

void apply_hand_frame_transform(xr_runtime::HandTrackingFrameF32V2& hand,
                                const StreamTransformConfig& cfg,
                                const V3d* hmd_position,
                                const Qd* hmd_orientation) {
  if ((hand.flags & xr_runtime::HAND_FLAG_LEFT_VALID) != 0u &&
      hand_side_has_transformable_payload(hand.left)) {
    apply_hand_side_transform(hand.left, cfg, hmd_position, hmd_orientation, true);
  }
  if ((hand.flags & xr_runtime::HAND_FLAG_RIGHT_VALID) != 0u &&
      hand_side_has_transformable_payload(hand.right)) {
    apply_hand_side_transform(hand.right, cfg, hmd_position, hmd_orientation, false);
  }
}

void apply_hand_frame_transform(xr_runtime::HandTrackingFrameF64V1& hand,
                                const StreamTransformConfig& cfg,
                                const V3d* hmd_position,
                                const Qd* hmd_orientation) {
  apply_hand_side_transform(hand.left, cfg, hmd_position, hmd_orientation, true);
  apply_hand_side_transform(hand.right, cfg, hmd_position, hmd_orientation, false);
}

void apply_body_tracker_frame_transform(xr_tracking::BodyTrackerSetFrameF32V1& frame,
                                        const StreamTransformConfig& cfg,
                                        const V3d* hmd_position) {
  frame.tracker_count = std::min<uint32_t>(frame.tracker_count, xr_tracking::BODY_TRACKER_MAX_TRACKERS);
  for (uint32_t i = 0; i < frame.tracker_count; ++i) {
    auto& pose = frame.trackers[i].pose;
    V3d p = apply_stream_position_transform(cfg, {pose.px, pose.py, pose.pz}, hmd_position);
    assign_v3(pose.px, pose.py, pose.pz, p);
    assign_q(pose.qw, pose.qx, pose.qy, pose.qz,
             apply_stream_orientation_transform(cfg, {pose.qw, pose.qx, pose.qy, pose.qz}));
    V3d v = apply_stream_vector_transform(cfg, {pose.vx, pose.vy, pose.vz});
    assign_v3(pose.vx, pose.vy, pose.vz, v);
    V3d w = apply_stream_vector_transform(cfg, {pose.wx, pose.wy, pose.wz});
    assign_v3(pose.wx, pose.wy, pose.wz, w);
  }
}

std::string axis_map_string(const CoordinateTransformConfig& cfg) {
  const char* names[] = {"x", "y", "z"};
  return std::string("[") + names[cfg.axis_map[0]] + "," + names[cfg.axis_map[1]] + "," + names[cfg.axis_map[2]] + "]";
}

void log_stream_transform(const char* name, const StreamTransformConfig& cfg, bool include_mesh_runtime) {
  std::cout << "[xr_runtime_adapter] transform " << name
            << " axis_map=" << axis_map_string(cfg.coordinate_transform)
            << " sign=(" << cfg.coordinate_transform.sign[0] << ","
            << cfg.coordinate_transform.sign[1] << ","
            << cfg.coordinate_transform.sign[2] << ")"
            << " scale=" << cfg.coordinate_transform.scale
            << " offset_m=(" << cfg.coordinate_transform.offset_m.x << ","
            << cfg.coordinate_transform.offset_m.y << ","
            << cfg.coordinate_transform.offset_m.z << ")"
            << " orientation_basis_deg=(" << cfg.orientation_transform.basis_rotation_deg.x << ","
            << cfg.orientation_transform.basis_rotation_deg.y << ","
            << cfg.orientation_transform.basis_rotation_deg.z << ")"
            << " orientation_enabled=" << (cfg.orientation_transform.enabled ? "true" : "false")
            << " hmd_relative=" << (cfg.hmd_relative.enabled ? "true" : "false")
            << " hmd_rotate_with_hmd=" << (cfg.hmd_relative.rotate_with_hmd_orientation ? "true" : "false")
            << " hmd_offset_m=(" << cfg.hmd_relative.offset_m.x << ","
            << cfg.hmd_relative.offset_m.y << ","
            << cfg.hmd_relative.offset_m.z << ")";
  if (include_mesh_runtime) {
    std::cout << " mesh_triangle_winding=" << cfg.spatial_mesh.triangle_winding
              << " mesh_extra_rotation_deg=(" << cfg.spatial_mesh.extra_rotation_deg.x << ","
              << cfg.spatial_mesh.extra_rotation_deg.y << ","
              << cfg.spatial_mesh.extra_rotation_deg.z << ")"
              << " mesh_extra_offset_m=(" << cfg.spatial_mesh.extra_offset_m.x << ","
              << cfg.spatial_mesh.extra_offset_m.y << ","
              << cfg.spatial_mesh.extra_offset_m.z << ")"
              << " mesh_apply_origin_position="
              << (cfg.spatial_mesh.apply_runtime_origin_position ? "true" : "false")
              << " mesh_apply_origin_orientation="
              << (cfg.spatial_mesh.apply_runtime_origin_orientation ? "true" : "false")
              << " mesh_camera_relative_runtime="
              << (cfg.spatial_mesh.camera_relative_runtime.enabled ? "true" : "false")
              << " mesh_camera_relative_require_hmd="
              << (cfg.spatial_mesh.camera_relative_runtime.require_hmd ? "true" : "false")
              << " mesh_camera_relative_apply_hmd_position="
              << (cfg.spatial_mesh.camera_relative_runtime.apply_hmd_position ? "true" : "false")
              << " mesh_camera_relative_apply_hmd_orientation="
              << (cfg.spatial_mesh.camera_relative_runtime.apply_hmd_orientation ? "true" : "false")
              << " mesh_camera_relative_max_hmd_age_ms="
              << cfg.spatial_mesh.camera_relative_runtime.max_hmd_age_ms
              << " mesh_camera_relative_offset_m=("
              << cfg.spatial_mesh.camera_relative_runtime.offset_m.x << ","
              << cfg.spatial_mesh.camera_relative_runtime.offset_m.y << ","
              << cfg.spatial_mesh.camera_relative_runtime.offset_m.z << ")";
  }
  std::cout << "\n";
}

RuntimeOriginMode parse_runtime_origin_mode(const std::string& s) {
  if (s == "none") return RuntimeOriginMode::NONE;
  if (s == "start_pose") return RuntimeOriginMode::START_POSE;
  if (s == "yaw_only") return RuntimeOriginMode::YAW_ONLY;
  throw std::runtime_error("unknown --origin-mode: " + s + "; expected none, start_pose, yaw_only");
}

RuntimeOriginState::RuntimeOriginState(std::string mode_name,
                                       std::string output_frame,
                                       bool recenter_on_reset_counter)
    : mode_name_(std::move(mode_name)),
      output_frame_(std::move(output_frame)),
      mode_(parse_runtime_origin_mode(mode_name_)),
      recenter_on_reset_counter_(recenter_on_reset_counter) {}

bool RuntimeOriginState::enabled() const { return mode_ != RuntimeOriginMode::NONE; }

bool RuntimeOriginState::ready() const { return ready_; }

const std::string& RuntimeOriginState::mode_name() const { return mode_name_; }

const std::string& RuntimeOriginState::output_frame() const { return output_frame_; }

uint64_t RuntimeOriginState::origin_reset_counter() const { return origin_reset_counter_; }

RuntimeOriginSnapshot RuntimeOriginState::snapshot() const {
  RuntimeOriginSnapshot out;
  out.enabled = enabled();
  out.ready = ready_;
  out.origin_p = origin_p_;
  out.origin_q = origin_q_;
  out.origin_reset_counter = origin_reset_counter_;
  out.output_frame = output_frame_;
  return out;
}

bool RuntimeOriginState::apply(xr_runtime::HmdPoseF64V1& hmd) {
  if (!enabled()) return false;
  if (hmd.sequence == 0) return false;

  Qd q_raw{hmd.qw, hmd.qx, hmd.qy, hmd.qz};
  V3d p_raw{hmd.px, hmd.py, hmd.pz};

  if (!finite_q(q_raw) || !finite_v3(p_raw)) return false;
  q_raw = normalize_q(q_raw);

  const bool need_capture =
      !ready_ ||
      (recenter_on_reset_counter_ && hmd.reset_counter != origin_reset_counter_);

  if (need_capture) {
    capture_origin(p_raw, q_raw, hmd.reset_counter);
  }

  const V3d dp{p_raw.x - origin_p_.x, p_raw.y - origin_p_.y, p_raw.z - origin_p_.z};
  const Qd inv_origin_q = q_conj(origin_q_);

  const V3d p_local = q_rotate(inv_origin_q, dp);
  const Qd q_local = q_mul(inv_origin_q, q_raw);

  hmd.px = p_local.x;
  hmd.py = p_local.y;
  hmd.pz = p_local.z;

  hmd.qw = q_local.w;
  hmd.qx = q_local.x;
  hmd.qy = q_local.y;
  hmd.qz = q_local.z;

  if ((hmd.flags & xr_runtime::HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
    const V3d v_local = q_rotate(inv_origin_q, {hmd.vx, hmd.vy, hmd.vz});
    hmd.vx = v_local.x;
    hmd.vy = v_local.y;
    hmd.vz = v_local.z;
  }
  if ((hmd.flags & xr_runtime::HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) {
    const V3d w_local = q_rotate(inv_origin_q, {hmd.wx, hmd.wy, hmd.wz});
    hmd.wx = w_local.x;
    hmd.wy = w_local.y;
    hmd.wz = w_local.z;
  }

  return true;
}

void RuntimeOriginState::capture_origin(const V3d& p_raw, const Qd& q_raw, uint64_t reset_counter) {
  origin_p_ = p_raw;

  if (mode_ == RuntimeOriginMode::START_POSE) {
    origin_q_ = q_raw;
  } else if (mode_ == RuntimeOriginMode::YAW_ONLY) {
    origin_q_ = q_from_yaw_z_up(yaw_from_q_z_up(q_raw));
  } else {
    origin_q_ = {};
  }

  origin_reset_counter_ = reset_counter;
  ready_ = true;

  std::cout << "[xr_runtime_adapter] origin captured: "
            << "mode=" << mode_name_
            << " output_frame=" << output_frame_
            << " reset_counter=" << origin_reset_counter_
            << " origin_p=(" << origin_p_.x << "," << origin_p_.y << "," << origin_p_.z << ")"
            << "\n";
}

void apply_body_tracker_origin_transform(xr_tracking::BodyTrackerSetFrameF32V1& frame,
                                         const RuntimeOriginSnapshot& origin) {
  if (!origin.enabled || !origin.ready) return;

  const Qd inv_origin_q = q_conj(normalize_q(origin.origin_q));
  frame.tracker_count = std::min<uint32_t>(frame.tracker_count, xr_tracking::BODY_TRACKER_MAX_TRACKERS);

  for (uint32_t i = 0; i < frame.tracker_count; ++i) {
    auto& pose = frame.trackers[i].pose;

    const V3d p_raw{pose.px, pose.py, pose.pz};
    if (finite_v3(p_raw)) {
      const V3d dp{
          p_raw.x - origin.origin_p.x,
          p_raw.y - origin.origin_p.y,
          p_raw.z - origin.origin_p.z};
      const V3d p_local = q_rotate(inv_origin_q, dp);
      assign_v3(pose.px, pose.py, pose.pz, p_local);
    }

    const Qd q_raw{pose.qw, pose.qx, pose.qy, pose.qz};
    if (finite_q(q_raw)) {
      const Qd q_local = q_mul(inv_origin_q, normalize_q(q_raw));
      assign_q(pose.qw, pose.qx, pose.qy, pose.qz, q_local);
    }

    const V3d v_local = q_rotate(inv_origin_q, {pose.vx, pose.vy, pose.vz});
    assign_v3(pose.vx, pose.vy, pose.vz, v_local);

    const V3d w_local = q_rotate(inv_origin_q, {pose.wx, pose.wy, pose.wz});
    assign_v3(pose.wx, pose.wy, pose.wz, w_local);
  }
}

}  // namespace xr_runtime_adapter::coordinate_util
