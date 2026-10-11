#include "capture_service_cpp/vendor/xreal_camera_metadata.hpp"

#include <limits>

namespace xr_capture_cpp {
namespace {
uint64_t read_le(const uint8_t* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}
}  // namespace

bool decode_xreal_camera_metadata(const uint8_t* footer, size_t size,
                                  XrealCameraMetadata& out) {
  out = {};
  if (!footer || size < 70 || footer[59] > 1) return false;
  const uint64_t t0 = read_le(footer, 8);
  const uint64_t t62 = read_le(footer + 62, 8);
  const auto max_time = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (!t0 || !t62 || t0 > max_time || t62 > max_time) return false;
  out = {t0, t62, static_cast<uint16_t>(read_le(footer + 18, 2)), footer[59]};
  return true;
}

XrealStereoTimestampPairer::Result XrealStereoTimestampPairer::push(
    const XrealCameraMetadata& eye) {
  if (!eye.t62_ns || eye.eye > 1 || eye.t62_ns <= published_t62_ns_) {
    ++dropped_;
    return Result::Discard;
  }
  if (eyes_ && eye.t62_ns < pending_t62_ns_) {
    ++dropped_;
    return Result::Discard;
  }
  if (!eyes_ || eye.t62_ns > pending_t62_ns_) {
    if (eyes_) ++dropped_;
    pending_t62_ns_ = eye.t62_ns;
    pending_counter_ = eye.pair_counter;
    eyes_ = 0;
  }
  const uint8_t bit = static_cast<uint8_t>(1U << eye.eye);
  if (eye.pair_counter != pending_counter_ || (eyes_ & bit)) {
    ++dropped_;
    return Result::Discard;
  }
  eyes_ |= bit;
  if (eyes_ != 3) return Result::Pending;
  published_t62_ns_ = pending_t62_ns_;
  eyes_ = 0;
  return Result::Complete;
}

}  // namespace xr_capture_cpp
