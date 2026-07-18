#pragma once

#include "capture_service_cpp/protocols/xr_controller_v1.hpp"

namespace xr_capture_cpp {

// Compatibility entry point retained for code written against the first
// serial-source revision. New code should use the protocol-specific CRC helper.
uint32_t xr_imu_crc32(const uint8_t* data, size_t size);

}  // namespace xr_capture_cpp
