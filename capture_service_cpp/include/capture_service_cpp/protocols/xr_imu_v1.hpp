#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace xr_capture_cpp {

// Hardware-independent wire contract for raw 6DoF IMU samples transported
// over a byte stream (USB CDC, UART, TCP, test pipes, and similar links).
// All integer and float fields are encoded explicitly as little-endian bytes.
// No compiler-specific struct layout is used on the wire.
constexpr std::array<uint8_t, 4> kXrImuV1Magic{{'X', 'I', 'M', 'U'}};
constexpr size_t kXrImuV1PacketSize = 48;
constexpr size_t kXrImuV1CrcOffset = 44;
constexpr uint8_t kXrImuV1Version = 1;
constexpr uint8_t kXrImuV1TimestampValid = 0x01;

struct XrImuV1Sample {
  uint8_t flags = 0;
  uint32_t sequence = 0;
  uint64_t device_timestamp_us = 0;
  std::array<float, 3> gyro_rad_s{};
  std::array<float, 3> accel_m_s2{};
};

enum class XrImuV1DecodeError {
  None,
  TooShort,
  BadMagic,
  UnsupportedVersion,
  BadPacketSize,
  BadCrc,
  NonFiniteSample,
};

uint32_t xr_imu_v1_crc32(const uint8_t* data, size_t size);

// Encodes exactly kXrImuV1PacketSize bytes. Returns false only when output is
// null/too small or the sample contains NaN/Inf.
bool encode_xr_imu_v1(const XrImuV1Sample& sample, uint8_t* output, size_t output_size);

// Decodes one complete packet. Additional bytes after the first packet are
// ignored, allowing callers to decode directly from a stream buffer.
bool decode_xr_imu_v1(const uint8_t* data,
                      size_t size,
                      XrImuV1Sample& sample,
                      XrImuV1DecodeError* error = nullptr);

const char* xr_imu_v1_decode_error_name(XrImuV1DecodeError error);

}  // namespace xr_capture_cpp
