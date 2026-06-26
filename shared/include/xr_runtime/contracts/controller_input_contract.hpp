#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <xr_runtime/registry/runtime_paths.hpp>

namespace xr_runtime {

enum class ControllerInputMode {
  HAND_TRACKING_ONLY,
  HAND_PLUS_CONTROLLER,
  CONTROLLER_BUTTONS_ONLY,
};

enum class ControllerInputTransport {
  NONE,
  SHM,
  UDP,
  TCP,
  NAMED_PIPE,
};

enum class ControllerInputConflictPolicy {
  CONTROLLER_OVERRIDE,
  ADDITIVE,
  HAND_OVERRIDE,
};

enum class ControllerInputStalePolicy {
  ZERO_ON_STALE,
  HOLD_LAST,
};

inline ControllerInputMode parse_controller_input_mode(const std::string& v) {
  if (v == "hand_tracking_only") return ControllerInputMode::HAND_TRACKING_ONLY;
  if (v == "hand_plus_controller") return ControllerInputMode::HAND_PLUS_CONTROLLER;
  if (v == "controller_buttons_only") return ControllerInputMode::CONTROLLER_BUTTONS_ONLY;
  throw std::runtime_error("--controller-input-mode must be one of: hand_tracking_only, hand_plus_controller, controller_buttons_only");
}

inline ControllerInputTransport parse_controller_input_transport(const std::string& v) {
  if (v == "none") return ControllerInputTransport::NONE;
  if (v == "shm") return ControllerInputTransport::SHM;
  if (v == "udp") return ControllerInputTransport::UDP;
  if (v == "tcp") return ControllerInputTransport::TCP;
  if (v == "named_pipe") return ControllerInputTransport::NAMED_PIPE;
  throw std::runtime_error("--controller-input-transport must be one of: none, shm, udp, tcp, named_pipe");
}

inline ControllerInputConflictPolicy parse_controller_input_conflict_policy(const std::string& v) {
  if (v == "controller_override") return ControllerInputConflictPolicy::CONTROLLER_OVERRIDE;
  if (v == "additive") return ControllerInputConflictPolicy::ADDITIVE;
  if (v == "hand_override") return ControllerInputConflictPolicy::HAND_OVERRIDE;
  throw std::runtime_error("--controller-input-conflict-policy must be one of: controller_override, additive, hand_override");
}

inline ControllerInputStalePolicy parse_controller_input_stale_policy(const std::string& v) {
  if (v == "zero_on_stale") return ControllerInputStalePolicy::ZERO_ON_STALE;
  if (v == "hold_last") return ControllerInputStalePolicy::HOLD_LAST;
  throw std::runtime_error("--controller-input-stale-policy must be one of: zero_on_stale, hold_last");
}

struct ControllerInputRuntimeConfig {
  std::string mode = "hand_tracking_only";
  std::string transport = "none";
  std::string stream = "controller_input";

  std::string registry = default_tracking_registry_path();
  std::string host = "127.0.0.1";
  int port = 45672;

  std::string named_pipe = R"(\\.\pipe\xr_controller_input)";

  std::string left_controller_id = "auto";
  std::string right_controller_id = "auto";

