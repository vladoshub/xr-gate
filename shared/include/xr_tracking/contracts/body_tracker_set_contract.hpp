#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace xr_tracking {

inline int64_t body_tracker_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* BODY_TRACKER_SET_FORMAT_NAME = "BODY_TRACKER_SET_F32_V1";
constexpr uint32_t BODY_TRACKER_SET_FORMAT_VERSION = 1;
constexpr uint32_t BODY_TRACKER_MAX_TRACKERS = 32;
constexpr uint32_t BODY_TRACKER_UDP_MAGIC = 0x4254524Bu; // 'BTRK' marker
constexpr uint32_t BODY_TRACKER_UDP_VERSION = 1;

enum BodyTrackerSpace : uint32_t {
  BODY_TRACKER_SPACE_UNKNOWN = 0,
  BODY_TRACKER_SPACE_TRACKING_WORLD = 1,
  BODY_TRACKER_SPACE_HMD = 2,
  BODY_TRACKER_SPACE_RUNTIME_HMD = 3,
  BODY_TRACKER_SPACE_STEAMVR_TRACKING = 4,
  BODY_TRACKER_SPACE_OPENXR_LOCAL = 5,
};

enum BodyTrackerSource : uint32_t {
  BODY_TRACKER_SOURCE_UNKNOWN = 0,
  BODY_TRACKER_SOURCE_STEAMVR = 1,
  BODY_TRACKER_SOURCE_OPENXR = 2,
  BODY_TRACKER_SOURCE_SLIMEVR = 3,
  BODY_TRACKER_SOURCE_VIVE_TRACKER = 4,
  BODY_TRACKER_SOURCE_TUNDRA_TRACKER = 5,
  BODY_TRACKER_SOURCE_APRILTAG = 6,
  BODY_TRACKER_SOURCE_IMU = 7,
};

enum BodyTrackerRole : uint32_t {
  BODY_TRACKER_ROLE_UNKNOWN = 0,
  BODY_TRACKER_ROLE_GENERIC = 1,
  BODY_TRACKER_ROLE_WAIST = 2,
  BODY_TRACKER_ROLE_CHEST = 3,
  BODY_TRACKER_ROLE_LEFT_FOOT = 4,
  BODY_TRACKER_ROLE_RIGHT_FOOT = 5,
  BODY_TRACKER_ROLE_LEFT_KNEE = 6,
  BODY_TRACKER_ROLE_RIGHT_KNEE = 7,
  BODY_TRACKER_ROLE_LEFT_ELBOW = 8,
  BODY_TRACKER_ROLE_RIGHT_ELBOW = 9,
  BODY_TRACKER_ROLE_LEFT_SHOULDER = 10,
  BODY_TRACKER_ROLE_RIGHT_SHOULDER = 11,
  BODY_TRACKER_ROLE_HEAD = 12,
  BODY_TRACKER_ROLE_CAMERA = 13,
};

enum BodyTrackerStatus : uint32_t {
  BODY_TRACKER_STATUS_UNAVAILABLE = 0,
  BODY_TRACKER_STATUS_CONNECTED = 1,
  BODY_TRACKER_STATUS_TRACKING = 2,
  BODY_TRACKER_STATUS_LOST = 3,
  BODY_TRACKER_STATUS_STALE = 4,
};

enum BodyTrackerFrameFlags : uint32_t {
  BODY_TRACKER_FRAME_HAS_TRACKERS = 1u << 0,
};

enum BodyTrackerFlags : uint32_t {
  BODY_TRACKER_FLAG_POSE_VALID = 1u << 0,
  BODY_TRACKER_FLAG_POSITION_VALID = 1u << 1,
  BODY_TRACKER_FLAG_ORIENTATION_VALID = 1u << 2,
  BODY_TRACKER_FLAG_LINEAR_VELOCITY_VALID = 1u << 3,
  BODY_TRACKER_FLAG_ANGULAR_VELOCITY_VALID = 1u << 4,
  BODY_TRACKER_FLAG_ROLE_VALID = 1u << 5,
  BODY_TRACKER_FLAG_CONNECTED = 1u << 6,
};

#pragma pack(push, 1)

struct BodyTrackerPoseF32V1 {
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
  float qw = 1.0f;
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  float wx = 0.0f;
  float wy = 0.0f;
  float wz = 0.0f;
};

struct BodyTrackerF32V1 {
  uint32_t tracker_index = 0;
  uint32_t role = BODY_TRACKER_ROLE_UNKNOWN;
  uint32_t status = BODY_TRACKER_STATUS_UNAVAILABLE;
  uint32_t flags = 0;

  uint64_t stable_id_hash = 0;
  uint64_t device_serial_hash = 0;

  float confidence = 0.0f;
  float battery = -1.0f;
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;

  BodyTrackerPoseF32V1 pose;

