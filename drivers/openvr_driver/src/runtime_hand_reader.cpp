#include "runtime_hand_reader.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "runtime_pose_reader.hpp"

namespace xr::openvr_driver {

std::unique_ptr<IRuntimeHandReader> create_posix_runtime_hand_reader(const RuntimeHandReaderConfig& cfg);
std::unique_ptr<IRuntimeHandReader> create_windows_runtime_hand_reader(const RuntimeHandReaderConfig& cfg);

namespace {

std::string lower_ascii(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return v;
}

}  // namespace

bool runtime_hand_frame_is_valid(const xr_tracking::HandTrackingFrameF32V2& frame) {
  if (frame.version != xr_tracking::HAND_TRACKING_FORMAT_VERSION_V2) return false;
  if (frame.size_bytes != sizeof(xr_tracking::HandTrackingFrameF32V2)) return false;
  return true;
}

bool runtime_hand_sample_is_fresh(const RuntimeHandSample& sample, uint32_t max_age_ms) {
  if (max_age_ms == 0) return true;
  if (sample.age_ns < 0) return true;  // Different monotonic clock domain: do not reject here.
  const int64_t max_age_ns = static_cast<int64_t>(max_age_ms) * 1000LL * 1000LL;
  return sample.age_ns <= max_age_ns;
}

bool runtime_hand_side_is_valid(const xr_tracking::HandTrackingFrameF32V2& frame,
                                const xr_tracking::HandSideF32V2& side,
                                bool left) {
  if (!runtime_hand_frame_is_valid(frame)) return false;

  const uint32_t expected_handedness = left ? 1u : 2u;
  const uint32_t frame_side_flag = left ? xr_tracking::HAND_FLAG_LEFT_VALID
                                        : xr_tracking::HAND_FLAG_RIGHT_VALID;

  if (side.handedness != expected_handedness) return false;
  if ((frame.flags & frame_side_flag) == 0u) return false;
  if ((side.flags & xr_tracking::HAND_POSE_VALID) == 0u) return false;
  if (side.confidence <= 0.0f) return false;

  const uint32_t tracking = static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Tracking);
  const uint32_t degraded = static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Degraded);
  if (side.status != tracking && side.status != degraded) return false;

  return true;
}

std::unique_ptr<IRuntimeHandReader> create_runtime_hand_reader(const RuntimeHandReaderConfig& cfg) {
  const std::string transport = lower_ascii(cfg.transport.empty() ? "auto" : cfg.transport);
#if defined(_WIN32)
  if (transport == "auto" || transport == "udp" || transport == "windows_udp") {
    return create_windows_runtime_hand_reader(cfg);
  }
#else
  if (transport == "auto" || transport == "posix_shm" || transport == "shm") {
    return create_posix_runtime_hand_reader(cfg);
  }
#endif
  return nullptr;
}

}  // namespace xr::openvr_driver
