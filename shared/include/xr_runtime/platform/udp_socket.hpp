#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace xr_runtime::platform {

// Platform seam for runtime UDP transport. Runtime code should include this
// wrapper instead of including sys/socket.h, arpa/inet.h, winsock2.h, unistd.h,
// or using sendto/recvfrom directly.

enum class UdpReceiveMode {
  NonBlocking,
  TimedBlocking,
};

class UdpError : public std::runtime_error {
 public:
  explicit UdpError(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace xr_runtime::platform

#if defined(_WIN32)
#include <xr_runtime/platform/windows/udp_socket_windows.hpp>
#else
#include <xr_runtime/platform/linux/udp_socket_linux.hpp>
#endif
