#pragma once

#include <cstdint>
#include <filesystem>

namespace xr_override_controller {

inline constexpr float kDefaultGravityMagnitudeMps2 = 9.80665f;

struct BackendControlSnapshot {
  float gravity_magnitude = kDefaultGravityMagnitudeMps2;
  uint64_t reset_counter = 0;
};

BackendControlSnapshot load_backend_control_snapshot(const std::filesystem::path& path);
BackendControlSnapshot current_backend_control_snapshot();
void update_backend_control_snapshot(const BackendControlSnapshot& snapshot);

}  // namespace xr_override_controller
