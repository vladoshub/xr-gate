#include "gearvr_protocol.hpp"

namespace xr_override_controller::gearvr {
namespace {

constexpr float kGravityMps2 = 9.80665f;
constexpr float kAccelLsbPerG = 2048.0f;
constexpr float kGyroLsbPerDegS = 14.285f;
constexpr float kMagUtPerLsb = 0.06f;
constexpr float kPi = 3.14159265358979323846f;

int16_t signed_be16(const uint8_t* data, size_t offset) {
  const uint16_t value = (static_cast<uint16_t>(data[offset]) << 8) |
                         static_cast<uint16_t>(data[offset + 1]);
  return static_cast<int16_t>(value);
}

}  // namespace

std::optional<DecodedPacket> decode_packet(const uint8_t* data, size_t size) {
  if (!data || size < 59) return std::nullopt;

  DecodedPacket packet;
  packet.touch_x = (((data[54] & 0x0f) << 6) | ((data[55] & 0xfc) >> 2)) & 0x3ff;
  packet.touch_y = (((data[55] & 0x03) << 8) | data[56]) & 0x3ff;
  packet.buttons = data[58];
  packet.accel_m_s2 = {
      signed_be16(data, 4) * kGravityMps2 / kAccelLsbPerG,
      signed_be16(data, 6) * kGravityMps2 / kAccelLsbPerG,
      signed_be16(data, 8) * kGravityMps2 / kAccelLsbPerG,
  };
  packet.gyro_rad_s = {
      signed_be16(data, 10) / kGyroLsbPerDegS * kPi / 180.0f,
      signed_be16(data, 12) / kGyroLsbPerDegS * kPi / 180.0f,
      signed_be16(data, 14) / kGyroLsbPerDegS * kPi / 180.0f,
  };
  packet.magnetic_uT = {
      signed_be16(data, 32) * kMagUtPerLsb,
      signed_be16(data, 34) * kMagUtPerLsb,
      signed_be16(data, 36) * kMagUtPerLsb,
  };
  return packet;
}

const std::array<Command, 8>& initialization_commands() {
  // Preserve the working 0013/0014 sequence: sensor x3, low-power enable x1,
  // low-power disable x1, VR mode x3.
  static const std::array<Command, 8> commands{
      kCommandSensor, kCommandSensor, kCommandSensor,
      kCommandLpmEnable, kCommandLpmDisable,
      kCommandVrMode, kCommandVrMode, kCommandVrMode,
  };
  return commands;
}

}  // namespace xr_override_controller::gearvr
