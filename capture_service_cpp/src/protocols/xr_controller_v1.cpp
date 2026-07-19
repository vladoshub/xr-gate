#include "capture_service_cpp/protocols/xr_controller_v1.hpp"

#include <cmath>
#include <cstring>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace xr_capture_cpp {
namespace {

uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

int16_t read_i16_le(const uint8_t* p) {
  return static_cast<int16_t>(read_u16_le(p));
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

void write_i16_le(uint8_t* p, int16_t value) {
  write_u16_le(p, static_cast<uint16_t>(value));
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

bool finite_sample(const XrControllerV1Sample& sample) {
  for (float value : sample.gyro_rad_s) if (!std::isfinite(value)) return false;
  for (float value : sample.accel_m_s2) if (!std::isfinite(value)) return false;
  return true;
}

void set_error(XrControllerV1DecodeError* output, XrControllerV1DecodeError value) {
  if (output) *output = value;
}

}  // namespace

uint32_t xr_controller_v1_crc32(const uint8_t* data, size_t size) {
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


bool encode_xr_controller_identity_v1(const XrControllerIdentityV1& identity,
                                      uint8_t* output,
                                      size_t output_size) {
  if (!output || output_size < kXrControllerIdentityV1PacketSize ||
      identity.device_uid_size > kXrControllerDeviceUidMaxSize) {
    return false;
  }
  std::memset(output, 0, kXrControllerIdentityV1PacketSize);
  std::memcpy(output, kXrControllerIdentityV1Magic.data(),
              kXrControllerIdentityV1Magic.size());
  output[4] = kXrControllerIdentityV1Version;
  output[5] = identity.flags;
  write_u16_le(output + 6,
               static_cast<uint16_t>(kXrControllerIdentityV1PacketSize));
  output[8] = static_cast<uint8_t>(identity.device_uid_size);
  output[9] = identity.controller_protocol_version;
  std::memcpy(output + 12, identity.device_uid.data(), identity.device_uid_size);
  write_u32_le(output + kXrControllerIdentityV1CrcOffset,
               xr_controller_v1_crc32(output,
                                      kXrControllerIdentityV1CrcOffset));
  return true;
}

bool decode_xr_controller_identity_v1(const uint8_t* data,
                                      size_t size,
                                      XrControllerIdentityV1& identity) {
  if (!data || size < kXrControllerIdentityV1PacketSize) return false;
  if (std::memcmp(data, kXrControllerIdentityV1Magic.data(),
                  kXrControllerIdentityV1Magic.size()) != 0) {
    return false;
  }
  if (data[4] != kXrControllerIdentityV1Version ||
      read_u16_le(data + 6) != kXrControllerIdentityV1PacketSize) {
    return false;
  }
  if (read_u32_le(data + kXrControllerIdentityV1CrcOffset) !=
      xr_controller_v1_crc32(data, kXrControllerIdentityV1CrcOffset)) {
    return false;
  }
  const size_t uid_size = data[8];
  if (uid_size > kXrControllerDeviceUidMaxSize) return false;
  XrControllerIdentityV1 decoded;
  decoded.flags = data[5];
  decoded.controller_protocol_version = data[9];
  decoded.device_uid_size = uid_size;
  std::memcpy(decoded.device_uid.data(), data + 12, uid_size);
  identity = decoded;
  return true;
}

std::string xr_controller_device_uid_hex(const XrControllerIdentityV1& identity) {
  if ((identity.flags & kXrControllerIdentityV1DeviceUidValid) == 0 ||
      identity.device_uid_size == 0) {
    return {};
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t i = 0; i < identity.device_uid_size; ++i) {
    out << std::setw(2) << static_cast<unsigned>(identity.device_uid[i]);
  }
  return out.str();
}

std::string normalize_xr_controller_device_uid(std::string value) {
  constexpr const char* kPrefix = "xiao_nrf54l15:uid:";
  if (value.rfind(kPrefix, 0) == 0) value.erase(0, std::strlen(kPrefix));
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (std::isxdigit(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
  }
  if (!out.empty() && (out.size() % 2) != 0) return {};
  return out;
}

bool encode_xr_controller_v1(const XrControllerV1Sample& sample,
                             uint8_t* output,
                             size_t output_size) {
  if (!output || output_size < kXrControllerV1PacketSize || !finite_sample(sample)) return false;

  std::memset(output, 0, kXrControllerV1PacketSize);
  std::memcpy(output, kXrControllerV1Magic.data(), kXrControllerV1Magic.size());
  output[4] = kXrControllerV1Version;
  output[5] = sample.flags;
  write_u16_le(output + 6, static_cast<uint16_t>(kXrControllerV1PacketSize));
  write_u32_le(output + 8, sample.sequence);
  write_u64_le(output + 12, sample.device_timestamp_us);
  for (int i = 0; i < 3; ++i) {
    write_f32_le(output + 20 + i * 4, sample.gyro_rad_s[static_cast<size_t>(i)]);
    write_f32_le(output + 32 + i * 4, sample.accel_m_s2[static_cast<size_t>(i)]);
  }
  write_u32_le(output + 44, sample.buttons);
  for (size_t i = 0; i < kXrControllerV1AxisCount; ++i) {
    write_i16_le(output + 48 + i * 2, sample.axes[i]);
  }
  write_u16_le(output + 56, sample.battery_mv);
  write_u16_le(output + 58, sample.controller_status);
  write_u32_le(output + kXrControllerV1CrcOffset,
               xr_controller_v1_crc32(output, kXrControllerV1CrcOffset));
  return true;
}

bool decode_xr_controller_v1(const uint8_t* data,
                             size_t size,
                             XrControllerV1Sample& sample,
                             XrControllerV1DecodeError* error) {
  set_error(error, XrControllerV1DecodeError::None);
  if (!data || size < kXrControllerV1PacketSize) {
    set_error(error, XrControllerV1DecodeError::TooShort);
    return false;
  }
  if (std::memcmp(data, kXrControllerV1Magic.data(), kXrControllerV1Magic.size()) != 0) {
    set_error(error, XrControllerV1DecodeError::BadMagic);
    return false;
  }
  if (data[4] != kXrControllerV1Version) {
    set_error(error, XrControllerV1DecodeError::UnsupportedVersion);
    return false;
  }
  if (read_u16_le(data + 6) != kXrControllerV1PacketSize) {
    set_error(error, XrControllerV1DecodeError::BadPacketSize);
    return false;
  }
  if (read_u32_le(data + kXrControllerV1CrcOffset) !=
      xr_controller_v1_crc32(data, kXrControllerV1CrcOffset)) {
    set_error(error, XrControllerV1DecodeError::BadCrc);
    return false;
  }

  XrControllerV1Sample decoded;
  decoded.flags = data[5];
  decoded.sequence = read_u32_le(data + 8);
  decoded.device_timestamp_us = read_u64_le(data + 12);
  for (int i = 0; i < 3; ++i) {
    decoded.gyro_rad_s[static_cast<size_t>(i)] = read_f32_le(data + 20 + i * 4);
    decoded.accel_m_s2[static_cast<size_t>(i)] = read_f32_le(data + 32 + i * 4);
  }
  decoded.buttons = read_u32_le(data + 44);
  for (size_t i = 0; i < kXrControllerV1AxisCount; ++i) {
    decoded.axes[i] = read_i16_le(data + 48 + i * 2);
  }
  decoded.battery_mv = read_u16_le(data + 56);
  decoded.controller_status = read_u16_le(data + 58);

  if (!finite_sample(decoded)) {
    set_error(error, XrControllerV1DecodeError::NonFiniteSample);
    return false;
  }
  sample = decoded;
  return true;
}

const char* xr_controller_v1_decode_error_name(XrControllerV1DecodeError error) {
  switch (error) {
    case XrControllerV1DecodeError::None: return "none";
    case XrControllerV1DecodeError::TooShort: return "too_short";
    case XrControllerV1DecodeError::BadMagic: return "bad_magic";
    case XrControllerV1DecodeError::UnsupportedVersion: return "unsupported_version";
    case XrControllerV1DecodeError::BadPacketSize: return "bad_packet_size";
    case XrControllerV1DecodeError::BadCrc: return "bad_crc";
    case XrControllerV1DecodeError::NonFiniteSample: return "non_finite_sample";
  }
  return "unknown";
}

}  // namespace xr_capture_cpp
