#include "capture_service_cpp/platform/tcp_socket.hpp"
#include "capture_service_cpp/sources/tcp_input.hpp"
#include "capture_service_cpp/timing/affine_device_clock_mapper.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#endif

namespace xr_capture_cpp {
namespace {

using namespace tcp_socket;
using Json = nlohmann::json;

enum class Message : uint16_t { Hello = 1, Frames, Imu, Ping, Pong, Ready };
constexpr size_t kHeaderBytes = 12;
constexpr size_t kSampleHeaderBytes = 16;
constexpr size_t kImuBytes = 40;
constexpr uint32_t kMaxHelloBytes = 65536;
constexpr uint64_t kHandshakeNs = 5000000000ULL;
constexpr uint64_t kReadNs = 5000000000ULL;
constexpr uint64_t kPingIntervalNs = 1000000000ULL;
constexpr unsigned kInitialProbes = 3;
constexpr int kPollMs = 50;

const char* role_name(TcpInputRole role) {
  if (role == TcpInputRole::Primary) return "primary";
  return role == TcpInputRole::External ? "external" : "imu";
}

template <typename T>
T read_le(const uint8_t* p) {
  T result = 0;
  for (size_t i = 0; i < sizeof(T); ++i) result |= static_cast<T>(p[i]) << (8 * i);
  return result;
}

float read_f32(const uint8_t* p) {
  static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
  const auto bits = read_le<uint32_t>(p);
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

bool valid_session_id(const std::string& id) {
  if (id.size() != 36) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? id[i] != '-' : !std::isxdigit(static_cast<unsigned char>(id[i]))) return false;
  }
  return true;
}

struct Probe {
  uint64_t sender_midpoint = 0;
  uint64_t receiver_midpoint = 0;
  uint64_t round_trip = std::numeric_limits<uint64_t>::max();
};

void set_nonblocking(socket_t sock, bool enabled) {
#ifdef _WIN32
  unsigned long mode = enabled ? 1 : 0;
  if (ioctlsocket(sock, FIONBIO, &mode) != 0) throw std::runtime_error("ioctlsocket failed");
#else
  const int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0 || fcntl(sock, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0) {
    throw std::runtime_error("fcntl failed");
  }
#endif
}

bool readable(socket_t sock) {
#ifdef _WIN32
  WSAPOLLFD item{sock, POLLIN, 0};
  return WSAPoll(&item, 1, kPollMs) > 0;
#else
  pollfd item{sock, POLLIN, 0};
  return poll(&item, 1, kPollMs) > 0;
#endif
}

socket_t listen_on(const TcpInputConfig& cfg) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* raw = nullptr;
  const auto port = std::to_string(cfg.port);
  const std::string endpoint = cfg.bind_host + ":" + port;
  if (getaddrinfo(cfg.bind_host.c_str(), port.c_str(), &hints, &raw) != 0) {
    throw std::runtime_error("cannot resolve TCP input address " + endpoint);
  }
  const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
  int error = 0;
  for (const auto* addr = addresses.get(); addr; addr = addr->ai_next) {
    const auto sock = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (sock == kInvalidSocket) {
      error = last_socket_error();
      continue;
    }
    const int one = 1;
#ifdef _WIN32
    constexpr int reuse = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int reuse = SO_REUSEADDR;
#endif
    setsockopt(sock, SOL_SOCKET, reuse, reinterpret_cast<const char*>(&one), sizeof(one));
    if (::bind(sock, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0 && ::listen(sock, 1) == 0) {
      try { set_nonblocking(sock, true); }
      catch (...) { close_socket(sock); throw; }
      return sock;
    }
    error = last_socket_error();
    close_socket(sock);
  }
  throw std::runtime_error("cannot listen for TCP input at " + endpoint + " (socket error " +
                           std::to_string(error) + ")");
}

void configure_client(socket_t sock) {
  set_nonblocking(sock, false);
  const int one = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
  const DWORD timeout = 250;
#else
  const timeval timeout{0, 250000};
#endif
  for (const int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
    if (setsockopt(sock, SOL_SOCKET, option, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
      throw std::runtime_error("cannot set TCP input socket timeout");
    }
  }
}

void read_exact(socket_t sock, uint8_t* bytes, size_t size, uint64_t deadline) {
  size_t received = 0;
  while (received < size) {
    if (g_stop.load() || steady_ns() >= deadline) throw std::runtime_error("incomplete message timed out");
    if (!readable(sock)) continue;
    const auto count = ::recv(sock, reinterpret_cast<char*>(bytes + received), static_cast<int>(size - received), 0);
    if (count <= 0) throw std::runtime_error("sender disconnected");
    received += static_cast<size_t>(count);
  }
}

void send_control(socket_t sock, Message kind, uint64_t t0 = 0) {
  std::array<uint8_t, kHeaderBytes + sizeof(t0)> bytes{{'X', 'R', 'C', 'I', 1, 0,
                                                       static_cast<uint8_t>(kind)}};
  const size_t body_size = kind == Message::Ping ? sizeof(t0) : 0;
  bytes[8] = static_cast<uint8_t>(body_size);
  for (size_t i = 0; i < body_size; ++i) bytes[kHeaderBytes + i] = static_cast<uint8_t>(t0 >> (8 * i));
#ifdef _WIN32
  constexpr int flags = 0;
#else
  constexpr int flags = MSG_NOSIGNAL;
#endif
  const size_t size = kHeaderBytes + body_size;
  for (size_t sent = 0; sent < size;) {
    const auto count = ::send(sock, reinterpret_cast<const char*>(bytes.data() + sent),
                              static_cast<int>(size - sent), flags);
    if (count <= 0) throw std::runtime_error("could not send TCP input control message");
    sent += static_cast<size_t>(count);
  }
}

}  // namespace

class TcpInputClock {
 public:
  void observe(const Probe& probe) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (probe.sender_midpoint < last_sender_midpoint_) {
      throw std::runtime_error("sender clock went backwards within a TCP session");
    }
    last_sender_midpoint_ = probe.sender_midpoint;
    delays_.push_back(probe.round_trip);
    if (delays_.size() > 8) delays_.pop_front();
    const auto best_delay = *std::min_element(delays_.begin(), delays_.end());
    if (probe.round_trip - best_delay <= 1000000ULL) {
      mapper_.map_ns(probe.sender_midpoint, probe.receiver_midpoint);
    }
  }

