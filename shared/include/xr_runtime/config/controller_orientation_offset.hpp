#pragma once

#include <array>
#include <cmath>
#include <string>
#include <nlohmann/json.hpp>
#include <xr_runtime/contracts/controller_runtime_profile.hpp>

namespace xr_runtime {

// Profile offsets follow the adapter's Euler (Rz * Ry * Rx) and aim_offset
// convention: aim_rotation * mounting_rotation.
inline ControllerRuntimeOrientationOffsetV1 parse_controller_orientation_offset(
    const nlohmann::json& j, const std::string& context,
    bool parent_enabled = true, const std::string& default_order = "post") {
  if (!j.is_object()) throw std::runtime_error(context + " must be an object");
  using Q = std::array<double, 4>;  // xyzw
  const auto mul = [](const Q& a, const Q& b) -> Q {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
  };
  const auto euler = [&](double x, double y, double z) {
    constexpr double half_deg = 3.14159265358979323846 / 360.0;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      throw std::runtime_error(context + " contains a non-finite angle");
    const Q qx{std::sin(x*half_deg),0,0,std::cos(x*half_deg)};
    const Q qy{0,std::sin(y*half_deg),0,std::cos(y*half_deg)};
    const Q qz{0,0,std::sin(z*half_deg),std::cos(z*half_deg)};
    return mul(qz, mul(qy, qx));
  };
  Q q{0,0,0,1};
  bool has_rotation = false;
  for (const char* key : {"offset_quat_xyzw", "quat_xyzw", "quaternion_xyzw"}) {
    if (!j.contains(key)) continue;
    const auto& v = j.at(key);
    if (!v.is_array() || v.size() != 4)
      throw std::runtime_error(context + "." + key + " must contain four numbers");
    for (size_t i = 0; i < 4; ++i) q[i] = v[i].get<double>();
    has_rotation = true;
    break;
  }
  if (!has_rotation) {
    const nlohmann::json* v = &j;
    for (const char* key : {"rotation_deg", "euler_deg", "rotation"}) {
      if (j.contains(key)) { v = &j.at(key); has_rotation = true; break; }
    }
    if (!v->is_object()) throw std::runtime_error(context + " rotation must be an object");
    for (const char* key : {"rx_deg", "ry_deg", "rz_deg", "roll", "pitch", "yaw", "x", "y", "z"})
      has_rotation = has_rotation || v->contains(key);
    q = euler(v->value("rx_deg", v->value("roll", v->value("x", 0.0))),
              v->value("ry_deg", v->value("pitch", v->value("y", 0.0))),
              v->value("rz_deg", v->value("yaw", v->value("z", 0.0))));
  }
  if (j.contains("aim_offset")) {
    const auto& aim = j.at("aim_offset");
    if (!aim.is_object()) throw std::runtime_error(context + ".aim_offset must be an object");
    const double pitch = aim.value("pitch_deg", 0.0), yaw = aim.value("yaw_deg", 0.0);
    if (!std::isfinite(pitch) || !std::isfinite(yaw) || std::abs(pitch) > 180 || std::abs(yaw) > 180)
      throw std::runtime_error(context + ".aim_offset angles must be between -180 and 180");
    q = mul(euler(pitch, yaw, 0), q);
    has_rotation = true;
  }
  double norm = 0;
  for (double v : q) norm += v*v;
  if (!std::isfinite(norm) || norm < 1e-12)
    throw std::runtime_error(context + " quaternion must be finite and nonzero");
  ControllerRuntimeOrientationOffsetV1 out;
  out.flags = CONTROLLER_RUNTIME_OFFSET_PROVIDED;
  if (parent_enabled && j.value("enabled", has_rotation)) out.flags |= CONTROLLER_RUNTIME_OFFSET_ENABLED;
  const auto order = j.value("multiply_order", default_order);
  if (order == "pre" || order == "world") out.flags |= CONTROLLER_RUNTIME_OFFSET_PRE_MULTIPLY;
  else if (order != "post" && order != "local")
    throw std::runtime_error(context + ".multiply_order must be post/local or pre/world");
  for (size_t i = 0; i < 4; ++i) out.rotation_xyzw[i] = static_cast<float>(q[i] / std::sqrt(norm));
  return out;
}

}  // namespace xr_runtime