  int max_age_ms = 250;
  std::string conflict_policy = "controller_override";
  std::string stale_policy = "zero_on_stale";
};

inline void validate_controller_input_runtime_config(const ControllerInputRuntimeConfig& c) {
  (void)parse_controller_input_mode(c.mode);
  (void)parse_controller_input_transport(c.transport);
  (void)parse_controller_input_conflict_policy(c.conflict_policy);
  (void)parse_controller_input_stale_policy(c.stale_policy);

  if (c.max_age_ms <= 0) {
    throw std::runtime_error("--max-controller-age-ms must be positive");
  }

  if (c.port <= 0 || c.port > 65535) {
    throw std::runtime_error("--controller-input-port must be in 1..65535");
  }

  const auto transport = parse_controller_input_transport(c.transport);

  if (transport == ControllerInputTransport::SHM && c.stream.empty()) {
    throw std::runtime_error("--controller-input-stream must not be empty for shm transport");
  }

  if ((transport == ControllerInputTransport::UDP ||
       transport == ControllerInputTransport::TCP) &&
      c.host.empty()) {
    throw std::runtime_error("--controller-input-host must not be empty for udp/tcp transport");
  }

  if (transport == ControllerInputTransport::NAMED_PIPE &&
      c.named_pipe.empty()) {
    throw std::runtime_error("--controller-input-named-pipe must not be empty for named_pipe transport");
  }
}

enum ControllerInputStatus : uint32_t {
  CONTROLLER_INPUT_UNAVAILABLE = 0,
  CONTROLLER_INPUT_CONNECTED = 1,
  CONTROLLER_INPUT_ACTIVE = 2,
  CONTROLLER_INPUT_STALE = 3,
  CONTROLLER_INPUT_LOST = 4,
};

enum ControllerSide : uint32_t {
  CONTROLLER_SIDE_UNKNOWN = 0,
  CONTROLLER_SIDE_LEFT = 1,
  CONTROLLER_SIDE_RIGHT = 2,
};

enum ControllerDeviceFlags : uint32_t {
  CONTROLLER_DEVICE_POSE_VALID = 1u << 0,
  CONTROLLER_DEVICE_BUTTONS_VALID = 1u << 1,
  CONTROLLER_DEVICE_ANALOG_VALID = 1u << 2,
};

enum ControllerInputFrameFlags : uint32_t {
  CONTROLLER_INPUT_FRAME_ACTIVE_LEFT = 1u << 0,
  CONTROLLER_INPUT_FRAME_ACTIVE_RIGHT = 1u << 1,
};

enum ControllerInputSourceType : uint32_t {
  CONTROLLER_INPUT_SOURCE_UNKNOWN = 0,
  CONTROLLER_INPUT_SOURCE_KEYBOARD = 1,
  CONTROLLER_INPUT_SOURCE_GAMEPAD = 2,
  CONTROLLER_INPUT_SOURCE_BLUETOOTH_GAMEPAD = 3,
  CONTROLLER_INPUT_SOURCE_HID = 4,
  CONTROLLER_INPUT_SOURCE_SYNTHETIC = 5,
};

enum ControllerButtonBits : uint64_t {
  CONTROLLER_BUTTON_TRIGGER = 1ull << 0,
  CONTROLLER_BUTTON_GRIP = 1ull << 1,
  CONTROLLER_BUTTON_MENU = 1ull << 2,
  CONTROLLER_BUTTON_A = 1ull << 3,
  CONTROLLER_BUTTON_B = 1ull << 4,
  CONTROLLER_BUTTON_THUMBSTICK = 1ull << 5,
  CONTROLLER_BUTTON_DPAD_UP = 1ull << 6,
  CONTROLLER_BUTTON_DPAD_DOWN = 1ull << 7,
  CONTROLLER_BUTTON_DPAD_LEFT = 1ull << 8,
  CONTROLLER_BUTTON_DPAD_RIGHT = 1ull << 9,
  CONTROLLER_BUTTON_DPAD_CENTER = 1ull << 10,
  CONTROLLER_BUTTON_X = 1ull << 11,
  CONTROLLER_BUTTON_Y = 1ull << 12,
  CONTROLLER_BUTTON_SYSTEM = 1ull << 13,
};

inline constexpr uint64_t CONTROLLER_BUTTON_DPAD_MASK =
    CONTROLLER_BUTTON_DPAD_UP |
    CONTROLLER_BUTTON_DPAD_DOWN |
    CONTROLLER_BUTTON_DPAD_LEFT |
    CONTROLLER_BUTTON_DPAD_RIGHT |
    CONTROLLER_BUTTON_DPAD_CENTER;

inline constexpr uint64_t CONTROLLER_BUTTON_KNOWN_MASK =
    CONTROLLER_BUTTON_TRIGGER |
    CONTROLLER_BUTTON_GRIP |
    CONTROLLER_BUTTON_MENU |
    CONTROLLER_BUTTON_A |
    CONTROLLER_BUTTON_B |
    CONTROLLER_BUTTON_THUMBSTICK |
    CONTROLLER_BUTTON_DPAD_MASK |
    CONTROLLER_BUTTON_X |
    CONTROLLER_BUTTON_Y |
    CONTROLLER_BUTTON_SYSTEM;

struct ControllerDeviceStateV1 {
  uint32_t status = CONTROLLER_INPUT_UNAVAILABLE;
  uint32_t side = CONTROLLER_SIDE_UNKNOWN;
  uint32_t flags = 0;
  uint32_t reserved0 = 0;

