#include "gearvr_protocol.hpp"

#include <xr_override_controller/backend_control.hpp>

namespace xr_override_controller::gearvr {
namespace {

constexpr float kAccelLsbPerG = 2048.0f;
constexpr float kGyroLsbPerDegS = 14.285f;
constexpr float kMagUtPerLsb = 0.06f;
constexpr float kPi = 3.14159265358979323846f;

uint16_t unsigned_le16(const uint8_t* data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) |
         (static_cast<uint16_t>(data[offset + 1]) << 8);
}

int16_t signed_le16(const uint8_t* data, size_t offset) {
  return static_cast<int16_t>(unsigned_le16(data, offset));
}

uint32_t unsigned_le32(const uint8_t* data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) |
         (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

DecodedImuSample decode_imu_sample(const uint8_t* data, size_t base, float gravity_magnitude) {
  DecodedImuSample sample;
  sample.device_timestamp_us = unsigned_le32(data, base);
  sample.accel_m_s2 = {
      signed_le16(data, base + 4) * gravity_magnitude / kAccelLsbPerG,
      signed_le16(data, base + 6) * gravity_magnitude / kAccelLsbPerG,
      signed_le16(data, base + 8) * gravity_magnitude / kAccelLsbPerG,
  };
  sample.gyro_rad_s = {
      signed_le16(data, base + 10) / kGyroLsbPerDegS * kPi / 180.0f,
      signed_le16(data, base + 12) / kGyroLsbPerDegS * kPi / 180.0f,
      signed_le16(data, base + 14) / kGyroLsbPerDegS * kPi / 180.0f,
  };
  return sample;
}

}  // namespace

std::optional<DecodedPacket> decode_packet(const uint8_t* data, size_t size) {
  if (!data || size < 59) return std::nullopt;

  DecodedPacket packet;
  packet.touch_x = (((data[54] & 0x0f) << 6) | ((data[55] & 0xfc) >> 2)) & 0x3ff;
  packet.touch_y = (((data[55] & 0x03) << 8) | data[56]) & 0x3ff;
  packet.buttons = data[58];

  // Wire layout: two 16-byte timestamp/accel/gyro records, then the
  // magnetometer at byte 32. The previous decoder treated bytes 32..47 as a
  // third IMU record and read the magnetometer from 48..53, corrupting AHRS.
  // All multi-byte sensor values are little-endian.
  const float gravity_magnitude = current_backend_control_snapshot().gravity_magnitude;
  packet.imu_samples[0] = decode_imu_sample(data, 0, gravity_magnitude);
  packet.imu_samples[1] = decode_imu_sample(data, 16, gravity_magnitude);
  packet.magnetic_uT = {
      signed_le16(data, 32) * kMagUtPerLsb,
      signed_le16(data, 34) * kMagUtPerLsb,
      signed_le16(data, 36) * kMagUtPerLsb,
  };
  return packet;
}

const std::array<Command, 1>& initialization_commands() {
  // Linux transport primes VR mode before notification subscription. Once the
  // CCC descriptor is active, CMD_SENSOR starts the continuous 60-byte stream.
  // Sending CMD_VR_MODE again after CMD_SENSOR stopped the tested controllers
  // after one frame, so SENSOR must be the final initialization write.
  static const std::array<Command, 1> commands{kCommandSensor};
  return commands;
}

}  // namespace xr_override_controller::gearvr
