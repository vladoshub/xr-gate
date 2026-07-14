#pragma once

#include <cstddef>
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
  // The side is backed by an IMU-capable provider. This remains set while the
  // IMU is configured/connected/stale/lost so consumers can distinguish an
  // IMU controller from a buttons-only controller.
  CONTROLLER_DEVICE_IMU_PRESENT = 1u << 3,
  // At least one current IMU datum is valid in ControllerDeviceStateV3::imu.
  CONTROLLER_DEVICE_IMU_ACTIVE = 1u << 4,
};

enum ControllerInputFrameFlags : uint32_t {
  CONTROLLER_INPUT_FRAME_ACTIVE_LEFT = 1u << 0,
  CONTROLLER_INPUT_FRAME_ACTIVE_RIGHT = 1u << 1,
  CONTROLLER_INPUT_FRAME_IMU_PRESENT_LEFT = 1u << 2,
  CONTROLLER_INPUT_FRAME_IMU_PRESENT_RIGHT = 1u << 3,
  CONTROLLER_INPUT_FRAME_IMU_ACTIVE_LEFT = 1u << 4,
  CONTROLLER_INPUT_FRAME_IMU_ACTIVE_RIGHT = 1u << 5,
};

enum ControllerInputSourceType : uint32_t {
  CONTROLLER_INPUT_SOURCE_UNKNOWN = 0,
  CONTROLLER_INPUT_SOURCE_KEYBOARD = 1,
  CONTROLLER_INPUT_SOURCE_GAMEPAD = 2,
  CONTROLLER_INPUT_SOURCE_BLUETOOTH_GAMEPAD = 3,
  CONTROLLER_INPUT_SOURCE_HID = 4,
  CONTROLLER_INPUT_SOURCE_SYNTHETIC = 5,
  CONTROLLER_INPUT_SOURCE_MOTION_CONTROLLER = 6,
};

enum ControllerImuStatus : uint32_t {
  // This controller side is buttons/axes only and has no IMU provider.
  CONTROLLER_IMU_NOT_SUPPORTED = 0,
  // An IMU provider is configured for this side, but it has not connected yet.
  CONTROLLER_IMU_CONFIGURED = 1,
  // The IMU transport is connected, but no current valid sample is available.
  CONTROLLER_IMU_CONNECTED = 2,
  // Current IMU data is available.
  CONTROLLER_IMU_ACTIVE = 3,
  // The last IMU data is retained but older than the provider freshness limit.
  CONTROLLER_IMU_STALE = 4,
  // The configured IMU provider disconnected after previously being available.
  CONTROLLER_IMU_LOST = 5,
};

enum ControllerImuCapabilityFlags : uint32_t {
  CONTROLLER_IMU_CAP_GYROSCOPE = 1u << 0,
  CONTROLLER_IMU_CAP_ACCELEROMETER = 1u << 1,
  CONTROLLER_IMU_CAP_MAGNETOMETER = 1u << 2,
  CONTROLLER_IMU_CAP_ORIENTATION = 1u << 3,
  CONTROLLER_IMU_CAP_TEMPERATURE = 1u << 4,
  CONTROLLER_IMU_CAP_DEVICE_TIMESTAMP = 1u << 5,
  CONTROLLER_IMU_CAP_MULTI_SAMPLE_PACKET = 1u << 6,
};

enum ControllerImuDataFlags : uint32_t {
  CONTROLLER_IMU_GYROSCOPE_VALID = 1u << 0,
  CONTROLLER_IMU_ACCELEROMETER_VALID = 1u << 1,
  CONTROLLER_IMU_MAGNETOMETER_VALID = 1u << 2,
  CONTROLLER_IMU_ORIENTATION_VALID = 1u << 3,
  CONTROLLER_IMU_TEMPERATURE_VALID = 1u << 4,
  CONTROLLER_IMU_GYROSCOPE_CALIBRATED = 1u << 5,
  CONTROLLER_IMU_ACCELEROMETER_CALIBRATED = 1u << 6,
  CONTROLLER_IMU_MAGNETOMETER_CALIBRATED = 1u << 7,
  // timestamp_ns has been mapped into the producer monotonic clock domain.
  CONTROLLER_IMU_HOST_TIME_SYNCED = 1u << 8,
};

