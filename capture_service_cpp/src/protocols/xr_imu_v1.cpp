#include "capture_service_cpp/protocols/xr_imu_v1.hpp"

#include <cmath>
#include <cstring>

namespace xr_capture_cpp {
namespace {

uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (8 * i);
  return value;
}

float read_f32_le(const uint8_t* p) {
  const uint32_t bits = read_u32_le(p);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(bits), "float32 is required");
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void write_u16_le(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void write_u32_le(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void write_u64_le(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void write_f32_le(uint8_t* p, float value) {
  uint32_t bits = 0;
  static_assert(sizeof(value) == sizeof(bits), "float32 is required");
  std::memcpy(&bits, &value, sizeof(bits));
  write_u32_le(p, bits);
}

bool finite_sample(const XrImuV1Sample& sample) {
  for (float value : sample.gyro_rad_s) if (!std::isfinite(value)) return false;
  for (float value : sample.accel_m_s2) if (!std::isfinite(value)) return false;
  return true;
}

void set_error(XrImuV1DecodeError* output, XrImuV1DecodeError value) {
  if (output) *output = value;
}

}  // namespace

uint32_t xr_imu_v1_crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^
            (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u))));
    }
  }
  return ~crc;
}

bool encode_xr_imu_v1(const XrImuV1Sample& sample, uint8_t* output, size_t output_size) {
  if (!output || output_size < kXrImuV1PacketSize || !finite_sample(sample)) return false;

  std::memset(output, 0, kXrImuV1PacketSize);
  std::memcpy(output, kXrImuV1Magic.data(), kXrImuV1Magic.size());
  output[4] = kXrImuV1Version;
  output[5] = sample.flags;
  write_u16_le(output + 6, static_cast<uint16_t>(kXrImuV1PacketSize));
  write_u32_le(output + 8, sample.sequence);
  write_u64_le(output + 12, sample.device_timestamp_us);
  for (int i = 0; i < 3; ++i) {
    write_f32_le(output + 20 + i * 4, sample.gyro_rad_s[static_cast<size_t>(i)]);
    write_f32_le(output + 32 + i * 4, sample.accel_m_s2[static_cast<size_t>(i)]);
  }
  write_u32_le(output + kXrImuV1CrcOffset,
               xr_imu_v1_crc32(output, kXrImuV1CrcOffset));
  return true;
}

bool decode_xr_imu_v1(const uint8_t* data,
                      size_t size,
                      XrImuV1Sample& sample,
                      XrImuV1DecodeError* error) {
  set_error(error, XrImuV1DecodeError::None);
  if (!data || size < kXrImuV1PacketSize) {
    set_error(error, XrImuV1DecodeError::TooShort);
    return false;
  }
  if (std::memcmp(data, kXrImuV1Magic.data(), kXrImuV1Magic.size()) != 0) {
    set_error(error, XrImuV1DecodeError::BadMagic);
    return false;
  }
  if (data[4] != kXrImuV1Version) {
    set_error(error, XrImuV1DecodeError::UnsupportedVersion);
    return false;
  }
  if (read_u16_le(data + 6) != kXrImuV1PacketSize) {
    set_error(error, XrImuV1DecodeError::BadPacketSize);
    return false;
  }
  if (read_u32_le(data + kXrImuV1CrcOffset) !=
      xr_imu_v1_crc32(data, kXrImuV1CrcOffset)) {
    set_error(error, XrImuV1DecodeError::BadCrc);
    return false;
  }

  XrImuV1Sample decoded;
  decoded.flags = data[5];
  decoded.sequence = read_u32_le(data + 8);
  decoded.device_timestamp_us = read_u64_le(data + 12);
  for (int i = 0; i < 3; ++i) {
    decoded.gyro_rad_s[static_cast<size_t>(i)] = read_f32_le(data + 20 + i * 4);
    decoded.accel_m_s2[static_cast<size_t>(i)] = read_f32_le(data + 32 + i * 4);
  }
  if (!finite_sample(decoded)) {
    set_error(error, XrImuV1DecodeError::NonFiniteSample);
    return false;
  }
  sample = decoded;
  return true;
}

const char* xr_imu_v1_decode_error_name(XrImuV1DecodeError error) {
  switch (error) {
    case XrImuV1DecodeError::None: return "none";
    case XrImuV1DecodeError::TooShort: return "too_short";
    case XrImuV1DecodeError::BadMagic: return "bad_magic";
    case XrImuV1DecodeError::UnsupportedVersion: return "unsupported_version";
    case XrImuV1DecodeError::BadPacketSize: return "bad_packet_size";
    case XrImuV1DecodeError::BadCrc: return "bad_crc";
    case XrImuV1DecodeError::NonFiniteSample: return "non_finite_sample";
  }
  return "unknown";
}

}  // namespace xr_capture_cpp
