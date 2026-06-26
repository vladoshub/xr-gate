#pragma once

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <xr_video/contracts/stereo_video_contract.hpp>

namespace xr_video {
namespace tcp_detail {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void init_socket_library_once() {
  static bool initialized = [] {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
    return true;
  }();
  (void)initialized;
}
inline void close_socket(socket_t fd) {
  if (fd != kInvalidSocket) closesocket(fd);
}

inline std::string last_socket_error() { return "winsock error " + std::to_string(WSAGetLastError()); }
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void init_socket_library_once() {}
inline void close_socket(socket_t fd) {
  if (fd >= 0) ::close(fd);
}
inline std::string last_socket_error() { return std::strerror(errno); }
#endif

inline void set_tcp_nodelay(socket_t fd) {
  const int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

inline void set_reuseaddr(socket_t fd) {
  const int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
}

inline void set_keepalive(socket_t fd) {
  const int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof(one));
}

inline void set_send_timeout(socket_t fd, int timeout_ms) {
  if (timeout_ms <= 0) return;
#ifdef _WIN32
  const DWORD tv = static_cast<DWORD>(timeout_ms);
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}


inline void set_recv_timeout(socket_t fd, int timeout_ms) {
  if (timeout_ms <= 0) return;
#ifdef _WIN32
  const DWORD tv = static_cast<DWORD>(timeout_ms);
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

inline void set_send_buffer_size(socket_t fd, int bytes) {
  if (bytes <= 0) return;
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

inline void shutdown_socket(socket_t fd) {
  if (fd == kInvalidSocket) return;
#ifdef _WIN32
  (void)::shutdown(fd, SD_BOTH);
#else
  (void)::shutdown(fd, SHUT_RDWR);
#endif
}

inline socket_t connect_tcp(const std::string& host, int port) {
  init_socket_library_once();
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  const std::string port_s = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &result);
  if (rc != 0) throw std::runtime_error("getaddrinfo failed for " + host + ":" + port_s);

  socket_t fd = kInvalidSocket;
  std::string last_error;
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == kInvalidSocket) {
      last_error = last_socket_error();
      continue;
    }
    set_tcp_nodelay(fd);
    if (::connect(fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
      ::freeaddrinfo(result);
      return fd;
    }
    last_error = last_socket_error();
    close_socket(fd);
    fd = kInvalidSocket;
  }

  ::freeaddrinfo(result);
  throw std::runtime_error("failed to connect to XR video TCP " + host + ":" + port_s + ": " + last_error);
}

inline socket_t listen_tcp(const std::string& bind_host, int port, int backlog = 8) {
  init_socket_library_once();
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* result = nullptr;
  const std::string port_s = std::to_string(port);
  const char* host = bind_host.empty() ? nullptr : bind_host.c_str();
  const int rc = ::getaddrinfo(host, port_s.c_str(), &hints, &result);
  if (rc != 0) throw std::runtime_error("getaddrinfo failed for bind " + bind_host + ":" + port_s);

  socket_t fd = kInvalidSocket;
  std::string last_error;
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == kInvalidSocket) {
      last_error = last_socket_error();
      continue;
    }
    set_reuseaddr(fd);
    set_tcp_nodelay(fd);
    if (::bind(fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0 && ::listen(fd, backlog) == 0) {
      ::freeaddrinfo(result);
      return fd;
    }
    last_error = last_socket_error();
    close_socket(fd);
    fd = kInvalidSocket;
  }

  ::freeaddrinfo(result);
  throw std::runtime_error("failed to listen for XR video TCP on " + bind_host + ":" + port_s + ": " + last_error);
}

inline void write_all(socket_t fd, const uint8_t* data, size_t size) {
  size_t off = 0;
  while (off < size) {
#ifdef _WIN32
    const int n = ::send(fd, reinterpret_cast<const char*>(data + off), static_cast<int>(size - off), 0);
#else
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const ssize_t n = ::send(fd, data + off, size - off, flags);
#endif
    if (n < 0) throw std::runtime_error("XR video TCP send failed: " + last_socket_error());
    if (n == 0) throw std::runtime_error("XR video TCP send returned 0");
    off += static_cast<size_t>(n);
  }
}

inline void read_all(socket_t fd, uint8_t* data, size_t size) {
  size_t off = 0;
  while (off < size) {
#ifdef _WIN32
    const int n = ::recv(fd, reinterpret_cast<char*>(data + off), static_cast<int>(size - off), 0);
#else
    const ssize_t n = ::recv(fd, data + off, size - off, 0);
#endif
    if (n < 0) throw std::runtime_error("XR video TCP recv failed: " + last_socket_error());
    if (n == 0) throw std::runtime_error("XR video TCP socket closed");
    off += static_cast<size_t>(n);
  }
}

inline void send_frame(socket_t fd, const StereoVideoFrame& frame) {
  validate_frame(frame);
  StereoVideoNetFrameHeaderV1 nh;
  nh.left_size_bytes = frame.header.left_size_bytes;
  nh.right_size_bytes = frame.header.right_size_bytes;
  nh.sequence = frame.header.sequence;
  nh.source_timestamp_ns = frame.header.source_timestamp_ns;
  nh.publish_timestamp_ns = frame.header.publish_timestamp_ns;

  write_all(fd, reinterpret_cast<const uint8_t*>(&nh), sizeof(nh));
  write_all(fd, reinterpret_cast<const uint8_t*>(&frame.header), sizeof(frame.header));
  if (!frame.left.empty()) write_all(fd, frame.left.data(), frame.left.size());
  if (!frame.right.empty()) write_all(fd, frame.right.data(), frame.right.size());
}

inline StereoVideoFrame recv_frame(socket_t fd) {
  StereoVideoNetFrameHeaderV1 nh;
  read_all(fd, reinterpret_cast<uint8_t*>(&nh), sizeof(nh));
  const char expected[8] = {'X', 'S', 'V', 'N', 'E', 'T', '1', '\0'};
  if (std::memcmp(nh.magic, expected, 8) != 0 || nh.version != 1) {
    throw std::runtime_error("bad XR video TCP frame header");
  }
  if (nh.frame_header_size != sizeof(StereoVideoFrameHeaderV1)) {
    throw std::runtime_error("unsupported XR video TCP frame header size");
  }

  StereoVideoFrame f;
  read_all(fd, reinterpret_cast<uint8_t*>(&f.header), sizeof(f.header));
  f.left.resize(nh.left_size_bytes);
  f.right.resize(nh.right_size_bytes);
  if (!f.left.empty()) read_all(fd, f.left.data(), f.left.size());
  if (!f.right.empty()) read_all(fd, f.right.data(), f.right.size());
  validate_frame(f);
  return f;
}

}  // namespace tcp_detail

