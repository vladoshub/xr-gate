#pragma once

#include <xr_override_controller/imu/controller_imu_processor.hpp>
#include <cmath>
#include <stdexcept>
#include <string>

namespace xr_override_controller::imu {

// Shared option spelling and SI units for every controller provider.
inline bool apply_gyro_bias_option(GyroBiasCalibrationConfig& cfg,
    const std::string& key, const std::string& value) {
  if (key.rfind("gyro_bias.", 0) != 0) return false;
  size_t consumed = 0;
  const double parsed = std::stod(value, &consumed);
  if (consumed != value.size() || !std::isfinite(parsed) || parsed <= 0 || parsed > 100'000) {
    throw std::invalid_argument(key + " expects a finite positive number");
  }
  const float number = static_cast<float>(parsed);
  if (key == "gyro_bias.stable_ms") cfg.stable_ms = number;
  else if (key == "gyro_bias.min_samples") {
    if (std::floor(parsed) != parsed) throw std::invalid_argument(key + " expects an integer");
    cfg.min_samples = static_cast<uint32_t>(parsed);
  } else if (key == "gyro_bias.max_gap_ms") cfg.max_gap_ms = number;
  else if (key == "gyro_bias.max_sample_speed_rad_s") cfg.max_sample_speed_rad_s = number;
  else if (key == "gyro_bias.max_bias_rad_s") cfg.max_bias_rad_s = number;
  else if (key == "gyro_bias.max_gyro_stddev_rad_s") cfg.max_gyro_stddev_rad_s = number;
  else if (key == "gyro_bias.max_force_error_m_s2") cfg.max_force_error_m_s2 = number;
  else if (key == "gyro_bias.max_force_range_m_s2") cfg.max_force_range_m_s2 = number;
  else throw std::invalid_argument("unknown gyro bias option: " + key);
  return true;
}

}  // namespace xr_override_controller::imu
