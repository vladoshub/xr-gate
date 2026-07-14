#include "capture_service_cpp/platform/runtime_defaults.hpp"

#include <ostream>

namespace xr_capture_cpp {

void apply_platform_runtime_defaults(RuntimeConfig& cfg) {
  cfg.registry_path = "/tmp/capture_service_streams.json";
  cfg.namespace_name = "xreal_air2ultra_linux";
  cfg.publish_modes = {"shm"};
  cfg.camera.primary.device_path = "/dev/video0";
  cfg.camera.primary.api = "v4l2";
  cfg.camera.primary.raw_format = true;
  cfg.camera.primary.convert_rgb = false;
  cfg.camera.primary.buffer_size = 1;
}

void print_platform_camera_usage(std::ostream& os) {
  os << " [--video-device /dev/video0] [--camera-api v4l2|any]";
}

void finalize_platform_runtime_config(RuntimeConfig& cfg) {
  if (cfg.publish_modes.empty()) cfg.publish_modes.push_back("shm");
}

}  // namespace xr_capture_cpp