  std::optional<uint64_t> project(uint64_t sender_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mapper_.project_ns(sender_ns);
  }

 private:
  mutable std::mutex mutex_;
  uint64_t last_sender_midpoint_ = 0;
  std::deque<uint64_t> delays_;
  AffineDeviceClockMapper mapper_;
};

TcpCaptureSession::TcpCaptureSession(std::function<void(bool)> on_imu_capability)
    : on_imu_capability_(std::move(on_imu_capability)) {}

std::shared_ptr<TcpInputClock> TcpCaptureSession::clock_for(const std::string& id, bool imu) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return id == id_ && (!imu || has_imu_) ? clock_ : nullptr;
}

bool TcpCaptureSession::imu_matches(const std::shared_ptr<TcpInputClock>& clock) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_imu_ && clock == clock_;
}

void TcpCaptureSession::set_primary(const std::string& id, bool has_imu,
                                    const std::shared_ptr<TcpInputClock>& clock) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (on_imu_capability_ && (id != id_ || has_imu != has_imu_)) on_imu_capability_(has_imu);
  id_ = id;
  has_imu_ = has_imu;
  clock_ = clock;
}

struct TcpSensorInput::Impl {
  Impl(TcpInputConfig input, TcpInputRole input_role, int stall_ms, std::vector<TcpInputView> input_views,
       std::shared_ptr<TcpCaptureSession> input_session)
      : cfg(std::move(input)), role(input_role), paired_imu(role == TcpInputRole::Imu && input_session),
        stall_ns(stall_ms > 0 ? static_cast<uint64_t>(stall_ms) * 1000000ULL : 0),
        views(std::move(input_views)), offsets(views.size()), session(std::move(input_session)) {
    if (role == TcpInputRole::Imu) {
      sample_bytes = kImuBytes;
      return;
    }
    sample_bytes = kSampleHeaderBytes;
    for (const auto& view : views) {
      const auto bytes = plane_bytes(view);
      if (bytes > static_cast<uint64_t>(std::numeric_limits<int>::max()) - sample_bytes) {
        throw std::runtime_error("configured TCP frame set is too large");
      }
      sample_bytes += static_cast<size_t>(bytes);
    }
  }

  ~Impl() { close(); }

  static uint64_t plane_bytes(const TcpInputView& view) {
    return static_cast<uint64_t>(view.width) * view.height * view.channels;
  }

  void close_client() {
    close_socket(std::exchange(client, kInvalidSocket));
    clock.reset();
    hello = ready = has_imu = false;
    session_id.clear();
    probes = 0;
    best_probe = {};
    ping = 0;
    last_sequence.reset();
    last_sender_ns = 0;
  }

  void close() {
    close_client();
    close_socket(std::exchange(listener, kInvalidSocket));
  }

