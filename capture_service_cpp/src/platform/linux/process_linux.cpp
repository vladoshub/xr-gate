#include "capture_service_cpp/platform/process.hpp"

#include <unistd.h>

namespace xr_capture_cpp {

long long current_process_id() {
  return static_cast<long long>(::getpid());
}

}  // namespace xr_capture_cpp
