#include "capture_service_cpp/protocols/xr_imu_v1.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace xr_capture_cpp;

int main() {
  XrImuV1Sample input;
  input.flags = kXrImuV1TimestampValid;
  input.sequence = 0x12345678u;
  input.device_timestamp_us = 0x0102030405060708ULL;
  input.gyro_rad_s = {0.25f, -1.5f, 2.75f};
  input.accel_m_s2 = {9.81f, -0.125f, 1.0f};

  std::array<uint8_t, kXrImuV1PacketSize> packet{};
  assert(encode_xr_imu_v1(input, packet.data(), packet.size()));
  assert(packet[0] == 'X' && packet[1] == 'I' && packet[2] == 'M' && packet[3] == 'U');
  assert(packet[4] == kXrImuV1Version);

  XrImuV1Sample decoded;
  XrImuV1DecodeError error = XrImuV1DecodeError::None;
  assert(decode_xr_imu_v1(packet.data(), packet.size(), decoded, &error));
  assert(error == XrImuV1DecodeError::None);
  assert(decoded.flags == input.flags);
  assert(decoded.sequence == input.sequence);
  assert(decoded.device_timestamp_us == input.device_timestamp_us);
  for (size_t i = 0; i < 3; ++i) {
    assert(decoded.gyro_rad_s[i] == input.gyro_rad_s[i]);
    assert(decoded.accel_m_s2[i] == input.accel_m_s2[i]);
  }

  packet[20] ^= 0x01;
  assert(!decode_xr_imu_v1(packet.data(), packet.size(), decoded, &error));
  assert(error == XrImuV1DecodeError::BadCrc);

  input.gyro_rad_s[0] = std::numeric_limits<float>::quiet_NaN();
  assert(!encode_xr_imu_v1(input, packet.data(), packet.size()));
  return 0;
}
