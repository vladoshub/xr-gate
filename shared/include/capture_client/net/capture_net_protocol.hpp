#pragma once

// capture_net_v1: small binary TCP framing used between capture_net_bridge and TcpCaptureTransport.
//
// MVP semantics:
//   - one TCP connection carries configured camera0/camera1/imu0 RawMessage packets;
//   - no compression;
//   - no request/subscribe control plane yet;
//   - every packet carries stream_id and frame_id strings followed by raw payload;
//   - source timestamps and source sequences are preserved.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <capture_client/contracts/messages.hpp>

namespace capture_client::capture_net_v1 {

static constexpr char kMagic[4] = {'C', 'A', 'P', 'N'};
static constexpr uint16_t kVersion = 1;
static constexpr uint32_t kPacketData = 1;
static constexpr uint32_t kFlagNone = 0;

#pragma pack(push, 1)
struct PacketHeader {
  char magic[4];
  uint16_t version;
  uint16_t header_size;
  uint32_t packet_type;
  uint32_t flags;
  uint64_t sequence;
  int64_t timestamp_ns;
  uint64_t monotonic_ns;
  uint32_t stream_id_len;
  uint32_t frame_id_len;
  uint32_t payload_size;
  uint32_t width;
  uint32_t height;
  uint32_t format_code;
  uint32_t reserved0;
  uint64_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 76, "unexpected capture_net_v1 PacketHeader size");

inline bool valid_magic(const char m[4]) {
  return m[0] == kMagic[0] && m[1] == kMagic[1] && m[2] == kMagic[2] && m[3] == kMagic[3];
}

inline void set_tcp_nodelay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

inline void set_socket_send_buffer(int fd, int bytes) {
  if (bytes > 0) ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

inline void set_socket_recv_buffer(int fd, int bytes) {
  if (bytes > 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

inline void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

inline bool read_exact(int fd, void* data, size_t len) {
  uint8_t* p = static_cast<uint8_t*>(data);
  size_t done = 0;
  while (done < len) {
    const ssize_t n = ::recv(fd, p + done, len - done, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

inline bool write_all(int fd, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  size_t done = 0;
  while (done < len) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd, p + done, len - done, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, p + done, len - done, 0);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

inline int connect_tcp(const std::string& host, int port) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  const std::string port_s = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &result);
  if (gai != 0) {
    throw std::runtime_error("getaddrinfo failed for " + host + ":" + port_s + ": " + gai_strerror(gai));
  }

  int last_errno = 0;
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      last_errno = errno;
      continue;
    }
    set_tcp_nodelay(fd);
    set_socket_recv_buffer(fd, 4 * 1024 * 1024);
    set_socket_send_buffer(fd, 4 * 1024 * 1024);
    if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      ::freeaddrinfo(result);
      return fd;
    }
    last_errno = errno;
    ::close(fd);
  }

  ::freeaddrinfo(result);
  throw std::runtime_error("connect failed to " + host + ":" + port_s + ": " + std::strerror(last_errno));
}

inline int create_server_socket(const std::string& host, int port, int backlog = 8) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* result = nullptr;
  const std::string port_s = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  const int gai = ::getaddrinfo(node, port_s.c_str(), &hints, &result);
  if (gai != 0) {
    throw std::runtime_error("getaddrinfo failed for listen " + host + ":" + port_s + ": " + gai_strerror(gai));
  }

  int last_errno = 0;
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      last_errno = errno;
      continue;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_tcp_nodelay(fd);
    set_socket_recv_buffer(fd, 4 * 1024 * 1024);
    set_socket_send_buffer(fd, 4 * 1024 * 1024);
    if (::bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && ::listen(fd, backlog) == 0) {
      ::freeaddrinfo(result);
      return fd;
    }
    last_errno = errno;
    ::close(fd);
  }

  ::freeaddrinfo(result);
  throw std::runtime_error("listen failed on " + host + ":" + port_s + ": " + std::strerror(last_errno));
}

inline PacketHeader make_data_header(const RawMessage& msg) {
  PacketHeader h{};
  h.magic[0] = kMagic[0];
  h.magic[1] = kMagic[1];
  h.magic[2] = kMagic[2];
  h.magic[3] = kMagic[3];
  h.version = kVersion;
  h.header_size = static_cast<uint16_t>(sizeof(PacketHeader));
  h.packet_type = kPacketData;
  h.flags = kFlagNone;
  h.sequence = msg.sequence;
  h.timestamp_ns = msg.timestamp_ns;
  h.monotonic_ns = msg.monotonic_ns;
  h.stream_id_len = static_cast<uint32_t>(msg.stream_id.size());
  h.frame_id_len = static_cast<uint32_t>(msg.frame_id.size());
  h.payload_size = msg.payload_size;
  h.width = msg.width;
  h.height = msg.height;
  h.format_code = msg.format_code;
  return h;
}

inline bool write_raw_message(int fd, const RawMessage& msg) {
  PacketHeader h = make_data_header(msg);
  if (!write_all(fd, &h, sizeof(h))) return false;
  if (!msg.stream_id.empty() && !write_all(fd, msg.stream_id.data(), msg.stream_id.size())) return false;
  if (!msg.frame_id.empty() && !write_all(fd, msg.frame_id.data(), msg.frame_id.size())) return false;
  if (!msg.payload.empty() && !write_all(fd, msg.payload.data(), msg.payload.size())) return false;
  return true;
}

inline bool read_raw_message(int fd, RawMessage& out) {
  PacketHeader h{};
  if (!read_exact(fd, &h, sizeof(h))) return false;
  if (!valid_magic(h.magic)) {
    throw std::runtime_error("capture_net_v1 bad packet magic");
  }
  if (h.version != kVersion) {
    throw std::runtime_error("capture_net_v1 unsupported packet version");
  }
  if (h.header_size != sizeof(PacketHeader)) {
    throw std::runtime_error("capture_net_v1 unsupported header size");
  }
  if (h.packet_type != kPacketData) {
    throw std::runtime_error("capture_net_v1 unsupported packet type");
  }
  if (h.stream_id_len > 1024 || h.frame_id_len > 1024) {
    throw std::runtime_error("capture_net_v1 unreasonable string length");
  }
  if (h.payload_size > 64u * 1024u * 1024u) {
    throw std::runtime_error("capture_net_v1 unreasonable payload size");
  }

  std::string stream_id(h.stream_id_len, '\0');
  std::string frame_id(h.frame_id_len, '\0');
  if (!stream_id.empty() && !read_exact(fd, stream_id.data(), stream_id.size())) return false;
  if (!frame_id.empty() && !read_exact(fd, frame_id.data(), frame_id.size())) return false;

  out.stream_id = std::move(stream_id);
  out.frame_id = std::move(frame_id);
  out.sequence = h.sequence;
  out.timestamp_ns = h.timestamp_ns;
  out.monotonic_ns = h.monotonic_ns;
  out.payload_size = h.payload_size;
  out.width = h.width;
  out.height = h.height;
  out.format_code = h.format_code;
  out.flags = h.flags;
  out.payload.resize(h.payload_size);
  if (h.payload_size > 0 && !read_exact(fd, out.payload.data(), out.payload.size())) return false;
  return true;
}

}  // namespace capture_client::capture_net_v1
