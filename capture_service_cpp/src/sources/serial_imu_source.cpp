#include "capture_service_cpp/sources/imu_source.hpp"

#include "capture_service_cpp/platform/serial_port.hpp"
#include "capture_service_cpp/sources/serial_imu_protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

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

class DeviceClockMapper {
 public:
  uint64_t map(uint64_t sensor_timestamp_us, uint64_t host_receive_ns) {
    const uint64_t sensor_ns = sensor_timestamp_us * 1000ULL;
    const int64_t candidate = static_cast<int64_t>(host_receive_ns) - static_cast<int64_t>(sensor_ns);
    if (!initialized_) {
      offset_ns_ = candidate;
      initialized_ = true;
    } else if (candidate < offset_ns_) {
      // The smallest observed receive offset is the best available estimate of
      // transport delay without a request/response clock synchronization link.
      offset_ns_ = candidate;
    } else if (candidate - offset_ns_ < 2000000LL) {
      // Track slow oscillator drift, but do not absorb ordinary USB/UART jitter.
      offset_ns_ += (candidate - offset_ns_) / 1024;
    }
    int64_t mapped_signed = static_cast<int64_t>(sensor_ns) + offset_ns_;
    uint64_t mapped = mapped_signed > 0 ? static_cast<uint64_t>(mapped_signed) : host_receive_ns;
    if (mapped <= last_mapped_ns_) mapped = last_mapped_ns_ + 1;
    last_mapped_ns_ = mapped;
    return mapped;
  }

 private:
  bool initialized_ = false;
  int64_t offset_ns_ = 0;
  uint64_t last_mapped_ns_ = 0;
};

class SerialImuSource final : public IImuSource {
 public:
  explicit SerialImuSource(RuntimeConfig cfg) : cfg_(std::move(cfg)) {
    buffer_.reserve(std::max<size_t>(4096, cfg_.imu.serial.max_packet_size * 4));
  }

  std::string name() const override { return "serial:" + cfg_.imu.serial.protocol; }

  std::vector<StreamSpec> stream_specs(uint32_t slot_count, uint32_t raw_slot_count) const override {
    std::vector<StreamSpec> specs;
    if (cfg_.imu.raw_enabled) {
      specs.push_back(StreamSpec{cfg_.imu.raw_stream_id, kKindBytes, "BYTES", 0, 0, kFormatBytes, "BYTES",
                                 static_cast<uint32_t>(cfg_.imu.raw_payload_size), raw_slot_count,
                                 cfg_.imu.raw_frame_id, "Raw serial IMU protocol packet"});
    }
    specs.push_back(StreamSpec{cfg_.imu.stream_id, kKindImu, "IMU", 0, 0, kFormatImuF32Le, "IMU_F32_LE",
                               24, slot_count, cfg_.imu.frame_id,
                               "Normalized gyro rad/s and accelerometer m/s^2 sample"});
    return specs;
  }

  void open() override {
    serial_.open(cfg_.imu.serial.port, cfg_.imu.serial.baud_rate);
    std::cerr << "[capture_service_cpp] imu source=serial port=" << cfg_.imu.serial.port
              << " baud=" << cfg_.imu.serial.baud_rate
              << " protocol=" << cfg_.imu.serial.protocol << std::endl;
  }

