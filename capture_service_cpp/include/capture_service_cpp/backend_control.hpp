#pragma once

#include <cstdint>
#include <filesystem>

namespace xr_capture_cpp {

inline constexpr double kDefaultGravityMagnitudeMps2 = 9.80665;

struct BackendControlSnapshot {
  double gravity_magnitude = kDefaultGravityMagnitudeMps2;
  uint64_t reset_counter = 0;
};

BackendControlSnapshot load_backend_control_snapshot(const std::filesystem::path& path);

class BackendControlReader {
 public:
  BackendControlReader(std::filesystem::path path, int poll_interval_ms);

  // Reloads the shared backend-control JSON when the configured polling
  // interval has elapsed. Until the file becomes available, the reader keeps
  // the standard-gravity fallback and retries on later polls.
  void poll_if_due(uint64_t now_ns);

  const BackendControlSnapshot& snapshot() const { return snapshot_; }

 private:
  std::filesystem::path path_;
  uint64_t poll_interval_ns_ = 0;
  uint64_t next_poll_ns_ = 0;
  BackendControlSnapshot snapshot_{};
  bool polled_once_ = false;
  bool loaded_once_ = false;
  bool failure_logged_ = false;
};

}  // namespace xr_capture_cpp
