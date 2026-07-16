#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <cstdint>
#include <string>

namespace xr_override_controller::gearvr {

struct GearVrOptions {
  uint32_t initial_scan_ms = 1500;
  uint32_t reconnect_ms = 1000;
  std::string touchpad_mode = "absolute_stick";
  double touchpad_deadzone = 0.12;
  double touchpad_radius = 90.0;
  bool touchpad_invert_x = false;
  bool touchpad_invert_y = true;
  double madgwick_beta = 0.04;
};

// Precedence: built-in defaults < legacy Gear VR environment variables <
// generic --provider-option values. Parsing and validation intentionally live
// beside the provider rather than in main.cpp.
GearVrOptions load_gearvr_options(const ProviderOptionValues& values);

}  // namespace xr_override_controller::gearvr
