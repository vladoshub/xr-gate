#include "app_tcp_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace xr_runtime_adapter {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket fd) { if (fd != invalid_socket) ::closesocket(fd); }
bool retry_socket_error() {
  const int e = ::WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINTR;
}
bool nonblocking(Socket fd) {
  u_long one = 1;
  return ::ioctlsocket(fd, FIONBIO, &one) == 0;
}
void init_sockets() {
  static const bool initialized = [] {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("app TCP: WSAStartup failed");
    return true;
  }();
  (void)initialized;
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket fd) { if (fd != invalid_socket) ::close(fd); }
bool retry_socket_error() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool nonblocking(Socket fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
void init_sockets() {}
#endif

// Also closes partially initialized sockets when bind or thread startup fails.
struct SocketOwner {
  Socket fd = invalid_socket;
  ~SocketOwner() { close_socket(fd); }
};

Socket listen_socket(const AppTcpServerConfig& config) {
  init_sockets();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const auto service = std::to_string(config.port);
  const int rc = ::getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses);
  if (rc != 0) throw std::runtime_error("app TCP: cannot resolve bind host '" + config.host + "'");
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(addresses, ::freeaddrinfo);
  for (const auto* addr = addresses; addr; addr = addr->ai_next) {
    SocketOwner socket{::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol)};
    if (socket.fd == invalid_socket) continue;
    const int one = 1;
#ifdef _WIN32
    ::setsockopt(socket.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(socket.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::fcntl(socket.fd, F_SETFD, FD_CLOEXEC);
#endif
    if (!nonblocking(socket.fd)) continue;
    if (::bind(socket.fd, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) != 0) continue;
    if (::listen(socket.fd, config.max_clients) != 0) continue;
    return std::exchange(socket.fd, invalid_socket);
  }
  throw std::runtime_error("app TCP: cannot listen on " + config.host + ":" + service);
}

Json vector3(double x, double y, double z) { return Json::array({x, y, z}); }
Json quaternion(double x, double y, double z, double w) { return Json::array({x, y, z, w}); }
Json pose(double x, double y, double z, double qx, double qy, double qz, double qw) {
  return {{"position", vector3(x, y, z)}, {"orientation_xyzw", quaternion(qx, qy, qz, qw)}};
}

Json hmd_json(const xr_runtime::RuntimeHmdPoseF64V1& p) {
  auto out = pose(p.px, p.py, p.pz, p.qx, p.qy, p.qz, p.qw);
  out.update({{"sequence", uint64_t(p.sequence)},
              {"timestamp_ns", std::to_string(p.timestamp_ns)},
              {"source_timestamp_ns", std::to_string(p.source_timestamp_ns)},
              {"target_timestamp_ns", std::to_string(p.target_timestamp_ns)},
              {"source_sequence", uint64_t(p.source_sequence)},
              {"reset_counter", uint64_t(p.reset_counter)},
              {"tracking_status", uint32_t(p.tracking_status)}, {"flags", uint32_t(p.flags)},
              {"pose_valid", (p.flags & xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID) != 0},
              {"confidence", double(p.confidence)},
              {"linear_velocity", vector3(p.vx, p.vy, p.vz)},
              {"angular_velocity", vector3(p.wx, p.wy, p.wz)}});
  return out;
}

Json controller_json(const xr_runtime::RuntimeControllerSideStateV1& c) {
  auto out = pose(c.position[0], c.position[1], c.position[2],
                  c.orientation_xyzw[0], c.orientation_xyzw[1], c.orientation_xyzw[2], c.orientation_xyzw[3]);
  Json press = Json::array(), release = Json::array();
  for (size_t i = 0; i < 32; ++i) {
    press.push_back(uint32_t(c.press_counters[i]));
    release.push_back(uint32_t(c.release_counters[i]));
  }
  out.update({{"flags", uint32_t(c.flags)}, {"source_mask", uint32_t(c.source_mask)},
              {"connected", (c.flags & xr_runtime::RUNTIME_CONTROLLER_CONNECTED) != 0},
              {"pose_valid", (c.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) != 0},
              {"input_valid", (c.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) != 0},
              {"source_device_hash", std::to_string(c.source_device_hash)},
              {"last_input_ns", std::to_string(c.last_input_ns)},
              {"last_pose_ns", std::to_string(c.last_pose_ns)},
              {"linear_velocity", vector3(c.linear_velocity[0], c.linear_velocity[1], c.linear_velocity[2])},
              {"angular_velocity", vector3(c.angular_velocity[0], c.angular_velocity[1], c.angular_velocity[2])},
              {"buttons", std::to_string(c.buttons)}, {"touches", std::to_string(c.touches)},
              {"changed_buttons", std::to_string(c.changed_buttons)},
              {"trigger", double(c.trigger)}, {"grip", double(c.grip)},
              {"thumbstick", Json::array({double(c.thumbstick_x), double(c.thumbstick_y)})},
              {"press_counters", std::move(press)}, {"release_counters", std::move(release)}});
  return out;
}

Json hand_json(const xr_tracking::HandSideF32V2& h) {
  Json joints = Json::array();
  const uint32_t joint_count = (h.flags & xr_tracking::HAND_JOINTS_VALID) != 0
      ? std::min(uint32_t(h.joint_count), xr_tracking::HAND_JOINT_COUNT_V2) : 0;
  for (uint32_t i = 0; i < joint_count; ++i) {
    const auto& j = h.joints[i];
    auto joint = pose(j.px, j.py, j.pz, j.qx, j.qy, j.qz, j.qw);
    joint.update({{"joint_id", uint32_t(j.joint_id)}, {"flags", uint32_t(j.flags)},
                  {"radius_m", double(j.radius_m)}, {"confidence", double(j.confidence)}});
    joints.push_back(std::move(joint));
  }
  return {{"status", uint32_t(h.status)}, {"flags", uint32_t(h.flags)},
          {"pose_valid", (h.flags & xr_tracking::HAND_POSE_VALID) != 0},
          {"confidence", double(h.confidence)},
          {"controller", pose(h.controller_px, h.controller_py, h.controller_pz,
                              h.controller_qx, h.controller_qy, h.controller_qz, h.controller_qw)},
          {"palm", pose(h.palm_px, h.palm_py, h.palm_pz, h.palm_qx, h.palm_qy, h.palm_qz, h.palm_qw)},
          {"wrist", pose(h.wrist_px, h.wrist_py, h.wrist_pz, h.wrist_qx, h.wrist_qy, h.wrist_qz, h.wrist_qw)},
          {"linear_velocity", vector3(h.vx, h.vy, h.vz)}, {"angular_velocity", vector3(h.wx, h.wy, h.wz)},
          {"pinch_strength", double(h.pinch_strength)}, {"grab_strength", double(h.grab_strength)},
          {"pinch_active", h.pinch_active != 0}, {"grab_active", h.grab_active != 0},
          {"joints", std::move(joints)}};
}

std::string frame_json(const AppTcpFrame& frame, uint64_t sequence) {
  Json out{{"type", "frame"}, {"sequence", sequence},
           {"read_timestamp_ns", std::to_string(frame.read_timestamp_ns)},
           {"hmd", nullptr}, {"hands", nullptr}, {"controllers", nullptr}};
  if (frame.hmd) out["hmd"] = hmd_json(*frame.hmd);
  if (frame.hands) {
    const auto& h = *frame.hands;
    out["hands"] = {{"sequence", uint64_t(h.sequence)}, {"timestamp_ns", std::to_string(h.timestamp_ns)},
                    {"source_timestamp_ns", std::to_string(h.source_timestamp_ns)},
                    {"reset_counter", uint64_t(h.reset_counter)}, {"tracking_status", uint32_t(h.tracking_status)},
                    {"flags", uint32_t(h.flags)}, {"confidence", double(h.confidence)},
                    {"hand_count", uint32_t(h.hand_count)}, {"left", hand_json(h.left)}, {"right", hand_json(h.right)}};
  }
  if (frame.controllers) {
    const auto& c = *frame.controllers;
    out["controllers"] = {{"sequence", uint64_t(c.sequence)}, {"timestamp_ns", std::to_string(c.timestamp_ns)},
                          {"flags", uint32_t(c.flags)}, {"left", controller_json(c.left)}, {"right", controller_json(c.right)}};
  }
  return out.dump() + '\n';
}

struct Client {
  Socket fd;
  std::shared_ptr<const std::string> current;
  std::shared_ptr<const std::string> next;
  size_t offset = 0;
  Clock::time_point started = Clock::now();
};

}  // namespace

