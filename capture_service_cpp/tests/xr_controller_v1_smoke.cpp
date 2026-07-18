#include "capture_service_cpp/protocols/xr_controller_v1.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

using namespace xr_capture_cpp;

int main() {
  XrControllerV1Sample input;
  input.flags = kXrControllerV1TimestampValid |
                kXrControllerV1ControlsValid |
                kXrControllerV1BatteryValid;
  input.sequence = 0x12345678u;
  input.device_timestamp_us = 0x0102030405060708ULL;
  input.gyro_rad_s = {0.25f, -1.5f, 2.75f};
  input.accel_m_s2 = {9.81f, -0.125f, 1.0f};
  input.buttons = XrControllerV1ButtonA | XrControllerV1ButtonDpadRight;
  input.axes = {-32768, 32767, 1234, -4321};
  input.battery_mv = 4123;
  input.controller_status = 0x55AA;

  std::array<uint8_t, kXrControllerV1PacketSize> packet{};
  assert(encode_xr_controller_v1(input, packet.data(), packet.size()));
  assert(packet[0] == 'X' && packet[1] == 'C' && packet[2] == 'T' && packet[3] == 'L');
  assert(packet[4] == kXrControllerV1Version);

  XrControllerV1Sample decoded;
  XrControllerV1DecodeError error = XrControllerV1DecodeError::None;
  assert(decode_xr_controller_v1(packet.data(), packet.size(), decoded, &error));
  assert(error == XrControllerV1DecodeError::None);
  assert(decoded.flags == input.flags);
  assert(decoded.sequence == input.sequence);
  assert(decoded.device_timestamp_us == input.device_timestamp_us);
  assert(decoded.buttons == input.buttons);
  assert(decoded.axes == input.axes);
  assert(decoded.battery_mv == input.battery_mv);
  assert(decoded.controller_status == input.controller_status);
  for (size_t i = 0; i < 3; ++i) {
    assert(decoded.gyro_rad_s[i] == input.gyro_rad_s[i]);
    assert(decoded.accel_m_s2[i] == input.accel_m_s2[i]);
  }

  packet[44] ^= 0x01;
  assert(!decode_xr_controller_v1(packet.data(), packet.size(), decoded, &error));
  assert(error == XrControllerV1DecodeError::BadCrc);

  input.gyro_rad_s[0] = std::numeric_limits<float>::quiet_NaN();
  assert(!encode_xr_controller_v1(input, packet.data(), packet.size()));
  return 0;
}
