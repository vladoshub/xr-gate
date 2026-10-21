#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace xr_override_controller::gearvr {

// Command completion is a controller notification, not the GATT write reply.
// Keep protocol timing separate from D-Bus so late replies and packet loss can
// be replayed without hardware. All timestamps use the host monotonic clock.
class SensorSession {
 public:
  enum class Action { None, StartSensor, RestartNotifications };
  static constexpr uint64_t kModeAckWaitNs = 3'000'000'000ull;
  static constexpr uint64_t kCommandGapNs = 20'000'000ull;
  static constexpr uint64_t kSensorTimeoutNs = 2'000'000'000ull;

  void reset() { *this = SensorSession{}; }

  void mode_requested(uint64_t now_ns) {
    reset();
    active_ = true;
    requested_generation_ = 1;
    sensor_due_ns_ = now_ns + kModeAckWaitNs;
  }

  bool mode_reply(const uint8_t* bytes, size_t size, uint64_t now_ns) {
    if (!active_ || mode_ack_seen_ || !bytes || size != 2 ||
        bytes[0] != 0x08 || bytes[1] != 0x00) return false;
    mode_ack_seen_ = true;
    ++requested_generation_;
    sensor_due_ns_ = now_ns + kCommandGapNs;
    return true;
  }

  void sensor_frame(uint64_t now_ns) {
    if (!active_) return;
    sensor_deadline_ns_ = now_ns + kSensorTimeoutNs;
    stall_retry_used_ = false;
  }

  Action next_action(uint64_t now_ns) const {
    if (!active_ || now_ns < retry_after_ns_) return Action::None;
    if (requested_generation_ > completed_generation_) {
      return now_ns >= sensor_due_ns_ ? Action::StartSensor : Action::None;
    }
    if (!sensor_started_ || now_ns < sensor_deadline_ns_) return Action::None;
    return stall_retry_used_ ? Action::RestartNotifications : Action::StartSensor;
  }

  uint64_t request_generation() const { return requested_generation_; }
  bool sensor_started() const { return sensor_started_; }
  bool mode_ack_seen() const { return mode_ack_seen_; }

  void sensor_write_succeeded(uint64_t now_ns, uint64_t generation) {
    stall_retry_used_ = generation <= completed_generation_;
    completed_generation_ = std::max(completed_generation_, generation);
    sensor_started_ = true;
    sensor_deadline_ns_ = now_ns + kSensorTimeoutNs;
    retry_after_ns_ = 0;
    // A late mode ack can arrive inside sd_bus_call(). Its newer generation
    // remains pending, so this earlier write cannot accidentally consume it.
  }

  void sensor_write_failed(uint64_t now_ns, uint64_t retry_delay_ns) {
    retry_after_ns_ = now_ns + retry_delay_ns;
  }

  uint64_t next_deadline_ns() const {
    if (!active_) return 0;
    const uint64_t deadline = requested_generation_ > completed_generation_
        ? sensor_due_ns_ : sensor_deadline_ns_;
    return std::max(deadline, retry_after_ns_);
  }

 private:
  bool active_ = false;
  bool mode_ack_seen_ = false;
  bool sensor_started_ = false;
  bool stall_retry_used_ = false;
  uint64_t requested_generation_ = 0;
  uint64_t completed_generation_ = 0;
  uint64_t sensor_due_ns_ = 0;
  uint64_t sensor_deadline_ns_ = 0;
  uint64_t retry_after_ns_ = 0;
};

}  // namespace xr_override_controller::gearvr