  void open() {
    listener = listen_on(cfg);
    std::cerr << "[capture_service_cpp][tcp_input][" << role_name(role) << "] listening on "
              << cfg.bind_host << ":" << cfg.port << std::endl;
  }

  void accept_client() {
    if (!readable(listener)) return;
    client = ::accept(listener, nullptr, nullptr);
    if (client == kInvalidSocket) return;
    configure_client(client);
    handshake_deadline = steady_ns() + kHandshakeNs;
  }

  void send_ping() {
    ping = steady_ns();
    last_ping = ping;
    send_control(client, Message::Ping, ping);
  }

  void send_ready() {
    if (role == TcpInputRole::Primary && session) session->set_primary(session_id, has_imu, clock);
    send_control(client, Message::Ready);
    last_data = steady_ns();
    ready = true;
  }

  void read_hello() {
    const auto document = Json::parse(body.begin(), body.end(),
        [](int depth, Json::parse_event_t, Json&) {
          if (depth > 8) throw std::runtime_error("HELLO exceeds the maximum JSON nesting depth");
          return true;
        });
    if (document.at("role") != role_name(role)) throw std::runtime_error("unexpected HELLO role");
    session_id = document.at("session_id").get<std::string>();
    if (!valid_session_id(session_id)) throw std::runtime_error("invalid HELLO session_id");
    if (role == TcpInputRole::Primary) has_imu = document.at("has_imu").get<bool>();
    if (role != TcpInputRole::Imu) {
      const auto& advertised = document.at("views");
      if (!advertised.is_array() || advertised.size() != views.size()) {
        throw std::runtime_error("HELLO must describe all configured camera views");
      }
      std::fill(offsets.begin(), offsets.end(), 0);
      size_t offset = kSampleHeaderBytes;
      for (size_t i = 0; i < advertised.size(); ++i) {
        const auto& plane = advertised[i];
        const auto id = plane.at("id").get<std::string>();
        const auto view = std::find_if(views.begin(), views.end(), [&](const auto& v) { return v.id == id; });
        if (view == views.end()) throw std::runtime_error("unconfigured TCP camera view");
        const size_t index = static_cast<size_t>(view - views.begin());
        if (offsets[index] || (i < 2 && i != index) || plane.at("width") != view->width ||
            plane.at("height") != view->height || plane.at("format") != (view->channels == 1 ? "gray8" : "bgr8")) {
          throw std::runtime_error("HELLO view order, size or pixel format differs from configuration");
        }
        offsets[index] = offset;
        offset += static_cast<size_t>(plane_bytes(*view));
      }
    }
    hello = true;
    if (session) clock = session->clock_for(session_id, paired_imu);
    if (paired_imu) {
      if (!clock) throw std::runtime_error("IMU must match a ready primary session with has_imu=true");
      send_ready();
    } else {
      if (!clock) clock = std::make_shared<TcpInputClock>();
      send_ping();
    }
  }

  void read_pong(uint64_t now) {
    const uint64_t t0 = read_le<uint64_t>(body.data());
    const uint64_t t1 = read_le<uint64_t>(body.data() + 8);
    const uint64_t t2 = read_le<uint64_t>(body.data() + 16);
    if (!ping || t0 != ping) return;
    if (t1 == 0 || t2 < t1 || t2 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        t0 >= now || t2 - t1 > now - t0) {
      throw std::runtime_error("invalid TIME_PONG timestamps");
    }
    ping = 0;
    const Probe observation{t1 + (t2 - t1) / 2, t0 + (now - t0) / 2, (now - t0) - (t2 - t1)};
    if (ready) {
      clock->observe(observation);
    } else {
      if (observation.round_trip < best_probe.round_trip) best_probe = observation;
      if (++probes == kInitialProbes) {
        clock->observe(best_probe);
        send_ready();
      } else {
        send_ping();
      }
    }
  }

