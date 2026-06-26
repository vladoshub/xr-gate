#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <xr_runtime/platform/udp_socket.hpp>

namespace xr_runtime::platform {

class UdpSender {
 public:
  UdpSender(const std::string& host, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
      throw UdpError("socket(AF_INET, SOCK_DGRAM) failed");
    }

    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr_.sin_addr) != 1) {
      struct addrinfo hints {};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;

      struct addrinfo* res = nullptr;
      const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
      if (rc != 0 || res == nullptr) {
        close_fd();
        throw UdpError("failed to resolve UDP target host '" + host + "'");
      }

      addr_.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
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
    const ssize_t sent = ::sendto(fd_, data, size, 0,
                                  reinterpret_cast<const sockaddr*>(&addr_),
                                  sizeof(addr_));
    if (sent < 0 || static_cast<size_t>(sent) != size) {
      throw UdpError(std::string("sendto failed: ") + std::strerror(errno));
    }
  }

 private:
  void close_fd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void move_from(UdpSender& other) noexcept {
    fd_ = other.fd_;
    addr_ = other.addr_;
    other.fd_ = -1;
  }

  int fd_ = -1;
  sockaddr_in addr_ {};
};

class UdpReceiver {
 public:
  UdpReceiver(const std::string& bind_host,
              uint16_t port,
              UdpReceiveMode mode,
              int timeout_ms = 0) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
      throw UdpError("socket(AF_INET, SOCK_DGRAM) failed");
    }

    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (mode == UdpReceiveMode::NonBlocking) {
      const int flags = ::fcntl(fd_, F_GETFL, 0);
      if (flags >= 0) {
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
      }
    } else if (timeout_ms > 0) {
      timeval tv {};
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_host == "0.0.0.0" || bind_host.empty()) {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
      close_fd();
      throw UdpError("invalid UDP bind host: " + bind_host);
    }

    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      const std::string err = std::strerror(errno);
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

  // Returns std::nullopt when no packet is currently available or a receive
  // timeout expires. Fatal socket errors throw UdpError.
  std::optional<size_t> receive(void* buffer, size_t size) {
    const ssize_t n = ::recvfrom(fd_, buffer, size, 0, nullptr, nullptr);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return std::nullopt;
      }
      throw UdpError(std::string("recvfrom failed: ") + std::strerror(errno));
    }
    if (n == 0) {
      return std::nullopt;
    }
    return static_cast<size_t>(n);
  }

 private:
  void close_fd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void move_from(UdpReceiver& other) noexcept {
    fd_ = other.fd_;
    other.fd_ = -1;
  }

  int fd_ = -1;
};

}  // namespace xr_runtime::platform
