#include "capture_service_cpp/sources/imu_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace xr_capture_cpp {
namespace {

class SyntheticImuSource final : public IImuSource {
 public:
  explicit SyntheticImuSource(RuntimeConfig cfg) : cfg_(std::move(cfg)) {}

  std::string name() const override { return "synthetic"; }

  std::vector<StreamSpec> stream_specs(uint32_t slot_count, uint32_t) const override {
    return {StreamSpec{cfg_.imu.stream_id,
                       kKindImu,
                       "IMU",
                       0,
                       0,
                       kFormatImuF32Le,
                       "IMU_F32_LE",
                       24,
                       slot_count,
                       cfg_.imu.frame_id,
                       "Synthetic normalized gyro rad/s and accelerometer m/s^2 sample"}};
  }

  void open() override {
    const double period_ns_double = 1.0e9 / cfg_.imu.synthetic.rate_hz;
    period_ns_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(period_ns_double)));
    next_sample_ns_ = steady_ns();
    sequence_ = 0;
    std::cerr << "[capture_service_cpp] imu source=synthetic"
              << " rate_hz=" << cfg_.imu.synthetic.rate_hz
              << " gyro_rad_s=[" << cfg_.imu.synthetic.gyro_rad_s[0] << ','
              << cfg_.imu.synthetic.gyro_rad_s[1] << ','
              << cfg_.imu.synthetic.gyro_rad_s[2] << ']'
              << " accel_m_s2=[" << cfg_.imu.synthetic.accel_m_s2[0] << ','
              << cfg_.imu.synthetic.accel_m_s2[1] << ','
              << cfg_.imu.synthetic.accel_m_s2[2] << ']'
              << " timestamp_mode=" << cfg_.imu.synthetic.timestamp_mode
              << std::endl;
  }

  SourceReadStatus read(ImuReadResult& result) override {
    result = {};
    const uint64_t now_before_wait = steady_ns();
    if (next_sample_ns_ > now_before_wait) {
      const auto wake = std::chrono::steady_clock::time_point(
          std::chrono::nanoseconds(next_sample_ns_));
      std::this_thread::sleep_until(wake);
    }

    const uint64_t sample_timestamp_ns = next_sample_ns_;
    const uint64_t now = steady_ns();
    do {
      next_sample_ns_ += static_cast<uint64_t>(period_ns_);
    } while (next_sample_ns_ <= now);

    result.has_sample = true;
    result.receive_timestamp_ns = now;
    result.sample.gyro_rad_s = cfg_.imu.synthetic.gyro_rad_s;
    result.sample.accel_m_s2 = cfg_.imu.synthetic.accel_m_s2;
    result.sample.timestamp_ns = sample_timestamp_ns;
    result.sample.source_sequence = ++sequence_;
    result.sample.source_timestamp_valid = true;
    return SourceReadStatus::Data;
  }

  void close() override {}

 private:
  RuntimeConfig cfg_;
  uint64_t next_sample_ns_ = 0;
  int64_t period_ns_ = 0;
  uint32_t sequence_ = 0;
};

}  // namespace

std::unique_ptr<IImuSource> make_synthetic_imu_source(const RuntimeConfig& cfg) {
  return std::make_unique<SyntheticImuSource>(cfg);
}

}  // namespace xr_capture_cpp
