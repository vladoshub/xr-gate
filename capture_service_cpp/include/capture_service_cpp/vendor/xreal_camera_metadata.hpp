#pragma once

#include <cstddef>
#include <cstdint>

namespace xr_capture_cpp {

struct XrealCameraMetadata {
  uint64_t t0_ns = 0;
  uint64_t t62_ns = 0;
  uint16_t pair_counter = 0;
  uint8_t eye = 0;
};

// Measured Air 2 Ultra footer layout. The names are byte offsets, not claims
// about exposure start/end: LE64@0, LE16@18, eye@59, LE64@62, units ns.
bool decode_xreal_camera_metadata(const uint8_t* footer, size_t size,
                                  XrealCameraMetadata& out);

class XrealStereoTimestampPairer {
 public:
  enum class Result { Discard, Pending, Complete };
  Result push(const XrealCameraMetadata& eye);
  uint64_t dropped() const { return dropped_; }

 private:
  uint64_t pending_t62_ns_ = 0;
  uint64_t published_t62_ns_ = 0;
  uint16_t pending_counter_ = 0;
  uint8_t eyes_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace xr_capture_cpp
