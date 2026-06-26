#pragma once

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include "windows/runtime_platform_windows.hpp"
#else
#include "linux/runtime_platform_linux.hpp"
#endif

namespace xr_runtime_adapter::platform {

inline void require_posix_shm_available(const std::string& option_name) {
  if (!supports_posix_shm()) {
    throw std::runtime_error(option_name + " uses POSIX SHM and is not supported on " + platform_name() +
                             "; use UDP/TCP runtime paths instead");
  }
}

inline void require_transport_available(const std::string& transport,
                                        const std::string& option_name) {
  if (transport == "shm") {
    require_posix_shm_available(option_name + "=shm");
  }
}

inline void require_shm_flag_available(bool enabled, const std::string& option_name) {
  if (enabled) {
    require_posix_shm_available(option_name);
  }
}

}  // namespace xr_runtime_adapter::platform
