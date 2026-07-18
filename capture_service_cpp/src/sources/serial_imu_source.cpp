#include "capture_service_cpp/sources/imu_source.hpp"

#include "capture_service_cpp/platform/serial_port.hpp"
#include "capture_service_cpp/protocols/xr_controller_v1.hpp"
#include "capture_service_cpp/protocols/xr_imu_v1.hpp"
#include "capture_service_cpp/timing/affine_device_clock_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace xr_capture_cpp {
namespace {

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
    clock_mapper_.reset();
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
      const size_t keep = std::max<size_t>(
          std::max(kXrControllerV1PacketSize, kXrImuV1PacketSize),
          cfg_.imu.serial.max_packet_size * 4);
      buffer_.erase(buffer_.begin(),
                    buffer_.end() - static_cast<std::ptrdiff_t>(std::min(buffer_.size(), keep)));
      ++resync_count_;
    }
    if (parse_buffer(result)) return SourceReadStatus::Data;

    // Transport bytes alone are not proof that the IMU is healthy. Returning a
    // distinct status prevents partial packets or an endless garbage stream
    // from resetting the pipeline's valid-sample stall timer.
    result.receive_timestamp_ns = last_receive_ns_;
    return SourceReadStatus::TransportActivity;
  }

  void close() override { serial_.close(); }

 private:
  bool parse_buffer(ImuReadResult& result) {
    if (cfg_.imu.serial.protocol == "csv_f32") return parse_csv(result);
    if (cfg_.imu.serial.protocol == "xr_imu_v1") return parse_xr_imu_v1(result);
    return parse_xr_controller_v1(result);
  }

  bool parse_xr_controller_v1(ImuReadResult& result) {
    while (buffer_.size() >= kXrControllerV1Magic.size()) {
      const auto found = std::search(buffer_.begin(), buffer_.end(),
                                     kXrControllerV1Magic.begin(), kXrControllerV1Magic.end());
      if (found != buffer_.begin()) {
        if (found == buffer_.end()) {
          const size_t keep = std::min<size_t>(buffer_.size(), kXrControllerV1Magic.size() - 1);
          buffer_.erase(buffer_.begin(),
                        buffer_.end() - static_cast<std::ptrdiff_t>(keep));
          ++resync_count_;
          return false;
        }
        buffer_.erase(buffer_.begin(), found);
        ++resync_count_;
      }
      if (buffer_.size() < kXrControllerV1PacketSize) return false;

      XrControllerV1Sample decoded;
      XrControllerV1DecodeError error = XrControllerV1DecodeError::None;
      if (!decode_xr_controller_v1(buffer_.data(), buffer_.size(), decoded, &error)) {
        buffer_.erase(buffer_.begin());
        if (error == XrControllerV1DecodeError::BadCrc) {
          ++crc_fail_count_;
        } else {
          ++invalid_count_;
        }
        continue;
      }

      std::vector<uint8_t> packet(
          buffer_.begin(),
          buffer_.begin() + static_cast<std::ptrdiff_t>(kXrControllerV1PacketSize));
      buffer_.erase(
          buffer_.begin(),
          buffer_.begin() + static_cast<std::ptrdiff_t>(kXrControllerV1PacketSize));

      result.receive_timestamp_ns = last_receive_ns_ ? last_receive_ns_ : steady_ns();
      result.sample.gyro_rad_s = decoded.gyro_rad_s;
      result.sample.accel_m_s2 = decoded.accel_m_s2;
      result.sample.source_sequence = decoded.sequence;
      result.sample.source_timestamp_valid =
          (decoded.flags & kXrControllerV1TimestampValid) != 0;
      if (cfg_.imu.serial.timestamp_mode == "device" &&
          result.sample.source_timestamp_valid) {
        result.sample.timestamp_ns =
            clock_mapper_.map_us(decoded.device_timestamp_us, result.receive_timestamp_ns);
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

  bool parse_xr_imu_v1(ImuReadResult& result) {
    while (buffer_.size() >= kXrImuV1Magic.size()) {
      const auto found = std::search(buffer_.begin(), buffer_.end(),
                                     kXrImuV1Magic.begin(), kXrImuV1Magic.end());
      if (found != buffer_.begin()) {
        if (found == buffer_.end()) {
          const size_t keep = std::min<size_t>(buffer_.size(), kXrImuV1Magic.size() - 1);
          buffer_.erase(buffer_.begin(),
                        buffer_.end() - static_cast<std::ptrdiff_t>(keep));
          ++resync_count_;
          return false;
        }
        buffer_.erase(buffer_.begin(), found);
        ++resync_count_;
      }
      if (buffer_.size() < kXrImuV1PacketSize) return false;

      XrImuV1Sample decoded;
      XrImuV1DecodeError error = XrImuV1DecodeError::None;
      if (!decode_xr_imu_v1(buffer_.data(), buffer_.size(), decoded, &error)) {
        buffer_.erase(buffer_.begin());
        if (error == XrImuV1DecodeError::BadCrc) {
          ++crc_fail_count_;
        } else {
          ++invalid_count_;
        }
        continue;
      }

      std::vector<uint8_t> packet(buffer_.begin(),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(kXrImuV1PacketSize));
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(kXrImuV1PacketSize));

      result.receive_timestamp_ns = last_receive_ns_ ? last_receive_ns_ : steady_ns();
      result.sample.gyro_rad_s = decoded.gyro_rad_s;
      result.sample.accel_m_s2 = decoded.accel_m_s2;
      result.sample.source_sequence = decoded.sequence;
      result.sample.source_timestamp_valid =
          (decoded.flags & kXrImuV1TimestampValid) != 0;
      if (cfg_.imu.serial.timestamp_mode == "device" &&
          result.sample.source_timestamp_valid) {
        result.sample.timestamp_ns =
            clock_mapper_.map_us(decoded.device_timestamp_us, result.receive_timestamp_ns);
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
    // Consume invalid complete lines until a valid sample is found or the
    // buffer contains only a partial line. This avoids requiring new serial
    // bytes before parsing an already-buffered valid line.
    while (true) {
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
        continue;
      }
      try {
        const uint64_t timestamp_us = static_cast<uint64_t>(std::stoull(fields[0]));
        ImuSample sample;
        size_t base = 1;
        if (fields.size() == 8) {
          sample.source_sequence = static_cast<uint32_t>(std::stoul(fields[1]));
          base = 2;
        } else {
          sample.source_sequence = ++generated_sequence_;
        }
        for (int i = 0; i < 3; ++i) {
          sample.gyro_rad_s[static_cast<size_t>(i)] =
              std::stof(fields[base + static_cast<size_t>(i)]);
          sample.accel_m_s2[static_cast<size_t>(i)] =
              std::stof(fields[base + 3 + static_cast<size_t>(i)]);
        }
        if (!sample_is_finite(sample)) throw std::runtime_error("non-finite CSV sample");

        result.receive_timestamp_ns = last_receive_ns_ ? last_receive_ns_ : steady_ns();
        sample.source_timestamp_valid = timestamp_us != 0;
        if (cfg_.imu.serial.timestamp_mode == "device" && timestamp_us != 0) {
          sample.timestamp_ns = clock_mapper_.map_us(timestamp_us, result.receive_timestamp_ns);
        } else {
          sample.timestamp_ns = result.receive_timestamp_ns;
        }
        result.sample = sample;
        result.raw_packet = std::move(line_bytes);
        result.has_sample = true;
        log_progress();
        return true;
      } catch (const std::exception&) {
        ++invalid_count_;
      }
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
      const auto timing = clock_mapper_.diagnostics();
      std::cerr << "[capture_service_cpp] imu source=serial samples=" << sample_count_
                << " crc_fail=" << crc_fail_count_ << " invalid=" << invalid_count_
                << " resync=" << resync_count_
                << " clock_anchors=" << timing.anchor_count
                << " clock_drift_ppm=" << timing.drift_ppm << std::endl;
    }
  }

  RuntimeConfig cfg_;
  SerialPort serial_;
  std::vector<uint8_t> buffer_;
  AffineDeviceClockMapper clock_mapper_;
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
