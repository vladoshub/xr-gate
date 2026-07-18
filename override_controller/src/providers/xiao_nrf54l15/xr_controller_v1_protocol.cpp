#include "xr_controller_v1_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

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

bool has_magic_at(const std::vector<uint8_t>& buffer, size_t offset) {
  return offset + kXrControllerV1Magic.size() <= buffer.size() &&
         std::equal(kXrControllerV1Magic.begin(), kXrControllerV1Magic.end(),
                    buffer.begin() + static_cast<std::ptrdiff_t>(offset));
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
  while (buffer_.size() >= kXrControllerV1Magic.size()) {
    size_t magic_offset = 0;
    while (magic_offset + kXrControllerV1Magic.size() <= buffer_.size() &&
           !has_magic_at(buffer_, magic_offset)) {
      ++magic_offset;
    }
    if (magic_offset > 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(magic_offset));
    }
    if (buffer_.size() < kXrControllerV1PacketSize) return std::nullopt;

    auto packet = decode_xr_controller_v1(buffer_.data(), kXrControllerV1PacketSize);
    if (packet) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(kXrControllerV1PacketSize));
      return packet;
    }

    // The bytes start with XCTL but fail version/size/CRC/finite validation.
    // Drop one byte instead of the full frame so a valid overlapping frame can
    // still be recovered after corruption or a partial serial reconnect.
    buffer_.erase(buffer_.begin());
  }
  return std::nullopt;
}

void XrControllerV1StreamDecoder::reset() {
  buffer_.clear();
}

}  // namespace xr_override_controller::xiao_nrf54l15
