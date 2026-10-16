#pragma once

#include <cstdint>
#include <string>

namespace xr::openvr_driver {

struct RuntimeTcpConfig {
  // IPv4 literal or localhost; avoids blocking DNS during SteamVR shutdown.
  std::string host = "127.0.0.1";
  uint16_t port = 45810;
  uint32_t reconnect_ms = 500;
  uint32_t timeout_ms = 1000;
};

}  // namespace xr::openvr_driver
