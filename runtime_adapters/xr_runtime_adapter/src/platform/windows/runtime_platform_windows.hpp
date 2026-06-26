#pragma once

namespace xr_runtime_adapter::platform {

inline const char* platform_name() { return "windows"; }
inline bool supports_posix_shm() { return false; }
inline const char* default_tracking_input_transport() { return "udp"; }

}  // namespace xr_runtime_adapter::platform