class AppTcpServer::Impl {
 public:
  explicit Impl(AppTcpServerConfig config) : config_(std::move(config)) {
    if (config_.host.empty()) throw std::runtime_error("--app-tcp-host must not be empty");
    if (config_.port < 1 || config_.port > 65535) throw std::runtime_error("--app-tcp-port must be 1..65535");
    if (!std::isfinite(config_.rate_hz) || config_.rate_hz < 1 || config_.rate_hz > 240) {
      throw std::runtime_error("--app-tcp-rate must be 1..240 Hz");
    }
    if (config_.max_clients < 1 || config_.max_clients > 32) {
      throw std::runtime_error("--app-tcp-max-clients must be 1..32");
    }
    hello_ = std::make_shared<const std::string>(Json{
        {"type", "hello"}, {"protocol", "xr_gate_app_v1"}, {"runtime_frame", config_.runtime_frame},
        {"rate_hz", config_.rate_hz}, {"position_unit", "metres"}, {"linear_velocity_unit", "metres/second"},
        {"angular_velocity_unit", "radians/second"}, {"quaternion_order", "xyzw"},
        {"hand_joint_order", "wrist,thumb[4],index[4],middle[4],ring[4],little[4]"},
        {"timestamp_clock", "server_monotonic"}, {"delivery", "latest_frame"}}.dump() + '\n');
    listener_.fd = listen_socket(config_);
    worker_ = std::thread([this] { run(); });
  }

