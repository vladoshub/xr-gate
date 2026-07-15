#include "capture_service_cpp/platform/camera_controls.hpp"

#include <iostream>
#include <stdexcept>

namespace xr_capture_cpp {

void apply_camera_controls(const CameraDeviceConfig& cfg, const std::string& label) {
  if (cfg.controls.values.empty()) return;

  const std::string message =
      "camera controls are configured for " + label +
      ", but the Windows camera-control backend is not implemented yet";
  if (cfg.controls.strict) throw std::runtime_error(message);
  std::cerr << "[capture_service_cpp][WARN] " << message << std::endl;
}

}  // namespace xr_capture_cpp
