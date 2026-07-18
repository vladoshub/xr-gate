#include "capture_service_cpp/sources/serial_imu_protocol.hpp"

namespace xr_capture_cpp {

uint32_t xr_imu_crc32(const uint8_t* data, size_t size) {
  return xr_controller_v1_crc32(data, size);
}

}  // namespace xr_capture_cpp
