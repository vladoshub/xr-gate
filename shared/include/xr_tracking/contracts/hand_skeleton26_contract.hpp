#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace xr_tracking {

inline int64_t skeleton26_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* HAND_SKELETON26_FORMAT_NAME = "HAND_SKELETON26_F32_V1";
constexpr uint32_t HAND_SKELETON26_FORMAT_VERSION = 1;
constexpr uint32_t HAND_SKELETON26_JOINT_COUNT = 26;
constexpr uint32_t HAND_SKELETON26_TCP_MAGIC = 0x48323654u; // 'H26T' little-endian marker
constexpr uint32_t HAND_SKELETON26_TCP_VERSION = 1;

// OpenXR XR_EXT_hand_tracking order:
// 0 PALM
// 1 WRIST
// 2..5 THUMB_METACARPAL/PROXIMAL/DISTAL/TIP
// 6..10 INDEX_METACARPAL/PROXIMAL/INTERMEDIATE/DISTAL/TIP
// 11..15 MIDDLE_METACARPAL/PROXIMAL/INTERMEDIATE/DISTAL/TIP
// 16..20 RING_METACARPAL/PROXIMAL/INTERMEDIATE/DISTAL/TIP
// 21..25 LITTLE_METACARPAL/PROXIMAL/INTERMEDIATE/DISTAL/TIP
enum HandSkeleton26JointId : uint32_t {
  HAND26_PALM = 0,
  HAND26_WRIST = 1,
  HAND26_THUMB_METACARPAL = 2,
  HAND26_THUMB_PROXIMAL = 3,
  HAND26_THUMB_DISTAL = 4,
  HAND26_THUMB_TIP = 5,
  HAND26_INDEX_METACARPAL = 6,
  HAND26_INDEX_PROXIMAL = 7,
  HAND26_INDEX_INTERMEDIATE = 8,
  HAND26_INDEX_DISTAL = 9,
  HAND26_INDEX_TIP = 10,
  HAND26_MIDDLE_METACARPAL = 11,
  HAND26_MIDDLE_PROXIMAL = 12,
  HAND26_MIDDLE_INTERMEDIATE = 13,
  HAND26_MIDDLE_DISTAL = 14,
  HAND26_MIDDLE_TIP = 15,
  HAND26_RING_METACARPAL = 16,
  HAND26_RING_PROXIMAL = 17,
  HAND26_RING_INTERMEDIATE = 18,
  HAND26_RING_DISTAL = 19,
  HAND26_RING_TIP = 20,
  HAND26_LITTLE_METACARPAL = 21,
  HAND26_LITTLE_PROXIMAL = 22,
  HAND26_LITTLE_INTERMEDIATE = 23,
  HAND26_LITTLE_DISTAL = 24,
  HAND26_LITTLE_TIP = 25,
};

enum HandSkeleton26Space : uint32_t {
  HAND_SKELETON26_SPACE_UNKNOWN = 0,
  HAND_SKELETON26_SPACE_HMD = 1,
  HAND_SKELETON26_SPACE_CAMERA0 = 2,
  HAND_SKELETON26_SPACE_CAMERA1 = 3,
  HAND_SKELETON26_SPACE_WORLD = 4,
  HAND_SKELETON26_SPACE_RUNTIME_HMD = 5,
};

enum HandSkeleton26Source : uint32_t {
  HAND_SKELETON26_SOURCE_UNKNOWN = 0,
  HAND_SKELETON26_SOURCE_OPENXR = 1,
  HAND_SKELETON26_SOURCE_ULTRALEAP = 2,
  HAND_SKELETON26_SOURCE_LEAPC = 3,
  HAND_SKELETON26_SOURCE_MEDIAPIPE = 4,
};

enum HandSkeleton26Status : uint32_t {
  HAND_SKELETON26_STATUS_NO_HAND = 0,
  HAND_SKELETON26_STATUS_TRACKING = 1,
  HAND_SKELETON26_STATUS_LOST = 2,
  HAND_SKELETON26_STATUS_DEGRADED = 3,
};

enum HandSkeleton26FrameFlags : uint32_t {
  HAND_SKELETON26_FRAME_LEFT_VALID = 1u << 0,
  HAND_SKELETON26_FRAME_RIGHT_VALID = 1u << 1,
  HAND_SKELETON26_FRAME_GESTURES_VALID = 1u << 2,
};

