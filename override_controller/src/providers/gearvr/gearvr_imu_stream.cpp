#include "gearvr_imu_stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace xr_override_controller::gearvr {

void GearVrImuStream::reset() {
  processor_.reset();
  clock_.reset();
  pending_.clear();
  have_device_time_ = false;
  last_raw_us_ = 0;
  last_device_us_ = 0;
  last_host_ns_ = 0;
  latest = {};
  ++epoch_;
}

bool GearVrImuStream::push(const DecodedPacket& packet, uint64_t host_receive_ns) {
  using namespace xr_runtime;
  if (host_receive_ns == 0) return false;
  const uint32_t first_raw = packet.imu_samples.front().device_timestamp_us;
  const uint32_t newest_raw = packet.imu_samples.back().device_timestamp_us;
  const uint32_t packet_span_us = newest_raw - first_raw;
  // Reject malformed/reversed records, but accept a wrap inside a packet.
  if (packet_span_us == 0 || packet_span_us > 100'000u) return false;

  if (have_device_time_) {
    const uint32_t forward_us = newest_raw - last_raw_us_;
    if (forward_us == 0) return false;  // Duplicate notification.
    if (forward_us >= 0x80000000u) reset();  // Device clock restarted.
  }
  const bool had_previous = have_device_time_;
  const uint64_t previous_device_us = last_device_us_;
  const uint64_t newest_us = had_previous
      ? last_device_us_ + static_cast<uint32_t>(newest_raw - last_raw_us_)
      : static_cast<uint64_t>(first_raw) + packet_span_us;
  const std::array<uint64_t, 2> device_us{newest_us - packet_span_us, newest_us};
  const uint64_t newest_host_ns = clock_.map_us(newest_us, host_receive_ns);
  const double scale = clock_.diagnostics().scale;
  std::array<uint64_t, 2> host_ns{};
  uint64_t previous_host_ns = last_host_ns_;
  for (size_t i = 0; i < device_us.size(); ++i) {
    if (had_previous && device_us[i] <= previous_device_us) continue;
    const uint64_t delta_ns = static_cast<uint64_t>(
        std::llround((newest_us - device_us[i]) * 1000.0 * scale));
    host_ns[i] = newest_host_ns > delta_ns ? newest_host_ns - delta_ns : 1;
    // A refit can shift the model backwards slightly. Preserve ordering
    // without changing the AHRS interval, which follows the device clock.
    host_ns[i] = std::max(host_ns[i], previous_host_ns + 1);
    if (host_ns[i] > host_receive_ns) {
      // Validate the whole batch before modifying AHRS/sample state.
      ++dropped_batches_;
      latest.reserved[CONTROLLER_IMU_DIAG_DROPPED_BATCHES] = dropped_batches_;
      return false;
    }
    previous_host_ns = host_ns[i];
  }

  ControllerImuStateV1 state{};
  state.status = CONTROLLER_IMU_ACTIVE;
  state.capability_flags = CONTROLLER_IMU_CAP_GYROSCOPE |
      CONTROLLER_IMU_CAP_ACCELEROMETER | CONTROLLER_IMU_CAP_MAGNETOMETER |
      CONTROLLER_IMU_CAP_ORIENTATION | CONTROLLER_IMU_CAP_DEVICE_TIMESTAMP |
      CONTROLLER_IMU_CAP_MULTI_SAMPLE_PACKET | CONTROLLER_IMU_CAP_STREAM_DIAGNOSTICS;
  state.data_flags = CONTROLLER_IMU_GYROSCOPE_VALID |
      CONTROLLER_IMU_ACCELEROMETER_VALID | CONTROLLER_IMU_MAGNETOMETER_VALID |
      CONTROLLER_IMU_ORIENTATION_VALID | CONTROLLER_IMU_HOST_TIME_SYNCED;
  state.sequence = ++sequence_;
  uint64_t previous_us = previous_device_us;
  bool have_previous = had_previous;
  for (size_t i = 0; i < device_us.size(); ++i) {
    // Some notifications overlap the preceding one. Process each device
    // sample once; do not calibrate or integrate duplicated records.
    if (have_previous && device_us[i] <= previous_us) continue;
    imu::RawControllerImuSample raw;
    raw.host_timestamp_ns = host_ns[i];
    raw.device_timestamp_ticks = device_us[i];
    if (have_previous) raw.sample_interval_seconds =
        static_cast<float>(device_us[i] - previous_us) / 1.0e6f;
    raw.angular_velocity_rad_s = packet.imu_samples[i].gyro_rad_s;
    raw.specific_force_m_s2 = packet.imu_samples[i].accel_m_s2;
    raw.magnetic_field_uT = packet.magnetic_uT;
    raw.gyroscope_valid = raw.accelerometer_valid = raw.magnetometer_valid = true;
    const auto orientation = processor_.process(raw);
    const auto& corrected = processor_.corrected_sample();
    auto& sample = state.samples[state.sample_count++];
    sample.timestamp_ns = host_ns[i];
    sample.device_timestamp_ticks = device_us[i];
    sample.angular_velocity_rad_s[0] = corrected.angular_velocity_rad_s.x;
    sample.angular_velocity_rad_s[1] = corrected.angular_velocity_rad_s.y;
    sample.angular_velocity_rad_s[2] = corrected.angular_velocity_rad_s.z;
    sample.specific_force_m_s2[0] = corrected.specific_force_m_s2.x;
    sample.specific_force_m_s2[1] = corrected.specific_force_m_s2.y;
    sample.specific_force_m_s2[2] = corrected.specific_force_m_s2.z;
    sample.flags = CONTROLLER_IMU_GYROSCOPE_VALID |
        CONTROLLER_IMU_ACCELEROMETER_VALID | CONTROLLER_IMU_HOST_TIME_SYNCED;
    if (processor_.gyro_calibrated()) sample.flags |= CONTROLLER_IMU_GYROSCOPE_CALIBRATED;
    state.orientation_xyzw[0] = orientation.x;
    state.orientation_xyzw[1] = orientation.y;
    state.orientation_xyzw[2] = orientation.z;
    state.orientation_xyzw[3] = orientation.w;
    last_host_ns_ = host_ns[i];
    previous_us = device_us[i];
    have_previous = true;
  }
  if (state.sample_count == 0) return false;
  have_device_time_ = true;
  last_raw_us_ = newest_raw;
  last_device_us_ = newest_us;
  state.latest_sample_timestamp_ns = last_host_ns_;
  state.orientation_timestamp_ns = last_host_ns_;
  state.latest_device_timestamp_ticks = newest_us;
  if (processor_.gyro_calibrated()) state.data_flags |= CONTROLLER_IMU_GYROSCOPE_CALIBRATED;
  state.magnetic_field_uT[0] = packet.magnetic_uT.x;
  state.magnetic_field_uT[1] = packet.magnetic_uT.y;
  state.magnetic_field_uT[2] = packet.magnetic_uT.z;
  if (pending_.size() == kMaxPendingBatches) {
    pending_.pop_front();
    ++dropped_batches_;
  }
  state.reserved[CONTROLLER_IMU_DIAG_EPOCH] = epoch_;
  state.reserved[CONTROLLER_IMU_DIAG_DROPPED_BATCHES] = dropped_batches_;
  state.reserved[CONTROLLER_IMU_DIAG_TICKS_PER_SECOND] = 1'000'000;
  latest = state;
  pending_.push_back(state);
  return true;
}

xr_runtime::ControllerImuStateV1 GearVrImuStream::consume() {
  if (pending_.empty()) return latest;
  auto result = pending_.front();
  pending_.pop_front();
  // Surface an overflow on the next consumed batch, even when it predates it.
  result.reserved[xr_runtime::CONTROLLER_IMU_DIAG_DROPPED_BATCHES] = dropped_batches_;
  return result;
}

}  // namespace xr_override_controller::gearvr
