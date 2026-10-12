#pragma once

#include <xr_override_controller/imu/controller_imu_processor.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace xr_override_controller::imu {

inline std::filesystem::path controller_imu_calibration_path() {
  const char* explicit_path = std::getenv("OVERRIDE_CONTROLLER_IMU_CALIBRATION_FILE");
  if (explicit_path && *explicit_path) return explicit_path;
  const char* config_home = std::getenv("XDG_CONFIG_HOME");
  if (config_home && *config_home) {
    return std::filesystem::path(config_home) / "xr_tracking/override_controller/controller_imu_calibration.json";
  }
  const char* user_home = std::getenv("HOME");
  if (user_home && *user_home) {
    return std::filesystem::path(user_home) / ".config/xr_tracking/override_controller/controller_imu_calibration.json";
  }
  return {};
}

// Load once when a physical session is created, not in the sensor loop. Keys
// are provider stable IDs, so changing left/right bindings cannot swap sensors'
// measured bias. A missing optional file/device retains the uncalibrated path.
inline AccelerometerCalibration load_accelerometer_calibration(const std::string& device_id) {
  const auto path = controller_imu_calibration_path();
  if (path.empty() || path == "none") return {};
  std::ifstream input(path);
  if (!input) return {};
  try {
    nlohmann::json document;
    input >> document;
    if (document.at("schema_version").get<int>() != 1 || !document.at("devices").is_object()) {
      throw std::invalid_argument("expected schema_version=1 and devices object");
    }
    const auto& devices = document.at("devices");
    const auto entry = devices.find(device_id);
    if (entry == devices.end()) return {};
    const auto read_vector = [](const nlohmann::json& value) {
      if (!value.is_array() || value.size() != 3) throw std::invalid_argument("expected three sensor-axis values");
      return Vec3f{value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
    };
    AccelerometerCalibration calibration;
    calibration.enabled = true;
    calibration.bias_m_s2 = read_vector(entry->at("accelerometer_bias_m_s2"));
    calibration.scale = read_vector(entry->at("accelerometer_scale"));
    ControllerImuProcessor validation;
    validation.set_accelerometer_calibration(calibration);
    std::cerr << "[override_controller] accelerometer calibration loaded for " << device_id
              << " file=" << path << " bias_m_s2=(" << calibration.bias_m_s2.x << ','
              << calibration.bias_m_s2.y << ',' << calibration.bias_m_s2.z << ") scale=("
              << calibration.scale.x << ',' << calibration.scale.y << ',' << calibration.scale.z << ")\n";
    return calibration;
  } catch (const std::exception& error) {
    std::cerr << "[override_controller][WARN] ignoring accelerometer calibration for "
              << device_id << " file=" << path << ": " << error.what() << '\n';
    return {};
  }
}

}  // namespace xr_override_controller::imu
