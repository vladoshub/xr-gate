#pragma once

#include "capture_service_cpp/common.hpp"

#include <iosfwd>

namespace xr_capture_cpp {

void apply_platform_runtime_defaults(RuntimeConfig& cfg);
void print_platform_camera_usage(std::ostream& os);
void finalize_platform_runtime_config(RuntimeConfig& cfg);

}  // namespace xr_capture_cpp
