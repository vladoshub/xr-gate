#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace xr_tracking {

inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

enum class HandTrackingStatus : uint32_t {
  NoHands = 0,
  Tracking = 1,
  Lost = 2,
  Degraded = 3,
};

enum HandTrackingFlags : uint32_t {
  HAND_FLAG_LEFT_VALID = 1u << 0,
  HAND_FLAG_RIGHT_VALID = 1u << 1,
  HAND_FLAG_JOINTS_VALID = 1u << 2,
  HAND_FLAG_GESTURES_VALID = 1u << 3,
};

enum HandPoseFlags : uint32_t {
  HAND_POSE_VALID = 1u << 0,
  HAND_LINEAR_VELOCITY_VALID = 1u << 1,
  HAND_ANGULAR_VELOCITY_VALID = 1u << 2,
  HAND_JOINTS_VALID = 1u << 3,
  HAND_PINCH_VALID = 1u << 4,
  HAND_GRAB_VALID = 1u << 5,
};

constexpr uint32_t HAND_JOINT_COUNT_V1 = 26;

// OpenXR-compatible ordering by convention, but runtime adapters may remap:
// 0 PALM
// 1 WRIST
// 2..5 THUMB_METACARPAL/PROXIMAL/DISTAL/TIP
// 6..10 INDEX_METACARPAL/PROXIMAL/INTERMEDIATE/DISTAL/TIP
// 11..15 MIDDLE...
// 16..20 RING...
// 21..25 LITTLE...
#pragma pack(push, 1)

struct HandJointF64V1 {
  uint32_t joint_id = 0;
  uint32_t flags = 0;
  double px = 0.0;
  double py = 0.0;
  double pz = 0.0;
  double qw = 1.0;
  double qx = 0.0;
  double qy = 0.0;
  double qz = 0.0;
  float radius_m = 0.0f;
  float confidence = 0.0f;
  uint32_t reserved0 = 0;
};

struct HandSideF64V1 {
  uint32_t handedness = 0; // 1=left, 2=right
  uint32_t status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  uint32_t flags = 0;
  float confidence = 0.0f;

  double palm_px = 0.0;
  double palm_py = 0.0;
  double palm_pz = 0.0;
  double palm_qw = 1.0;
  double palm_qx = 0.0;
  double palm_qy = 0.0;
  double palm_qz = 0.0;

  double wrist_px = 0.0;
  double wrist_py = 0.0;
  double wrist_pz = 0.0;
  double wrist_qw = 1.0;
  double wrist_qx = 0.0;
  double wrist_qy = 0.0;
  double wrist_qz = 0.0;

  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
  double wx = 0.0;
  double wy = 0.0;
  double wz = 0.0;

  float pinch_strength = 0.0f;
  float grab_strength = 0.0f;
  uint32_t pinch_active = 0;
  uint32_t grab_active = 0;

  uint32_t joint_count = HAND_JOINT_COUNT_V1;
  uint32_t reserved0 = 0;

  HandJointF64V1 joints[HAND_JOINT_COUNT_V1];
};

struct HandTrackingFrameF64V1 {
  uint32_t version = 1;
  uint32_t size_bytes = sizeof(HandTrackingFrameF64V1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t tracking_status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  uint32_t flags = 0;
  float confidence = 0.0f;
  uint32_t hand_count = 0;

  HandSideF64V1 left;
  HandSideF64V1 right;
};


// 21-joint runtime hand contract.  Keep V1
// available for existing consumers, and add V2 beside it instead of changing
// V1's ABI.  V2 intentionally uses float32 for poses/joints so that it can be
// reused by UDP/network transports without depending on host double layout.
constexpr uint32_t HAND_JOINT_COUNT_V2 = 21;
constexpr uint32_t HAND_TRACKING_FORMAT_VERSION_V1 = 1;
constexpr uint32_t HAND_TRACKING_FORMAT_VERSION_V2 = 2;
inline constexpr const char* HAND_TRACKING_21_JOINT_FORMAT_NAME = "HAND_TRACKING_21_JOINT_F32_V2";
inline constexpr const char* HAND_TRACKING_21_JOINT_ORDER = "hand_tracking_21_joint";

struct HandJointF32V2 {
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

struct HandSideF32V2 {
  uint32_t handedness = 0; // 1=left, 2=right
  uint32_t status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  uint32_t flags = 0;
  float confidence = 0.0f;

  // Controller pose is the pose runtime adapters/OpenVR should use first.
  // Palm/wrist remain available for gesture/skeleton consumers.
  float controller_px = 0.0f;
  float controller_py = 0.0f;
  float controller_pz = 0.0f;
  float controller_qw = 1.0f;
  float controller_qx = 0.0f;
  float controller_qy = 0.0f;
  float controller_qz = 0.0f;

