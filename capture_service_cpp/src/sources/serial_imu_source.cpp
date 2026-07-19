#include "capture_service_cpp/sources/imu_source.hpp"

#include "capture_service_cpp/platform/serial_port.hpp"
#include "capture_service_cpp/protocols/xr_controller_v1.hpp"
#include "capture_service_cpp/protocols/xr_imu_v1.hpp"
#include "capture_service_cpp/timing/affine_device_clock_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace xr_capture_cpp {
namespace {


namespace fs = std::filesystem;

bool magic_at(const std::vector<uint8_t>& buffer,
              size_t offset,
              const std::array<uint8_t, 4>& magic) {
  return offset + magic.size() <= buffer.size() &&
         std::equal(magic.begin(), magic.end(),
                    buffer.begin() + static_cast<std::ptrdiff_t>(offset));
}

size_t next_xr_controller_magic(const std::vector<uint8_t>& buffer) {
  for (size_t offset = 0; offset + 4 <= buffer.size(); ++offset) {
    if (magic_at(buffer, offset, kXrControllerV1Magic) ||
        magic_at(buffer, offset, kXrControllerIdentityV1Magic)) {
      return offset;
    }
  }
  return std::string::npos;
}

std::optional<std::string> identity_uid_from_buffer(std::vector<uint8_t>& buffer) {
  while (buffer.size() >= 4) {
    const size_t offset = next_xr_controller_magic(buffer);
    if (offset == std::string::npos) {
      const size_t keep = std::min<size_t>(buffer.size(), 3);
      buffer.erase(buffer.begin(),
                   buffer.end() - static_cast<std::ptrdiff_t>(keep));
      return std::nullopt;
    }
    if (offset > 0) {
      buffer.erase(buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    if (magic_at(buffer, 0, kXrControllerV1Magic)) {
      if (buffer.size() < kXrControllerV1PacketSize) return std::nullopt;
      buffer.erase(buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(kXrControllerV1PacketSize));
      continue;
    }
    if (buffer.size() < kXrControllerIdentityV1PacketSize) return std::nullopt;
    XrControllerIdentityV1 identity;
    if (!decode_xr_controller_identity_v1(
            buffer.data(), kXrControllerIdentityV1PacketSize, identity)) {
      buffer.erase(buffer.begin());
      continue;
    }
    buffer.erase(buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(
                     kXrControllerIdentityV1PacketSize));
    return xr_controller_device_uid_hex(identity);
  }
  return std::nullopt;
}

std::vector<std::string> serial_uid_candidates(const std::string& configured_port) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  auto add = [&](const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    const std::string key = ec ? path : canonical.string();
    if (seen.insert(key).second) out.push_back(path);
  };
  add(configured_port);
#ifndef _WIN32
  std::error_code ec;
  for (const char* directory : {"/dev/serial/by-id", "/dev"}) {
    if (!fs::exists(directory, ec)) continue;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
      if (ec) break;
      const std::string name = entry.path().filename().string();
      if (std::string(directory) == "/dev" && name.rfind("ttyACM", 0) != 0) {
        continue;
      }
      add(entry.path().string());
    }
  }
#else
  for (int index = 1; index <= 32; ++index) add("COM" + std::to_string(index));
#endif
  return out;
}

std::optional<std::string> probe_xr_controller_uid(const std::string& port,
                                                   int baud_rate,
                                                   int timeout_ms) {
  SerialPort serial;
  try {
    serial.open(port, baud_rate);
  } catch (const std::exception&) {
    return std::nullopt;
  }
  std::vector<uint8_t> buffer;
  buffer.reserve(4096);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  std::array<uint8_t, 1024> chunk{};
  while (std::chrono::steady_clock::now() < deadline) {
    const int remaining = static_cast<int>(std::chrono::duration_cast<
        std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    const int count = serial.read_timeout(chunk.data(), chunk.size(),
                                          std::max(1, std::min(100, remaining)));
    if (count < 0) break;
    if (count > 0) {
      buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + count);
      if (auto uid = identity_uid_from_buffer(buffer)) {
        serial.close();
        return uid;
      }
    }
  }
  serial.close();
  return std::nullopt;
}

std::string resolve_xr_controller_port(const SerialImuConfig& serial_cfg) {
  const std::string expected =
      normalize_xr_controller_device_uid(serial_cfg.protocol_device_uid);
  if (serial_cfg.protocol_device_uid.empty()) return serial_cfg.port;
  if (expected.empty() || expected.size() > kXrControllerDeviceUidMaxSize * 2) {
    throw std::runtime_error(
        "imu.serial.protocol_device_uid must be an even-length hexadecimal UID");
  }

  std::vector<std::string> attempted;
  for (const std::string& candidate : serial_uid_candidates(serial_cfg.port)) {
    attempted.push_back(candidate);
    const auto uid = probe_xr_controller_uid(candidate,
                                             serial_cfg.baud_rate,
                                             1500);
    if (uid && *uid == expected) return candidate;
  }

  std::ostringstream message;
  message << "xr_controller_v1 device_uid=" << expected
          << " was not found";
  if (!attempted.empty()) {
    message << "; probed ports=";
    for (size_t i = 0; i < attempted.size(); ++i) {
      if (i) message << ',';
      message << attempted[i];
    }
  }
  throw std::runtime_error(message.str());
}

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
    expected_device_uid_ = normalize_xr_controller_device_uid(
        cfg_.imu.serial.protocol_device_uid);
    resolved_port_ = cfg_.imu.serial.protocol == "xr_controller_v1"
                         ? resolve_xr_controller_port(cfg_.imu.serial)
                         : cfg_.imu.serial.port;
    serial_.open(resolved_port_, cfg_.imu.serial.baud_rate);
    std::cerr << "[capture_service_cpp] imu source=serial port=" << resolved_port_
              << " baud=" << cfg_.imu.serial.baud_rate
              << " protocol=" << cfg_.imu.serial.protocol;
    if (!expected_device_uid_.empty()) {
      std::cerr << " protocol_device_uid=" << expected_device_uid_;
    }
    std::cerr << std::endl;
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
    while (buffer_.size() >= 4) {
      const size_t magic_offset = next_xr_controller_magic(buffer_);
      if (magic_offset == std::string::npos) {
        const size_t keep = std::min<size_t>(buffer_.size(), 3);
        buffer_.erase(buffer_.begin(),
                      buffer_.end() - static_cast<std::ptrdiff_t>(keep));
        ++resync_count_;
        return false;
      }
      if (magic_offset > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(magic_offset));
        ++resync_count_;
      }

