#include "capture_service_cpp/platform/camera_controls.hpp"

#include <cctype>

namespace xr_capture_cpp {

std::string normalize_camera_control_name(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  bool pending_separator = false;

  for (unsigned char c : value) {
    if (std::isalnum(c)) {
      if (pending_separator && !out.empty()) out.push_back('_');
      out.push_back(static_cast<char>(std::tolower(c)));
      pending_separator = false;
    } else {
      pending_separator = !out.empty();
    }
  }
  return out;
}

}  // namespace xr_capture_cpp
