#pragma once

#include <xr_tracking/timing/affine_device_clock_mapper.hpp>

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace xr_tracking {

// One clock per physical serial IMU. Observe only the newest acquisition time
// in a read, then preserve device intervals for all earlier packets in it.
// Sequence wrap is normal; rollback, time-mode changes and long gaps start a
// new epoch. Returned indices always belong to the final epoch in the read.
class SerialImuClock {
 public:
  struct Sample {
    uint32_t sequence = 0;
    uint64_t device_us = 0;
    bool device_time_valid = false;
  };
  struct MappedSample {
    size_t index = 0;
    uint64_t host_ns = 0;
    float device_interval_seconds = 0.0f;
    bool device_time_valid = false;
  };

  void reset() {
    clock_.reset();
    have_sequence_ = have_mode_ = have_device_time_ = false;
    last_device_us_ = last_host_ns_ = 0;
    ++epoch_;
  }
  uint32_t epoch() const { return epoch_; }
  uint32_t dropped_samples() const { return dropped_; }
  AffineDeviceClockMapper::Diagnostics diagnostics() const { return clock_.diagnostics(); }

  std::vector<MappedSample> map_batch(const std::vector<Sample>& samples,
                                     uint64_t receive_ns) {
    std::vector<MappedSample> out;
    if (!receive_ns) return out;
    for (size_t i = 0; i < samples.size(); ++i) {
      const auto& sample = samples[i];
      const bool representable = !sample.device_time_valid ||
          sample.device_us <= std::numeric_limits<uint64_t>::max() / 1000ULL;
      const bool rollback = representable && have_device_time_ && sample.device_time_valid &&
          sample.device_us < last_device_us_;
      const uint32_t delta = sample.sequence - last_sequence_;
      if (rollback || (have_sequence_ && delta > 0x80000000u) ||
          (representable && have_mode_ && device_mode_ != sample.device_time_valid) ||
          (representable && have_device_time_ && sample.device_time_valid &&
           sample.device_us > last_device_us_ &&
           sample.device_us - last_device_us_ > kMaxContinuousGapUs)) {
        reset();
        out.clear();
      }
      if (have_sequence_ && delta == 0) continue;
      if (have_sequence_ && delta > 1) dropped_ += delta - 1;
      last_sequence_ = sample.sequence;
      have_sequence_ = true;
      // Track the rejected packet's sequence so the next valid packet does
      // not count the same loss again as a firmware sequence gap.
      if (!representable) {
        ++dropped_;
        continue;
      }
      have_mode_ = true;
      device_mode_ = sample.device_time_valid;
      // Repeated acquisition ticks must never be integrated twice, even if
      // firmware retransmits them with a new sequence number.
      if (have_device_time_ && sample.device_time_valid &&
          sample.device_us == last_device_us_) continue;
      const float interval = have_device_time_ && sample.device_time_valid
          ? static_cast<float>(sample.device_us - last_device_us_) / 1.0e6f : 0.0f;
      out.push_back({i, 0, interval, sample.device_time_valid});
      have_device_time_ = sample.device_time_valid;
      last_device_us_ = sample.device_us;
    }
    if (out.empty()) return out;
    if (!out.back().device_time_valid) {
      // A read timestamp cannot recover several unknown acquisition times.
      // Keep only the newest unsynchronised sample; never invent future +1 ns
      // timestamps. The caller must not set HOST_TIME_SYNCED for this fallback.
      dropped_ += static_cast<uint32_t>(out.size() - 1);
      const auto newest = out.back();
      out.assign(1, newest);
      if (receive_ns <= last_host_ns_) {
        clock_.reset();
        ++epoch_;
      }
      out.front().host_ns = receive_ns;
      last_host_ns_ = receive_ns;
      return out;
    }

    const uint64_t newest_us = samples[out.back().index].device_us;
    // If a clock refit/arrival discontinuity cannot fit this entire batch
    // after the previous one and before receipt, reanchor in a new epoch.
    // This preserves intervals instead of squeezing them to nanoseconds.
    for (int attempt = 0; attempt < 2; ++attempt) {
      const uint64_t newest_ns = clock_.map_us(newest_us, receive_ns);
      const long double scale = clock_.diagnostics().scale;
      bool fits = newest_ns <= receive_ns;
      uint64_t previous_ns = 0;
      for (auto& mapped : out) {
        const long double age_ns = static_cast<long double>(
            newest_us - samples[mapped.index].device_us) * 1000.0L * scale;
        if (age_ns >= newest_ns) { fits = false; break; }
        const uint64_t age = static_cast<uint64_t>(std::round(age_ns));
        mapped.host_ns = newest_ns - age;
        if (mapped.host_ns <= previous_ns) { fits = false; break; }
        previous_ns = mapped.host_ns;
      }
      if (fits && out.front().host_ns <= last_host_ns_) {
        const uint64_t shift = last_host_ns_ - out.front().host_ns + 1;
        fits = shift <= receive_ns - newest_ns;
        if (fits) for (auto& mapped : out) mapped.host_ns += shift;
      }
      if (fits) {
        last_host_ns_ = out.back().host_ns;
        return out;
      }
      clock_.reset();
      last_host_ns_ = 0;
      out.front().device_interval_seconds = 0.0f;
      ++epoch_;
    }
    dropped_ += static_cast<uint32_t>(out.size());
    reset();
    out.clear();
    return out;
  }

 private:
  static constexpr uint64_t kMaxContinuousGapUs = 250'000;
  AffineDeviceClockMapper clock_;
  bool have_sequence_ = false;
  bool have_mode_ = false;
  bool device_mode_ = false;
  bool have_device_time_ = false;
  uint32_t last_sequence_ = 0;
  uint64_t last_device_us_ = 0;
  uint64_t last_host_ns_ = 0;
  uint32_t epoch_ = 1;
  uint32_t dropped_ = 0;
};

}  // namespace xr_tracking