  bool read_sample(TcpInputSample& sample, uint64_t now) {
    if (paired_imu && !session->imu_matches(clock)) throw std::runtime_error("primary session changed");
    const uint64_t sequence = read_le<uint64_t>(body.data());
    const uint64_t capture_ns = read_le<uint64_t>(body.data() + 8);
    if ((last_sequence && sequence <= *last_sequence) || capture_ns <= last_sender_ns ||
        capture_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    const auto mapped = clock->project(capture_ns);
    if (!mapped || *mapped <= last_timestamp_ns || (*mapped > now && *mapped - now > 100000000ULL)) return false;
    if (role == TcpInputRole::Imu) {
      for (size_t i = 0; i < sample.imu.size(); ++i) {
        sample.imu[i] = read_f32(body.data() + kSampleHeaderBytes + i * sizeof(float));
        if (!std::isfinite(sample.imu[i])) return false;
      }
    }
    last_timestamp_ns = *mapped;
    last_sequence = sample.sequence = sequence;
    last_sender_ns = capture_ns;
    sample.timestamp_ns = *mapped;
    last_data = now;
    return true;
  }

  bool read_message(TcpInputSample& sample) {
    const uint64_t now = steady_ns();
    uint64_t deadline = now + kReadNs;
    if (!ready) deadline = handshake_deadline;
    else if (stall_ns) deadline = std::min(deadline, last_data + stall_ns);
    if (now >= deadline) throw std::runtime_error(ready ? "sensor input stalled" : "handshake timed out");
    if (ready && !paired_imu && now - last_ping >= kPingIntervalNs) send_ping();
    if (!readable(client)) return false;
    std::array<uint8_t, kHeaderBytes> header{};
    read_exact(client, header.data(), header.size(), deadline);
    if (std::memcmp(header.data(), "XRCI", 4) != 0 || read_le<uint16_t>(header.data() + 4) != 1) {
      throw std::runtime_error("expected XRCI protocol version 1");
    }
    const auto kind = static_cast<Message>(read_le<uint16_t>(header.data() + 6));
    const size_t size = read_le<uint32_t>(header.data() + 8);
    const auto data_kind = role == TcpInputRole::Imu ? Message::Imu : Message::Frames;
    const bool hello_message = !hello && kind == Message::Hello && size > 0 && size <= kMaxHelloBytes;
    const bool pong_message = hello && !paired_imu && kind == Message::Pong && size == 24;
    const bool data_message = ready && kind == data_kind && size == sample_bytes;
    if (!hello_message && !pong_message && !data_message) throw std::runtime_error("unexpected message type or size");
    body.resize(size);
    read_exact(client, body.data(), body.size(), deadline);
    const auto received = steady_ns();
    if (hello_message) read_hello();
    else if (pong_message) read_pong(received);
    else return read_sample(sample, received);
    return false;
  }

  bool read(TcpInputSample& sample) {
    try {
      if (client == kInvalidSocket) accept_client();
      if (client != kInvalidSocket) return read_message(sample);
    } catch (const std::runtime_error& e) {
      if (!g_stop.load()) {
        const bool exit_on_disconnect = cfg.exit_on_disconnect && ready;
        std::cerr << "[capture_service_cpp][tcp_input][" << role_name(role) << ":" << cfg.port << "] "
                  << e.what() << (exit_on_disconnect ? "; stopping capture" : "; waiting for a sender")
                  << std::endl;
        if (exit_on_disconnect) request_stop_with_exit_code(kExitDeviceLost);
      }
      close_client();
    } catch (const Json::exception&) {
      std::cerr << "[capture_service_cpp][tcp_input][" << role_name(role) << ":" << cfg.port
                << "] invalid HELLO; waiting for a sender" << std::endl;
      close_client();
    }
    return false;
  }

  TcpInputConfig cfg;
  TcpInputRole role;
  bool paired_imu;
  uint64_t stall_ns;
  std::vector<TcpInputView> views;
  std::vector<size_t> offsets;
  std::shared_ptr<TcpCaptureSession> session;
  SocketRuntime sockets;
  socket_t listener = kInvalidSocket;
  socket_t client = kInvalidSocket;
  size_t sample_bytes = 0;
  std::vector<uint8_t> body;
  bool hello = false;
  bool ready = false;
  bool has_imu = false;
  std::string session_id;
  std::shared_ptr<TcpInputClock> clock;
  uint64_t handshake_deadline = 0;
  Probe best_probe;
  unsigned probes = 0;
  uint64_t ping = 0;
  uint64_t last_ping = 0;
  uint64_t last_data = 0;
  std::optional<uint64_t> last_sequence;
  uint64_t last_sender_ns = 0;
  uint64_t last_timestamp_ns = 0;
};

TcpSensorInput::TcpSensorInput(TcpInputConfig cfg, TcpInputRole role, int stall_ms, std::vector<TcpInputView> views,
                               std::shared_ptr<TcpCaptureSession> session)
    : impl_(std::make_unique<Impl>(std::move(cfg), role, stall_ms, std::move(views), std::move(session))) {}

TcpSensorInput::~TcpSensorInput() = default;
void TcpSensorInput::open() { impl_->open(); }
void TcpSensorInput::close() { impl_->close(); }
bool TcpSensorInput::read(TcpInputSample& sample) { return impl_->read(sample); }
uint8_t* TcpSensorInput::pixels(size_t view_index) { return impl_->body.data() + impl_->offsets.at(view_index); }

}  // namespace xr_capture_cpp
