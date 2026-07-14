#include "capture_service_cpp/timing/affine_device_clock_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xr_capture_cpp {

uint64_t AffineDeviceClockMapper::map_us(uint64_t device_timestamp_us,
                                        uint64_t host_receive_ns) {
  if (device_timestamp_us > std::numeric_limits<uint64_t>::max() / 1000ULL) {
    reset();
    return host_receive_ns;
  }
  return map_ns(device_timestamp_us * 1000ULL, host_receive_ns);
}

uint64_t AffineDeviceClockMapper::map_ns(uint64_t device_ns, uint64_t host_receive_ns) {
  // A timestamp rollback normally means MCU reset/reconnect. Keep the mapper
  // self-contained so the serial source does not need device-specific logic.
  if (initialized_ && device_ns < last_device_ns_) reset();

  if (!initialized_) {
    initialized_ = true;
    scale_ = 1.0;
    offset_ns_ = static_cast<double>(host_receive_ns) - static_cast<double>(device_ns);
    start_window(device_ns, host_receive_ns);
  } else if (!window_active_) {
    start_window(device_ns, host_receive_ns);
  } else if (device_ns - window_start_device_ns_ >= kWindowNs) {
    commit_window();
    start_window(device_ns, host_receive_ns);
  } else {
    update_window(device_ns, host_receive_ns);
  }

  last_device_ns_ = device_ns;
  double mapped_d = scale_ * static_cast<double>(device_ns) + offset_ns_;
  // A sample cannot occur after the host received it. Correct the intercept
  // immediately rather than emitting a future timestamp after a noisy refit.
  if (mapped_d > static_cast<double>(host_receive_ns)) {
    offset_ns_ -= mapped_d - static_cast<double>(host_receive_ns);
    mapped_d = static_cast<double>(host_receive_ns);
  }

  uint64_t mapped = mapped_d > 0.0 ? static_cast<uint64_t>(mapped_d) : host_receive_ns;
  if (mapped <= last_mapped_ns_) mapped = last_mapped_ns_ + 1;
  last_mapped_ns_ = mapped;
  return mapped;
}

void AffineDeviceClockMapper::reset() {
  initialized_ = false;
  window_active_ = false;
  window_start_device_ns_ = 0;
  window_best_ = {};
  anchors_.clear();
  scale_ = 1.0;
  offset_ns_ = 0.0;
  last_device_ns_ = 0;
  last_mapped_ns_ = 0;
  ++reset_count_;
}

AffineDeviceClockMapper::Diagnostics AffineDeviceClockMapper::diagnostics() const {
  Diagnostics result;
  result.initialized = initialized_;
  result.scale = scale_;
  result.drift_ppm = (scale_ - 1.0) * 1000000.0;
  result.offset_ns = offset_ns_;
  result.anchor_count = anchors_.size();
  result.reset_count = reset_count_;
  return result;
}

void AffineDeviceClockMapper::start_window(uint64_t device_ns, uint64_t host_ns) {
  window_active_ = true;
  window_start_device_ns_ = device_ns;
  window_best_ = Anchor{device_ns, host_ns};
}

void AffineDeviceClockMapper::update_window(uint64_t device_ns, uint64_t host_ns) {
  const double current_residual =
      static_cast<double>(host_ns) -
      (scale_ * static_cast<double>(device_ns) + offset_ns_);
  const double best_residual =
      static_cast<double>(window_best_.host_ns) -
      (scale_ * static_cast<double>(window_best_.device_ns) + offset_ns_);
  if (current_residual < best_residual) window_best_ = Anchor{device_ns, host_ns};
}

void AffineDeviceClockMapper::commit_window() {
  if (!window_active_) return;
  anchors_.push_back(window_best_);
  while (anchors_.size() > kMaxAnchors) anchors_.pop_front();
  refit();
  window_active_ = false;
}

void AffineDeviceClockMapper::refit() {
  if (anchors_.empty()) return;
  if (anchors_.size() == 1) {
    offset_ns_ = static_cast<double>(anchors_.front().host_ns) -
                 scale_ * static_cast<double>(anchors_.front().device_ns);
    return;
  }

  const long double x0 = static_cast<long double>(anchors_.front().device_ns);
  const long double y0 = static_cast<long double>(anchors_.front().host_ns);
  long double mean_x = 0.0L;
  long double mean_y = 0.0L;
  for (const Anchor& anchor : anchors_) {
    mean_x += static_cast<long double>(anchor.device_ns) - x0;
    mean_y += static_cast<long double>(anchor.host_ns) - y0;
  }
  mean_x /= static_cast<long double>(anchors_.size());
  mean_y /= static_cast<long double>(anchors_.size());

  long double covariance = 0.0L;
  long double variance = 0.0L;
  for (const Anchor& anchor : anchors_) {
    const long double x = static_cast<long double>(anchor.device_ns) - x0 - mean_x;
    const long double y = static_cast<long double>(anchor.host_ns) - y0 - mean_y;
    covariance += x * y;
    variance += x * x;
  }
  if (variance <= 0.0L) return;

  double fitted_scale = static_cast<double>(covariance / variance);
  fitted_scale = std::clamp(fitted_scale,
                            1.0 - kMaxScaleError,
                            1.0 + kMaxScaleError);
  const long double mean_device = x0 + mean_x;
  const long double mean_host = y0 + mean_y;
  const double fitted_offset =
      static_cast<double>(mean_host - static_cast<long double>(fitted_scale) * mean_device);

  // Smooth the model after the initial two-anchor estimate to prevent a single
  // unusually delayed window from moving timestamps abruptly.
  const double alpha = anchors_.size() <= 2 ? 1.0 : 0.2;
  scale_ += alpha * (fitted_scale - scale_);
  offset_ns_ += alpha * (fitted_offset - offset_ns_);
}

}  // namespace xr_capture_cpp
