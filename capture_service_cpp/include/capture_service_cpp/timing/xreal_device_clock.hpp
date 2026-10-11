#pragma once

#include "capture_service_cpp/timing/affine_device_clock_mapper.hpp"

#include <cstdint>
#include <mutex>
#include <optional>

namespace xr_capture_cpp {

// One instance per XREAL camera/IMU rig. Only validated HID motion reports
// train the model; camera timestamps are projected through that same model.
// This retains relative sensor timing despite different HID/UVC USB delays.
class XrealDeviceClock {
 public:
  std::optional<uint64_t> map_imu_ns(uint64_t device_ns, uint64_t host_receive_ns);
  std::optional<uint64_t> map_camera_ns(uint64_t device_ns, uint64_t host_receive_ns) const;
  AffineDeviceClockMapper::Diagnostics diagnostics() const;

 private:
  mutable std::mutex mutex_;
  AffineDeviceClockMapper mapper_;
  uint64_t last_imu_device_ns_ = 0;
};

}  // namespace xr_capture_cpp
