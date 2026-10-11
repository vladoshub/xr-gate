#include "capture_service_cpp/timing/xreal_device_clock.hpp"

#include <limits>
#include <stdexcept>

namespace xr_capture_cpp {

std::optional<uint64_t> XrealDeviceClock::map_imu_ns(uint64_t device_ns,
                                                  uint64_t host_receive_ns) {
  if (device_ns == 0 || device_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_imu_device_ns_ && device_ns < last_imu_device_ns_) {
    // A silent reset would mix two clock epochs in a running VIO estimator.
    throw std::runtime_error("XREAL IMU clock moved backwards; restart capture and VIO");
  }
  if (device_ns == last_imu_device_ns_) return std::nullopt;
  last_imu_device_ns_ = device_ns;
  return mapper_.map_ns(device_ns, host_receive_ns);
}

std::optional<uint64_t> XrealDeviceClock::map_camera_ns(uint64_t device_ns,
                                                     uint64_t host_receive_ns) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!last_imu_device_ns_ || !device_ns) return std::nullopt;
  const uint64_t clock_delta = device_ns > last_imu_device_ns_
      ? device_ns - last_imu_device_ns_ : last_imu_device_ns_ - device_ns;
  // Detect wrong units/epochs instead of publishing host receive time as if
  // it were a valid device timestamp. This is not an exposure correction.
  constexpr uint64_t kMaxClockDeltaNs = 2000000000ULL;
  if (clock_delta > kMaxClockDeltaNs) return std::nullopt;
  const auto mapped = mapper_.project_ns(device_ns);
  if (!mapped) return std::nullopt;
  const uint64_t age = *mapped > host_receive_ns
      ? *mapped - host_receive_ns : host_receive_ns - *mapped;
  if (age > kMaxClockDeltaNs) return std::nullopt;
  return mapped;
}

AffineDeviceClockMapper::Diagnostics XrealDeviceClock::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mapper_.diagnostics();
}

}  // namespace xr_capture_cpp