  SourceReadStatus read(ImuReadResult& result) override {
    result = {};
    if (parse_buffer(result)) return SourceReadStatus::Data;

    std::array<uint8_t, 1024> chunk{};
    const int count = serial_.read_timeout(chunk.data(), chunk.size(), cfg_.imu.serial.read_timeout_ms);
    if (count < 0) return SourceReadStatus::EndOfStream;
    if (count == 0) return SourceReadStatus::Timeout;
    last_receive_ns_ = steady_ns();
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + count);
    if (buffer_.size() > 1024 * 1024) {
      buffer_.erase(buffer_.begin(), buffer_.end() - static_cast<std::ptrdiff_t>(cfg_.imu.serial.max_packet_size * 4));
      ++resync_count_;
    }
    if (parse_buffer(result)) return SourceReadStatus::Data;
    result.receive_timestamp_ns = last_receive_ns_;
    return SourceReadStatus::Data;
  }

  void close() override { serial_.close(); }

 private:
  bool parse_buffer(ImuReadResult& result) {
    if (cfg_.imu.serial.protocol == "csv_f32") return parse_csv(result);
    return parse_xr_imu_v1(result);
  }

  bool parse_xr_imu_v1(ImuReadResult& result) {
    static constexpr std::array<uint8_t, 4> magic{{'X', 'I', 'M', 'U'}};
    while (buffer_.size() >= magic.size()) {
      const auto found = std::search(buffer_.begin(), buffer_.end(), magic.begin(), magic.end());
      if (found != buffer_.begin()) {
        if (found == buffer_.end()) {
          const size_t keep = std::min<size_t>(buffer_.size(), magic.size() - 1);
          buffer_.erase(buffer_.begin(), buffer_.end() - static_cast<std::ptrdiff_t>(keep));
          ++resync_count_;
          return false;
        }
        buffer_.erase(buffer_.begin(), found);
        ++resync_count_;
      }
      if (buffer_.size() < 8) return false;
      const uint8_t version = buffer_[4];
      const uint16_t packet_size = read_u16_le(buffer_.data() + 6);
      if (version != kXrImuV1Version || packet_size != kXrImuV1PacketSize ||
          packet_size > cfg_.imu.serial.max_packet_size) {
        buffer_.erase(buffer_.begin());
        ++invalid_count_;
        continue;
      }
      if (buffer_.size() < packet_size) return false;
      const uint32_t expected_crc = read_u32_le(buffer_.data() + 44);
      const uint32_t actual_crc = xr_imu_crc32(buffer_.data(), 44);
      if (actual_crc != expected_crc) {
        buffer_.erase(buffer_.begin());
        ++crc_fail_count_;
        continue;
      }

      std::vector<uint8_t> packet(buffer_.begin(), buffer_.begin() + packet_size);
      buffer_.erase(buffer_.begin(), buffer_.begin() + packet_size);
      const uint8_t flags = packet[5];
      const uint32_t sequence = read_u32_le(packet.data() + 8);
      const uint64_t device_timestamp_us = read_u64_le(packet.data() + 12);
      for (int i = 0; i < 3; ++i) {
        result.sample.gyro_rad_s[static_cast<size_t>(i)] = read_f32_le(packet.data() + 20 + i * 4);
        result.sample.accel_m_s2[static_cast<size_t>(i)] = read_f32_le(packet.data() + 32 + i * 4);
      }
      if (!sample_is_finite(result.sample)) {
        ++invalid_count_;
        continue;
      }
      result.receive_timestamp_ns = last_receive_ns_ ? last_receive_ns_ : steady_ns();
      result.sample.source_sequence = sequence;
      result.sample.source_timestamp_valid = (flags & kXrImuV1TimestampValid) != 0;
      if (cfg_.imu.serial.timestamp_mode == "device" && result.sample.source_timestamp_valid) {
        result.sample.timestamp_ns = clock_mapper_.map(device_timestamp_us, result.receive_timestamp_ns);
      } else {
        result.sample.timestamp_ns = result.receive_timestamp_ns;
      }
      result.raw_packet = std::move(packet);
      result.has_sample = true;
      log_progress();
      return true;
    }
    return false;
  }

  bool parse_csv(ImuReadResult& result) {
    const auto newline = std::find(buffer_.begin(), buffer_.end(), static_cast<uint8_t>('\n'));
    if (newline == buffer_.end()) return false;
    std::vector<uint8_t> line_bytes(buffer_.begin(), newline + 1);
    buffer_.erase(buffer_.begin(), newline + 1);
    std::string line(line_bytes.begin(), line_bytes.end());
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::replace(line.begin(), line.end(), ';', ',');
    std::stringstream ss(line);
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(field);
    if (fields.size() != 7 && fields.size() != 8) {
      ++invalid_count_;
      return false;
    }
    try {
      const uint64_t timestamp_us = static_cast<uint64_t>(std::stoull(fields[0]));
      size_t base = 1;
      if (fields.size() == 8) {
        result.sample.source_sequence = static_cast<uint32_t>(std::stoul(fields[1]));
        base = 2;
      } else {
        result.sample.source_sequence = ++generated_sequence_;
      }
      for (int i = 0; i < 3; ++i) {
        result.sample.gyro_rad_s[static_cast<size_t>(i)] = std::stof(fields[base + static_cast<size_t>(i)]);
        result.sample.accel_m_s2[static_cast<size_t>(i)] = std::stof(fields[base + 3 + static_cast<size_t>(i)]);
      }
      if (!sample_is_finite(result.sample)) throw std::runtime_error("non-finite CSV sample");
      result.receive_timestamp_ns = last_receive_ns_ ? last_receive_ns_ : steady_ns();
      result.sample.source_timestamp_valid = timestamp_us != 0;
      if (cfg_.imu.serial.timestamp_mode == "device" && timestamp_us != 0) {
        result.sample.timestamp_ns = clock_mapper_.map(timestamp_us, result.receive_timestamp_ns);
      } else {
        result.sample.timestamp_ns = result.receive_timestamp_ns;
      }
      result.raw_packet = std::move(line_bytes);
      result.has_sample = true;
      log_progress();
      return true;
    } catch (const std::exception&) {
      ++invalid_count_;
      return false;
    }
  }

  static bool sample_is_finite(const ImuSample& sample) {
    for (float value : sample.gyro_rad_s) if (!std::isfinite(value)) return false;
    for (float value : sample.accel_m_s2) if (!std::isfinite(value)) return false;
    return true;
  }

  void log_progress() {
    ++sample_count_;
    if (sample_count_ % 1000 == 0) {
      std::cerr << "[capture_service_cpp] imu source=serial samples=" << sample_count_
                << " crc_fail=" << crc_fail_count_ << " invalid=" << invalid_count_
                << " resync=" << resync_count_ << std::endl;
    }
  }

  RuntimeConfig cfg_;
  SerialPort serial_;
  std::vector<uint8_t> buffer_;
  DeviceClockMapper clock_mapper_;
  uint64_t last_receive_ns_ = 0;
  uint32_t generated_sequence_ = 0;
  uint64_t sample_count_ = 0;
  uint64_t crc_fail_count_ = 0;
  uint64_t invalid_count_ = 0;
  uint64_t resync_count_ = 0;
};

}  // namespace

std::unique_ptr<IImuSource> make_serial_imu_source(const RuntimeConfig& cfg) {
  return std::make_unique<SerialImuSource>(cfg);
}

}  // namespace xr_capture_cpp
