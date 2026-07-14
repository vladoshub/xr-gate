#pragma once

#include <cstddef>
#include <cstdint>

namespace xr_capture_cpp {

// xr_imu_v1 is a fixed-size little-endian packet intended for USB CDC/UART:
//   0   char[4]  magic = "XIMU"
//   4   uint8    version = 1
//   5   uint8    flags; bit 0 = device timestamp valid
//   6   uint16   packet_size = 48
//   8   uint32   sequence
//   12  uint64   device_timestamp_us
//   20  float32  gyro_xyz_rad_s[3]
//   32  float32  accel_xyz_m_s2[3]
//   44  uint32   IEEE CRC32 over bytes [0, 44)
constexpr size_t kXrImuV1PacketSize = 48;
constexpr uint8_t kXrImuV1Version = 1;
constexpr uint8_t kXrImuV1TimestampValid = 0x01;

uint32_t xr_imu_crc32(const uint8_t* data, size_t size);

}  // namespace xr_capture_cpp
