#pragma once

#include "capture_service_cpp/common.hpp"

#include <cstdint>

namespace xr_capture_cpp {

StreamSpec make_xreal_camera0_stream(uint32_t slot_count);
StreamSpec make_xreal_camera1_stream(uint32_t slot_count);
StreamSpec make_xreal_raw_hid_stream(uint32_t slot_count);
StreamSpec make_xreal_imu_stream(uint32_t slot_count);

}  // namespace xr_capture_cpp
