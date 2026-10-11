#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <capture_client/transports/transport.hpp>

namespace capture_client {

inline int64_t shift_camera_timestamp_ns(int64_t timestamp_ns, int64_t offset_ns) {
  if ((offset_ns > 0 && timestamp_ns > std::numeric_limits<int64_t>::max() - offset_ns) ||
      (offset_ns < 0 && timestamp_ns < std::numeric_limits<int64_t>::min() - offset_ns)) {
    throw std::overflow_error("camera timestamp plus calibration offset overflows int64");
  }
  const int64_t shifted = timestamp_ns + offset_ns;
  if (shifted < 0) throw std::runtime_error("camera calibration offset produces a negative timestamp");
  return shifted;
}

struct StereoPair {
  int64_t timestamp_ns = 0;
  ImageFrame cam0;
  ImageFrame cam1;

  uint64_t sequence() const { return std::min(cam0.sequence, cam1.sequence); }
  int64_t timestamp_delta_ns() const { return cam0.timestamp_ns - cam1.timestamp_ns; }
};

struct SyncedStereoImu {
  StereoPair pair;
  std::vector<ImuSample> imu_samples;
  std::optional<int64_t> previous_camera_timestamp_ns;

  double latest_imu_delta_ms() const {
    if (imu_samples.empty()) return 0.0;
    return double(imu_samples.back().timestamp_ns - pair.timestamp_ns) / 1e6;
  }
};

class StereoImuSynchronizer {
 public:
  StereoImuSynchronizer(ICaptureTransport& transport, int64_t max_stereo_delta_ns = 1'000'000,
                        double wait_for_imu_s = 0.05, bool use_imu = true,
                        int64_t camera_time_offset_ns = 0, bool include_imu_after_frame = false)
      : transport_(transport), max_stereo_delta_ns_(max_stereo_delta_ns),
        wait_for_imu_s_(wait_for_imu_s), use_imu_(use_imu),
        camera_time_offset_ns_(camera_time_offset_ns), include_imu_after_frame_(include_imu_after_frame) {
    last_pair_seq_ = std::min(transport_.cam0().latest_sequence(), transport_.cam1().latest_sequence());
    if (use_imu_) last_imu_seq_ = transport_.imu().latest_sequence();
  }

  std::optional<SyncedStereoImu> read_next(double timeout_s = 1.0) {
    auto pair = read_next_pair(timeout_s);
    if (!pair) return std::nullopt;
    SyncedStereoImu out;
    out.previous_camera_timestamp_ns = previous_camera_timestamp_ns_;
    out.pair = std::move(*pair);
    if (use_imu_) {
      wait_until_imu_at_least(out.pair.timestamp_ns, wait_for_imu_s_);
      out.imu_samples = read_imu_window_until(out.pair.timestamp_ns);
    }
    previous_camera_timestamp_ns_ = out.pair.timestamp_ns;
    return out;
  }

 private:
  std::optional<StereoPair> read_next_pair(double timeout_s) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      const uint64_t latest0 = transport_.cam0().latest_sequence();
      const uint64_t latest1 = transport_.cam1().latest_sequence();
      const uint64_t target = std::min(latest0, latest1);

      if (target <= last_pair_seq_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      for (uint64_t seq = last_pair_seq_ + 1; seq <= target; ++seq) {
        ImageFrame c0, c1;
        if (!transport_.cam0().read_image_sequence(seq, c0)) continue;
        if (!transport_.cam1().read_image_sequence(seq, c1)) continue;
        last_pair_seq_ = seq;
        const int64_t delta = c0.timestamp_ns - c1.timestamp_ns;
        if (std::llabs(delta) <= max_stereo_delta_ns_) {
          StereoPair p;
          // Kalibr/Basalt convention: t_imu = t_camera + offset. Shift here,
          // before waiting for and selecting the IMU window, exactly once.
          p.timestamp_ns = shift_camera_timestamp_ns(
              std::max(c0.timestamp_ns, c1.timestamp_ns), camera_time_offset_ns_);
          p.cam0 = std::move(c0);
          p.cam1 = std::move(c1);
          return p;
        }
      }

      last_pair_seq_ = target;
    }
    return std::nullopt;
  }

  void wait_until_imu_at_least(int64_t timestamp_ns, double timeout_s) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      ImuSample s;
      if (transport_.imu().read_latest_imu(s) &&
          (include_imu_after_frame_ ? s.timestamp_ns > timestamp_ns : s.timestamp_ns >= timestamp_ns)) return;
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  }

  std::vector<ImuSample> read_imu_window_until(int64_t timestamp_ns) {
    std::vector<ImuSample> samples;
    const uint64_t latest = transport_.imu().latest_sequence();
    if (latest <= last_imu_seq_) return samples;

    uint64_t new_last = last_imu_seq_;
    for (uint64_t seq = last_imu_seq_ + 1; seq <= latest; ++seq) {
      ImuSample s;
      if (!transport_.imu().read_imu_sequence(seq, s)) continue;
      if (s.timestamp_ns <= timestamp_ns || include_imu_after_frame_) {
        samples.push_back(s);
        new_last = seq;
        // Basalt needs the first sample strictly after the frame to finish
        // interpolation/preintegration without waiting for the next image.
        if (s.timestamp_ns > timestamp_ns) break;
      } else {
        break;
      }
    }
    last_imu_seq_ = std::max(last_imu_seq_, new_last);
    return samples;
  }

  ICaptureTransport& transport_;
  int64_t max_stereo_delta_ns_ = 1'000'000;
  double wait_for_imu_s_ = 0.05;
  bool use_imu_ = true;
  int64_t camera_time_offset_ns_ = 0;
  bool include_imu_after_frame_ = false;
  uint64_t last_pair_seq_ = 0;
  uint64_t last_imu_seq_ = 0;
  std::optional<int64_t> previous_camera_timestamp_ns_;
};

}  // namespace capture_client