  float palm_px = 0.0f;
  float palm_py = 0.0f;
  float palm_pz = 0.0f;
  float palm_qw = 1.0f;
  float palm_qx = 0.0f;
  float palm_qy = 0.0f;
  float palm_qz = 0.0f;

  float wrist_px = 0.0f;
  float wrist_py = 0.0f;
  float wrist_pz = 0.0f;
  float wrist_qw = 1.0f;
  float wrist_qx = 0.0f;
  float wrist_qy = 0.0f;
  float wrist_qz = 0.0f;

  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  float wx = 0.0f;
  float wy = 0.0f;
  float wz = 0.0f;

  float pinch_strength = 0.0f;
  float grab_strength = 0.0f;
  uint32_t pinch_active = 0;
  uint32_t grab_active = 0;

  uint32_t joint_count = HAND_JOINT_COUNT_V2;
  uint32_t reserved0 = 0;

  // 21-joint hand tracking order:
  // 0 wrist, 1..4 thumb, 5..8 index, 9..12 middle, 13..16 ring, 17..20 little.
  HandJointF32V2 joints[HAND_JOINT_COUNT_V2];
};

struct HandTrackingFrameF32V2 {
  uint32_t version = HAND_TRACKING_FORMAT_VERSION_V2;
  uint32_t size_bytes = sizeof(HandTrackingFrameF32V2);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t tracking_status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  uint32_t flags = 0;
  float confidence = 0.0f;
  uint32_t hand_count = 0;

  HandSideF32V2 left;
  HandSideF32V2 right;
};

struct TrackingRingHeaderV1 {
  char magic[8] = {'H', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = 4096;
  uint32_t slot_count = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_header_size = 128;
  uint32_t payload_size = sizeof(HandTrackingFrameF64V1);
  uint32_t reserved0 = 0;
  uint64_t latest_sequence = 0;
};

struct TrackingSlotHeaderV1 {
  uint64_t seq_begin = 0; // odd while writer is active, even when committed
  uint64_t seq_end = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint32_t payload_size = sizeof(HandTrackingFrameF64V1);
  uint32_t flags = 0;
  uint8_t reserved[88] = {};
};

#pragma pack(pop)

static_assert(sizeof(TrackingSlotHeaderV1) == 128, "slot header must be 128 bytes");
static_assert(sizeof(HandJointF32V2) == 44, "HAND_TRACKING_21_JOINT_F32_V2 joint must be 44 bytes");
static_assert(sizeof(HandSideF32V2) == 1072, "HAND_TRACKING_21_JOINT_F32_V2 side must be 1072 bytes");
static_assert(sizeof(HandTrackingFrameF32V2) == 2200, "HAND_TRACKING_21_JOINT_F32_V2 frame must be 2200 bytes");

inline HandTrackingFrameF64V1 make_no_hands_frame(int64_t source_timestamp_ns,
                                                  uint64_t reset_counter = 0) {
  HandTrackingFrameF64V1 f;
  f.timestamp_ns = static_cast<uint64_t>(now_ns());
  f.source_timestamp_ns = static_cast<uint64_t>(source_timestamp_ns);
  f.reset_counter = reset_counter;
  f.tracking_status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  f.flags = 0;
  f.confidence = 0.0f;
  f.hand_count = 0;

  f.left.handedness = 1;
  f.left.status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  f.left.confidence = 0.0f;

  f.right.handedness = 2;
  f.right.status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  f.right.confidence = 0.0f;

  for (uint32_t i = 0; i < HAND_JOINT_COUNT_V1; ++i) {
    f.left.joints[i].joint_id = i;
    f.right.joints[i].joint_id = i;
  }

  return f;
}

inline HandTrackingFrameF32V2 make_no_hands_frame_v2(int64_t source_timestamp_ns,
                                                     uint64_t reset_counter = 0) {
  HandTrackingFrameF32V2 f;
  f.timestamp_ns = static_cast<uint64_t>(now_ns());
  f.source_timestamp_ns = static_cast<uint64_t>(source_timestamp_ns);
  f.reset_counter = reset_counter;
  f.tracking_status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  f.flags = 0;
  f.confidence = 0.0f;
  f.hand_count = 0;

  f.left.handedness = 1;
  f.left.status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  f.left.confidence = 0.0f;

  f.right.handedness = 2;
  f.right.status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  f.right.confidence = 0.0f;

  for (uint32_t i = 0; i < HAND_JOINT_COUNT_V2; ++i) {
    f.left.joints[i].joint_id = i;
    f.right.joints[i].joint_id = i;
  }

  return f;
}

struct HandTrackingShmPublisherConfig {
  std::string registry_path = "/tmp/tracking_streams.json";
  std::string stream_id = "hand_tracking";
  std::string shm_name = "track_hand_tracking";
  std::string frame_id = "tracking_world";
  uint32_t slot_count = 1024;
  bool unlink_existing = true;
  std::string created_by = "capture_hand_tracking_backend";
};

#ifndef _WIN32
class HandTrackingShmPublisher {
 public:
  explicit HandTrackingShmPublisher(HandTrackingShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.slot_count == 0) throw std::runtime_error("hand_tracking slot_count must be > 0");