  ~Impl() {
    stop_ = true;
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
    for (const auto& client : clients_) close_socket(client.fd);
  }

  bool has_clients() const { return client_count_.load(std::memory_order_relaxed) != 0; }

  void publish(const AppTcpFrame& frame) {
    if (!has_clients()) return;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;  // A newer runtime tick will replace it.
    latest_ = frame;
    ++generation_;
    lock.unlock();
    wake_.notify_one();
  }

 private:
  void accept_clients() {
    // Bound accept work even if other processes repeatedly connect.
    for (int i = 0; i < 16; ++i) {
      Socket fd = ::accept(listener_.fd, nullptr, nullptr);
      if (fd == invalid_socket) break;
      if (clients_.size() >= static_cast<size_t>(config_.max_clients) || !nonblocking(fd)) {
        close_socket(fd);
        continue;
      }
      const int one = 1, send_buffer = 16 * 1024;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
      ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer));
#ifndef _WIN32
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
      ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
      clients_.push_back(Client{fd, hello_, nullptr});
    }
    client_count_.store(clients_.size(), std::memory_order_relaxed);
  }

  bool send_client(Client& client, Clock::time_point now) {
    char input;
    const auto received = ::recv(client.fd, &input, 1, 0);
    // This is a read-only streaming API; there is no inbound command protocol.
    if (received >= 0 || !retry_socket_error()) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!client.current) {
        client.current = std::move(client.next);
        client.offset = 0;
        client.started = now;
      }
      if (!client.current) break;
      if (now - client.started > std::chrono::milliseconds(250)) return false;
      const auto& data = *client.current;
#ifdef MSG_NOSIGNAL
      constexpr int send_flags = MSG_NOSIGNAL;
#else
      constexpr int send_flags = 0;
#endif
      const auto sent = ::send(client.fd, data.data() + client.offset,
                               static_cast<int>(data.size() - client.offset), send_flags);
      if (sent < 0) return retry_socket_error();
      if (sent == 0) return false;
      client.offset += static_cast<size_t>(sent);
      if (client.offset != data.size()) break;
      client.current.reset();
    }
    return true;
  }

  void run() noexcept {
    try {
      const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / config_.rate_hz));
      auto next_frame = Clock::now();
      uint64_t serialized_generation = 0;
      while (!stop_) {
        accept_clients();
        const auto now = Clock::now();
        if (!clients_.empty() && now >= next_frame) {
          std::optional<AppTcpFrame> snapshot;
          uint64_t generation;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            generation = generation_;
            if (generation != serialized_generation) snapshot = latest_;
          }
          if (snapshot) {
            auto packet = std::make_shared<const std::string>(frame_json(*snapshot, generation));
            for (auto& client : clients_) client.next = packet;
            serialized_generation = generation;
            next_frame = Clock::now() + period;
          }
        }
        for (auto it = clients_.begin(); it != clients_.end();) {
          if (send_client(*it, Clock::now())) { ++it; }
          else { close_socket(it->fd); it = clients_.erase(it); }
        }
        client_count_.store(clients_.size(), std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(mutex_);
        auto deadline = Clock::now() + std::chrono::milliseconds(10);
        if (!clients_.empty() && generation_ != serialized_generation) deadline = std::min(deadline, next_frame);
        wake_.wait_until(lock, deadline);
      }
    } catch (const std::exception& e) {
      std::cerr << "[xr_runtime_adapter] app TCP worker stopped: " << e.what() << '\n';
      client_count_ = 0;
    }
  }

  AppTcpServerConfig config_;
  SocketOwner listener_;
  std::shared_ptr<const std::string> hello_;
  std::atomic_bool stop_{false};
  std::atomic<size_t> client_count_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  AppTcpFrame latest_;
  uint64_t generation_ = 0;
  std::vector<Client> clients_;  // Worker-owned until joined.
  std::thread worker_;
};

AppTcpServer::AppTcpServer(AppTcpServerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
AppTcpServer::~AppTcpServer() = default;
bool AppTcpServer::has_clients() const { return impl_->has_clients(); }
void AppTcpServer::publish(const AppTcpFrame& frame) { impl_->publish(frame); }

}  // namespace xr_runtime_adapter
