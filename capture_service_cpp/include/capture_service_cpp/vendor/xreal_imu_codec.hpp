#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace xr_capture_cpp {

constexpr uint16_t kXrealHidVendorId = 0x3318;
constexpr uint16_t kXrealHidProductId = 0x0426;
constexpr int kXrealImuInterfaceNumber = 2;
constexpr size_t kXrealHidPacketSize = 64;
constexpr size_t kXrealHidMaxPacketSize = 256;
constexpr int kXrealDefaultImuDropFirstPackets = 500;

const std::array<uint8_t, 10>& xreal_imu_start_command();
bool decode_xreal_imu_timestamp(const uint8_t* packet, size_t size, uint64_t& timestamp_ns);
bool normalize_xreal_imu_packet(const uint8_t* packet,
                                size_t size,
                                double gravity_magnitude_mps2,
                                float out_gyro_accel[6]);

}  // namespace xr_capture_cpp
