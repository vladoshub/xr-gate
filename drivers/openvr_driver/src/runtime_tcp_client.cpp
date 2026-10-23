#include "runtime_tcp_client.hpp"
#include <xr_spatial/contracts/spatial_mesh_wire.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xr::openvr_driver {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxLineBytes = 1024 * 1024;

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

uint64_t integer(const Json& value) {
  if (value.is_number_unsigned()) return value.get<uint64_t>();
  if (value.is_number_integer() && value.get<int64_t>() >= 0) return value.get<uint64_t>();
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return result;
  }
  throw std::runtime_error("TCP protocol: expected unsigned decimal integer");
}

uint32_t integer32(const Json& value) {
  const auto n = integer(value);
  if (n > UINT32_MAX) throw std::runtime_error("TCP protocol: integer exceeds uint32");
  return static_cast<uint32_t>(n);
}

template <class T>
bool real(const Json& value, T& out) {
  out = T{};
  if (!value.is_number()) return false;  // JSON null represents non-finite server values.
  const double n = value.get<double>();
  if (!std::isfinite(n) || std::abs(n) > static_cast<double>(std::numeric_limits<T>::max())) return false;
  out = static_cast<T>(n);
  return true;
}

template <class T, size_t N>
bool vector(const Json& value, std::array<T, N>& out) {
  if (!value.is_array() || value.size() != N) return false;
  bool valid = true;
  for (size_t i = 0; i < N; ++i) valid = real(value[i], out[i]) && valid;
  return valid;
}

template <class T>
bool pose(const Json& value, T& px, T& py, T& pz, T& qx, T& qy, T& qz, T& qw) {
  std::array<T, 3> p{};
  std::array<T, 4> q{};
  const bool valid = vector(value.at("position"), p) && vector(value.at("orientation_xyzw"), q);
  const double norm = double(q[0])*q[0] + double(q[1])*q[1] + double(q[2])*q[2] + double(q[3])*q[3];
  if (!valid || norm < 1e-12) { px = py = pz = qx = qy = qz = 0; qw = 1; return false; }
  px = p[0]; py = p[1]; pz = p[2];
  qx = q[0]; qy = q[1]; qz = q[2]; qw = q[3];
  return true;
}

template <class T>
bool velocity(const Json& value, T& x, T& y, T& z) {
  std::array<T, 3> v{};
  const bool valid = vector(value, v);
  x = v[0]; y = v[1]; z = v[2];
  return valid;
}

int64_t server_age(uint64_t tick, uint64_t timestamp) {
  // Both operands are in the SERVER clock domain. Predicted poses may be ahead.
  const auto age = tick > timestamp ? tick - timestamp : 0;
  return static_cast<int64_t>(std::min(age, uint64_t(INT64_MAX)));
}

xr_runtime::RuntimeHmdPoseF64V1 hmd(const Json& value) {
  xr_runtime::RuntimeHmdPoseF64V1 out;
  out.sequence = integer(value.at("sequence"));
  out.timestamp_ns = integer(value.at("timestamp_ns"));
  out.source_timestamp_ns = integer(value.at("source_timestamp_ns"));
  out.target_timestamp_ns = integer(value.at("target_timestamp_ns"));
  out.source_sequence = integer(value.at("source_sequence"));
  out.reset_counter = integer(value.at("reset_counter"));
  out.tracking_status = integer32(value.at("tracking_status"));
  out.flags = integer32(value.at("flags"));
  const bool valid_pose = pose(value, out.px, out.py, out.pz, out.qx, out.qy, out.qz, out.qw);
  const bool confidence = real(value.at("confidence"), out.confidence);
  if (!valid_pose || !confidence || !value.at("pose_valid").get<bool>()) out.flags &= ~xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID;
  if (!velocity(value.at("linear_velocity"), out.vx, out.vy, out.vz)) out.flags &= ~xr_runtime::RUNTIME_HMD_FLAG_LINEAR_VELOCITY_VALID;
  if (!velocity(value.at("angular_velocity"), out.wx, out.wy, out.wz)) out.flags &= ~xr_runtime::RUNTIME_HMD_FLAG_ANGULAR_VELOCITY_VALID;
  return out;
}

