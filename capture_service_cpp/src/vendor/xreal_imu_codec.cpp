#include "capture_service_cpp/vendor/xreal_imu_codec.hpp"

#include <cmath>

namespace xr_capture_cpp {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

int32_t s24le(const uint8_t* p) {
  int32_t v = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
  if (v & 0x800000) v -= 0x1000000;
  return v;
}

}  // namespace

const std::array<uint8_t, 10>& xreal_imu_start_command() {
  static const std::array<uint8_t, 10> kStartCmd = {0x00, 0xaa, 0xc5, 0xd1, 0x21, 0x42, 0x04, 0x00, 0x19, 0x01};
  return kStartCmd;
}

bool normalize_xreal_imu_packet(const uint8_t* packet, size_t size, float out_gyro_accel[6]) {
  if (!packet || size != kXrealHidPacketSize || !out_gyro_accel) return false;

  const int32_t gx_raw = s24le(packet + 18);
  const int32_t gy_raw = s24le(packet + 21);
  const int32_t gz_raw = s24le(packet + 24);
  const int32_t ax_raw = s24le(packet + 33);
  const int32_t ay_raw = s24le(packet + 36);
  const int32_t az_raw = s24le(packet + 39);

  const double gyro_scalar_dps = 1.0 / 8388608.0 * 2000.0;
  const double accel_scalar_g = 1.0 / 8388608.0 * 16.0;
  const double gravity = 9.80665;

  out_gyro_accel[0] = static_cast<float>((gx_raw * gyro_scalar_dps) * kPi / 180.0);
  out_gyro_accel[1] = static_cast<float>((gy_raw * gyro_scalar_dps) * kPi / 180.0);
  out_gyro_accel[2] = static_cast<float>((gz_raw * gyro_scalar_dps) * kPi / 180.0);
  out_gyro_accel[3] = static_cast<float>(ax_raw * accel_scalar_g * gravity);
  out_gyro_accel[4] = static_cast<float>(ay_raw * accel_scalar_g * gravity);
  out_gyro_accel[5] = static_cast<float>(az_raw * accel_scalar_g * gravity);
  return true;
}

}  // namespace xr_capture_cpp
