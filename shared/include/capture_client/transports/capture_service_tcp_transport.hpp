#pragma once

#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <capture_client/transports/tcp_transport.hpp>

namespace capture_client {

struct CaptureServiceTcpTransportConfig {
  std::string host = "127.0.0.1";
  int port = 45660;
  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string imu_stream = "imu0";
  bool subscribe_imu = true;
  size_t camera_slots = 256;
  size_t imu_slots = 8192;
  double first_packet_timeout_s = 3.0;
};

namespace capture_service_tcp_detail {

inline void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

inline int connect_tcp(const std::string& host, int port) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  const std::string port_s = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &result);
  if (rc != 0) {
    throw std::runtime_error("getaddrinfo failed for " + host + ":" + port_s + ": " + ::gai_strerror(rc));
  }

  int fd = -1;
  std::string last_error;
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }

    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      ::freeaddrinfo(result);
      return fd;
    }

    last_error = std::strerror(errno);
    ::close(fd);
    fd = -1;
  }

  ::freeaddrinfo(result);
  throw std::runtime_error("failed to connect to capture_service TCP " + host + ":" + port_s + ": " + last_error);
}

inline void write_all(int fd, const uint8_t* data, size_t size) {
  size_t off = 0;
  while (off < size) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const ssize_t n = ::send(fd, data + off, size - off, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
    }
    if (n == 0) {
      throw std::runtime_error("send returned 0");
    }
    off += static_cast<size_t>(n);
  }
}

inline void write_all(int fd, const std::string& s) {
  write_all(fd, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline std::string read_exact(int fd, size_t size) {
  std::string out;
  out.resize(size);
  size_t off = 0;
  while (off < size) {
    const ssize_t n = ::recv(fd, &out[off], size - off, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
    }
    if (n == 0) {
      throw std::runtime_error("socket closed while reading");
    }
    off += static_cast<size_t>(n);
  }
  return out;
}

inline std::string read_line(int fd, size_t limit = 4096) {
  std::string out;
  out.reserve(128);
  while (true) {
    char c = 0;
    const ssize_t n = ::recv(fd, &c, 1, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("recv line failed: ") + std::strerror(errno));
    }
    if (n == 0) {
      throw std::runtime_error("socket closed while reading line");
    }
    if (c == '\n') break;
    out.push_back(c);
    if (out.size() > limit) {
      throw std::runtime_error("capture_service TCP line too long");
    }
  }
  return out;
}

inline nlohmann::json read_hello(int fd) {
  const std::string line = read_line(fd);
  std::istringstream is(line);
  std::string prefix;
  size_t json_len = 0;
  is >> prefix >> json_len;
  if (prefix != "CAPHELLO" || json_len == 0) {
    throw std::runtime_error("bad capture_service TCP hello line: " + line);
  }

  const std::string raw = read_exact(fd, json_len);
  const nlohmann::json hello = nlohmann::json::parse(raw);
  if (!hello.is_object() || !hello.contains("streams")) {
    throw std::runtime_error("bad capture_service TCP hello JSON: no streams object");
  }
  return hello;
}

inline void write_subscribe(int fd, const std::vector<std::string>& streams) {
  std::string line = "SUBSCRIBE ";
  if (streams.empty()) {
    line += "*";
  } else {
    for (size_t i = 0; i < streams.size(); ++i) {
      if (i) line += ",";
      line += streams[i];
    }
  }
  line += "\n";
  write_all(fd, line);
}

inline RawMessage read_message(int fd) {
  const std::string line = read_line(fd);
  std::istringstream is(line);
  std::string prefix;
  size_t header_len = 0;
  size_t payload_len = 0;
  is >> prefix >> header_len >> payload_len;
  if (prefix != "CAPMSG1" || header_len == 0) {
    throw std::runtime_error("bad capture_service TCP message line: " + line);
  }

  const std::string header_raw = read_exact(fd, header_len);
  const nlohmann::json h = nlohmann::json::parse(header_raw);
  std::string payload_raw;
  if (payload_len > 0) {
    payload_raw = read_exact(fd, payload_len);
  }

  RawMessage msg;
  msg.stream_id = h.value("stream_id", std::string());
  msg.sequence = h.value("sequence", uint64_t{0});
  msg.timestamp_ns = h.value("timestamp_ns", int64_t{0});
  msg.monotonic_ns = h.value("monotonic_ns", uint64_t{0});
  msg.payload_size = h.value("payload_size", static_cast<uint32_t>(payload_raw.size()));
  msg.width = h.value("width", uint32_t{0});
  msg.height = h.value("height", uint32_t{0});
  msg.format_code = h.value("format_code", uint32_t{0});
  msg.flags = h.value("flags", uint32_t{0});
  msg.frame_id = h.value("frame_id", msg.stream_id);
  msg.payload.assign(payload_raw.begin(), payload_raw.end());
  if (msg.payload_size != msg.payload.size()) {
    msg.payload_size = static_cast<uint32_t>(msg.payload.size());
  }
  return msg;
}

}  // namespace capture_service_tcp_detail

