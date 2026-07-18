#include "xr_controller_v1_protocol.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace xr_override_controller::xiao_nrf54l15;

int main() {
  const std::array<uint8_t, kXrControllerV1PacketSize> canonical{{
      0x58,0x43,0x54,0x4c,0x01,0x07,0x40,0x00,0x78,0x56,0x34,0x12,0x08,0x07,0x06,0x05,
      0x04,0x03,0x02,0x01,0x00,0x00,0xa0,0x3f,0x00,0x00,0x20,0xc0,0x00,0x00,0x70,0x40,
      0x0a,0xe8,0x1c,0x41,0x00,0x00,0x00,0x00,0x0a,0xe8,0x1c,0xc1,0x09,0x04,0x00,0x00,
      0x00,0x80,0x2e,0xfb,0x29,0x09,0xff,0x7f,0x1b,0x10,0xaa,0x55,0x82,0xb8,0x78,0x7e,
  }};

  const auto decoded = decode_xr_controller_v1(canonical.data(), canonical.size());
  assert(decoded);
  assert(decoded->flags == 0x07);
  assert(decoded->sequence == 0x12345678u);
  assert(decoded->timestamp_us == 0x0102030405060708ull);
  assert(std::fabs(decoded->gyro_rad_s[0] - 1.25f) < 1e-6f);
  assert(std::fabs(decoded->gyro_rad_s[1] + 2.5f) < 1e-6f);
  assert(std::fabs(decoded->gyro_rad_s[2] - 3.75f) < 1e-6f);
  assert(decoded->buttons == (kButtonA | kButtonTrigger | kButtonDpadRight));
  assert(decoded->axes[0] == -32768);
  assert(decoded->axes[1] == -1234);
  assert(decoded->axes[2] == 2345);
  assert(decoded->axes[3] == 32767);
  assert(decoded->battery_mv == 4123);
  assert(decoded->controller_status == 0x55aa);

  auto corrupted = canonical;
  corrupted[24] ^= 0x80;
  assert(!decode_xr_controller_v1(corrupted.data(), corrupted.size()));

  XrControllerV1StreamDecoder stream;
  const std::array<uint8_t, 5> noise{{0xde, 0xad, 0x58, 0x43, 0x00}};
  stream.append(noise.data(), noise.size());
  stream.append(canonical.data(), 19);
  assert(!stream.pop());
  stream.append(canonical.data() + 19, canonical.size() - 19);
  const auto resynchronized = stream.pop();
  assert(resynchronized);
  assert(resynchronized->sequence == decoded->sequence);
  assert(!stream.pop());

  return 0;
}
