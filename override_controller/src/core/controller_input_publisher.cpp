#include <xr_override_controller/controller_input_publisher.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <xr_tracking/types/tracking_types.hpp>

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM 1
#else
#define XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM 0
#endif

#if XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace xr_override_controller {
namespace {

uint64_t now_ns_u64() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string normalize_posix_name(std::string name) {
  if (name.empty()) throw std::runtime_error("empty SHM name");
  if (name[0] != '/') name = "/" + name;
  return name;
}

void fill_side(xr_runtime::ControllerDeviceStateV2& dst,
               const SideOutputState& src,
               xr_runtime::ControllerSide side) {
  dst = xr_runtime::ControllerDeviceStateV2{};
  dst.side = side;
  if (!src.configured) {
    dst.status = xr_runtime::CONTROLLER_INPUT_UNAVAILABLE;
    return;
  }
  dst.status = src.connected ? xr_runtime::CONTROLLER_INPUT_ACTIVE : xr_runtime::CONTROLLER_INPUT_LOST;
  dst.flags = xr_runtime::CONTROLLER_DEVICE_BUTTONS_VALID | xr_runtime::CONTROLLER_DEVICE_ANALOG_VALID;
  dst.source_type = xr_runtime::CONTROLLER_INPUT_SOURCE_HID;
  dst.buttons = src.buttons;
  dst.touches = src.touches;
  dst.changed_buttons = src.changed_buttons;
  dst.trigger = src.trigger;
  dst.grip = src.grip;
  dst.thumbstick_x = src.thumbstick_x;
  dst.thumbstick_y = src.thumbstick_y;
  dst.stable_device_hash = xr_override_controller::stable_hash64(src.device_id);
  dst.physical_device_hash = dst.stable_device_hash;
  std::memcpy(dst.press_counters, src.press_counters, sizeof(dst.press_counters));
  std::memcpy(dst.release_counters, src.release_counters, sizeof(dst.release_counters));
  std::snprintf(dst.device_id, sizeof(dst.device_id), "%s", src.device_id.c_str());
}

xr_runtime::ControllerInputV2 controller_input_frame_from_output(const OutputState& state,
                                                                 uint64_t sequence,
                                                                 uint64_t timestamp_ns) {
  xr_runtime::ControllerInputV2 frame{};
  frame.version = xr_runtime::CONTROLLER_INPUT_V2_FORMAT_VERSION;
  frame.size_bytes = sizeof(frame);
  frame.sequence = sequence;
  frame.timestamp_ns = timestamp_ns;
  frame.source_timestamp_ns = timestamp_ns;
  fill_side(frame.left, state.left, xr_runtime::CONTROLLER_SIDE_LEFT);
  fill_side(frame.right, state.right, xr_runtime::CONTROLLER_SIDE_RIGHT);
  if (frame.left.status == xr_runtime::CONTROLLER_INPUT_ACTIVE) {
    frame.active_mask |= xr_runtime::CONTROLLER_INPUT_FRAME_ACTIVE_LEFT;
  }
  if (frame.right.status == xr_runtime::CONTROLLER_INPUT_ACTIVE) {
    frame.active_mask |= xr_runtime::CONTROLLER_INPUT_FRAME_ACTIVE_RIGHT;
  }
  if (frame.left.status == xr_runtime::CONTROLLER_INPUT_ACTIVE ||
      frame.left.status == xr_runtime::CONTROLLER_INPUT_CONNECTED) {
    frame.connected_mask |= xr_runtime::CONTROLLER_INPUT_FRAME_ACTIVE_LEFT;
  }
  if (frame.right.status == xr_runtime::CONTROLLER_INPUT_ACTIVE ||
      frame.right.status == xr_runtime::CONTROLLER_INPUT_CONNECTED) {
    frame.connected_mask |= xr_runtime::CONTROLLER_INPUT_FRAME_ACTIVE_RIGHT;
  }
  return frame;
}

#if XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM
bool parse_env_uid_gid(const char* uid_name, const char* gid_name, uid_t& uid, gid_t& gid) {
  const char* uid_s = std::getenv(uid_name);
  const char* gid_s = std::getenv(gid_name);
  if (!uid_s || !gid_s || uid_s[0] == '\0' || gid_s[0] == '\0') return false;
  char* uid_end = nullptr;
  char* gid_end = nullptr;
  errno = 0;
  const unsigned long uid_v = std::strtoul(uid_s, &uid_end, 10);
  const unsigned long gid_v = std::strtoul(gid_s, &gid_end, 10);
  if (errno != 0 || !uid_end || *uid_end != '\0' || !gid_end || *gid_end != '\0') return false;
  uid = static_cast<uid_t>(uid_v);
  gid = static_cast<gid_t>(gid_v);
  return true;
}

bool sudo_target_uid_gid(uid_t& uid, gid_t& gid) {
  return geteuid() == 0 && parse_env_uid_gid("SUDO_UID", "SUDO_GID", uid, gid);
}

void chown_to_sudo_target_fd(int fd) {
  uid_t uid = 0;
  gid_t gid = 0;
  if (fd >= 0 && sudo_target_uid_gid(uid, gid)) {
    (void)fchown(fd, uid, gid);
  }
}

void chown_to_sudo_target_path(const fs::path& path) {
  uid_t uid = 0;
  gid_t gid = 0;
  if (sudo_target_uid_gid(uid, gid)) {
    (void)chown(path.c_str(), uid, gid);
  }
}

int open_lock_file_for_flock(const fs::path& lock_path) {
  int fd = open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd >= 0) {
    chown_to_sudo_target_fd(fd);
    return fd;
  }
  if (errno != ENOENT) {
    const int first_errno = errno;
    fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd >= 0) {
      chown_to_sudo_target_fd(fd);
      return fd;
    }
    errno = first_errno;
    return -1;
  }
  fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (fd >= 0) chown_to_sudo_target_fd(fd);
  return fd;
}
#endif

}  // namespace

