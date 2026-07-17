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

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const fs::path config_path = fs::temp_directory_path() / "capture_service_cpp_legacy_config_smoke.yaml";
  {
    std::ofstream out(config_path);
    if (!out) throw std::runtime_error("failed to create temporary config");
    out << R"YAML(capture_service:
  service:
    namespace: legacy_namespace
    registry_path: /tmp/legacy_registry.json
    publish:
      - shm
    tcp_enabled: false
    tcp_bind_host: 127.0.0.1
    tcp_port: 45661
    tcp_client_queue_size: 123
  rig:
    notes:
      - Data path uses capture_service_cpp in-process XREAL UVC decoding and publishes
        true 480x640 portrait camera0/camera1 streams.
  xreal_linux:
    camera:
      enabled: true
      backend: capture_service_cpp
      device: /dev/video9
      raw_width: 640
      raw_height: 241
      raw_fps_num: 60
      left_stream_id: legacy_camera0
      right_stream_id: legacy_camera1
      post_rotate_left: ccw90
      post_rotate_right: ccw90
    imu:
      enabled: true
      backend: hidapi
      vendor_id: '0x3318'
      product_id: '0x0426'
      interface_number: 2
      read_size: 64
      drop_first_packets: 321
      publish_raw_hid: true
      raw_stream_id: legacy_raw
      imu_stream_id: legacy_imu
)YAML";
  }

  std::vector<std::string> args_storage{
      "capture_config_legacy_smoke", "--config", config_path.string()};
  std::vector<char*> args;
  args.reserve(args_storage.size());
  for (auto& value : args_storage) args.push_back(value.data());

  try {
    const xr_capture_cpp::RuntimeConfig cfg =
        xr_capture_cpp::parse_args(static_cast<int>(args.size()), args.data());

    require(cfg.namespace_name == "legacy_namespace", "legacy service.namespace was not loaded");
    require(cfg.registry_path == "/tmp/legacy_registry.json", "legacy registry_path was not loaded");
    require(cfg.tcp_bind_host == "127.0.0.1" && cfg.tcp_port == 45661 &&
                cfg.tcp_client_queue_size == 123,
            "legacy TCP settings were not loaded");
    require(cfg.camera.driver == "xreal_ultra" && cfg.camera.layout == "xreal_packed",
            "legacy XREAL camera was not selected");
    require(cfg.camera.primary.device_path == "/dev/video9", "legacy camera device was not loaded");
    require(cfg.camera.primary.width == 640 && cfg.camera.primary.height == 241 &&
                cfg.camera.primary.fps == 60,
            "legacy camera capture properties were not loaded");
    require(cfg.camera.left_stream_id == "legacy_camera0" &&
                cfg.camera.right_stream_id == "legacy_camera1",
            "legacy camera stream ids were not loaded");
    require(cfg.camera.right_transform.flip == "xy",
            "built-in XREAL right-eye compatibility transform changed");
    require(cfg.imu.driver == "xreal_hid", "legacy XREAL IMU was not selected");
    require(cfg.imu.transform.mode == xr_capture_cpp::ImuTransformMode::Identity,
            "legacy XREAL IMU transform must remain identity");
    require(cfg.imu.xreal_hid.vendor_id == 0x3318 && cfg.imu.xreal_hid.product_id == 0x0426 &&
                cfg.imu.xreal_hid.interface_number == 2,
            "legacy XREAL HID identity was not loaded");
    require(cfg.imu.xreal_hid.drop_first_packets == 321,
            "legacy XREAL drop_first_packets was not loaded");
    require(cfg.imu.stream_id == "legacy_imu" && cfg.imu.raw_stream_id == "legacy_raw" &&
                cfg.imu.raw_payload_size == 64,
            "legacy IMU streams were not loaded");
  } catch (...) {
    std::error_code ignored;
    fs::remove(config_path, ignored);
    throw;
  }

  std::error_code ignored;
  fs::remove(config_path, ignored);
  std::cout << "legacy config smoke test passed\n";
  return 0;
}
