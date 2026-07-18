#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace xr_override_controller::xiao_nrf54l15 {

constexpr std::array<uint8_t, 4> kXrControllerV1Magic{{'X', 'C', 'T', 'L'}};
constexpr uint8_t kXrControllerV1Version = 1;
constexpr size_t kXrControllerV1PacketSize = 64;
constexpr size_t kXrControllerV1CrcOffset = 60;
constexpr size_t kXrControllerV1AxisCount = 4;

constexpr uint8_t kFlagTimestampValid = 1u << 0;
constexpr uint8_t kFlagControlsValid = 1u << 1;
constexpr uint8_t kFlagBatteryValid = 1u << 2;

constexpr uint32_t kButtonA = 1u << 0;
constexpr uint32_t kButtonB = 1u << 1;
constexpr uint32_t kButtonC = 1u << 2;
constexpr uint32_t kButtonTrigger = 1u << 3;
constexpr uint32_t kButtonGrip = 1u << 4;
constexpr uint32_t kButtonMenu = 1u << 5;
constexpr uint32_t kButtonStickClick = 1u << 6;
constexpr uint32_t kButtonDpadUp = 1u << 7;
constexpr uint32_t kButtonDpadDown = 1u << 8;
constexpr uint32_t kButtonDpadLeft = 1u << 9;
constexpr uint32_t kButtonDpadRight = 1u << 10;
constexpr uint32_t kKnownButtonMask = (1u << 11) - 1u;

struct XrControllerV1Packet {
  uint8_t flags = 0;
  uint32_t sequence = 0;
  uint64_t timestamp_us = 0;
  std::array<float, 3> gyro_rad_s{};
  std::array<float, 3> accel_m_s2{};
  uint32_t buttons = 0;
  std::array<int16_t, kXrControllerV1AxisCount> axes{};
  uint16_t battery_mv = 0;
  uint16_t controller_status = 0;
};

uint32_t xr_controller_crc32_ieee(const uint8_t* data, size_t size);
std::optional<XrControllerV1Packet> decode_xr_controller_v1(
    const uint8_t* data, size_t size);

class XrControllerV1StreamDecoder {
 public:
  void append(const uint8_t* data, size_t size);
  std::optional<XrControllerV1Packet> pop();
  void reset();
  size_t buffered_size() const { return buffer_.size(); }

 private:
  std::vector<uint8_t> buffer_;
};

}  // namespace xr_override_controller::xiao_nrf54l15
