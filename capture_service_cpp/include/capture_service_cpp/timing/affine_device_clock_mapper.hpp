#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace xr_capture_cpp {

// Maps timestamps from an independent device clock into host steady-clock
// nanoseconds using an affine model:
//
//   host_ns = scale * device_ns + offset_ns
//
// Receive timestamps include non-negative transport delay. To avoid fitting
// ordinary USB/UART jitter, the mapper keeps the lowest-delay observation in
// each device-time window and performs a bounded least-squares fit over those
// lower-envelope anchors. The class is transport- and hardware-independent.
class AffineDeviceClockMapper {
 public:
  struct Diagnostics {
    bool initialized = false;
    double scale = 1.0;
    double drift_ppm = 0.0;
    double offset_ns = 0.0;
    size_t anchor_count = 0;
    uint64_t reset_count = 0;
  };

  uint64_t map_us(uint64_t device_timestamp_us, uint64_t host_receive_ns);
  uint64_t map_ns(uint64_t device_timestamp_ns, uint64_t host_receive_ns);
  void reset();
  Diagnostics diagnostics() const;

 private:
  struct Anchor {
    uint64_t device_ns = 0;
    uint64_t host_ns = 0;
  };

  void start_window(uint64_t device_ns, uint64_t host_ns);
  void update_window(uint64_t device_ns, uint64_t host_ns);
  void commit_window();
  void refit();

  static constexpr uint64_t kWindowNs = 250000000ULL;
  static constexpr size_t kMaxAnchors = 32;
  static constexpr double kMaxScaleError = 0.002;  // +/-2000 ppm safety bound.

  bool initialized_ = false;
  bool window_active_ = false;
  uint64_t window_start_device_ns_ = 0;
  Anchor window_best_{};
  std::deque<Anchor> anchors_;

  double scale_ = 1.0;
  double offset_ns_ = 0.0;
  uint64_t last_device_ns_ = 0;
  uint64_t last_mapped_ns_ = 0;
  uint64_t reset_count_ = 0;
};

}  // namespace xr_capture_cpp