  uint64_t buttons = 0;
  uint64_t touches = 0;

  float trigger = 0.0f;
  float grip = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;

  uint32_t press_counters[32] = {};
  uint32_t release_counters[32] = {};

  char device_id[64] = {};
};

struct ControllerInputV1 {
  uint32_t version = 1;
  uint32_t size_bytes = sizeof(ControllerInputV1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;

  ControllerDeviceStateV1 left;
  ControllerDeviceStateV1 right;
};

#pragma pack(push, 1)
struct ControllerDeviceStateV2 {
  uint32_t status = CONTROLLER_INPUT_UNAVAILABLE;
  uint32_t side = CONTROLLER_SIDE_UNKNOWN;
  uint32_t flags = 0;
  uint32_t source_type = CONTROLLER_INPUT_SOURCE_UNKNOWN;

  uint64_t buttons = 0;
  uint64_t touches = 0;
  uint64_t changed_buttons = 0;

  float trigger = 0.0f;
  float grip = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  float brake = 0.0f;
  float accelerator = 0.0f;

  uint64_t stable_device_hash = 0;
  uint64_t physical_device_hash = 0;

  uint32_t press_counters[32] = {};
  uint32_t release_counters[32] = {};

  char device_id[64] = {};
};

struct ControllerInputV2 {
  uint32_t version = 2;
  uint32_t size_bytes = sizeof(ControllerInputV2);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t flags = 0;
  uint32_t active_mask = 0;
  uint32_t connected_mask = 0;
  uint32_t reserved0 = 0;

  ControllerDeviceStateV2 left;
  ControllerDeviceStateV2 right;
};
#pragma pack(pop)

static_assert(sizeof(ControllerDeviceStateV2) == 400, "ControllerDeviceStateV2 must stay ABI-stable at 400 bytes");
static_assert(sizeof(ControllerInputV2) == 856, "ControllerInputV2 must stay ABI-stable at 856 bytes");

inline ControllerDeviceStateV2 controller_device_v2_from_v1(const ControllerDeviceStateV1& v1) {
  ControllerDeviceStateV2 v2{};
  v2.status = v1.status;
  v2.side = v1.side;
  v2.flags = v1.flags;
  v2.buttons = v1.buttons;
  v2.touches = v1.touches;
  v2.changed_buttons = 0;
  v2.trigger = v1.trigger;
  v2.grip = v1.grip;
  v2.thumbstick_x = v1.thumbstick_x;
  v2.thumbstick_y = v1.thumbstick_y;
  std::memcpy(v2.press_counters, v1.press_counters, sizeof(v1.press_counters));
  std::memcpy(v2.release_counters, v1.release_counters, sizeof(v1.release_counters));
  std::memcpy(v2.device_id, v1.device_id, sizeof(v1.device_id));
  return v2;
}

inline ControllerInputV2 controller_input_v2_from_v1(const ControllerInputV1& v1) {
  ControllerInputV2 v2{};
  v2.sequence = v1.sequence;
  v2.timestamp_ns = v1.timestamp_ns;
  v2.source_timestamp_ns = v1.source_timestamp_ns;
  v2.left = controller_device_v2_from_v1(v1.left);
  v2.right = controller_device_v2_from_v1(v1.right);
  if (v2.left.status == CONTROLLER_INPUT_ACTIVE) v2.active_mask |= CONTROLLER_INPUT_FRAME_ACTIVE_LEFT;
  if (v2.right.status == CONTROLLER_INPUT_ACTIVE) v2.active_mask |= CONTROLLER_INPUT_FRAME_ACTIVE_RIGHT;
  if (v2.left.status == CONTROLLER_INPUT_ACTIVE || v2.left.status == CONTROLLER_INPUT_CONNECTED) {
    v2.connected_mask |= CONTROLLER_INPUT_FRAME_ACTIVE_LEFT;
  }
  if (v2.right.status == CONTROLLER_INPUT_ACTIVE || v2.right.status == CONTROLLER_INPUT_CONNECTED) {
    v2.connected_mask |= CONTROLLER_INPUT_FRAME_ACTIVE_RIGHT;
  }
  return v2;
}

constexpr const char* CONTROLLER_INPUT_FORMAT_NAME = "CONTROLLER_INPUT_V1";
constexpr uint32_t CONTROLLER_INPUT_FORMAT_VERSION = 1;
constexpr const char* CONTROLLER_INPUT_V2_FORMAT_NAME = "CONTROLLER_INPUT_V2";
constexpr uint32_t CONTROLLER_INPUT_V2_FORMAT_VERSION = 2;
constexpr uint32_t CONTROLLER_INPUT_TCP_MAGIC = 0x43495631u; // "CIV1" marker
constexpr uint32_t CONTROLLER_INPUT_TCP_VERSION = 1;
constexpr uint32_t CONTROLLER_INPUT_TCP_MAGIC_V2 = 0x43495632u; // "CIV2" marker
constexpr uint32_t CONTROLLER_INPUT_TCP_VERSION_V2 = 2;

#pragma pack(push, 1)
struct ControllerInputTcpHeaderV1 {
  uint32_t magic = CONTROLLER_INPUT_TCP_MAGIC;
  uint16_t version = CONTROLLER_INPUT_TCP_VERSION;
  uint16_t header_size = sizeof(ControllerInputTcpHeaderV1);
  uint32_t payload_size = sizeof(ControllerInputV1);
  uint32_t reserved0 = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
};
#pragma pack(pop)

static_assert(sizeof(ControllerInputTcpHeaderV1) == 32, "controller input TCP header must be 32 bytes");

#ifndef _WIN32
class ControllerInputTcpClient {
 public:
  ControllerInputTcpClient(std::string host, int port)
      : host_(std::move(host)), port_(port) {
    connect_socket();
  }

