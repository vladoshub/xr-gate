#include "capture_service_cpp/sources/imu_source.hpp"

#include "capture_service_cpp/backend_control.hpp"
#include "capture_service_cpp/platform/hid_input_device.hpp"
#include "capture_service_cpp/vendor/xreal_imu_codec.hpp"
#include "capture_service_cpp/timing/xreal_device_clock.hpp"

#include <array>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace xr_capture_cpp {
namespace {

class XrealHidImuSource final : public IImuSource {
 public:
  explicit XrealHidImuSource(RuntimeConfig cfg, std::shared_ptr<XrealDeviceClock> clock)
      : cfg_(std::move(cfg)),
        clock_(std::move(clock)),
        backend_control_(cfg_.backend_control_file, cfg_.backend_control_poll_ms) {}

  std::string name() const override { return "xreal_hid"; }

  std::vector<StreamSpec> stream_specs(uint32_t slot_count, uint32_t raw_slot_count) const override {
    std::vector<StreamSpec> specs;
    if (cfg_.imu.raw_enabled) {
      specs.push_back(StreamSpec{cfg_.imu.raw_stream_id, kKindBytes, "BYTES", 0, 0, kFormatBytes, "BYTES",
                                 static_cast<uint32_t>(cfg_.imu.raw_payload_size), raw_slot_count,
                                 cfg_.imu.raw_frame_id, "Raw IMU source packet"});
    }
    specs.push_back(StreamSpec{cfg_.imu.stream_id, kKindImu, "IMU", 0, 0, kFormatImuF32Le, "IMU_F32_LE",
                               24, slot_count, cfg_.imu.frame_id,
                               "Normalized gyro rad/s and accelerometer m/s^2 sample"});
    return specs;
  }

  void open() override {
    if (cfg_.imu.xreal_hid.timestamp_mode == "device" && !clock_) {
      throw std::runtime_error("XREAL device IMU timestamp mode requires a clock mapper");
    }
    backend_control_.poll_if_due(steady_ns());
    const auto& x = cfg_.imu.xreal_hid;
    device_.open_interface(x.vendor_id, x.product_id, x.interface_number, "XREAL HID IMU interface");
    const auto& start_cmd = xreal_imu_start_command();
    const int written = device_.write(start_cmd.data(), start_cmd.size());
    std::cerr << "[capture_service_cpp] imu source=xreal_hid start command result=" << written << std::endl;
    if (written < 0) throw std::runtime_error("XREAL HID IMU start command failed");
  }

  SourceReadStatus read(ImuReadResult& result) override {
    result = {};
    const int count = device_.read_timeout(packet_.data(), packet_.size(), cfg_.imu.xreal_hid.read_timeout_ms);
    if (count < 0) return SourceReadStatus::EndOfStream;
    if (count == 0) return SourceReadStatus::Timeout;

    result.receive_timestamp_ns = steady_ns();
    backend_control_.poll_if_due(result.receive_timestamp_ns);
    const size_t raw_size = std::min(static_cast<size_t>(count), cfg_.imu.raw_payload_size);
    result.raw_packet.assign(packet_.begin(), packet_.begin() + raw_size);
    ++raw_count_;

    float payload[6]{};
    if (!normalize_xreal_imu_packet(
            packet_.data(), static_cast<size_t>(count),
            backend_control_.snapshot().gravity_magnitude, payload)) {
      return SourceReadStatus::TransportActivity;
    }
    uint64_t sample_time = result.receive_timestamp_ns;
    const bool device_time = cfg_.imu.xreal_hid.timestamp_mode == "device";
    if (device_time) {
      uint64_t device_ns = 0;
      if (!decode_xreal_imu_timestamp(packet_.data(), static_cast<size_t>(count), device_ns)) {
        return SourceReadStatus::TransportActivity;
      }
      const auto mapped = clock_->map_imu_ns(device_ns, result.receive_timestamp_ns);
      if (!mapped) return SourceReadStatus::TransportActivity;
      sample_time = *mapped;
    }
    // Train during startup drops too, so UVC can use the established model.
    if (raw_count_ <= static_cast<uint64_t>(std::max(0, cfg_.imu.xreal_hid.drop_first_packets))) {
      return SourceReadStatus::Data;
    }
    for (int i = 0; i < 3; ++i) {
      result.sample.gyro_rad_s[static_cast<size_t>(i)] = payload[i];
      result.sample.accel_m_s2[static_cast<size_t>(i)] = payload[i + 3];
    }
    result.sample.timestamp_ns = sample_time;
    result.sample.source_sequence = static_cast<uint32_t>(raw_count_);
    result.sample.source_timestamp_valid = device_time;
    result.has_sample = true;
    ++sample_count_;
    if (sample_count_ % 1000 == 0) {
      std::cerr << "[capture_service_cpp] imu source=xreal_hid raw=" << raw_count_
                << " normalized=" << sample_count_ << " report_bytes=" << count
                << " timestamp_mode=" << cfg_.imu.xreal_hid.timestamp_mode;
      if (device_time) {
        const auto d = clock_->diagnostics();
        std::cerr << " clock_anchors=" << d.anchor_count << " clock_drift_ppm=" << d.drift_ppm;
      }
      std::cerr << std::endl;
    }
    return SourceReadStatus::Data;
  }

  void close() override {}

 private:
  RuntimeConfig cfg_;
  std::shared_ptr<XrealDeviceClock> clock_;
  BackendControlReader backend_control_;
  HidInputDevice device_;
  std::array<uint8_t, kXrealHidMaxPacketSize> packet_{};
  uint64_t raw_count_ = 0;
  uint64_t sample_count_ = 0;
};

}  // namespace

std::unique_ptr<IImuSource> make_xreal_hid_imu_source(
    const RuntimeConfig& cfg, std::shared_ptr<XrealDeviceClock> clock) {
  return std::make_unique<XrealHidImuSource>(cfg, std::move(clock));
}

}  // namespace xr_capture_cpp
