#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <xr_runtime/platform/udp_socket.hpp>

namespace xr_runtime::platform {
namespace detail {
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

inline void init_winsock_once() {
  static bool initialized = [] {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      throw UdpError("WSAStartup failed");
    }
    return true;
  }();
  (void)initialized;
}

inline std::string last_error_string() {
  return "WinSock error " + std::to_string(::WSAGetLastError());
}

inline void close_socket(socket_t fd) {
  if (fd != kInvalidSocket) {
    ::closesocket(fd);
  }
}
}  // namespace detail

class UdpSender {
 public:
  UdpSender(const std::string& host, uint16_t port) {
    detail::init_winsock_once();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ == detail::kInvalidSocket) {
      throw UdpError("socket(AF_INET, SOCK_DGRAM) failed: " + detail::last_error_string());
    }

    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);

    if (::InetPtonA(AF_INET, host.c_str(), &addr_.sin_addr) != 1) {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      addrinfo* res = nullptr;
      const std::string port_s = std::to_string(port);
      const int rc = ::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
      if (rc != 0 || res == nullptr) {
        close_fd();
        throw UdpError("failed to resolve UDP target host '" + host + "': " + std::to_string(rc));
      }
      addr_ = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
      addr_.sin_port = htons(port);
      ::freeaddrinfo(res);
    }
  }

  ~UdpSender() { close_fd(); }

  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  UdpSender(UdpSender&& other) noexcept { move_from(other); }
  UdpSender& operator=(UdpSender&& other) noexcept {
    if (this != &other) {
      close_fd();
      move_from(other);
    }
    return *this;
  }

  void send_packet(const void* data, size_t size) {
    const int sent = ::sendto(fd_, static_cast<const char*>(data), static_cast<int>(size), 0,
                              reinterpret_cast<const sockaddr*>(&addr_),
                              static_cast<int>(sizeof(addr_)));
    if (sent < 0 || static_cast<size_t>(sent) != size) {
      throw UdpError("sendto failed: " + detail::last_error_string());
    }
  }

 private:
  void close_fd() {
    detail::close_socket(fd_);
    fd_ = detail::kInvalidSocket;
  }

  void move_from(UdpSender& other) noexcept {
    fd_ = other.fd_;
    addr_ = other.addr_;
    other.fd_ = detail::kInvalidSocket;
  }

  detail::socket_t fd_ = detail::kInvalidSocket;
  sockaddr_in addr_{};
};

class UdpReceiver {
 public:
  UdpReceiver(const std::string& bind_host,
              uint16_t port,
              UdpReceiveMode mode,
              int timeout_ms = 0) {
    detail::init_winsock_once();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ == detail::kInvalidSocket) {
      throw UdpError("socket(AF_INET, SOCK_DGRAM) failed: " + detail::last_error_string());
    }

    BOOL reuse = TRUE;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (mode == UdpReceiveMode::NonBlocking) {
      u_long one = 1;
      ::ioctlsocket(fd_, FIONBIO, &one);
    } else if (timeout_ms > 0) {
      const DWORD tv = static_cast<DWORD>(timeout_ms);
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_host == "0.0.0.0" || bind_host.empty()) {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::InetPtonA(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
      close_fd();
      throw UdpError("invalid UDP bind host: " + bind_host);
    }

    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      const std::string err = detail::last_error_string();
      close_fd();
      throw UdpError("UDP bind failed: " + err);
    }
  }

  ~UdpReceiver() { close_fd(); }

  UdpReceiver(const UdpReceiver&) = delete;
  UdpReceiver& operator=(const UdpReceiver&) = delete;

  UdpReceiver(UdpReceiver&& other) noexcept { move_from(other); }
  UdpReceiver& operator=(UdpReceiver&& other) noexcept {
    if (this != &other) {
      close_fd();
      move_from(other);
    }
    return *this;
  }

  std::optional<size_t> receive(void* buffer, size_t size) {
    const int n = ::recvfrom(fd_, static_cast<char*>(buffer), static_cast<int>(size), 0, nullptr, nullptr);
    if (n < 0) {
      const int err = ::WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
        return std::nullopt;
      }
      throw UdpError("recvfrom failed: " + std::to_string(err));
    }
    if (n == 0) return std::nullopt;
    return static_cast<size_t>(n);
  }

 private:
  void close_fd() {
    detail::close_socket(fd_);
    fd_ = detail::kInvalidSocket;
  }

  void move_from(UdpReceiver& other) noexcept {
    fd_ = other.fd_;
    other.fd_ = detail::kInvalidSocket;
  }

  detail::socket_t fd_ = detail::kInvalidSocket;
};

}  // namespace xr_runtime::platform