ControllerInputShmPublisher::ControllerInputShmPublisher(PublishConfig cfg) : cfg_(std::move(cfg)) {
#if XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM
  if (cfg_.slot_count == 0) throw std::runtime_error("controller input slot_count must be > 0");
  slot_header_size_ = sizeof(xr_runtime::RingSlotHeaderV1);
  payload_size_ = sizeof(xr_runtime::ControllerInputV2);
  slot_stride_ = slot_header_size_ + payload_size_;
  size_ = header_size_ + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

  const std::string posix_name = normalize_posix_name(cfg_.shm_name);
  if (cfg_.unlink_existing) shm_unlink(posix_name.c_str());
  fd_ = shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
  if (fd_ < 0) throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
  chown_to_sudo_target_fd(fd_);
  if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
    const std::string err = std::strerror(errno);
    close(fd_); fd_ = -1;
    throw std::runtime_error("ftruncate failed for " + posix_name + ": " + err);
  }
  data_ = static_cast<uint8_t*>(mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  if (data_ == MAP_FAILED) {
    const std::string err = std::strerror(errno);
    data_ = nullptr;
    close(fd_); fd_ = -1;
    throw std::runtime_error("mmap failed for " + posix_name + ": " + err);
  }
  std::memset(data_, 0, size_);
  auto* h = reinterpret_cast<xr_runtime::RingHeaderV1*>(data_);
  std::memset(h, 0, sizeof(*h));
  const char magic[8] = {'C','I','N','P','U','T','2','\0'};
  std::memcpy(h->magic, magic, 8);
  h->version = 1;
  h->header_size = static_cast<uint32_t>(header_size_);
  h->slot_count = cfg_.slot_count;
  h->slot_stride = static_cast<uint32_t>(slot_stride_);
  h->slot_header_size = static_cast<uint32_t>(slot_header_size_);
  h->payload_size = static_cast<uint32_t>(payload_size_);
  h->latest_sequence = 0;
  write_metadata();
  update_registry();
#else
  (void)cfg_;
  throw std::runtime_error("override_controller SHM publisher is only implemented for POSIX shared memory in this build; native Windows publisher will need a Windows-specific backend");
#endif
}

