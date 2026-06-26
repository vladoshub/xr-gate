#include "capture_service_cpp/platform/process.hpp"

#include <process.h>

namespace xr_capture_cpp {

long long current_process_id() {
  return static_cast<long long>(::_getpid());
}

}  // namespace xr_capture_cpp