  ~ControllerInputTcpClient() {
    if (fd_ >= 0) close(fd_);
  }

  ControllerInputTcpClient(const ControllerInputTcpClient&) = delete;
  ControllerInputTcpClient& operator=(const ControllerInputTcpClient&) = delete;

  ControllerInputV2 read_next() {
    ControllerInputTcpHeaderV1 header{};
    read_exact(&header, sizeof(header));
    if (header.magic == CONTROLLER_INPUT_TCP_MAGIC && header.version == CONTROLLER_INPUT_TCP_VERSION) {
      if (header.payload_size != sizeof(ControllerInputV1)) {
        throw std::runtime_error("unexpected controller_input TCP V1 payload size");
      }
      ControllerInputV1 frame{};
      read_exact(&frame, sizeof(frame));
      if (frame.size_bytes != sizeof(ControllerInputV1)) {
        throw std::runtime_error("invalid controller_input V1 frame size_bytes");
      }
      return controller_input_v2_from_v1(frame);
    }
    if (header.magic == CONTROLLER_INPUT_TCP_MAGIC_V2 && header.version == CONTROLLER_INPUT_TCP_VERSION_V2) {
      if (header.payload_size != sizeof(ControllerInputV2)) {
        throw std::runtime_error("unexpected controller_input TCP V2 payload size");
      }
      ControllerInputV2 frame{};
      read_exact(&frame, sizeof(frame));
      if (frame.version != 2 || frame.size_bytes != sizeof(ControllerInputV2)) {
        throw std::runtime_error("invalid controller_input V2 frame header");
      }
      return frame;
    }
    throw std::runtime_error("invalid or unsupported controller_input TCP magic/version");
  }

 private:
  void connect_socket() {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string port_s = std::to_string(port_);
    const int rc = getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &result);
    if (rc != 0) {
      throw std::runtime_error("getaddrinfo failed for controller_input TCP: " + std::string(gai_strerror(rc)));
    }

    for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
      fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd_ < 0) continue;
      if (connect(fd_, rp->ai_addr, rp->ai_addrlen) == 0) {
        struct timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        freeaddrinfo(result);
        return;
      }
      close(fd_);
      fd_ = -1;
    }

