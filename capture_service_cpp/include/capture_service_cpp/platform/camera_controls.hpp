#pragma once

#include "capture_service_cpp/common.hpp"

#include <string>

namespace xr_capture_cpp {

// Convert a human-readable/native control label to the canonical YAML key form.
// Example: "Exposure Time, Absolute" -> "exposure_time_absolute".
std::string normalize_camera_control_name(const std::string& value);

// Apply all explicitly configured controls to a camera device. The public
// contract is platform-neutral; the implementation is selected by CMake.
void apply_camera_controls(const CameraDeviceConfig& cfg, const std::string& label);

}  // namespace xr_capture_cpp