ControllerInputShmPublisher::~ControllerInputShmPublisher() {
#if XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM
  if (data_) {
    munmap(data_, size_);
    data_ = nullptr;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
#endif
}

void ControllerInputShmPublisher::publish(const OutputState& state) {
#if XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM
  if (!data_) return;
  xr_runtime::ControllerInputV2 frame = controller_input_frame_from_output(state, ++sequence_, now_ns_u64());

  const uint64_t committed_seq = frame.sequence * 2;
  const uint64_t writing_seq = committed_seq - 1;
  const uint32_t idx = static_cast<uint32_t>((frame.sequence - 1) % cfg_.slot_count);
  uint8_t* slot_base = data_ + header_size_ + static_cast<size_t>(idx) * slot_stride_;
  auto* slot = reinterpret_cast<xr_runtime::RingSlotHeaderV1*>(slot_base);
  slot->seq_begin = writing_seq;
  slot->seq_end = writing_seq;
  slot->timestamp_ns = frame.timestamp_ns;
  slot->source_timestamp_ns = frame.source_timestamp_ns;
  slot->payload_size = sizeof(frame);
  slot->flags = 0;
  std::memcpy(slot_base + slot_header_size_, &frame, sizeof(frame));
  std::atomic_thread_fence(std::memory_order_release);
  slot->seq_end = committed_seq;
  std::atomic_thread_fence(std::memory_order_release);
  slot->seq_begin = committed_seq;
  std::atomic_thread_fence(std::memory_order_release);
  auto* h = reinterpret_cast<xr_runtime::RingHeaderV1*>(data_);
  h->latest_sequence = frame.sequence;
#else
  (void)state;
#endif
}

void ControllerInputShmPublisher::write_metadata() {
  nlohmann::json meta = {
      {"stream_id", cfg_.stream_id},
      {"kind", "CONTROLLER_INPUT"},
      {"format_name", xr_runtime::CONTROLLER_INPUT_V2_FORMAT_NAME},
      {"format_version", xr_runtime::CONTROLLER_INPUT_V2_FORMAT_VERSION},
      {"payload_schema", "ControllerInputV2"},
      {"timestamp_clock", "monotonic_ns"},
      {"producer", "override_controller"},
      {"button_bits", {
          {"trigger", 0}, {"grip", 1}, {"menu", 2}, {"a", 3}, {"b", 4},
          {"thumbstick", 5}, {"dpad_up", 6}, {"dpad_down", 7},
          {"dpad_left", 8}, {"dpad_right", 9}, {"dpad_center", 10},
          {"x", 11}, {"y", 12}, {"system", 13}}}
  };
  const std::string meta_s = meta.dump();
  const size_t meta_offset = sizeof(xr_runtime::RingHeaderV1);
  const size_t max_meta = header_size_ > meta_offset ? header_size_ - meta_offset : 0;
  if (data_ && meta_s.size() < max_meta) {
    std::memcpy(data_ + meta_offset, meta_s.data(), meta_s.size());
  }
}

void ControllerInputShmPublisher::update_registry() const {
#if XR_OVERRIDE_CONTROLLER_HAS_POSIX_SHM
  if (cfg_.registry_path.empty()) return;
  const fs::path registry_path(cfg_.registry_path);
  const fs::path parent = registry_path.parent_path();
  if (!parent.empty()) fs::create_directories(parent);
  const fs::path lock_path = registry_path.string() + ".lock";
  const int lock_fd = open_lock_file_for_flock(lock_path);
  if (lock_fd < 0) {
    const std::string err = std::strerror(errno);
    throw std::runtime_error("failed to open registry lock: " + lock_path.string() + ": " + err);
  }
  if (flock(lock_fd, LOCK_EX) != 0) {
    close(lock_fd);
    throw std::runtime_error("failed to lock registry: " + lock_path.string() + ": " + std::strerror(errno));
  }

  nlohmann::json j;
  try {
    if (fs::exists(registry_path)) {
      std::ifstream in(registry_path);
      if (in) in >> j;
    }
  } catch (...) {
    j = nlohmann::json::object();
  }
  if (!j.is_object()) j = nlohmann::json::object();
  j["version"] = 1;
  if (!j.contains("streams") || !j["streams"].is_object()) j["streams"] = nlohmann::json::object();
  j["streams"][cfg_.stream_id] = {
      {"shm_name", cfg_.shm_name},
      {"kind", "CONTROLLER_INPUT"},
      {"format_name", xr_runtime::CONTROLLER_INPUT_V2_FORMAT_NAME},
      {"format_version", xr_runtime::CONTROLLER_INPUT_V2_FORMAT_VERSION},
      {"payload_size", payload_size_},
      {"slot_count", cfg_.slot_count},
      {"header_size", header_size_},
      {"slot_header_size", slot_header_size_},
      {"slot_stride", slot_stride_},
      {"frame_id", "controller_input"},
      {"payload_schema", "ControllerInputV2"},
      {"payload_version", 2},
      {"created_by", "override_controller"},
      {"timestamp_clock", "monotonic_ns"},
  };
  const fs::path tmp_path = registry_path.string() + ".tmp." + std::to_string(getpid());
  {
    std::ofstream out(tmp_path);
    out << j.dump(2) << "\n";
  }
  chown_to_sudo_target_path(tmp_path);
  fs::rename(tmp_path, registry_path);
  chown_to_sudo_target_path(registry_path);
  chown_to_sudo_target_path(lock_path);
  flock(lock_fd, LOCK_UN);
  close(lock_fd);
#endif
}


namespace {
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void close_socket(socket_t s) { if (s != kInvalidSocket) closesocket(s); }
inline std::string socket_error_string() { return "WinSock error " + std::to_string(WSAGetLastError()); }
inline void init_socket_library_once() {
  static bool initialized = [] {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
    return true;
  }();
  (void)initialized;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void close_socket(socket_t s) { if (s >= 0) close(s); }
inline std::string socket_error_string() { return std::strerror(errno); }
inline void init_socket_library_once() {}
#endif

void send_all(socket_t fd, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t off = 0;
  while (off < size) {
#ifdef _WIN32
    const int n = send(fd, reinterpret_cast<const char*>(p + off), static_cast<int>(size - off), 0);
#else
    const ssize_t n = send(fd, p + off, size - off,
#ifdef MSG_NOSIGNAL
                           MSG_NOSIGNAL
#else
                           0
#endif
    );
#endif
    if (n <= 0) throw std::runtime_error("controller_input TCP send failed: " + socket_error_string());
    off += static_cast<size_t>(n);
  }
}
}  // namespace

struct ControllerInputTcpPublisher::Impl {
  explicit Impl(const PublishConfig& cfg) : cfg(cfg) {
    init_socket_library_once();
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd == kInvalidSocket) throw std::runtime_error("socket failed: " + socket_error_string());

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg.tcp_port));
    if (cfg.tcp_bind_host.empty() || cfg.tcp_bind_host == "0.0.0.0") {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
#ifdef _WIN32
      if (InetPtonA(AF_INET, cfg.tcp_bind_host.c_str(), &addr.sin_addr) != 1) {
#else
      if (inet_pton(AF_INET, cfg.tcp_bind_host.c_str(), &addr.sin_addr) != 1) {
#endif
        close_socket(listen_fd);
        listen_fd = kInvalidSocket;
        throw std::runtime_error("invalid controller_input TCP bind host: " + cfg.tcp_bind_host);
      }
    }
    if (bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      const std::string err = socket_error_string();
      close_socket(listen_fd); listen_fd = kInvalidSocket;
      throw std::runtime_error("controller_input TCP bind failed: " + err);
    }
    if (listen(listen_fd, 4) != 0) {
      const std::string err = socket_error_string();
      close_socket(listen_fd); listen_fd = kInvalidSocket;
      throw std::runtime_error("controller_input TCP listen failed: " + err);
    }
    accept_thread = std::thread([this] { accept_loop(); });
  }

  ~Impl() {
    stop = true;
#ifdef _WIN32
    if (listen_fd != kInvalidSocket) shutdown(listen_fd, SD_BOTH);
#else
    if (listen_fd != kInvalidSocket) shutdown(listen_fd, SHUT_RDWR);
#endif
    close_socket(listen_fd);
    listen_fd = kInvalidSocket;
    if (accept_thread.joinable()) accept_thread.join();
    std::lock_guard<std::mutex> lock(mutex);
    for (socket_t fd : clients) close_socket(fd);
    clients.clear();
  }

  void publish_frame(const xr_runtime::ControllerInputV2& frame) {
    xr_runtime::ControllerInputTcpHeaderV1 header{};
    header.magic = xr_runtime::CONTROLLER_INPUT_TCP_MAGIC_V2;
    header.version = xr_runtime::CONTROLLER_INPUT_TCP_VERSION_V2;
    header.header_size = sizeof(header);
    header.payload_size = sizeof(frame);
    header.sequence = frame.sequence;
    header.timestamp_ns = frame.timestamp_ns;

    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = clients.begin(); it != clients.end();) {
      try {
        send_all(*it, &header, sizeof(header));
        send_all(*it, &frame, sizeof(frame));
        ++it;
      } catch (...) {
        close_socket(*it);
        it = clients.erase(it);
      }
    }
  }

  void accept_loop() {
    while (!stop) {
      sockaddr_in peer{};
#ifdef _WIN32
      int len = sizeof(peer);
      socket_t fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
#else
      socklen_t len = sizeof(peer);
      socket_t fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
#endif
      if (fd == kInvalidSocket) {
        if (stop) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      std::lock_guard<std::mutex> lock(mutex);
      clients.push_back(fd);
    }
  }

  PublishConfig cfg;
  std::atomic_bool stop{false};
  socket_t listen_fd = kInvalidSocket;
  std::thread accept_thread;
  std::mutex mutex;
  std::vector<socket_t> clients;
};

