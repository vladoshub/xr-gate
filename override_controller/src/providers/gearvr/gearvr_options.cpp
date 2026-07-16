#include "gearvr_options.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace xr_override_controller::gearvr {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string env_value(const char* primary, const char* legacy) {
  if (const char* value = std::getenv(primary); value && *value) return value;
  if (const char* value = std::getenv(legacy); value && *value) return value;
  return {};
}

uint32_t parse_u32(const std::string& value, const std::string& name) {
  size_t consumed = 0;
  const unsigned long long parsed = std::stoull(value, &consumed, 10);
  if (consumed != value.size() || parsed > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(name + " expects an unsigned 32-bit integer: " + value);
  }
  return static_cast<uint32_t>(parsed);
}

double parse_double(const std::string& value, const std::string& name) {
  size_t consumed = 0;
  const double parsed = std::stod(value, &consumed);
  if (consumed != value.size()) {
    throw std::runtime_error(name + " expects a number: " + value);
  }
  return parsed;
}

bool parse_bool(const std::string& value, const std::string& name) {
  const std::string normalized = lower_copy(value);
  if (normalized == "1" || normalized == "true" || normalized == "yes" ||
      normalized == "y" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" ||
      normalized == "n" || normalized == "off") {
    return false;
  }
  throw std::runtime_error(name + " expects bool value: 0/1/true/false/on/off");
}

void apply_option(GearVrOptions& options,
                  const std::string& raw_key,
                  const std::string& value,
                  const std::string& source_prefix) {
  const std::string key = lower_copy(raw_key);
  const std::string name = source_prefix + raw_key;
  if (key == "initial_scan_ms") {
    options.initial_scan_ms = parse_u32(value, name);
  } else if (key == "reconnect_ms") {
    options.reconnect_ms = parse_u32(value, name);
  } else if (key == "touchpad.mode" || key == "touchpad_mode") {
    options.touchpad_mode = lower_copy(value);
  } else if (key == "touchpad.deadzone" || key == "touchpad_deadzone") {
    options.touchpad_deadzone = parse_double(value, name);
  } else if (key == "touchpad.radius" || key == "touchpad_radius") {
    options.touchpad_radius = parse_double(value, name);
  } else if (key == "touchpad.invert_x" || key == "touchpad_invert_x") {
    options.touchpad_invert_x = parse_bool(value, name);
  } else if (key == "touchpad.invert_y" || key == "touchpad_invert_y") {
    options.touchpad_invert_y = parse_bool(value, name);
  } else if (key == "madgwick_beta") {
    options.madgwick_beta = parse_double(value, name);
  } else {
    throw std::runtime_error("unknown gearvr_ble provider option: " + raw_key);
  }
}

void apply_env(GearVrOptions& options,
               const char* primary,
               const char* legacy,
               const char* key) {
  const std::string value = env_value(primary, legacy);
  if (!value.empty()) apply_option(options, key, value, std::string("environment ") + primary + " -> ");
}

void validate(const GearVrOptions& options) {
  if (options.touchpad_mode != "relative_stick" &&
      options.touchpad_mode != "absolute_stick" &&
      options.touchpad_mode != "dpad" &&
      options.touchpad_mode != "raw") {
    throw std::runtime_error(
        "gearvr_ble.touchpad.mode expects relative_stick, absolute_stick, dpad, or raw");
  }
  if (!(options.touchpad_deadzone >= 0.0 && options.touchpad_deadzone < 1.0)) {
    throw std::runtime_error("gearvr_ble.touchpad.deadzone must be in [0,1)");
  }
  if (!(options.touchpad_radius > 0.0)) {
    throw std::runtime_error("gearvr_ble.touchpad.radius must be > 0");
  }
  if (!(options.madgwick_beta >= 0.0)) {
    throw std::runtime_error("gearvr_ble.madgwick_beta must be >= 0");
  }
}

}  // namespace

GearVrOptions load_gearvr_options(const ProviderOptionValues& values) {
  GearVrOptions options;

  // Keep the existing environment API compatible without involving the common
  // launcher or argument parser in Gear VR-specific behavior.
  apply_env(options, "GEARVR_INITIAL_SCAN_MS", "OVERRIDE_CONTROLLER_GEARVR_INITIAL_SCAN_MS",
            "initial_scan_ms");
  apply_env(options, "GEARVR_RECONNECT_MS", "OVERRIDE_CONTROLLER_GEARVR_RECONNECT_MS",
            "reconnect_ms");
  apply_env(options, "GEARVR_TOUCHPAD_MODE", "OVERRIDE_CONTROLLER_GEARVR_TOUCHPAD_MODE",
            "touchpad.mode");
  apply_env(options, "GEARVR_TOUCHPAD_DEADZONE", "OVERRIDE_CONTROLLER_GEARVR_TOUCHPAD_DEADZONE",
            "touchpad.deadzone");
  apply_env(options, "GEARVR_TOUCHPAD_RADIUS", "OVERRIDE_CONTROLLER_GEARVR_TOUCHPAD_RADIUS",
            "touchpad.radius");
  apply_env(options, "GEARVR_TOUCHPAD_INVERT_X", "OVERRIDE_CONTROLLER_GEARVR_TOUCHPAD_INVERT_X",
            "touchpad.invert_x");
  apply_env(options, "GEARVR_TOUCHPAD_INVERT_Y", "OVERRIDE_CONTROLLER_GEARVR_TOUCHPAD_INVERT_Y",
            "touchpad.invert_y");
  apply_env(options, "GEARVR_MADGWICK_BETA", "OVERRIDE_CONTROLLER_GEARVR_MADGWICK_BETA",
            "madgwick_beta");

  for (const auto& [key, value] : values) {
    apply_option(options, key, value, "--provider-option gearvr_ble.");
  }
  validate(options);
  return options;
}

}  // namespace xr_override_controller::gearvr