xr_runtime::RuntimeControllerSideStateV1 controller(const Json& value, uint32_t role) {
  xr_runtime::RuntimeControllerSideStateV1 out;
  out.role = role;
  out.flags = integer32(value.at("flags"));
  out.source_mask = integer32(value.at("source_mask"));
  out.source_device_hash = integer(value.at("source_device_hash"));
  out.last_input_ns = integer(value.at("last_input_ns"));
  out.last_pose_ns = integer(value.at("last_pose_ns"));
  if (!pose(value, out.position[0], out.position[1], out.position[2], out.orientation_xyzw[0],
            out.orientation_xyzw[1], out.orientation_xyzw[2], out.orientation_xyzw[3]) ||
      !value.at("pose_valid").get<bool>()) out.flags &= ~xr_runtime::RUNTIME_CONTROLLER_POSE_VALID;
  if (!value.at("connected").get<bool>()) out.flags &= ~xr_runtime::RUNTIME_CONTROLLER_CONNECTED;
  if (!value.at("input_valid").get<bool>()) out.flags &= ~xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  velocity(value.at("linear_velocity"), out.linear_velocity[0], out.linear_velocity[1], out.linear_velocity[2]);
  velocity(value.at("angular_velocity"), out.angular_velocity[0], out.angular_velocity[1], out.angular_velocity[2]);
  out.buttons = integer(value.at("buttons"));
  out.touches = integer(value.at("touches"));
  out.changed_buttons = integer(value.at("changed_buttons"));
  bool analog_valid = real(value.at("trigger"), out.trigger);
  analog_valid = real(value.at("grip"), out.grip) && analog_valid;
  std::array<float, 2> stick{};
  analog_valid = vector(value.at("thumbstick"), stick) && analog_valid;
  out.thumbstick_x = stick[0]; out.thumbstick_y = stick[1];
  const auto& press = value.at("press_counters");
  const auto& release = value.at("release_counters");
  if (!press.is_array() || press.size() != 32 || !release.is_array() || release.size() != 32)
    throw std::runtime_error("TCP protocol: expected 32 button counters");
  for (size_t i = 0; i < 32; ++i) { out.press_counters[i] = integer32(press[i]); out.release_counters[i] = integer32(release[i]); }
  if (!analog_valid) out.flags &= ~xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  if ((out.flags & xr_runtime::RUNTIME_CONTROLLER_CONNECTED) == 0) out.flags &= ~xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  if ((out.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) == 0) {
    out.buttons = out.touches = out.changed_buttons = 0;
    out.trigger = out.grip = out.thumbstick_x = out.thumbstick_y = 0;
  }
  return out;
}

