#include "runtime_pose_reader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>

namespace xr::openvr_driver {

std::unique_ptr<IRuntimePoseReader> create_posix_runtime_pose_reader(const RuntimePoseReaderConfig& cfg);
std::unique_ptr<IRuntimePoseReader> create_windows_runtime_pose_reader(const RuntimePoseReaderConfig& cfg);

int64_t monotonic_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::string lower_ascii(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return v;
}

bool runtime_pose_is_valid_for_openvr(const xr_runtime::RuntimeHmdPoseF64V1& pose) {
  if (pose.version != xr_runtime::RUNTIME_HMD_POSE_FORMAT_VERSION) return false;
  if (pose.size_bytes != sizeof(xr_runtime::RuntimeHmdPoseF64V1)) return false;
  if ((pose.flags & xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID) == 0u) return false;
  if (pose.tracking_status != 2u) return false;  // 2 == tracking, same status numbering as HmdPoseF64V1.
  if (pose.confidence <= 0.0f) return false;
  return true;
}

bool runtime_pose_is_fresh(const RuntimePoseSample& sample, uint32_t max_age_ms) {
  if (max_age_ms == 0) return true;
  if (sample.age_ns < 0) return true;  // Different monotonic clock domain: do not reject here.
  const int64_t max_age_ns = static_cast<int64_t>(max_age_ms) * 1000LL * 1000LL;
  return sample.age_ns <= max_age_ns;
}

std::unique_ptr<IRuntimePoseReader> create_runtime_pose_reader(const RuntimePoseReaderConfig& cfg) {
  const std::string transport = lower_ascii(cfg.transport.empty() ? "auto" : cfg.transport);
#if defined(_WIN32)
  if (transport == "auto" || transport == "udp" || transport == "windows_udp") {
    return create_windows_runtime_pose_reader(cfg);
  }
#else
  if (transport == "auto" || transport == "posix_shm" || transport == "shm") {
    return create_posix_runtime_pose_reader(cfg);
  }
#endif
  return nullptr;
}

}  // namespace xr::openvr_driver