  char tracker_id[64] = {};
};

struct BodyTrackerSetFrameF32V1 {
  uint32_t version = BODY_TRACKER_SET_FORMAT_VERSION;
  uint32_t size_bytes = sizeof(BodyTrackerSetFrameF32V1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t space = BODY_TRACKER_SPACE_UNKNOWN;
  uint32_t source = BODY_TRACKER_SOURCE_UNKNOWN;
  uint32_t flags = 0;
  uint32_t tracker_count = 0;

  BodyTrackerF32V1 trackers[BODY_TRACKER_MAX_TRACKERS];
};

struct BodyTrackerSetUdpHeaderV1 {
  uint32_t magic = BODY_TRACKER_UDP_MAGIC;
  uint16_t version = BODY_TRACKER_UDP_VERSION;
  uint16_t header_size = sizeof(BodyTrackerSetUdpHeaderV1);
  uint32_t payload_size = sizeof(BodyTrackerSetFrameF32V1);
  uint32_t reserved0 = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
};

struct BodyTrackerSetRingHeaderV1 {
  char magic[8] = {'B', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = 4096;
  uint32_t slot_count = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_header_size = 128;
  uint32_t payload_size = sizeof(BodyTrackerSetFrameF32V1);
  uint32_t reserved0 = 0;
  uint64_t latest_sequence = 0;
};

struct BodyTrackerSetRingSlotHeaderV1 {
  uint64_t seq_begin = 0;
  uint64_t seq_end = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint32_t payload_size = sizeof(BodyTrackerSetFrameF32V1);
  uint32_t flags = 0;
  uint8_t reserved[88] = {};
};

#pragma pack(pop)

static_assert(sizeof(BodyTrackerSetUdpHeaderV1) == 32, "body tracker UDP header must be 32 bytes");
static_assert(sizeof(BodyTrackerSetRingSlotHeaderV1) == 128, "body tracker ring slot header must be 128 bytes");

inline std::string normalize_body_tracker_posix_shm_name(std::string name) {
  if (name.empty()) throw std::runtime_error("empty SHM name");
  if (name[0] != '/') name = "/" + name;
  return name;
}

inline BodyTrackerSetFrameF32V1 make_empty_body_tracker_set_frame(
    uint64_t sequence,
    int64_t timestamp_ns,
    uint32_t space = BODY_TRACKER_SPACE_UNKNOWN,
    uint32_t source = BODY_TRACKER_SOURCE_UNKNOWN) {
  BodyTrackerSetFrameF32V1 f;
  f.sequence = sequence;
  f.timestamp_ns = static_cast<uint64_t>(timestamp_ns > 0 ? timestamp_ns : body_tracker_now_ns());
  f.source_timestamp_ns = f.timestamp_ns;
  f.space = space;
  f.source = source;
  f.flags = 0;
  f.tracker_count = 0;
  return f;
}

inline BodyTrackerSetFrameF32V1 decode_body_tracker_set_udp_packet(const void* data, size_t size) {
  if (size != sizeof(BodyTrackerSetUdpHeaderV1) + sizeof(BodyTrackerSetFrameF32V1)) {
    throw std::runtime_error("invalid body_tracker_set UDP packet size");
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  BodyTrackerSetUdpHeaderV1 header{};
  std::memcpy(&header, bytes, sizeof(header));
  if (header.magic != BODY_TRACKER_UDP_MAGIC) {
    throw std::runtime_error("invalid body_tracker_set UDP magic");
  }
  if (header.version != BODY_TRACKER_UDP_VERSION) {
    throw std::runtime_error("unsupported body_tracker_set UDP version");
  }
  if (header.header_size != sizeof(BodyTrackerSetUdpHeaderV1) ||
      header.payload_size != sizeof(BodyTrackerSetFrameF32V1)) {
    throw std::runtime_error("invalid body_tracker_set UDP header sizes");
  }
  BodyTrackerSetFrameF32V1 frame{};
  std::memcpy(&frame, bytes + sizeof(header), sizeof(frame));
  if (frame.version != BODY_TRACKER_SET_FORMAT_VERSION ||
      frame.size_bytes != sizeof(BodyTrackerSetFrameF32V1)) {
    throw std::runtime_error("invalid body_tracker_set frame header");
  }
  if (frame.tracker_count > BODY_TRACKER_MAX_TRACKERS) {
    throw std::runtime_error("body_tracker_set tracker_count exceeds max");
  }
  return frame;
}

struct BodyTrackerSetShmPublisherConfig {
  std::string registry_path = "/tmp/runtime_tracking_streams.json";
  std::string stream_id = "runtime_body_trackers";
  std::string shm_name = "runtime_body_trackers";
  std::string frame_id = "runtime_local";
  uint32_t slot_count = 1024;
  bool unlink_existing = true;
  std::string created_by = "xr_runtime_adapter";
  uint32_t space = BODY_TRACKER_SPACE_RUNTIME_HMD;
  uint32_t source = BODY_TRACKER_SOURCE_UNKNOWN;
};

#ifndef _WIN32
class BodyTrackerSetShmPublisher {
 public:
  explicit BodyTrackerSetShmPublisher(BodyTrackerSetShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.slot_count == 0) throw std::runtime_error("body tracker slot_count must be > 0");

    header_size_ = 4096;
    slot_header_size_ = sizeof(BodyTrackerSetRingSlotHeaderV1);
    payload_size_ = sizeof(BodyTrackerSetFrameF32V1);
    slot_stride_ = slot_header_size_ + payload_size_;
    size_ = header_size_ + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

    const std::string posix_name = normalize_body_tracker_posix_shm_name(cfg_.shm_name);
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
    auto* h = reinterpret_cast<BodyTrackerSetRingHeaderV1*>(data_);
    *h = BodyTrackerSetRingHeaderV1{};
    h->slot_count = cfg_.slot_count;
    h->slot_stride = static_cast<uint32_t>(slot_stride_);
    h->slot_header_size = static_cast<uint32_t>(slot_header_size_);
    h->payload_size = static_cast<uint32_t>(payload_size_);

    update_registry();
  }

  ~BodyTrackerSetShmPublisher() {
    if (data_) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  BodyTrackerSetShmPublisher(const BodyTrackerSetShmPublisher&) = delete;
  BodyTrackerSetShmPublisher& operator=(const BodyTrackerSetShmPublisher&) = delete;

  uint64_t next_sequence() const { return sequence_ + 1; }

  void publish(BodyTrackerSetFrameF32V1 frame) {
    if (!data_) return;
    const uint64_t seq = ++sequence_;
    if (frame.timestamp_ns == 0) frame.timestamp_ns = static_cast<uint64_t>(body_tracker_now_ns());
    if (frame.source_timestamp_ns == 0) frame.source_timestamp_ns = frame.timestamp_ns;
    frame.sequence = seq;
    frame.version = BODY_TRACKER_SET_FORMAT_VERSION;
    frame.size_bytes = sizeof(BodyTrackerSetFrameF32V1);
    frame.tracker_count = std::min<uint32_t>(frame.tracker_count, BODY_TRACKER_MAX_TRACKERS);
    frame.flags = frame.tracker_count > 0 ? (frame.flags | BODY_TRACKER_FRAME_HAS_TRACKERS) : frame.flags;
    if (cfg_.space != BODY_TRACKER_SPACE_UNKNOWN) frame.space = cfg_.space;
    if (cfg_.source != BODY_TRACKER_SOURCE_UNKNOWN) frame.source = cfg_.source;

    const uint64_t committed_seq = seq * 2;
    const uint64_t writing_seq = committed_seq - 1;
    const uint32_t slot_idx = static_cast<uint32_t>((seq - 1) % cfg_.slot_count);
    uint8_t* slot_base = data_ + header_size_ + static_cast<size_t>(slot_idx) * slot_stride_;
    auto* slot = reinterpret_cast<BodyTrackerSetRingSlotHeaderV1*>(slot_base);

    slot->seq_begin = writing_seq;
    slot->seq_end = writing_seq;
    slot->timestamp_ns = frame.timestamp_ns;
    slot->source_timestamp_ns = frame.source_timestamp_ns;
    slot->payload_size = sizeof(BodyTrackerSetFrameF32V1);
    slot->flags = 0;

    std::memcpy(slot_base + slot_header_size_, &frame, sizeof(BodyTrackerSetFrameF32V1));
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);

    auto* h = reinterpret_cast<BodyTrackerSetRingHeaderV1*>(data_);
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
        {"format_name", BODY_TRACKER_SET_FORMAT_NAME},
        {"format_version", BODY_TRACKER_SET_FORMAT_VERSION},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"header_size", header_size_},
        {"slot_header_size", slot_header_size_},
        {"slot_stride", slot_stride_},
        {"frame_id", cfg_.frame_id},
        {"created_by", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"},
        {"max_trackers", BODY_TRACKER_MAX_TRACKERS},
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

  BodyTrackerSetShmPublisherConfig cfg_;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t header_size_ = 4096;
  size_t slot_header_size_ = sizeof(BodyTrackerSetRingSlotHeaderV1);
  size_t payload_size_ = sizeof(BodyTrackerSetFrameF32V1);
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
};
#else
class BodyTrackerSetShmPublisher {
 public:
  explicit BodyTrackerSetShmPublisher(BodyTrackerSetShmPublisherConfig) {
    throw std::runtime_error("BodyTrackerSetShmPublisher is not available in native Windows builds yet");
  }
  void publish(BodyTrackerSetFrameF32V1) {}
};
#endif

}  // namespace xr_tracking
