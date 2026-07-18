#include "xiao_nrf54l15_options.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace xr_override_controller::xiao_nrf54l15 {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string trim_copy(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  return value.substr(first);
}

std::vector<std::string> split_ports(const std::string& value) {
  std::vector<std::string> ports;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    std::string port = trim_copy(value.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!port.empty()) ports.push_back(std::move(port));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return ports;
}

uint32_t parse_u32(const std::string& value, const std::string& name) {
  size_t consumed = 0;
  const unsigned long long parsed = std::stoull(value, &consumed, 10);
  if (consumed != value.size() || parsed > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(name + " expects an unsigned 32-bit integer: " + value);
  }
  return static_cast<uint32_t>(parsed);
}

int parse_int(const std::string& value, const std::string& name) {
  size_t consumed = 0;
  const long parsed = std::stol(value, &consumed, 10);
  if (consumed != value.size() || parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    throw std::runtime_error(name + " expects an integer: " + value);
  }
  return static_cast<int>(parsed);
}

double parse_double(const std::string& value, const std::string& name) {
  size_t consumed = 0;
  const double parsed = std::stod(value, &consumed);
  if (consumed != value.size()) {
    throw std::runtime_error(name + " expects a number: " + value);
  }
  return parsed;
}

void apply_option(XiaoNrf54l15Options& options,
                  const std::string& raw_key,
                  const std::string& value,
                  const std::string& source_prefix) {
  const std::string key = lower_copy(raw_key);
  const std::string name = source_prefix + raw_key;
  if (key == "port" || key == "ports" || key == "device" || key == "devices") {
    options.ports = split_ports(value);
  } else if (key == "baud" || key == "baud_rate") {
    options.baud_rate = parse_u32(value, name);
  } else if (key == "initial_scan_ms") {
    options.initial_scan_ms = parse_u32(value, name);
  } else if (key == "reconnect_ms") {
    options.reconnect_ms = parse_u32(value, name);
  } else if (key == "stale_ms") {
    options.stale_ms = parse_u32(value, name);
  } else if (key == "axis_flat") {
    options.axis_flat = parse_int(value, name);
  } else if (key == "madgwick_beta") {
    options.madgwick_beta = parse_double(value, name);
  } else {
    throw std::runtime_error("unknown xiao_nrf54l15 provider option: " + raw_key);
  }
}

void apply_env(XiaoNrf54l15Options& options, const char* env, const char* key) {
  const char* value = std::getenv(env);
  if (value && *value) {
    apply_option(options, key, value, std::string("environment ") + env + " -> ");
  }
}

void validate(const XiaoNrf54l15Options& options) {
  if (options.baud_rate != 115200 && options.baud_rate != 230400 &&
      options.baud_rate != 460800 && options.baud_rate != 921600) {
    throw std::runtime_error(
        "xiao_nrf54l15.baud_rate must be 115200, 230400, 460800, or 921600");
  }
  if (options.reconnect_ms == 0) {
    throw std::runtime_error("xiao_nrf54l15.reconnect_ms must be > 0");
  }
  if (options.stale_ms == 0) {
    throw std::runtime_error("xiao_nrf54l15.stale_ms must be > 0");
  }
  if (options.axis_flat < 0 || options.axis_flat > 32767) {
    throw std::runtime_error("xiao_nrf54l15.axis_flat must be in [0,32767]");
  }
  if (!(options.madgwick_beta >= 0.0)) {
    throw std::runtime_error("xiao_nrf54l15.madgwick_beta must be >= 0");
  }
}

}  // namespace

XiaoNrf54l15Options load_xiao_nrf54l15_options(const ProviderOptionValues& values) {
  XiaoNrf54l15Options options;
  apply_env(options, "XIAO_NRF54L15_PORTS", "ports");
  apply_env(options, "XIAO_NRF54L15_BAUD", "baud_rate");
  apply_env(options, "XIAO_NRF54L15_INITIAL_SCAN_MS", "initial_scan_ms");
  apply_env(options, "XIAO_NRF54L15_RECONNECT_MS", "reconnect_ms");
  apply_env(options, "XIAO_NRF54L15_STALE_MS", "stale_ms");
  apply_env(options, "XIAO_NRF54L15_AXIS_FLAT", "axis_flat");
  apply_env(options, "XIAO_NRF54L15_MADGWICK_BETA", "madgwick_beta");
  for (const auto& [key, value] : values) {
    apply_option(options, key, value, "--provider-option xiao_nrf54l15.");
  }
  validate(options);
  return options;
}

}  // namespace xr_override_controller::xiao_nrf54l15