inline constexpr uint32_t CONTROLLER_IMU_MAX_SAMPLES = 4;

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

#pragma pack(push, 1)
// One sensor sample. timestamp_ns is in the producer monotonic clock domain
// when CONTROLLER_IMU_HOST_TIME_SYNCED is set in flags; device_timestamp_ticks
// preserves the extended raw device counter when available.
// angular_velocity_rad_s and specific_force_m_s2 are expressed in the
// controller sensor-local axes. specific_force_m_s2 includes gravity, matching
// a physical accelerometer. samples are ordered oldest to newest.
struct ControllerImuSampleV1 {
  uint64_t timestamp_ns = 0;
  uint64_t device_timestamp_ticks = 0;

  float angular_velocity_rad_s[3] = {};
  float specific_force_m_s2[3] = {};

  uint32_t flags = 0;
  uint32_t reserved0 = 0;
};

// Per-controller IMU state. status explicitly distinguishes buttons-only
// controllers (NOT_SUPPORTED) from an IMU controller that is configured,
// connected, active, stale, or lost.
struct ControllerImuStateV1 {
  uint32_t status = CONTROLLER_IMU_NOT_SUPPORTED;
  uint32_t capability_flags = 0;
  // Validity/calibration of the latest state-level values. Per-sample gyro and
  // accelerometer validity is also carried in ControllerImuSampleV1::flags.
  uint32_t data_flags = 0;
  uint32_t sample_count = 0;

  uint64_t sequence = 0;
  uint64_t latest_sample_timestamp_ns = 0;
  uint64_t latest_device_timestamp_ticks = 0;
  uint64_t orientation_timestamp_ns = 0;

  // q_provider_from_sensor in xyzw order. The provider reference frame is not
  // necessarily the XR tracking frame; mounting/optical fusion belongs in the
  // runtime adapter.
  float orientation_xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  // Sensor-local axes.
  float magnetic_field_uT[3] = {};
  float temperature_c = 0.0f;

  ControllerImuSampleV1 samples[CONTROLLER_IMU_MAX_SAMPLES] = {};
  uint32_t reserved[4] = {};
};

// Current controller input ABI: buttons/axes followed by per-side IMU state.
struct ControllerDeviceStateV3 {
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

  ControllerImuStateV1 imu;
};

struct ControllerInputV3 {
  uint32_t version = 3;
  uint32_t size_bytes = sizeof(ControllerInputV3);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t flags = 0;
  uint32_t active_mask = 0;
  uint32_t connected_mask = 0;
  uint32_t reserved0 = 0;

  ControllerDeviceStateV3 left;
  ControllerDeviceStateV3 right;
};
#pragma pack(pop)

static_assert(sizeof(ControllerImuSampleV1) == 48, "ControllerImuSampleV1 must stay ABI-stable at 48 bytes");
static_assert(sizeof(ControllerImuStateV1) == 288, "ControllerImuStateV1 must stay ABI-stable at 288 bytes");
static_assert(offsetof(ControllerDeviceStateV3, imu) == 400,
              "ControllerDeviceStateV3 IMU offset must stay ABI-stable at 400 bytes");
static_assert(sizeof(ControllerDeviceStateV3) == 688, "ControllerDeviceStateV3 must stay ABI-stable at 688 bytes");
static_assert(sizeof(ControllerInputV3) == 1432, "ControllerInputV3 must stay ABI-stable at 1432 bytes");

using ControllerDeviceState = ControllerDeviceStateV3;
using ControllerInput = ControllerInputV3;

inline bool controller_imu_is_present(const ControllerImuStateV1& imu) {
  return imu.status != CONTROLLER_IMU_NOT_SUPPORTED;
}