enum HandSkeleton26JointFlags : uint32_t {
  HAND_SKELETON26_JOINT_POSITION_VALID = 1u << 0,
  HAND_SKELETON26_JOINT_ORIENTATION_VALID = 1u << 1,
  HAND_SKELETON26_JOINT_RADIUS_VALID = 1u << 2,
  HAND_SKELETON26_JOINT_TRACKED = 1u << 3,
};

enum HandSkeleton26SideFlags : uint32_t {
  HAND_SKELETON26_SIDE_POSE_VALID = 1u << 0,
  HAND_SKELETON26_SIDE_JOINTS_VALID = 1u << 1,
  HAND_SKELETON26_SIDE_GESTURES_VALID = 1u << 2,
  HAND_SKELETON26_SIDE_PINCH_VALID = 1u << 3,
  HAND_SKELETON26_SIDE_GRAB_VALID = 1u << 4,
};

#pragma pack(push, 1)

struct HandSkeleton26JointF32V1 {
  uint32_t joint_id = 0;
  uint32_t flags = 0;
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
  float qw = 1.0f;
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float radius_m = 0.0f;
  float confidence = 0.0f;
};

struct HandSkeleton26SideF32V1 {
  uint32_t handedness = 0; // 1=left, 2=right
  uint32_t status = HAND_SKELETON26_STATUS_NO_HAND;
  uint32_t flags = 0;
  float confidence = 0.0f;

  uint32_t joint_count = HAND_SKELETON26_JOINT_COUNT;
  uint32_t reserved0 = 0;

  float pinch_strength = 0.0f;
  float grab_strength = 0.0f;
  uint32_t pinch_active = 0;
  uint32_t grab_active = 0;

  HandSkeleton26JointF32V1 joints[HAND_SKELETON26_JOINT_COUNT];
};

struct HandSkeleton26FrameF32V1 {
  uint32_t version = HAND_SKELETON26_FORMAT_VERSION;
  uint32_t size_bytes = sizeof(HandSkeleton26FrameF32V1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t space = HAND_SKELETON26_SPACE_UNKNOWN;
  uint32_t source = HAND_SKELETON26_SOURCE_UNKNOWN;
  uint32_t flags = 0;
  uint32_t hand_count = 0;

  HandSkeleton26SideF32V1 left;
  HandSkeleton26SideF32V1 right;
};

struct HandSkeleton26TcpHeaderV1 {
  uint32_t magic = HAND_SKELETON26_TCP_MAGIC;
  uint16_t version = HAND_SKELETON26_TCP_VERSION;
  uint16_t header_size = sizeof(HandSkeleton26TcpHeaderV1);
  uint32_t payload_size = sizeof(HandSkeleton26FrameF32V1);
  uint32_t reserved0 = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
};

struct HandSkeleton26RingHeaderV1 {
  char magic[8] = {'H', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = 4096;
  uint32_t slot_count = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_header_size = 128;
  uint32_t payload_size = sizeof(HandSkeleton26FrameF32V1);
  uint32_t reserved0 = 0;
  uint64_t latest_sequence = 0;
};

struct HandSkeleton26RingSlotHeaderV1 {
  uint64_t seq_begin = 0;
  uint64_t seq_end = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint32_t payload_size = sizeof(HandSkeleton26FrameF32V1);
  uint32_t flags = 0;
  uint8_t reserved[88] = {};
};

#pragma pack(pop)

static_assert(sizeof(HandSkeleton26JointF32V1) == 44, "skeleton26 joint must be 44 bytes");
static_assert(sizeof(HandSkeleton26RingSlotHeaderV1) == 128, "skeleton26 ring slot header must be 128 bytes");

inline HandSkeleton26FrameF32V1 make_no_hands_skeleton26_frame(uint64_t sequence,
                                                               int64_t timestamp_ns,
                                                               uint32_t space = HAND_SKELETON26_SPACE_UNKNOWN,
                                                               uint32_t source = HAND_SKELETON26_SOURCE_UNKNOWN) {
  HandSkeleton26FrameF32V1 f;
  f.sequence = sequence;
  f.timestamp_ns = static_cast<uint64_t>(timestamp_ns > 0 ? timestamp_ns : skeleton26_now_ns());
  f.source_timestamp_ns = f.timestamp_ns;
  f.space = space;
  f.source = source;
  f.flags = 0;
  f.hand_count = 0;
  f.left.handedness = 1;
  f.right.handedness = 2;
  for (uint32_t i = 0; i < HAND_SKELETON26_JOINT_COUNT; ++i) {
    f.left.joints[i].joint_id = i;
    f.right.joints[i].joint_id = i;
  }
  return f;
}

inline std::string normalize_posix_shm_name(std::string name) {
  if (name.empty()) throw std::runtime_error("empty SHM name");
  if (name[0] != '/') name = "/" + name;
  return name;
}

struct HandSkeleton26ShmPublisherConfig {
  std::string registry_path = "/tmp/tracking_streams.json";
  std::string stream_id = "hand_skeleton26";
  std::string shm_name = "track_hand_skeleton26";
  std::string frame_id = "tracking_world";
  uint32_t slot_count = 1024;
  bool unlink_existing = true;
  std::string created_by = "hand_skeleton26_backend";
  uint32_t space = HAND_SKELETON26_SPACE_UNKNOWN;
  uint32_t source = HAND_SKELETON26_SOURCE_UNKNOWN;
};

#ifndef _WIN32
class HandSkeleton26ShmPublisher {
 public:
  explicit HandSkeleton26ShmPublisher(HandSkeleton26ShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.slot_count == 0) throw std::runtime_error("hand_skeleton26 slot_count must be > 0");

