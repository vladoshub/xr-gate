#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <capture_client/contracts/messages.hpp>

namespace capture_client {

class IStreamReader {
 public:
  virtual ~IStreamReader() = default;

  virtual const StreamInfo& info() const = 0;
  virtual uint64_t latest_sequence() const = 0;
  virtual bool read_latest(RawMessage& out) const = 0;
  virtual bool read_sequence(uint64_t sequence, RawMessage& out) const = 0;

  virtual bool read_latest_image(ImageFrame& out) const {
    RawMessage msg;
    if (!read_latest(msg)) return false;
    return raw_to_image(msg, out);
  }

  virtual bool read_image_sequence(uint64_t sequence, ImageFrame& out) const {
    RawMessage msg;
    if (!read_sequence(sequence, msg)) return false;
    return raw_to_image(msg, out);
  }

  virtual bool read_latest_imu(ImuSample& out) const {
    RawMessage msg;
    if (!read_latest(msg)) return false;
    return raw_to_imu(msg, out);
  }

  virtual bool read_imu_sequence(uint64_t sequence, ImuSample& out) const {
    RawMessage msg;
    if (!read_sequence(sequence, msg)) return false;
    return raw_to_imu(msg, out);
  }

 protected:
  static bool raw_to_image(const RawMessage& msg, ImageFrame& out) {
    if (msg.format_code != static_cast<uint32_t>(FormatCode::GRAY8)) return false;
    if (msg.width == 0 || msg.height == 0) return false;
    if (msg.payload_size != msg.width * msg.height) return false;
    out.sequence = msg.sequence;
    out.timestamp_ns = msg.timestamp_ns;
    out.width = msg.width;
    out.height = msg.height;
    out.gray8 = msg.payload;
    return true;
  }

  static float read_f32_le(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, sizeof(v));
    return v;
  }

  static bool raw_to_imu(const RawMessage& msg, ImuSample& out) {
    if (msg.format_code != static_cast<uint32_t>(FormatCode::IMU_F32_LE)) return false;
    if (msg.payload_size < 24) return false;
    const uint8_t* p = msg.payload.data();
    out.sequence = msg.sequence;
    out.timestamp_ns = msg.timestamp_ns;
    out.gyro_rad_s[0] = read_f32_le(p + 0);
    out.gyro_rad_s[1] = read_f32_le(p + 4);
    out.gyro_rad_s[2] = read_f32_le(p + 8);
    out.accel_m_s2[0] = read_f32_le(p + 12);
    out.accel_m_s2[1] = read_f32_le(p + 16);
    out.accel_m_s2[2] = read_f32_le(p + 20);
    return true;
  }
};

class ICaptureTransport {
 public:
  virtual ~ICaptureTransport() = default;

  virtual const std::string& type() const = 0;
  virtual IStreamReader& cam0() = 0;
  virtual IStreamReader& cam1() = 0;
  virtual IStreamReader& imu() = 0;
};

}  // namespace capture_client
