#include "capture_service_cpp/common.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{kExitOk};
}

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "capture_service_cpp_camera_controls.yaml";
  {
    std::ofstream out(path);
    out << R"YAML(version: 1
service:
  publish: [shm]
camera:
  enabled: true
  driver: opencv
  layout: interleaved_columns
  primary:
    device:
      linux: /dev/video9
      windows: 4
    controls_policy: best_effort
    controls:
      brightness: 10
      gain: 32
      exposure_time_absolute: 5000
      focus_automatic_continuous: false
      linux:
        brightness: 12
      windows:
        brightness: 14
    width: 640
    height: 480
  output:
    width: 640
    height: 480
imu:
  enabled: false
)YAML";
  }

  std::vector<std::string> args_storage{
      "camera_controls_config_smoke", "--config", path.string()};
  std::vector<char*> args;
  for (auto& value : args_storage) args.push_back(value.data());

  const xr_capture_cpp::RuntimeConfig cfg =
      xr_capture_cpp::parse_args(static_cast<int>(args.size()), args.data());

  require(!cfg.camera.primary.controls.strict, "controls policy");
  require(cfg.camera.primary.controls.values.at("gain") == 32, "gain control");
  require(cfg.camera.primary.controls.values.at("exposure_time_absolute") == 5000,
          "exposure control");
  require(cfg.camera.primary.controls.values.at("focus_automatic_continuous") == 0,
          "boolean control");
#ifdef _WIN32
  require(cfg.camera.primary.controls.values.at("brightness") == 14,
          "Windows override");
#else
  require(cfg.camera.primary.controls.values.at("brightness") == 12,
          "Linux override");
#endif
  require(cfg.camera.secondary.controls.values.empty(), "secondary controls must be empty");

  std::filesystem::remove(path);
  std::cout << "camera controls config smoke test passed\n";
  return 0;
}
