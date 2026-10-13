#pragma once

#include "gearvr_protocol.hpp"

#include <xr_runtime/contracts/controller_input_contract.hpp>
#include <xr_tracking/timing/affine_device_clock_mapper.hpp>

#include <deque>

namespace xr_override_controller::gearvr {

// One queued state per BLE notification, retaining both sensor records and
// the orientation produced from that notification. This does not change V3.
class GearVrImuStream {
 public:
  static constexpr size_t kMaxPendingBatches = 256;
  explicit GearVrImuStream(float beta) : processor_(beta) {}

  void set_accelerometer_calibration(const imu::AccelerometerCalibration& calibration) {
    processor_.set_accelerometer_calibration(calibration);
    if (have_device_time_) reset();
  }
  const imu::AccelerometerCalibration& accelerometer_calibration() const {
    return processor_.accelerometer_calibration();
  }

  bool push(const DecodedPacket& packet, uint64_t host_receive_ns);
  xr_runtime::ControllerImuStateV1 consume();
  bool pending() const { return !pending_.empty(); }
  void clear_pending() { pending_.clear(); }
  // Stop advertising stale measurements without changing the AHRS reference,
  // calibrated bias or device/host clock mapping.
  void mark_stale();
  void reset();
  uint32_t epoch() const { return epoch_; }
  uint32_t dropped_batches() const { return dropped_batches_; }
  uint32_t clock_wraps() const { return clock_wraps_; }
  xr_tracking::AffineDeviceClockMapper::Diagnostics clock_diagnostics() const {
    return clock_.diagnostics();
  }
  xr_runtime::ControllerImuStateV1 latest{};

 private:
  static constexpr uint64_t kMaxUnobservedIntervalUs = 250'000;
  imu::ControllerImuProcessor processor_;
  xr_tracking::AffineDeviceClockMapper clock_;
  std::deque<xr_runtime::ControllerImuStateV1> pending_;
  bool have_device_time_ = false;
  uint32_t last_raw_us_ = 0;
  uint64_t last_device_us_ = 0;
  uint64_t last_host_ns_ = 0;
  uint64_t last_receive_ns_ = 0;
  uint64_t sequence_ = 0;
  uint32_t epoch_ = 1;
  uint32_t dropped_batches_ = 0;
  uint32_t clock_wraps_ = 0;
};

}  // namespace xr_override_controller::gearvr
