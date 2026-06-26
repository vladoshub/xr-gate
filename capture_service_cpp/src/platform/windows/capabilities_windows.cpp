#include "capture_service_cpp/platform/capabilities.hpp"

namespace xr_capture_cpp {

bool platform_supports_shm_publish() { return false; }
bool platform_requires_tcp_publish() { return true; }

}  // namespace xr_capture_cpp
