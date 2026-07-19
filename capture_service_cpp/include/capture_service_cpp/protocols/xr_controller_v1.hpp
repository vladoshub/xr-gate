#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace xr_capture_cpp {

// Fixed-size controller packet. The first 44 bytes retain the sequence,
// acquisition timestamp, gyroscope and accelerometer layout used by the
// original IMU-only transport. Control fields are reserved in v1 so firmware
// can add buttons and axes later without changing capture_service_cpp framing.
constexpr std::array<uint8_t, 4> kXrControllerV1Magic{{'X', 'C', 'T', 'L'}};
constexpr size_t kXrControllerV1PacketSize = 64;
constexpr size_t kXrControllerV1CrcOffset = 60;
constexpr size_t kXrControllerV1AxisCount = 4;
constexpr uint8_t kXrControllerV1Version = 1;
constexpr uint8_t kXrControllerV1TimestampValid = 0x01;
constexpr uint8_t kXrControllerV1ControlsValid = 0x02;
constexpr uint8_t kXrControllerV1BatteryValid = 0x04;

constexpr std::array<uint8_t, 4> kXrControllerIdentityV1Magic{{'X', 'C', 'I', 'D'}};
constexpr size_t kXrControllerIdentityV1PacketSize = 32;
constexpr size_t kXrControllerIdentityV1CrcOffset = 28;
constexpr size_t kXrControllerDeviceUidMaxSize = 16;
constexpr uint8_t kXrControllerIdentityV1Version = 1;
constexpr uint8_t kXrControllerIdentityV1DeviceUidValid = 0x01;

enum XrControllerV1Button : uint32_t {
  XrControllerV1ButtonA = 1u << 0,
  XrControllerV1ButtonB = 1u << 1,
  XrControllerV1ButtonC = 1u << 2,
  XrControllerV1ButtonTrigger = 1u << 3,
  XrControllerV1ButtonGrip = 1u << 4,
  XrControllerV1ButtonMenu = 1u << 5,
  XrControllerV1ButtonStickClick = 1u << 6,
  XrControllerV1ButtonDpadUp = 1u << 7,
  XrControllerV1ButtonDpadDown = 1u << 8,
  XrControllerV1ButtonDpadLeft = 1u << 9,
  XrControllerV1ButtonDpadRight = 1u << 10,
};

enum XrControllerV1Axis : size_t {
  XrControllerV1AxisThumbstickX = 0,
  XrControllerV1AxisThumbstickY = 1,
  XrControllerV1AxisTrigger = 2,
  XrControllerV1AxisGrip = 3,
};

struct XrControllerIdentityV1 {
  uint8_t flags = 0;
  uint8_t controller_protocol_version = 0;
  std::array<uint8_t, kXrControllerDeviceUidMaxSize> device_uid{};
  size_t device_uid_size = 0;
};

struct XrControllerV1Sample {
  uint8_t flags = 0;
  uint32_t sequence = 0;
  uint64_t device_timestamp_us = 0;
  std::array<float, 3> gyro_rad_s{};
  std::array<float, 3> accel_m_s2{};
  uint32_t buttons = 0;
  std::array<int16_t, kXrControllerV1AxisCount> axes{};
  uint16_t battery_mv = 0;
  uint16_t controller_status = 0;
};

enum class XrControllerV1DecodeError {
  None,
  TooShort,
  BadMagic,
  UnsupportedVersion,
  BadPacketSize,
  BadCrc,
  NonFiniteSample,
};

uint32_t xr_controller_v1_crc32(const uint8_t* data, size_t size);

bool encode_xr_controller_identity_v1(const XrControllerIdentityV1& identity,
                                      uint8_t* output,
                                      size_t output_size);

bool decode_xr_controller_identity_v1(const uint8_t* data,
                                      size_t size,
                                      XrControllerIdentityV1& identity);

std::string xr_controller_device_uid_hex(const XrControllerIdentityV1& identity);
std::string normalize_xr_controller_device_uid(std::string value);

bool encode_xr_controller_v1(const XrControllerV1Sample& sample,
                             uint8_t* output,
                             size_t output_size);

bool decode_xr_controller_v1(const uint8_t* data,
                             size_t size,
                             XrControllerV1Sample& sample,
                             XrControllerV1DecodeError* error = nullptr);

const char* xr_controller_v1_decode_error_name(XrControllerV1DecodeError error);

}  // namespace xr_capture_cpp
