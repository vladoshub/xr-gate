#pragma once

#include "capture_service_cpp/common.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace xr_capture_cpp {

struct ImuSample {
  std::array<float, 3> gyro_rad_s{};
  std::array<float, 3> accel_m_s2{};
  uint64_t timestamp_ns = 0;
  uint32_t source_sequence = 0;
  bool source_timestamp_valid = false;
};

struct ImuReadResult {
  bool has_sample = false;
  ImuSample sample;
  std::vector<uint8_t> raw_packet;
  uint64_t receive_timestamp_ns = 0;
};

class IImuSource {
 public:
  virtual ~IImuSource() = default;
  virtual std::string name() const = 0;
  virtual std::vector<StreamSpec> stream_specs(uint32_t slot_count, uint32_t raw_slot_count) const = 0;
  virtual void open() = 0;
  virtual SourceReadStatus read(ImuReadResult& result) = 0;
  virtual void close() = 0;
};

std::unique_ptr<IImuSource> create_imu_source(const RuntimeConfig& cfg);

}  // namespace xr_capture_cpp