class CaptureServiceTcpTransport final : public ICaptureTransport {
 public:
  explicit CaptureServiceTcpTransport(CaptureServiceTcpTransportConfig cfg)
      : cfg_(std::move(cfg)),
        cam0_(std::make_unique<TcpStreamReader>(cfg_.cam0_stream, cfg_.camera_slots)),
        cam1_(std::make_unique<TcpStreamReader>(cfg_.cam1_stream, cfg_.camera_slots)),
        imu_(std::make_unique<TcpStreamReader>(cfg_.imu_stream, cfg_.imu_slots)) {
    fd_ = capture_service_tcp_detail::connect_tcp(cfg_.host, cfg_.port);
    const nlohmann::json hello = capture_service_tcp_detail::read_hello(fd_);
    validate_hello_streams(hello);

    std::vector<std::string> streams{cfg_.cam0_stream, cfg_.cam1_stream};
    if (cfg_.subscribe_imu) streams.push_back(cfg_.imu_stream);
    capture_service_tcp_detail::write_subscribe(fd_, streams);

    running_ = true;
    recv_thread_ = std::thread([this] { recv_loop(); });

    const bool got_all = wait_for_required_first_packets(cfg_.first_packet_timeout_s);
    if (!got_all) {
      throw std::runtime_error(
          "CaptureServiceTcpTransport connected, but did not receive initial packets for required streams "
          "within timeout.");
    }
  }

  ~CaptureServiceTcpTransport() override {
    running_ = false;
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
    }
    if (recv_thread_.joinable()) recv_thread_.join();
    capture_service_tcp_detail::close_fd(fd_);
  }

  CaptureServiceTcpTransport(const CaptureServiceTcpTransport&) = delete;
  CaptureServiceTcpTransport& operator=(const CaptureServiceTcpTransport&) = delete;

  const std::string& type() const override {
    static const std::string kType = "capture_tcp";
    return kType;
  }

  IStreamReader& cam0() override { return *cam0_; }
  IStreamReader& cam1() override { return *cam1_; }
  IStreamReader& imu() override { return *imu_; }

 private:
  void validate_hello_streams(const nlohmann::json& hello) const {
    const auto& streams = hello.at("streams");
    for (const auto& sid : required_streams()) {
      if (!streams.contains(sid)) {
        throw std::runtime_error("capture_service TCP hello missing required stream: " + sid);
      }
    }
  }

  std::vector<std::string> required_streams() const {
    std::vector<std::string> streams{cfg_.cam0_stream, cfg_.cam1_stream};
    if (cfg_.subscribe_imu) streams.push_back(cfg_.imu_stream);
    return streams;
  }

  bool wait_for_required_first_packets(double timeout_s) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cam0_->latest_sequence() != 0 && cam1_->latest_sequence() != 0 &&
          (!cfg_.subscribe_imu || imu_->latest_sequence() != 0)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cerr << "[CaptureServiceTcpTransport] initial stream state: "
              << "cam0_seq=" << cam0_->latest_sequence()
              << " cam1_seq=" << cam1_->latest_sequence()
              << " imu_seq=" << imu_->latest_sequence()
              << " subscribe_imu=" << (cfg_.subscribe_imu ? "true" : "false") << "\n";

    return cam0_->latest_sequence() != 0 && cam1_->latest_sequence() != 0 &&
           (!cfg_.subscribe_imu || imu_->latest_sequence() != 0);
  }

  void recv_loop() {
    try {
      while (running_) {
        RawMessage msg = capture_service_tcp_detail::read_message(fd_);
        if (msg.stream_id == cfg_.cam0_stream) {
          cam0_->push(std::move(msg));
        } else if (msg.stream_id == cfg_.cam1_stream) {
          cam1_->push(std::move(msg));
        } else if (msg.stream_id == cfg_.imu_stream) {
          imu_->push(std::move(msg));
        }
      }
    } catch (const std::exception& e) {
      if (running_) {
        std::cerr << "[CaptureServiceTcpTransport] recv loop error: " << e.what() << "\n";
      }
    }
    running_ = false;
  }

  CaptureServiceTcpTransportConfig cfg_;
  int fd_ = -1;
  std::atomic_bool running_{false};
  std::thread recv_thread_;
  std::unique_ptr<TcpStreamReader> cam0_;
  std::unique_ptr<TcpStreamReader> cam1_;
  std::unique_ptr<TcpStreamReader> imu_;
};

}  // namespace capture_client
