#include "xr_controller_v1_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace xr_override_controller::xiao_nrf54l15 {
namespace {

uint16_t get_u16_le(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8u);
}

int16_t get_i16_le(const uint8_t* data) {
  return static_cast<int16_t>(get_u16_le(data));
}

uint32_t get_u32_le(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8u) |
         (static_cast<uint32_t>(data[2]) << 16u) |
         (static_cast<uint32_t>(data[3]) << 24u);
}

uint64_t get_u64_le(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(data[i]) << (8u * i);
  }
  return value;
}

float get_f32_le(const uint8_t* data) {
  const uint32_t bits = get_u32_le(data);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(bits), "xr_controller_v1 requires float32");
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool has_magic_at(const std::vector<uint8_t>& buffer,
                  size_t offset,
                  const std::array<uint8_t, 4>& magic) {
  return offset + magic.size() <= buffer.size() &&
         std::equal(magic.begin(), magic.end(),
                    buffer.begin() + static_cast<std::ptrdiff_t>(offset));
}

bool has_any_magic_at(const std::vector<uint8_t>& buffer, size_t offset) {
  return has_magic_at(buffer, offset, kXrControllerV1Magic) ||
         has_magic_at(buffer, offset, kXrControllerIdentityV1Magic);
}

}  // namespace

uint32_t xr_controller_crc32_ieee(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-
          static_cast<int32_t>(crc & 1u));
      crc = (crc >> 1u) ^ (0xedb88320u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

std::optional<XrControllerV1Packet> decode_xr_controller_v1(
    const uint8_t* data, size_t size) {
  if (!data || size != kXrControllerV1PacketSize) return std::nullopt;
  if (!std::equal(kXrControllerV1Magic.begin(), kXrControllerV1Magic.end(), data)) {
    return std::nullopt;
  }
  if (data[4] != kXrControllerV1Version) return std::nullopt;
  if (get_u16_le(data + 6) != kXrControllerV1PacketSize) return std::nullopt;
  if (get_u32_le(data + kXrControllerV1CrcOffset) !=
      xr_controller_crc32_ieee(data, kXrControllerV1CrcOffset)) {
    return std::nullopt;
  }

  XrControllerV1Packet packet;
  packet.flags = data[5];
  packet.sequence = get_u32_le(data + 8);
  packet.timestamp_us = get_u64_le(data + 12);
  for (size_t i = 0; i < 3; ++i) {
    packet.gyro_rad_s[i] = get_f32_le(data + 20 + i * 4);
    packet.accel_m_s2[i] = get_f32_le(data + 32 + i * 4);
    if (!std::isfinite(packet.gyro_rad_s[i]) ||
        !std::isfinite(packet.accel_m_s2[i])) {
      return std::nullopt;
    }
  }
  packet.buttons = get_u32_le(data + 44);
  for (size_t i = 0; i < packet.axes.size(); ++i) {
    packet.axes[i] = get_i16_le(data + 48 + i * 2);
  }
  packet.battery_mv = get_u16_le(data + 56);
  packet.controller_status = get_u16_le(data + 58);
  return packet;
}

std::optional<XrControllerIdentityV1Packet> decode_xr_controller_identity_v1(
    const uint8_t* data, size_t size) {
  if (!data || size != kXrControllerIdentityV1PacketSize) return std::nullopt;
  if (!std::equal(kXrControllerIdentityV1Magic.begin(),
                  kXrControllerIdentityV1Magic.end(), data)) {
    return std::nullopt;
  }
  if (data[4] != kXrControllerIdentityV1Version) return std::nullopt;
  if (get_u16_le(data + 6) != kXrControllerIdentityV1PacketSize) {
    return std::nullopt;
  }
  if (get_u32_le(data + kXrControllerIdentityV1CrcOffset) !=
      xr_controller_crc32_ieee(data, kXrControllerIdentityV1CrcOffset)) {
    return std::nullopt;
  }
  const size_t uid_size = data[8];
  if (uid_size > kXrControllerDeviceUidMaxSize) return std::nullopt;
  XrControllerIdentityV1Packet identity;
  identity.flags = data[5];
  identity.controller_protocol_version = data[9];
  identity.device_uid_size = uid_size;
  std::copy(data + 12, data + 12 + uid_size, identity.device_uid.begin());
  return identity;
}

std::string xr_controller_device_uid_hex(
    const XrControllerIdentityV1Packet& identity) {
  if ((identity.flags & kIdentityFlagDeviceUidValid) == 0 ||
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

void XrControllerV1StreamDecoder::append(const uint8_t* data, size_t size) {
  if (!data || size == 0) return;
  constexpr size_t kMaxBufferedBytes = kXrControllerV1PacketSize * 64;
  if (size >= kMaxBufferedBytes) {
    buffer_.assign(data + (size - kMaxBufferedBytes), data + size);
    return;
  }
  if (buffer_.size() + size > kMaxBufferedBytes) {
    const size_t remove = buffer_.size() + size - kMaxBufferedBytes;
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(remove));
  }
  buffer_.insert(buffer_.end(), data, data + size);
}

std::optional<XrControllerV1Packet> XrControllerV1StreamDecoder::pop() {
  while (buffer_.size() >= 4) {
    size_t magic_offset = 0;
    while (magic_offset + 4 <= buffer_.size() &&
           !has_any_magic_at(buffer_, magic_offset)) {
      ++magic_offset;
    }
    if (magic_offset > 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(magic_offset));
    }
    if (buffer_.size() < 4) return std::nullopt;

    if (has_magic_at(buffer_, 0, kXrControllerIdentityV1Magic)) {
      if (buffer_.size() < kXrControllerIdentityV1PacketSize) return std::nullopt;
      auto identity = decode_xr_controller_identity_v1(
          buffer_.data(), kXrControllerIdentityV1PacketSize);
      if (identity) {
        identities_.push_back(*identity);
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(
                          kXrControllerIdentityV1PacketSize));
        continue;
      }
      buffer_.erase(buffer_.begin());
      continue;
    }

    if (buffer_.size() < kXrControllerV1PacketSize) return std::nullopt;
    auto packet = decode_xr_controller_v1(buffer_.data(), kXrControllerV1PacketSize);
    if (packet) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(
                        kXrControllerV1PacketSize));
      return packet;
    }
    buffer_.erase(buffer_.begin());
  }
  return std::nullopt;
}

std::optional<XrControllerIdentityV1Packet>
XrControllerV1StreamDecoder::pop_identity() {
  if (identities_.empty()) return std::nullopt;
  XrControllerIdentityV1Packet identity = identities_.front();
  identities_.pop_front();
  return identity;
}

void XrControllerV1StreamDecoder::reset() {
  buffer_.clear();
  identities_.clear();
}

}  // namespace xr_override_controller::xiao_nrf54l15