    header_size_ = 4096;
    slot_header_size_ = sizeof(TrackingSlotHeaderV1);
    payload_size_ = sizeof(HandTrackingFrameF64V1);
    slot_stride_ = slot_header_size_ + payload_size_;
    size_ = header_size_ + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

    const std::string posix_name = normalize_posix_name(cfg_.shm_name);

    if (cfg_.unlink_existing) {
      shm_unlink(posix_name.c_str());
    }

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

    auto* h = reinterpret_cast<TrackingRingHeaderV1*>(data_);
    *h = TrackingRingHeaderV1{};
    h->slot_count = cfg_.slot_count;
    h->slot_stride = static_cast<uint32_t>(slot_stride_);
    h->slot_header_size = static_cast<uint32_t>(slot_header_size_);
    h->payload_size = static_cast<uint32_t>(payload_size_);

    update_registry();
  }

  ~HandTrackingShmPublisher() {
    if (data_ != nullptr) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  HandTrackingShmPublisher(const HandTrackingShmPublisher&) = delete;
  HandTrackingShmPublisher& operator=(const HandTrackingShmPublisher&) = delete;

  void publish(HandTrackingFrameF64V1 frame) {
    if (!data_) return;

    const uint64_t seq = ++sequence_;
    if (frame.timestamp_ns == 0) frame.timestamp_ns = static_cast<uint64_t>(now_ns());
    if (frame.source_timestamp_ns == 0) frame.source_timestamp_ns = frame.timestamp_ns;
    frame.sequence = seq;
    frame.size_bytes = sizeof(HandTrackingFrameF64V1);

    const uint64_t committed_seq = seq * 2;
    const uint64_t writing_seq = committed_seq - 1;

    const uint32_t slot_idx = static_cast<uint32_t>((seq - 1) % cfg_.slot_count);
    uint8_t* slot_base = data_ + header_size_ + static_cast<size_t>(slot_idx) * slot_stride_;
    auto* slot = reinterpret_cast<TrackingSlotHeaderV1*>(slot_base);

    slot->seq_begin = writing_seq;
    slot->seq_end = writing_seq;
    slot->timestamp_ns = frame.timestamp_ns;
    slot->source_timestamp_ns = frame.source_timestamp_ns;
    slot->payload_size = sizeof(HandTrackingFrameF64V1);
    slot->flags = 0;

    std::memcpy(slot_base + slot_header_size_, &frame, sizeof(HandTrackingFrameF64V1));

    // Commit protocol:
    //   odd seq_begin/seq_end while writing;
    //   even seq_begin == seq_end when the slot is fully readable.
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);

    auto* h = reinterpret_cast<TrackingRingHeaderV1*>(data_);
    h->latest_sequence = seq;
  }

 private:
  static std::string normalize_posix_name(const std::string& name) {
    if (name.empty()) throw std::runtime_error("empty SHM name");
    if (name[0] == '/') return name;
    return "/" + name;
  }

  void update_registry() const {
    namespace fs = std::filesystem;

    const fs::path registry_path(cfg_.registry_path);
    const fs::path parent = registry_path.parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    const fs::path lock_path = registry_path.string() + ".lock";
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) {
      throw std::runtime_error("failed to open registry lock: " + lock_path.string());
    }

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
    if (!j.contains("streams") || !j["streams"].is_object()) {
      j["streams"] = nlohmann::json::object();
    }

    j["streams"][cfg_.stream_id] = {
        {"shm_name", cfg_.shm_name},
        {"format_name", "HAND_TRACKING_V1"},
        {"format_version", 1},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"header_size", header_size_},
        {"slot_header_size", slot_header_size_},
        {"slot_stride", slot_stride_},
        {"frame_id", cfg_.frame_id},
        {"created_by", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"},
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

  HandTrackingShmPublisherConfig cfg_;

  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t header_size_ = 4096;
  size_t slot_header_size_ = sizeof(TrackingSlotHeaderV1);
  size_t payload_size_ = sizeof(HandTrackingFrameF64V1);
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
};


class HandTrackingShmPublisherV2 {
 public:
  explicit HandTrackingShmPublisherV2(HandTrackingShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.slot_count == 0) throw std::runtime_error("hand_tracking slot_count must be > 0");

    header_size_ = 4096;
    slot_header_size_ = sizeof(TrackingSlotHeaderV1);
    payload_size_ = sizeof(HandTrackingFrameF32V2);
    slot_stride_ = slot_header_size_ + payload_size_;
    size_ = header_size_ + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

