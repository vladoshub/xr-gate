#pragma once

#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xr_capture_cpp::tcp_socket {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void close_socket(socket_t s) { if (s != kInvalidSocket) closesocket(s); }
inline int last_socket_error() { return WSAGetLastError(); }
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void close_socket(socket_t s) { if (s != kInvalidSocket) ::close(s); }
inline int last_socket_error() { return errno; }
#endif

class SocketRuntime {
 public:
  SocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
#endif
  }
  ~SocketRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;
};

}  // namespace xr_capture_cpp::tcp_socket