xr_tracking::HandSideF32V2 hand(const Json& value, uint32_t handedness) {
  xr_tracking::HandSideF32V2 out;
  out.handedness = handedness;
  out.status = integer32(value.at("status")); out.flags = integer32(value.at("flags"));
  bool valid = real(value.at("confidence"), out.confidence);
  valid = pose(value.at("controller"), out.controller_px, out.controller_py, out.controller_pz,
               out.controller_qx, out.controller_qy, out.controller_qz, out.controller_qw) && valid;
  valid = pose(value.at("palm"), out.palm_px, out.palm_py, out.palm_pz,
               out.palm_qx, out.palm_qy, out.palm_qz, out.palm_qw) && valid;
  valid = pose(value.at("wrist"), out.wrist_px, out.wrist_py, out.wrist_pz,
               out.wrist_qx, out.wrist_qy, out.wrist_qz, out.wrist_qw) && valid;
  if (!valid || !value.at("pose_valid").get<bool>()) out.flags &= ~xr_tracking::HAND_POSE_VALID;
  if (!velocity(value.at("linear_velocity"), out.vx, out.vy, out.vz)) out.flags &= ~xr_tracking::HAND_LINEAR_VELOCITY_VALID;
  if (!velocity(value.at("angular_velocity"), out.wx, out.wy, out.wz)) out.flags &= ~xr_tracking::HAND_ANGULAR_VELOCITY_VALID;
  if (!real(value.at("pinch_strength"), out.pinch_strength)) out.flags &= ~xr_tracking::HAND_PINCH_VALID;
  if (!real(value.at("grab_strength"), out.grab_strength)) out.flags &= ~xr_tracking::HAND_GRAB_VALID;
  out.pinch_active = value.at("pinch_active").get<bool>();
  out.grab_active = value.at("grab_active").get<bool>();
  if ((out.flags & xr_tracking::HAND_PINCH_VALID) == 0) out.pinch_active = 0;
  if ((out.flags & xr_tracking::HAND_GRAB_VALID) == 0) out.grab_active = 0;
  const auto& joints = value.at("joints");
  if (!joints.is_array() || joints.size() > xr_tracking::HAND_JOINT_COUNT_V2)
    throw std::runtime_error("TCP protocol: invalid hand joint count");
  out.joint_count = static_cast<uint32_t>(joints.size());
  for (size_t i = 0; i < joints.size(); ++i) {
    const auto& src = joints[i]; auto& dst = out.joints[i];
    dst.joint_id = integer32(src.at("joint_id")); dst.flags = integer32(src.at("flags"));
    bool joint_valid = pose(src, dst.px, dst.py, dst.pz, dst.qx, dst.qy, dst.qz, dst.qw);
    joint_valid = real(src.at("radius_m"), dst.radius_m) && joint_valid;
    joint_valid = real(src.at("confidence"), dst.confidence) && joint_valid;
    if (!joint_valid) { dst.flags = 0; out.flags &= ~xr_tracking::HAND_JOINTS_VALID; }
  }
  return out;
}

RuntimeTcpSnapshot decode_frame(const Json& value) {
  if (value.at("type") != "frame") throw std::runtime_error("TCP protocol: expected frame");
  RuntimeTcpSnapshot out;
  out.sequence = integer(value.at("sequence"));
  const auto tick = integer(value.at("read_timestamp_ns"));
  if (!value.at("hmd").is_null()) {
    out.pose = hmd(value.at("hmd")); out.pose_age_ns = server_age(tick, out.pose->timestamp_ns);
  }
  if (!value.at("hands").is_null()) {
    const auto& src = value.at("hands"); xr_tracking::HandTrackingFrameF32V2 h;
    h.sequence = integer(src.at("sequence")); h.timestamp_ns = integer(src.at("timestamp_ns"));
    h.source_timestamp_ns = integer(src.at("source_timestamp_ns")); h.reset_counter = integer(src.at("reset_counter"));
    h.tracking_status = integer32(src.at("tracking_status")); h.flags = integer32(src.at("flags"));
    real(src.at("confidence"), h.confidence); h.hand_count = integer32(src.at("hand_count"));
    h.left = hand(src.at("left"), 1); h.right = hand(src.at("right"), 2);
    out.hands = h; out.hand_age_ns = server_age(tick, h.timestamp_ns);
  }
  if (!value.at("controllers").is_null()) {
    const auto& src = value.at("controllers"); xr_runtime::RuntimeControllerStateFrameV1 c;
    c.sequence = integer(src.at("sequence")); c.timestamp_ns = integer(src.at("timestamp_ns"));
    c.flags = integer32(src.at("flags"));
    c.left = controller(src.at("left"), xr_runtime::CONTROLLER_SIDE_LEFT);
    c.right = controller(src.at("right"), xr_runtime::CONTROLLER_SIDE_RIGHT);
    out.controllers = c; out.controller_age_ns = server_age(tick, c.timestamp_ns);
  }
  out.received_ns = now_ns();
  return out;
}

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
using SocketLength = int;
void close_socket(Socket s) { if (s != kInvalidSocket) ::closesocket(s); }
bool pending_error() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
using SocketLength = socklen_t;
void close_socket(Socket s) { if (s != kInvalidSocket) ::close(s); }
bool pending_error() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR; }
#endif