inline bool controller_imu_has_current_data(const ControllerImuStateV1& imu) {
  return imu.status == CONTROLLER_IMU_ACTIVE && imu.data_flags != 0u;
}

inline const char* controller_imu_status_name(uint32_t status) {
  switch (status) {
    case CONTROLLER_IMU_NOT_SUPPORTED: return "not_supported";
    case CONTROLLER_IMU_CONFIGURED: return "configured";
    case CONTROLLER_IMU_CONNECTED: return "connected";
    case CONTROLLER_IMU_ACTIVE: return "active";
    case CONTROLLER_IMU_STALE: return "stale";
    case CONTROLLER_IMU_LOST: return "lost";
    default: return "unknown";
  }
}

constexpr const char* CONTROLLER_INPUT_V3_FORMAT_NAME = "CONTROLLER_INPUT_V3";
constexpr uint32_t CONTROLLER_INPUT_V3_FORMAT_VERSION = 3;
constexpr uint32_t CONTROLLER_INPUT_TCP_MAGIC_V3 = 0x43495633u; // "CIV3" marker
constexpr uint32_t CONTROLLER_INPUT_TCP_VERSION_V3 = 3;

#pragma pack(push, 1)
struct ControllerInputTcpHeader {
  uint32_t magic = CONTROLLER_INPUT_TCP_MAGIC_V3;
  uint16_t version = CONTROLLER_INPUT_TCP_VERSION_V3;
  uint16_t header_size = sizeof(ControllerInputTcpHeader);
  uint32_t payload_size = sizeof(ControllerInputV3);
  uint32_t reserved0 = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
};
#pragma pack(pop)

static_assert(sizeof(ControllerInputTcpHeader) == 32, "controller input TCP header must be 32 bytes");

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

  ControllerInputV3 read_next() {
    ControllerInputTcpHeader header{};
    read_exact(&header, sizeof(header));
    if (header.magic != CONTROLLER_INPUT_TCP_MAGIC_V3 ||
        header.version != CONTROLLER_INPUT_TCP_VERSION_V3 ||
        header.header_size != sizeof(ControllerInputTcpHeader)) {
      throw std::runtime_error(
          "unsupported controller_input TCP ABI; expected CIV3/version 3. "
          "Update override_controller and xr_runtime_adapter from the same XR Gate release");
    }
    if (header.payload_size != sizeof(ControllerInputV3)) {
      throw std::runtime_error("unexpected controller_input TCP V3 payload size");
    }
    ControllerInputV3 frame{};
    read_exact(&frame, sizeof(frame));
    if (frame.version != CONTROLLER_INPUT_V3_FORMAT_VERSION ||
        frame.size_bytes != sizeof(ControllerInputV3)) {
      throw std::runtime_error("invalid controller_input V3 frame header");
    }
    return frame;
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

  ControllerInputV3 read_next() {
    ControllerInputTcpHeader header{};
    read_exact(&header, sizeof(header));
    if (header.magic != CONTROLLER_INPUT_TCP_MAGIC_V3 ||
        header.version != CONTROLLER_INPUT_TCP_VERSION_V3 ||
        header.header_size != sizeof(ControllerInputTcpHeader)) {
      throw std::runtime_error(
          "unsupported controller_input TCP ABI; expected CIV3/version 3. "
          "Update override_controller and xr_runtime_adapter from the same XR Gate release");
    }
    if (header.payload_size != sizeof(ControllerInputV3)) {
      throw std::runtime_error("unexpected controller_input TCP V3 payload size");
    }
    ControllerInputV3 frame{};
    read_exact(&frame, sizeof(frame));
    if (frame.version != CONTROLLER_INPUT_V3_FORMAT_VERSION ||
        frame.size_bytes != sizeof(ControllerInputV3)) {
      throw std::runtime_error("invalid controller_input V3 frame header");
    }
    return frame;
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