ControllerInputTcpPublisher::ControllerInputTcpPublisher(PublishConfig cfg)
    : impl_(std::make_unique<Impl>(cfg)), cfg_(std::move(cfg)) {}
ControllerInputTcpPublisher::~ControllerInputTcpPublisher() = default;
void ControllerInputTcpPublisher::publish(const OutputState& state) {
  auto frame = controller_input_frame_from_output(state, ++sequence_, now_ns_u64());
  impl_->publish_frame(frame);
}

ControllerInputPublisher::ControllerInputPublisher(PublishConfig cfg) {
  transport_ = cfg.transport.empty() ? "shm" : cfg.transport;
  std::transform(transport_.begin(), transport_.end(), transport_.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (transport_ == "shm") {
    shm_ = std::make_unique<ControllerInputShmPublisher>(std::move(cfg));
  } else if (transport_ == "tcp") {
    tcp_ = std::make_unique<ControllerInputTcpPublisher>(std::move(cfg));
  } else {
    throw std::runtime_error("unsupported override_controller publish transport: " + transport_);
  }
}
ControllerInputPublisher::~ControllerInputPublisher() = default;
void ControllerInputPublisher::publish(const OutputState& state) {
  if (shm_) shm_->publish(state);
  if (tcp_) tcp_->publish(state);
}
uint64_t ControllerInputPublisher::sequence() const {
  if (shm_) return shm_->sequence();
  if (tcp_) return tcp_->sequence();
  return 0;
}


}  // namespace xr_override_controller
