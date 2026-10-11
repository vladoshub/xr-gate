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

  bool push(const DecodedPacket& packet, uint64_t host_receive_ns);
  xr_runtime::ControllerImuStateV1 consume();
  bool pending() const { return !pending_.empty(); }
  void clear_pending() { pending_.clear(); }
  void reset();
  uint32_t dropped_batches() const { return dropped_batches_; }
  xr_tracking::AffineDeviceClockMapper::Diagnostics clock_diagnostics() const {
    return clock_.diagnostics();
  }
  xr_runtime::ControllerImuStateV1 latest{};

 private:
  imu::ControllerImuProcessor processor_;
  xr_tracking::AffineDeviceClockMapper clock_;
  std::deque<xr_runtime::ControllerImuStateV1> pending_;
  bool have_device_time_ = false;
  uint32_t last_raw_us_ = 0;
  uint64_t last_device_us_ = 0;
  uint64_t last_host_ns_ = 0;
  uint64_t sequence_ = 0;
  uint32_t epoch_ = 1;
  uint32_t dropped_batches_ = 0;
};

}  // namespace xr_override_controller::gearvr