      if (magic_at(buffer_, 0, kXrControllerIdentityV1Magic)) {
        if (buffer_.size() < kXrControllerIdentityV1PacketSize) return false;
        XrControllerIdentityV1 identity;
        if (!decode_xr_controller_identity_v1(
                buffer_.data(), kXrControllerIdentityV1PacketSize, identity)) {
          buffer_.erase(buffer_.begin());
          ++invalid_count_;
          continue;
        }
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(
                          kXrControllerIdentityV1PacketSize));
        const std::string uid = xr_controller_device_uid_hex(identity);
        if (!uid.empty()) {
          if (!expected_device_uid_.empty() && uid != expected_device_uid_) {
            throw std::runtime_error(
                "xr_controller_v1 UID changed or mismatched on port " +
                resolved_port_ + ": expected=" + expected_device_uid_ +
                " observed=" + uid);
          }
          if (observed_device_uid_ != uid) {
            observed_device_uid_ = uid;
            std::cerr << "[capture_service_cpp] xr_controller_v1 device_uid="
                      << uid << " port=" << resolved_port_ << std::endl;
          }
        } else if (!expected_device_uid_.empty()) {
          throw std::runtime_error(
              "xr_controller_v1 identity frame on port " + resolved_port_ +
              " does not contain a hardware device UID");
        }
        continue;
      }

      if (buffer_.size() < kXrControllerV1PacketSize) return false;
      XrControllerV1Sample decoded;
      XrControllerV1DecodeError error = XrControllerV1DecodeError::None;
      if (!decode_xr_controller_v1(buffer_.data(), kXrControllerV1PacketSize,
                                   decoded, &error)) {
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
            clock_mapper_.map_us(decoded.device_timestamp_us,
                                 result.receive_timestamp_ns);
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
  std::string resolved_port_;
  std::string expected_device_uid_;
  std::string observed_device_uid_;
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
