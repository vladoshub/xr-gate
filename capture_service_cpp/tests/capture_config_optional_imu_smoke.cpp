#include "capture_service_cpp/config/capture_config.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{kExitOk};
}

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("failed to create temporary config: " + path.string());
  out << text;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path camera_only = fs::temp_directory_path() /
      "capture_service_cpp_optional_imu_camera_only.yaml";
  const fs::path synthetic = fs::temp_directory_path() /
      "capture_service_cpp_optional_imu_synthetic.yaml";

  try {
    write_text(camera_only, R"YAML(version: 1
service:
  publish: [shm]
camera:
  enabled: true
  driver: opencv
  layout: side_by_side_horizontal
  output:
    width: 320
    height: 240
)YAML");

    xr_capture_cpp::RuntimeConfig camera_cfg;
    require(xr_capture_cpp::load_runtime_config_file(camera_only.string(), camera_cfg),
            "camera-only config was not loaded");
    xr_capture_cpp::validate_runtime_config(camera_cfg);
    require(!camera_cfg.imu.enabled,
            "omitting the imu mapping from an explicit config must disable IMU");

    write_text(synthetic, R"YAML(version: 1
service:
  publish: [shm]
camera:
  enabled: false
imu:
  enabled: true
  driver: synthetic
  output:
    stream: imu_synthetic
    frame: imu_synthetic
  synthetic:
    rate_hz: 200
    gyro_rad_s: [0.1, -0.2, 0.3]
    accel_m_s2: [0.0, 0.0, 0.0]
    timestamp_mode: host_monotonic
)YAML");

    xr_capture_cpp::RuntimeConfig synthetic_cfg;
    require(xr_capture_cpp::load_runtime_config_file(synthetic.string(), synthetic_cfg),
            "synthetic config was not loaded");
    xr_capture_cpp::validate_runtime_config(synthetic_cfg);
    require(synthetic_cfg.imu.enabled, "synthetic IMU must be enabled");
    require(synthetic_cfg.imu.driver == "synthetic", "synthetic IMU driver was not selected");
    require(!synthetic_cfg.imu.raw_enabled, "synthetic IMU must not enable a raw stream by default");
    require(synthetic_cfg.imu.stream_id == "imu_synthetic", "synthetic stream id");
    require(synthetic_cfg.imu.synthetic.rate_hz == 200.0, "synthetic rate");
    require(synthetic_cfg.imu.synthetic.gyro_rad_s[0] == 0.1f &&
                synthetic_cfg.imu.synthetic.gyro_rad_s[1] == -0.2f &&
                synthetic_cfg.imu.synthetic.gyro_rad_s[2] == 0.3f,
            "synthetic gyro vector");
  } catch (...) {
    std::error_code ignored;
    fs::remove(camera_only, ignored);
    fs::remove(synthetic, ignored);
    throw;
  }

  std::error_code ignored;
  fs::remove(camera_only, ignored);
  fs::remove(synthetic, ignored);
  std::cout << "optional/synthetic IMU config smoke test passed\n";
  return 0;
}
