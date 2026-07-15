#include "capture_service_cpp/common.hpp"

#include <atomic>
#include <filesystem>
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

int main(int argc, char** argv) {
  if (argc != 2) throw std::runtime_error("usage: capture_config_canonical_smoke CONFIG");
  std::vector<std::string> args_storage{"capture_config_canonical_smoke", "--config", argv[1]};
  std::vector<char*> args;
  for (auto& value : args_storage) args.push_back(value.data());

  const xr_capture_cpp::RuntimeConfig cfg =
      xr_capture_cpp::parse_args(static_cast<int>(args.size()), args.data());

  require(cfg.profile_name == "xreal_air2ultra_unified_480", "capture profile");
  require(cfg.camera.driver == "xreal_ultra", "camera driver");
  require(cfg.camera.primary.device_path == "/dev/video0", "camera device");
  require(cfg.camera.primary.width == 640 && cfg.camera.primary.height == 241 &&
              cfg.camera.primary.fps == 60,
          "camera capture properties");
  require(cfg.camera.output_width == 480 && cfg.camera.output_height == 640,
          "camera output dimensions");
  require(cfg.camera.slot_count == 8, "camera slot count");
  require(cfg.camera.left_transform.rotate == "ccw90" &&
              cfg.camera.right_transform.rotate == "ccw90" &&
              cfg.camera.right_transform.flip == "xy",
          "camera transforms");
  require(cfg.imu.driver == "xreal_hid", "imu driver");
  require(cfg.imu.slot_count == 2048 && cfg.imu.raw_slot_count == 2048,
          "imu slot counts");
  require(cfg.imu.xreal_hid.read_timeout_ms == 100, "imu timeout");
  require(cfg.imu.xreal_hid.drop_first_packets == 500, "imu drop count");
  std::cout << "canonical config smoke test passed\n";
  return 0;
}