class StereoVideoTcpServer {
 public:
  StereoVideoTcpServer(std::string bind_host, int port)
      : bind_host_(std::move(bind_host)), port_(port) {
    listen_fd_ = tcp_detail::listen_tcp(bind_host_, port_);
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
  }

  ~StereoVideoTcpServer() { stop(); }

  StereoVideoTcpServer(const StereoVideoTcpServer&) = delete;
  StereoVideoTcpServer& operator=(const StereoVideoTcpServer&) = delete;

  void publish(StereoVideoFrame frame) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      latest_ = std::move(frame);
      latest_seq_ = latest_.header.sequence;
    }
    cv_.notify_all();
  }

  uint64_t client_count() const { return client_count_.load(); }

 private:
  void stop() {
    if (!running_.exchange(false)) return;

    // Wake accept_loop reliably. close() from another thread is not enough on
    // all POSIX implementations; shutdown() makes blocking accept() return.
    const auto fd = listen_fd_;
    listen_fd_ = tcp_detail::kInvalidSocket;
    if (fd != tcp_detail::kInvalidSocket) {
      tcp_detail::shutdown_socket(fd);
      tcp_detail::close_socket(fd);
    }

    cv_.notify_all();
    if (accept_thread_.joinable()) accept_thread_.join();
    std::lock_guard<std::mutex> lock(client_threads_mu_);
    for (auto& t : client_threads_) {
      if (t.joinable()) t.join();
    }
    client_threads_.clear();
  }

  void accept_loop() {
    while (running_) {
      tcp_detail::socket_t fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd == tcp_detail::kInvalidSocket) {
        if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      tcp_detail::set_tcp_nodelay(fd);
      tcp_detail::set_keepalive(fd);
      // XR video is a latest-frame stream. A client that stops reading must not
      // keep a sender thread blocked indefinitely or accumulate seconds of video
      // latency in the kernel send buffer.
      tcp_detail::set_send_timeout(fd, 500);
      tcp_detail::set_send_buffer_size(fd, 2 * 1024 * 1024);
      client_count_++;
      std::lock_guard<std::mutex> lock(client_threads_mu_);
      client_threads_.emplace_back([this, fd] { client_loop(fd); });
    }
  }

  void client_loop(tcp_detail::socket_t fd) {
    uint64_t last_sent = 0;
    try {
      while (running_) {
        StereoVideoFrame copy;
        {
          std::unique_lock<std::mutex> lock(mu_);
          cv_.wait_for(lock, std::chrono::milliseconds(250), [&] {
            return !running_ || latest_seq_ > last_sent;
          });
          if (!running_) break;
          if (latest_seq_ <= last_sent) continue;
          copy = latest_;
          last_sent = latest_seq_;
        }
        tcp_detail::send_frame(fd, copy);
      }
    } catch (const std::exception& e) {
      if (running_) std::cerr << "[xr_video_tcp] client disconnected: " << e.what() << "\n";
    }
    tcp_detail::shutdown_socket(fd);
    tcp_detail::close_socket(fd);
    client_count_--;
  }

  std::string bind_host_;
  int port_ = 45700;
  tcp_detail::socket_t listen_fd_ = tcp_detail::kInvalidSocket;
  std::atomic_bool running_{false};
  std::thread accept_thread_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  StereoVideoFrame latest_;
  uint64_t latest_seq_ = 0;

  std::atomic<uint64_t> client_count_{0};
  std::mutex client_threads_mu_;
  std::vector<std::thread> client_threads_;
};

class StereoVideoTcpClient {
 public:
  StereoVideoTcpClient(std::string host, int port)
      : fd_(tcp_detail::connect_tcp(host, port)) {
    tcp_detail::set_keepalive(fd_);
    // Keep runtime/monitor shutdown responsive even if the peer stops sending.
    tcp_detail::set_recv_timeout(fd_, 1000);
  }

  ~StereoVideoTcpClient() {
    tcp_detail::shutdown_socket(fd_);
    tcp_detail::close_socket(fd_);
  }

  StereoVideoTcpClient(const StereoVideoTcpClient&) = delete;
  StereoVideoTcpClient& operator=(const StereoVideoTcpClient&) = delete;

  StereoVideoFrame read_next() { return tcp_detail::recv_frame(fd_); }

 private:
  tcp_detail::socket_t fd_ = tcp_detail::kInvalidSocket;
};

}  // namespace xr_video