    freeaddrinfo(result);
    throw std::runtime_error("failed to connect controller_input TCP " + host_ + ":" + port_s + ": " + std::strerror(errno));
  }

  void read_exact(void* dst, size_t size) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t off = 0;
    while (off < size) {
      const ssize_t n = recv(fd_, out + off, size - off, 0);
      if (n == 0) throw std::runtime_error("controller_input TCP peer closed");
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          throw std::runtime_error("controller_input TCP recv timeout");
        }
        throw std::runtime_error("controller_input TCP recv failed: " + std::string(std::strerror(errno)));
      }
      off += static_cast<size_t>(n);
    }
  }

  std::string host_;
  int port_ = 0;
  int fd_ = -1;
};
#else
class ControllerInputTcpClient {
 public:
  ControllerInputTcpClient(std::string host, int port)
      : host_(std::move(host)), port_(port) {
    init_winsock_once();
    connect_socket();
  }

  ~ControllerInputTcpClient() {
    if (fd_ != INVALID_SOCKET) closesocket(fd_);
  }

  ControllerInputTcpClient(const ControllerInputTcpClient&) = delete;
  ControllerInputTcpClient& operator=(const ControllerInputTcpClient&) = delete;

  ControllerInputV2 read_next() {
    ControllerInputTcpHeaderV1 header{};
    read_exact(&header, sizeof(header));
    if (header.magic == CONTROLLER_INPUT_TCP_MAGIC && header.version == CONTROLLER_INPUT_TCP_VERSION) {
      if (header.payload_size != sizeof(ControllerInputV1)) {
        throw std::runtime_error("unexpected controller_input TCP V1 payload size");
      }
      ControllerInputV1 frame{};
      read_exact(&frame, sizeof(frame));
      if (frame.size_bytes != sizeof(ControllerInputV1)) {
        throw std::runtime_error("invalid controller_input V1 frame size_bytes");
      }
      return controller_input_v2_from_v1(frame);
    }
    if (header.magic == CONTROLLER_INPUT_TCP_MAGIC_V2 && header.version == CONTROLLER_INPUT_TCP_VERSION_V2) {
      if (header.payload_size != sizeof(ControllerInputV2)) {
        throw std::runtime_error("unexpected controller_input TCP V2 payload size");
      }
      ControllerInputV2 frame{};
      read_exact(&frame, sizeof(frame));
      if (frame.version != 2 || frame.size_bytes != sizeof(ControllerInputV2)) {
        throw std::runtime_error("invalid controller_input V2 frame header");
      }
      return frame;
    }
    throw std::runtime_error("invalid or unsupported controller_input TCP magic/version");
  }

 private:
  static void init_winsock_once() {
    static bool initialized = [] {
      WSADATA wsa{};
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw std::runtime_error("WSAStartup failed for controller_input TCP");
      }
      return true;
    }();
    (void)initialized;
  }

  static std::string last_winsock_error() {
    return "WinSock error " + std::to_string(WSAGetLastError());
  }

  void connect_socket() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_s = std::to_string(port_);
    const int rc = getaddrinfo(host_.c_str(), port_s.c_str(), &hints, &result);
    if (rc != 0) {
      throw std::runtime_error("getaddrinfo failed for controller_input TCP: " + std::to_string(rc));
    }

    for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
      fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd_ == INVALID_SOCKET) continue;
      if (connect(fd_, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
        const DWORD tv = 200;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        freeaddrinfo(result);
        return;
      }
      closesocket(fd_);
      fd_ = INVALID_SOCKET;
    }

    freeaddrinfo(result);
    throw std::runtime_error("failed to connect controller_input TCP " + host_ + ":" + port_s + ": " + last_winsock_error());
  }

  void read_exact(void* dst, size_t size) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t off = 0;
    while (off < size) {
      const int n = recv(fd_, reinterpret_cast<char*>(out + off), static_cast<int>(size - off), 0);
      if (n == 0) throw std::runtime_error("controller_input TCP peer closed");
      if (n < 0) {
        const int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
          throw std::runtime_error("controller_input TCP recv timeout");
        }
        throw std::runtime_error("controller_input TCP recv failed: " + std::to_string(err));
      }
      off += static_cast<size_t>(n);
    }
  }

  std::string host_;
  int port_ = 0;
  SOCKET fd_ = INVALID_SOCKET;
};
#endif

}  // namespace xr_runtime
