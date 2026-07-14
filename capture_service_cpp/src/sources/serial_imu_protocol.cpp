#include "capture_service_cpp/sources/serial_imu_protocol.hpp"

namespace xr_capture_cpp {

uint32_t xr_imu_crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u))));
    }
  }
  return ~crc;
}

}  // namespace xr_capture_cpp
