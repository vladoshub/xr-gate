#pragma once

namespace xr_runtime_adapter::platform {

inline const char* platform_name() { return "linux"; }
inline bool supports_posix_shm() { return true; }
inline const char* default_tracking_input_transport() { return "shm"; }

}  // namespace xr_runtime_adapter::platform