struct Connection {
  Socket fd = kInvalidSocket;
  ~Connection() { close_socket(fd); }
};

bool wait_socket(Socket fd, bool writing) {
  fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
  timeval timeout{}; timeout.tv_usec = 50000;
  const int result = ::select(static_cast<int>(fd + 1), writing ? nullptr : &set, writing ? &set : nullptr,
                              nullptr, &timeout);
  if (result < 0 && !pending_error()) throw std::runtime_error("TCP socket wait failed");
  return result > 0;
}
}  // namespace

struct RuntimeTcpClient::Impl {
  explicit Impl(RuntimeTcpConfig value) : config(std::move(value)) {
    if (config.host == "localhost") config.host = "127.0.0.1";
    worker = std::thread([this] { run(); });
  }
  ~Impl() { stopping = true; wake.notify_all(); if (worker.joinable()) worker.join(); }

  void invalidate(const std::string& message) {
    std::lock_guard<std::mutex> guard(mutex);
    connected = false; latest = {}; error = message;
  }

  void connect_socket(Connection& socket) {
    // Winsock startup and configuration errors also stay off the VR thread.
#ifdef _WIN32
    static const bool winsock = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    if (!winsock) throw std::runtime_error("TCP WSAStartup failed");
#endif
    in_addr parsed_address{};
    if (::inet_pton(AF_INET, config.host.c_str(), &parsed_address) != 1 || !config.port || !config.timeout_ms)
      throw std::runtime_error("runtimeTcpHost must be an IPv4 address or localhost; TCP port/timeout must be positive");
    socket.fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket.fd == kInvalidSocket) throw std::runtime_error("TCP socket creation failed");
#ifdef _WIN32
    u_long nonblocking = 1;
    if (::ioctlsocket(socket.fd, FIONBIO, &nonblocking) != 0) throw std::runtime_error("TCP nonblocking socket failed");
#else
    if (::fcntl(socket.fd, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("TCP nonblocking socket failed");
#endif
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config.port);
    address.sin_addr = parsed_address;
    if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      if (!pending_error()) throw std::runtime_error("TCP connect failed");
      const auto deadline = Clock::now() + std::chrono::milliseconds(config.timeout_ms);
      while (!stopping && Clock::now() < deadline) {
        if (!wait_socket(socket.fd, true)) continue;
        int status = 0; SocketLength length = sizeof(status);
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &length) != 0 || status)
          throw std::runtime_error("TCP connect failed");
        return;
      }
      throw std::runtime_error("TCP connect timeout");
    }
  }

  void session() {
    Connection socket; connect_socket(socket);
    const uint64_t epoch = ++connection_epoch;
    int one = 1;
    ::setsockopt(socket.fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    std::array<char, 8192> chunk{};
    std::string buffer;
    bool hello = false;
    uint64_t last_sequence = 0;
    auto progress = Clock::now();
    while (!stopping) {
      if (Clock::now() - progress > std::chrono::milliseconds(config.timeout_ms))
        throw std::runtime_error("TCP frame/hello timeout");
      if (!wait_socket(socket.fd, false)) continue;
      const auto size = ::recv(socket.fd, chunk.data(), static_cast<int>(chunk.size()), 0);
      if (size == 0) throw std::runtime_error("TCP server disconnected");
      if (size < 0) { if (pending_error()) continue; throw std::runtime_error("TCP receive failed"); }
      buffer.append(chunk.data(), static_cast<size_t>(size));
      size_t consumed = 0;
      for (;;) {
        const auto end = buffer.find('\n', consumed);
        if (end == std::string::npos) break;
        if (end - consumed > kMaxLineBytes) throw std::runtime_error("TCP message exceeds 1 MiB");
        const auto value = Json::parse(buffer.begin() + consumed, buffer.begin() + end);
        consumed = end + 1;
        if (!hello) {
          if (value.at("type") != "hello" || value.at("protocol") != "xr_gate_app_v1" ||
              value.at("quaternion_order") != "xyzw" || value.at("position_unit") != "metres" ||
              value.at("timestamp_clock") != "server_monotonic")
            throw std::runtime_error("TCP incompatible hello");
          hello = true; progress = Clock::now(); continue;
        }
        if (value.at("type") == "spatial_mesh") {
          if (value.at("format") != xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME ||
              value.at("byte_order") != "little_endian")
            throw std::runtime_error("TCP incompatible spatial mesh contract");
          std::shared_ptr<const xr_spatial::RuntimeSpatialProxyMeshF32V1> mesh;
          int64_t initial_age = 0;
          if (!value.at("mesh_hex").is_null()) {
            auto decoded = xr_spatial::mesh_from_hex(value.at("mesh_hex").get<std::string>());
            initial_age = server_age(integer(value.at("read_timestamp_ns")), decoded.timestamp_ns);
            mesh = std::make_shared<const xr_spatial::RuntimeSpatialProxyMeshF32V1>(std::move(decoded));
          }
          std::lock_guard<std::mutex> guard(mutex);
          latest.spatial = std::move(mesh);
          latest.spatial_received_ns = now_ns(); latest.spatial_age_ns = initial_age;
          continue;
        }
        auto frame = decode_frame(value);
        frame.connection_epoch = epoch;
        // Replayed snapshots must not keep a frozen pose or held button alive.
        if (frame.sequence <= last_sequence) continue;
        last_sequence = frame.sequence;
        { std::lock_guard<std::mutex> guard(mutex); frame.spatial=latest.spatial; frame.spatial_received_ns=latest.spatial_received_ns;
          frame.spatial_age_ns=latest.spatial_age_ns; latest = std::move(frame); connected = true; error.clear(); }
        progress = Clock::now();
      }
      buffer.erase(0, consumed);
      if (buffer.size() > kMaxLineBytes) throw std::runtime_error("TCP unterminated message exceeds 1 MiB");
    }
  }

  void run() {
    while (!stopping) {
      try { session(); }
      catch (const std::exception& e) { invalidate(e.what()); }
      std::unique_lock<std::mutex> lock(wait_mutex);
      wake.wait_for(lock, std::chrono::milliseconds(std::max(10u, config.reconnect_ms)), [this] { return stopping.load(); });
    }
    invalidate("TCP client stopped");
  }

  RuntimeTcpConfig config;
  std::atomic<bool> stopping{false};
  std::mutex mutex, wait_mutex;
  std::condition_variable wake;
  RuntimeTcpSnapshot latest;
  bool connected = false;
  uint64_t connection_epoch = 0;
  std::string error = "TCP waiting for runtime server";
  std::thread worker;
};