    header_size_ = 4096;
    slot_header_size_ = sizeof(HandSkeleton26RingSlotHeaderV1);
    payload_size_ = sizeof(HandSkeleton26FrameF32V1);
    slot_stride_ = slot_header_size_ + payload_size_;
    size_ = header_size_ + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

    const std::string posix_name = normalize_posix_shm_name(cfg_.shm_name);
    if (cfg_.unlink_existing) shm_unlink(posix_name.c_str());

    fd_ = shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    }
    if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
      const std::string err = std::strerror(errno);
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("ftruncate failed for " + posix_name + ": " + err);
    }

    data_ = static_cast<uint8_t*>(mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      const std::string err = std::strerror(errno);
      data_ = nullptr;
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("mmap failed for " + posix_name + ": " + err);
    }

    std::memset(data_, 0, size_);
    auto* h = reinterpret_cast<HandSkeleton26RingHeaderV1*>(data_);
    *h = HandSkeleton26RingHeaderV1{};
    h->slot_count = cfg_.slot_count;
    h->slot_stride = static_cast<uint32_t>(slot_stride_);
    h->slot_header_size = static_cast<uint32_t>(slot_header_size_);
    h->payload_size = static_cast<uint32_t>(payload_size_);

    update_registry();
  }

  ~HandSkeleton26ShmPublisher() {
    if (data_) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  HandSkeleton26ShmPublisher(const HandSkeleton26ShmPublisher&) = delete;
  HandSkeleton26ShmPublisher& operator=(const HandSkeleton26ShmPublisher&) = delete;

  uint64_t next_sequence() const { return sequence_ + 1; }

  void publish(HandSkeleton26FrameF32V1 frame) {
    if (!data_) return;
    const uint64_t seq = ++sequence_;
    if (frame.timestamp_ns == 0) frame.timestamp_ns = static_cast<uint64_t>(skeleton26_now_ns());
    if (frame.source_timestamp_ns == 0) frame.source_timestamp_ns = frame.timestamp_ns;
    frame.sequence = seq;
    frame.version = HAND_SKELETON26_FORMAT_VERSION;
    frame.size_bytes = sizeof(HandSkeleton26FrameF32V1);

    const uint64_t committed_seq = seq * 2;
    const uint64_t writing_seq = committed_seq - 1;
    const uint32_t slot_idx = static_cast<uint32_t>((seq - 1) % cfg_.slot_count);
    uint8_t* slot_base = data_ + header_size_ + static_cast<size_t>(slot_idx) * slot_stride_;
    auto* slot = reinterpret_cast<HandSkeleton26RingSlotHeaderV1*>(slot_base);

    slot->seq_begin = writing_seq;
    slot->seq_end = writing_seq;
    slot->timestamp_ns = frame.timestamp_ns;
    slot->source_timestamp_ns = frame.source_timestamp_ns;
    slot->payload_size = sizeof(HandSkeleton26FrameF32V1);
    slot->flags = 0;

    std::memcpy(slot_base + slot_header_size_, &frame, sizeof(HandSkeleton26FrameF32V1));
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);

    auto* h = reinterpret_cast<HandSkeleton26RingHeaderV1*>(data_);
    h->latest_sequence = seq;
  }

 private:
  void update_registry() const {
    namespace fs = std::filesystem;
    const fs::path registry_path(cfg_.registry_path);
    const fs::path parent = registry_path.parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    const fs::path lock_path = registry_path.string() + ".lock";
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) throw std::runtime_error("failed to open registry lock: " + lock_path.string());
    if (flock(lock_fd, LOCK_EX) != 0) {
      close(lock_fd);
      throw std::runtime_error("failed to lock registry: " + lock_path.string());
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
    if (!j.contains("streams") || !j["streams"].is_object()) j["streams"] = nlohmann::json::object();

    j["streams"][cfg_.stream_id] = {
        {"shm_name", cfg_.shm_name},
        {"format_name", HAND_SKELETON26_FORMAT_NAME},
        {"format_version", HAND_SKELETON26_FORMAT_VERSION},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"header_size", header_size_},
        {"slot_header_size", slot_header_size_},
        {"slot_stride", slot_stride_},
        {"frame_id", cfg_.frame_id},
        {"created_by", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"},
        {"joint_count", HAND_SKELETON26_JOINT_COUNT},
        {"joint_order", "openxr_ext_hand_tracking_26"},
        {"numeric_type", "float32_le"},
        {"space", cfg_.space},
        {"source", cfg_.source},
    };

    const fs::path tmp_path = registry_path.string() + ".tmp." + std::to_string(getpid());
    {
      std::ofstream out(tmp_path);
      out << j.dump(2) << "\n";
    }
    fs::rename(tmp_path, registry_path);

    flock(lock_fd, LOCK_UN);
    close(lock_fd);
  }

  HandSkeleton26ShmPublisherConfig cfg_;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t header_size_ = 4096;
  size_t slot_header_size_ = sizeof(HandSkeleton26RingSlotHeaderV1);
  size_t payload_size_ = sizeof(HandSkeleton26FrameF32V1);
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
};

