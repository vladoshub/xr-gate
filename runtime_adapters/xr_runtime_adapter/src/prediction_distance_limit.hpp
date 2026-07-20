#pragma once

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace xr_runtime_adapter::prediction_distance {

enum class ReferenceAxis {
  None,
  X,
  Y,
  Z,
};

inline ReferenceAxis parse_reference_axis(const std::string& value,
                                          const char* option_name) {
  if (value == "none" || value == "off" || value == "disabled") {
    return ReferenceAxis::None;
  }
  if (value == "x" || value == "X") return ReferenceAxis::X;
  if (value == "y" || value == "Y") return ReferenceAxis::Y;
  if (value == "z" || value == "Z") return ReferenceAxis::Z;
  throw std::runtime_error(std::string(option_name) +
                           " must be one of: none, x, y, z");
}

inline const char* reference_axis_name(ReferenceAxis axis) {
  switch (axis) {
    case ReferenceAxis::None: return "none";
    case ReferenceAxis::X: return "x";
    case ReferenceAxis::Y: return "y";
    case ReferenceAxis::Z: return "z";
  }
  return "none";
}

struct Config {
  // Maximum Euclidean distance from a prediction reference point to the
  // predicted tracking position. <= 0 disables this limit.
  double max_distance_m = 0.0;

  // Optional runtime-axis adjustment applied to the already transformed HMD
  // position. ReferenceAxis::None keeps the reference at the HMD position.
  ReferenceAxis reference_axis = ReferenceAxis::None;
  double reference_offset_m = 0.0;
};

inline std::array<double, 3> adjusted_reference(
    const std::array<double, 3>& transformed_hmd_position,
    const Config& cfg) {
  std::array<double, 3> out = transformed_hmd_position;
  switch (cfg.reference_axis) {
    case ReferenceAxis::None:
      break;
    case ReferenceAxis::X:
      out[0] += cfg.reference_offset_m;
      break;
    case ReferenceAxis::Y:
      out[1] += cfg.reference_offset_m;
      break;
    case ReferenceAxis::Z:
      out[2] += cfg.reference_offset_m;
      break;
  }
  return out;
}

inline bool enabled(const Config& cfg) {
  return std::isfinite(cfg.max_distance_m) && cfg.max_distance_m > 0.0;
}

inline bool exceeds(const std::array<double, 3>& position,
                    const std::array<double, 3>& transformed_hmd_position,
                    const Config& cfg,
                    double* distance_m = nullptr) {
  if (!enabled(cfg)) return false;
  for (double value : position) {
    if (!std::isfinite(value)) return false;
  }
  for (double value : transformed_hmd_position) {
    if (!std::isfinite(value)) return false;
  }
  if (!std::isfinite(cfg.reference_offset_m)) return false;

  const auto reference = adjusted_reference(transformed_hmd_position, cfg);
  const double dx = position[0] - reference[0];
  const double dy = position[1] - reference[1];
  const double dz = position[2] - reference[2];
  const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (distance_m != nullptr) *distance_m = distance;
  constexpr double kComparisonEpsilonM = 1.0e-9;
  return std::isfinite(distance) &&
         distance > cfg.max_distance_m + kComparisonEpsilonM;
}

}  // namespace xr_runtime_adapter::prediction_distance
