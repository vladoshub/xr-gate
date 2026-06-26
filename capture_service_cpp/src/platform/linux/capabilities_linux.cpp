#include "capture_service_cpp/platform/capabilities.hpp"

namespace xr_capture_cpp {

bool platform_supports_shm_publish() { return true; }
bool platform_requires_tcp_publish() { return false; }

}  // namespace xr_capture_cpp