class HandSkeleton26TcpClient {
 public:
  HandSkeleton26TcpClient(std::string host, int port)
      : host_(std::move(host)), port_(port) {
    connect_socket();
  }

  ~HandSkeleton26TcpClient() {
    if (fd_ >= 0) close(fd_);
  }

  HandSkeleton26TcpClient(const HandSkeleton26TcpClient&) = delete;
  HandSkeleton26TcpClient& operator=(const HandSkeleton26TcpClient&) = delete;

  HandSkeleton26FrameF32V1 read_next() {
    HandSkeleton26TcpHeaderV1 header{};
    read_exact(&header, sizeof(header));
    if (header.magic != HAND_SKELETON26_TCP_MAGIC) {
      throw std::runtime_error("invalid hand_skeleton26 TCP magic");
    }
    if (header.version != HAND_SKELETON26_TCP_VERSION) {
      throw std::runtime_error("unsupported hand_skeleton26 TCP version");
    }
    if (header.payload_size != sizeof(HandSkeleton26FrameF32V1)) {
      throw std::runtime_error("unexpected hand_skeleton26 TCP payload size");
    }
    HandSkeleton26FrameF32V1 frame{};
    read_exact(&frame, sizeof(frame));
    if (frame.size_bytes != sizeof(HandSkeleton26FrameF32V1)) {
      throw std::runtime_error("invalid hand_skeleton26 frame size_bytes");
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
      throw std::runtime_error("getaddrinfo failed for hand_skeleton26 TCP: " + std::string(gai_strerror(rc)));
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
    throw std::runtime_error("failed to connect hand_skeleton26 TCP " + host_ + ":" + port_s + ": " + std::strerror(errno));
  }

  void read_exact(void* dst, size_t size) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t off = 0;
    while (off < size) {
      const ssize_t n = recv(fd_, out + off, size - off, 0);
      if (n == 0) throw std::runtime_error("hand_skeleton26 TCP peer closed");
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          throw std::runtime_error("hand_skeleton26 TCP recv timeout");
        }
        throw std::runtime_error("hand_skeleton26 TCP recv failed: " + std::string(std::strerror(errno)));
      }
      off += static_cast<size_t>(n);
    }
  }

  std::string host_;
  int port_ = 0;
  int fd_ = -1;
};
#else
class HandSkeleton26ShmPublisher {
 public:
  explicit HandSkeleton26ShmPublisher(HandSkeleton26ShmPublisherConfig) {
    throw std::runtime_error("HandSkeleton26ShmPublisher is not available in native Windows builds yet");
  }
};

class HandSkeleton26TcpClient {
 public:
  HandSkeleton26TcpClient(std::string, int) {
    throw std::runtime_error("HandSkeleton26TcpClient native Windows implementation is not available yet");
  }
  HandSkeleton26FrameF32V1 read_next() { return {}; }
};
#endif

}  // namespace xr_tracking
