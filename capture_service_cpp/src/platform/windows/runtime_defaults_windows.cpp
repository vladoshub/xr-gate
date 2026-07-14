#include "capture_service_cpp/platform/runtime_defaults.hpp"

#include <algorithm>
#include <iostream>
#include <ostream>
#include <vector>

namespace xr_capture_cpp {

void apply_platform_runtime_defaults(RuntimeConfig& cfg) {
  cfg.registry_path = "capture_service_streams.json";
  cfg.namespace_name = "xreal_air2ultra_windows";
  cfg.publish_modes = {"tcp"};
  cfg.camera.primary.index = 0;
  cfg.camera.primary.api = "msmf";
  cfg.camera.primary.raw_format = true;
  cfg.camera.primary.convert_rgb = false;
  cfg.camera.primary.buffer_size = 1;
}

void print_platform_camera_usage(std::ostream& os) {
  os << " [--camera-index N] [--camera-api msmf|dshow|any]";
}

void finalize_platform_runtime_config(RuntimeConfig& cfg) {
  bool has_tcp = std::find(cfg.publish_modes.begin(), cfg.publish_modes.end(), "tcp") != cfg.publish_modes.end();
  std::vector<std::string> filtered;
  for (const auto& mode : cfg.publish_modes) {
    if (mode == "shm") std::cerr << "[capture_service_cpp][WARN] SHM is not supported on native Windows; using TCP only" << std::endl;
    else filtered.push_back(mode);
  }
  if (!has_tcp) filtered.push_back("tcp");
  if (filtered.empty()) filtered.push_back("tcp");
  cfg.publish_modes = filtered;
}

}  // namespace xr_capture_cpp