    const std::string posix_name = normalize_posix_name(cfg_.shm_name);

    if (cfg_.unlink_existing) {
      shm_unlink(posix_name.c_str());
    }

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

    auto* h = reinterpret_cast<TrackingRingHeaderV1*>(data_);
    *h = TrackingRingHeaderV1{};
    h->slot_count = cfg_.slot_count;
    h->slot_stride = static_cast<uint32_t>(slot_stride_);
    h->slot_header_size = static_cast<uint32_t>(slot_header_size_);
    h->payload_size = static_cast<uint32_t>(payload_size_);

    update_registry();
  }

  ~HandTrackingShmPublisherV2() {
    if (data_ != nullptr) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  HandTrackingShmPublisherV2(const HandTrackingShmPublisherV2&) = delete;
  HandTrackingShmPublisherV2& operator=(const HandTrackingShmPublisherV2&) = delete;

  void publish(HandTrackingFrameF32V2 frame) {
    if (!data_) return;

    const uint64_t seq = ++sequence_;
    if (frame.timestamp_ns == 0) frame.timestamp_ns = static_cast<uint64_t>(now_ns());
    if (frame.source_timestamp_ns == 0) frame.source_timestamp_ns = frame.timestamp_ns;
    frame.sequence = seq;
    frame.version = HAND_TRACKING_FORMAT_VERSION_V2;
    frame.size_bytes = sizeof(HandTrackingFrameF32V2);

    const uint64_t committed_seq = seq * 2;
    const uint64_t writing_seq = committed_seq - 1;

    const uint32_t slot_idx = static_cast<uint32_t>((seq - 1) % cfg_.slot_count);
    uint8_t* slot_base = data_ + header_size_ + static_cast<size_t>(slot_idx) * slot_stride_;
    auto* slot = reinterpret_cast<TrackingSlotHeaderV1*>(slot_base);

    slot->seq_begin = writing_seq;
    slot->seq_end = writing_seq;
    slot->timestamp_ns = frame.timestamp_ns;
    slot->source_timestamp_ns = frame.source_timestamp_ns;
    slot->payload_size = sizeof(HandTrackingFrameF32V2);
    slot->flags = 0;

    std::memcpy(slot_base + slot_header_size_, &frame, sizeof(HandTrackingFrameF32V2));

    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);

    auto* h = reinterpret_cast<TrackingRingHeaderV1*>(data_);
    h->latest_sequence = seq;
  }

 private:
  static std::string normalize_posix_name(const std::string& name) {
    if (name.empty()) throw std::runtime_error("empty SHM name");
    if (name[0] == '/') return name;
    return "/" + name;
  }

  void update_registry() const {
    namespace fs = std::filesystem;

    const fs::path registry_path(cfg_.registry_path);
    const fs::path parent = registry_path.parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    const fs::path lock_path = registry_path.string() + ".lock";
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) {
      throw std::runtime_error("failed to open registry lock: " + lock_path.string());
    }

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
    if (!j.contains("streams") || !j["streams"].is_object()) {
      j["streams"] = nlohmann::json::object();
    }

    j["streams"][cfg_.stream_id] = {
        {"shm_name", cfg_.shm_name},
        {"format_name", HAND_TRACKING_21_JOINT_FORMAT_NAME},
        {"format_version", 2},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"header_size", header_size_},
        {"slot_header_size", slot_header_size_},
        {"slot_stride", slot_stride_},
        {"frame_id", cfg_.frame_id},
        {"created_by", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"},
        {"joint_count", HAND_JOINT_COUNT_V2},
        {"joint_order", HAND_TRACKING_21_JOINT_ORDER},
        {"numeric_type", "float32_le"},
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

  HandTrackingShmPublisherConfig cfg_;

  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t header_size_ = 4096;
  size_t slot_header_size_ = sizeof(TrackingSlotHeaderV1);
  size_t payload_size_ = sizeof(HandTrackingFrameF32V2);
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
};

#else
class HandTrackingShmPublisher {
 public:
  explicit HandTrackingShmPublisher(HandTrackingShmPublisherConfig cfg) {
    (void)cfg;
    throw std::runtime_error("hand tracking POSIX SHM publisher is not available in native Windows builds");
  }
  void publish(const HandTrackingFrameF64V1&) {}
};

class HandTrackingShmPublisherV2 {
 public:
  explicit HandTrackingShmPublisherV2(HandTrackingShmPublisherConfig cfg) {
    (void)cfg;
    throw std::runtime_error("hand tracking POSIX SHM publisher is not available in native Windows builds");
  }
  void publish(const HandTrackingFrameF32V2&) {}
};
#endif

}  // namespace xr_tracking
