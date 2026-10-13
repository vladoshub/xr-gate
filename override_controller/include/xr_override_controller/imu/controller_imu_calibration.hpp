#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace xr_override_controller::imu {

inline void validate_accelerometer_calibration(const AccelerometerCalibration& c) {
  const auto valid_axis = [](float bias, float scale) {
    return std::isfinite(bias) && std::abs(bias) <= 2.0f &&
           std::isfinite(scale) && scale >= 0.5f && scale <= 2.0f;
  };
  if (!valid_axis(c.bias_m_s2.x, c.scale.x) ||
      !valid_axis(c.bias_m_s2.y, c.scale.y) ||
      !valid_axis(c.bias_m_s2.z, c.scale.z)) {
    throw std::invalid_argument("invalid controller accelerometer calibration");
  }
}

inline bool same_accelerometer_calibration(const AccelerometerCalibration& a,
                                          const AccelerometerCalibration& b) {
  return a.enabled == b.enabled && (!a.enabled ||
      (a.bias_m_s2.x == b.bias_m_s2.x && a.bias_m_s2.y == b.bias_m_s2.y &&
       a.bias_m_s2.z == b.bias_m_s2.z && a.scale.x == b.scale.x &&
       a.scale.y == b.scale.y && a.scale.z == b.scale.z));
}

// Use the same unique fingerprint match required for button and IMU routing.
// device_id selects the config device; its current live match selects where
// to apply the values. An unconnected/ambiguous template has no live match.
inline int resolve_imu_config_device(const DeviceFingerprint& wanted,
                                    const std::vector<DeviceInfo>& devices) {
  int best_score = -1;
  int best_index = -1;
  bool ambiguous = false;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (!devices[i].readable && !devices[i].identity_known) continue;
    const int score = fingerprint_match_score(wanted, devices[i].fingerprint);
    if (score > best_score) {
      best_score = score;
      best_index = static_cast<int>(i);
      ambiguous = false;
    } else if (score == best_score && score > 0) {
      ambiguous = true;
    }
  }
  return best_score >= 55 && !ambiguous ? best_index : -1;
}

// Synchronize before consuming any output. A provider may discover its IMU
// only after the first packet; changing calibration discards that old output
// so uncorrected orientation/force cannot be published as calibrated data.
inline void configure_imu_calibrations(InputProvider& provider, const AppConfig& cfg,
                                       const std::vector<DeviceInfo>& devices) {
  std::vector<AccelerometerCalibration> wanted(devices.size());
  std::vector<int> configured_ids(devices.size(), 0);
  for (const auto& entry : cfg.imu_calibrations) {
    if (!entry.accelerometer.enabled) continue;
    const ConfigDevice* configured = nullptr;
    for (const auto& d : cfg.devices) {
      if (d.id == entry.device_id) configured = &d;
    }
    if (!configured) throw std::invalid_argument("IMU calibration references unknown device_id");
    const int index = resolve_imu_config_device(configured->fingerprint, devices);
    if (index < 0) continue;
    if (configured_ids[index] != 0) {
      throw std::invalid_argument("multiple calibrated device_id values resolve to the same IMU");
    }
    configured_ids[index] = entry.device_id;
    wanted[index] = entry.accelerometer;
  }
  constexpr uint32_t required = xr_runtime::CONTROLLER_IMU_CAP_GYROSCOPE |
                                xr_runtime::CONTROLLER_IMU_CAP_ACCELEROMETER;
  for (size_t i = 0; i < devices.size(); ++i) {
    // A button-only controller must never receive accelerometer correction.
    if ((provider.imu_state(devices[i]).capability_flags & required) != required) continue;
    if (provider.set_accelerometer_calibration(devices[i], wanted[i])) {
      const auto& c = wanted[i];
      std::cerr << "[override_controller] accelerometer calibration configured: device_id="
                << configured_ids[i] << " enabled=" << (c.enabled ? "true" : "false")
                << " source=config bias_m_s2=(" << c.bias_m_s2.x << ',' << c.bias_m_s2.y
                << ',' << c.bias_m_s2.z << ") scale=(" << c.scale.x << ',' << c.scale.y
                << ',' << c.scale.z << "); AHRS=reset\n";
    }
  }
}

}  // namespace xr_override_controller::imu