RuntimeTcpClient::RuntimeTcpClient(RuntimeTcpConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
RuntimeTcpClient::~RuntimeTcpClient() = default;

std::shared_ptr<RuntimeTcpClient> RuntimeTcpClient::acquire(const RuntimeTcpConfig& config) {
  static std::mutex mutex;
  using Key = std::tuple<std::string, uint16_t, uint32_t, uint32_t>;
  static std::map<Key, std::weak_ptr<RuntimeTcpClient>> clients;
  std::lock_guard<std::mutex> guard(mutex);
  for (auto it = clients.begin(); it != clients.end();) {
    if (it->second.expired()) it = clients.erase(it); else ++it;
  }
  const Key key{config.host == "localhost" ? "127.0.0.1" : config.host, config.port, config.reconnect_ms, config.timeout_ms};
  if (auto previous = clients[key].lock()) return previous;
  auto client = std::shared_ptr<RuntimeTcpClient>(new RuntimeTcpClient(config));
  clients[key] = client;
  return client;
}

bool RuntimeTcpClient::read_latest(RuntimeTcpSnapshot& out, std::string* error) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->connected) { out = {}; if (error) *error = impl_->error; return false; }
  out = impl_->latest;
  if (error) error->clear();
  return true;
}

}  // namespace xr::openvr_driver
