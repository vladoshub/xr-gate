#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <thread>

#include <capture_client/sync/synchronizer.hpp>

namespace capture_client {

// Reader for consumers that need only stereo images, not IMU.
// Intended for hand tracking / debug tools.
//
// Policy:
// - latest_only=true: skip backlog and return the newest valid stereo pair;
// - latest_only=false: walk frames sequentially.
// This reader does not block or depend on IMU.
class LatestStereoReader {
 public:
  LatestStereoReader(ICaptureTransport& transport,
                     int64_t max_stereo_delta_ns = 1'000'000,
                     bool latest_only = true,
                     uint64_t max_scan_back = 8)
      : transport_(transport),
        max_stereo_delta_ns_(max_stereo_delta_ns),
        latest_only_(latest_only),
        max_scan_back_(std::max<uint64_t>(1, max_scan_back)) {
    last_pair_seq_ = std::min(transport_.cam0().latest_sequence(),
                              transport_.cam1().latest_sequence());
  }

  std::optional<StereoPair> read_next(double timeout_s = 1.0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);

    while (std::chrono::steady_clock::now() < deadline) {
      const uint64_t latest0 = transport_.cam0().latest_sequence();
      const uint64_t latest1 = transport_.cam1().latest_sequence();
      const uint64_t target = std::min(latest0, latest1);

      if (target <= last_pair_seq_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      if (latest_only_) {
        const uint64_t min_seq = (target > max_scan_back_) ? (target - max_scan_back_ + 1) : 1;

        for (uint64_t seq = target; seq >= min_seq && seq > last_pair_seq_; --seq) {
          auto pair = read_pair_sequence(seq);
          if (pair) {
            // Drop all older frames. This is intentional for low-latency hand tracking.
            last_pair_seq_ = target;
            return pair;
          }
          if (seq == 0) break;
        }

        // Could not read a valid recent stereo pair. Move forward to avoid accumulating backlog.
        last_pair_seq_ = target;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      for (uint64_t seq = last_pair_seq_ + 1; seq <= target; ++seq) {
        auto pair = read_pair_sequence(seq);
        last_pair_seq_ = seq;
        if (pair) return pair;
      }
    }

    return std::nullopt;
  }

  uint64_t last_pair_sequence() const { return last_pair_seq_; }

 private:
  std::optional<StereoPair> read_pair_sequence(uint64_t seq) {
    ImageFrame c0, c1;
    if (!transport_.cam0().read_image_sequence(seq, c0)) return std::nullopt;
    if (!transport_.cam1().read_image_sequence(seq, c1)) return std::nullopt;

    const int64_t delta = c0.timestamp_ns - c1.timestamp_ns;
    if (std::llabs(delta) > max_stereo_delta_ns_) return std::nullopt;

    StereoPair p;
    p.timestamp_ns = std::max(c0.timestamp_ns, c1.timestamp_ns);
    p.cam0 = std::move(c0);
    p.cam1 = std::move(c1);
    return p;
  }

  ICaptureTransport& transport_;
  int64_t max_stereo_delta_ns_ = 1'000'000;
  bool latest_only_ = true;
  uint64_t max_scan_back_ = 8;
  uint64_t last_pair_seq_ = 0;
};

}  // namespace capture_client
