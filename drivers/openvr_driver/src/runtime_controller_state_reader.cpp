#include "runtime_controller_state_reader.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "runtime_pose_reader.hpp"

namespace xr::openvr_driver {

std::unique_ptr<IRuntimeControllerStateReader> create_posix_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& cfg);
std::unique_ptr<IRuntimeControllerStateReader> create_windows_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& cfg);

namespace {

std::string lower_ascii(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return v;
}

}  // namespace

bool runtime_controller_state_frame_is_valid(const xr_runtime::RuntimeControllerStateFrameV1& frame) {
  if (frame.magic != xr_runtime::RUNTIME_CONTROLLER_STATE_MAGIC) return false;
  if (frame.version != xr_runtime::RUNTIME_CONTROLLER_STATE_FORMAT_VERSION) return false;
  if (frame.size_bytes != sizeof(xr_runtime::RuntimeControllerStateFrameV1)) return false;
  return true;
}

bool runtime_controller_state_sample_is_fresh(const RuntimeControllerStateSample& sample,
                                              uint32_t max_age_ms) {
  if (max_age_ms == 0) return true;
  if (sample.age_ns < 0) return true;
  const int64_t max_age_ns = static_cast<int64_t>(max_age_ms) * 1000LL * 1000LL;
  return sample.age_ns <= max_age_ns;
}

bool runtime_controller_side_has_pose(const xr_runtime::RuntimeControllerSideStateV1& side,
                                      bool left) {
  const uint32_t expected_role = left ? xr_runtime::CONTROLLER_SIDE_LEFT : xr_runtime::CONTROLLER_SIDE_RIGHT;
  if (side.role != expected_role) return false;
  if ((side.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) == 0u) return false;
  if ((side.flags & xr_runtime::RUNTIME_CONTROLLER_CONNECTED) == 0u) return false;
  return true;
}

std::unique_ptr<IRuntimeControllerStateReader> create_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& cfg) {
  const std::string transport = lower_ascii(cfg.transport.empty() ? "auto" : cfg.transport);
#if defined(_WIN32)
  if (transport == "auto" || transport == "udp" || transport == "windows_udp") {
    return create_windows_runtime_controller_state_reader(cfg);
  }
#else
  if (transport == "auto" || transport == "posix_shm" || transport == "shm") {
    return create_posix_runtime_controller_state_reader(cfg);
  }
#endif
  return nullptr;
}

}  // namespace xr::openvr_driver
