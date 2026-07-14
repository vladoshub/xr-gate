#include "capture_service_cpp/platform/camera_controls.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require_equal(const std::string& actual,
                   const std::string& expected,
                   const std::string& label) {
  if (actual != expected) {
    throw std::runtime_error(label + ": got '" + actual + "', expected '" + expected + "'");
  }
}
}

int main() {
  using xr_capture_cpp::normalize_camera_control_name;
  require_equal(normalize_camera_control_name("Brightness"), "brightness", "brightness");
  require_equal(normalize_camera_control_name("Exposure Time, Absolute"),
                "exposure_time_absolute", "exposure");
  require_equal(normalize_camera_control_name("White Balance Temperature"),
                "white_balance_temperature", "white balance");
  require_equal(normalize_camera_control_name("  Focus--Absolute  "),
                "focus_absolute", "separator collapsing");
  std::cout << "camera control name smoke test passed\n";
  return 0;
}
