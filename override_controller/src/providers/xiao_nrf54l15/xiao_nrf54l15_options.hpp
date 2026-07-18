#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace xr_override_controller::xiao_nrf54l15 {

struct XiaoNrf54l15Options {
  // Empty means automatic Linux discovery through /dev/serial/by-id and
  // /dev/ttyACM*. Multiple explicit ports are comma-separated.
  std::vector<std::string> ports;
  uint32_t baud_rate = 230400;
  uint32_t initial_scan_ms = 1200;
  uint32_t reconnect_ms = 1000;
  uint32_t stale_ms = 250;
  int axis_flat = 1024;
  double madgwick_beta = 0.04;
};

XiaoNrf54l15Options load_xiao_nrf54l15_options(const ProviderOptionValues& values);

}  // namespace xr_override_controller::xiao_nrf54l15
