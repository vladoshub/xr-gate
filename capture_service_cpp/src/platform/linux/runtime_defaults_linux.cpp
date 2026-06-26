#include "capture_service_cpp/platform/runtime_defaults.hpp"

#include <ostream>

namespace xr_capture_cpp {

void apply_platform_runtime_defaults(RuntimeConfig& cfg) {
  cfg.registry_path = env_or("REGISTRY_PATH", cfg.registry_path);
  cfg.namespace_name = env_or("NAMESPACE", cfg.namespace_name);
  cfg.publish_modes = split_publish_modes(env_or("PUBLISH", "shm"));
}

void print_platform_camera_usage(std::ostream& os) {
  os << " [--video-device /dev/video0]";
}

void finalize_platform_runtime_config(RuntimeConfig& cfg) {
  if (cfg.publish_modes.empty()) cfg.publish_modes.push_back("shm");
}

}  // namespace xr_capture_cpp
