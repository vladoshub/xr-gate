#pragma once

// Shared source-HMD pose publisher for backend -> xr_runtime_adapter/UDP bridge.
//
// This publishes the source tracking stream named "hmd_pose" by default. It is
// intentionally distinct from RuntimePoseShmPublisher, which publishes the
// runtime-facing post-transform/predicted stream named "runtime_hmd_pose".

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <xr_tracking/types/tracking_types.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xr_runtime {

constexpr uint32_t HMD_POSE_STREAM_KIND_POSE = 4;
constexpr uint32_t HMD_POSE_FORMAT_CODE_F64_LE = 201;
constexpr const char* HMD_POSE_FORMAT_NAME = "HMD_POSE_F64_LE";

struct HmdPoseShmPublisherConfig {
  std::string registry_path = "/tmp/tracking_streams.json";
  std::string stream_id = "hmd_pose";
  std::string shm_name = "track_hmd_pose";
  std::string frame_id = "tracking_world";
  uint32_t slot_count = 1024;
  uint32_t header_size = 4096;
  uint32_t slot_header_size = 128;
  std::string created_by = "hmd_pose_backend";
};

#ifndef _WIN32
class HmdPoseShmPublisher {
 public:
  explicit HmdPoseShmPublisher(HmdPoseShmPublisherConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.stream_id.empty()) throw std::runtime_error("pose stream_id is empty");
    if (cfg_.shm_name.empty()) throw std::runtime_error("pose shm_name is empty");
    if (cfg_.slot_count == 0) throw std::runtime_error("pose slot_count must be > 0");

    payload_size_ = sizeof(HmdPoseF64V1);
    slot_stride_ = cfg_.slot_header_size + payload_size_;
    total_size_ = cfg_.header_size + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

    open_or_create_shm();
    init_global_header();
    write_registry();
  }

  ~HmdPoseShmPublisher() { close(); }

  HmdPoseShmPublisher(const HmdPoseShmPublisher&) = delete;
  HmdPoseShmPublisher& operator=(const HmdPoseShmPublisher&) = delete;

  void publish(HmdPoseF64V1 pose) {
    if (!data_) return;

    const uint64_t seq = ++sequence_;
    pose.version = 1;
    pose.size_bytes = sizeof(HmdPoseF64V1);
    if (pose.sequence == 0) pose.sequence = seq;
    if (pose.timestamp_ns == 0) pose.timestamp_ns = monotonic_ns();
    if (pose.source_timestamp_ns == 0) pose.source_timestamp_ns = pose.timestamp_ns;

    const uint64_t seq_odd = seq * 2 - 1;
    const uint64_t seq_even = seq * 2;

    const uint64_t slot_index = (seq - 1) % cfg_.slot_count;
    uint8_t* slot = data_ + cfg_.header_size + slot_index * slot_stride_;
    uint8_t* payload = slot + cfg_.slot_header_size;

    store_u64(slot + 0, seq_odd);
    store_u64(slot + 8, seq_odd);
    std::atomic_thread_fence(std::memory_order_release);

    store_i64(slot + 16, static_cast<int64_t>(pose.timestamp_ns));
    store_u64(slot + 24, monotonic_ns());
    store_u32(slot + 32, static_cast<uint32_t>(payload_size_));
    store_u32(slot + 36, 0);
    store_u32(slot + 40, 0);
    store_u32(slot + 44, HMD_POSE_FORMAT_CODE_F64_LE);
    store_u32(slot + 48, pose.flags);
    store_u32(slot + 52, 0);

    std::memset(slot + 60, 0, 32);
    const size_t frame_len = std::min<size_t>(cfg_.frame_id.size(), 31);
    std::memcpy(slot + 60, cfg_.frame_id.data(), frame_len);

    std::memcpy(payload, &pose, sizeof(HmdPoseF64V1));

    std::atomic_thread_fence(std::memory_order_release);
    store_u64(slot + 0, seq_even);
    store_u64(slot + 8, seq_even);
    std::atomic_thread_fence(std::memory_order_release);

    store_u64(data_ + 40, seq);
  }

  const HmdPoseShmPublisherConfig& config() const { return cfg_; }

 private:
  static uint64_t monotonic_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  static void store_u64(void* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }
  static void store_i64(void* p, int64_t v) { std::memcpy(p, &v, sizeof(v)); }
  static void store_u32(void* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

  void open_or_create_shm() {
    std::string posix_name = cfg_.shm_name;
    if (posix_name.empty() || posix_name[0] != '/') posix_name = "/" + posix_name;

    fd_ = ::shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    }

    if (::ftruncate(fd_, static_cast<off_t>(total_size_)) != 0) {
      const int err = errno;
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("ftruncate failed for " + posix_name + ": " + std::strerror(err));
    }

    data_ = static_cast<uint8_t*>(::mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      const int err = errno;
      data_ = nullptr;
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("mmap failed for " + posix_name + ": " + std::strerror(err));
    }

    std::memset(data_, 0, total_size_);
  }

  void init_global_header() {
    const char magic[8] = {'C', 'A', 'P', 'S', 'H', 'M', '1', '\0'};
    std::memcpy(data_ + 0, magic, 8);
    store_u32(data_ + 8, 1);
    store_u32(data_ + 12, HMD_POSE_STREAM_KIND_POSE);
    store_u32(data_ + 16, 0);
    store_u32(data_ + 20, 0);
    store_u32(data_ + 24, HMD_POSE_FORMAT_CODE_F64_LE);
    store_u32(data_ + 28, static_cast<uint32_t>(payload_size_));
    store_u64(data_ + 32, cfg_.slot_count);
    store_u64(data_ + 40, 0);

    nlohmann::json meta = {
        {"stream_id", cfg_.stream_id},
        {"kind", "POSE"},
        {"format_name", HMD_POSE_FORMAT_NAME},
        {"frame_id", cfg_.frame_id},
        {"payload_schema", "HmdPoseF64"},
        {"payload_version", 1},
        {"coordinate_frame", cfg_.frame_id},
        {"units", {{"position", "m"}, {"linear_velocity", "m/s"}, {"angular_velocity", "rad/s"}}},
        {"quaternion_order", "wxyz"},
        {"producer", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"},
        {"status", "0 invalid, 1 initializing, 2 tracking, 3 degraded, 4 lost"}};

    const std::string meta_s = meta.dump();
    const size_t max_meta = cfg_.header_size > 48 ? cfg_.header_size - 48 : 0;
    if (meta_s.size() < max_meta) {
      std::memcpy(data_ + 48, meta_s.data(), meta_s.size());
    }
  }

  void write_registry() {
    nlohmann::json reg;
    const std::filesystem::path path(cfg_.registry_path);

    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path());
    }

    const std::filesystem::path lock_path = path.string() + ".lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) {
      throw std::runtime_error("failed to open pose registry lock: " + lock_path.string() +
                               ": " + std::strerror(errno));
    }

    struct RegistryLockGuard {
      explicit RegistryLockGuard(int fd_) : fd(fd_) {}
      int fd = -1;
      ~RegistryLockGuard() {
        if (fd >= 0) {
          ::flock(fd, LOCK_UN);
          ::close(fd);
        }
      }
      RegistryLockGuard(const RegistryLockGuard&) = delete;
      RegistryLockGuard& operator=(const RegistryLockGuard&) = delete;
    } lock_guard{lock_fd};

    if (::flock(lock_fd, LOCK_EX) != 0) {
      throw std::runtime_error("failed to lock pose registry: " + lock_path.string() +
                               ": " + std::strerror(errno));
    }

    try {
      if (std::filesystem::exists(path)) {
        std::ifstream is(path);
        if (is) is >> reg;
      }
    } catch (...) {
      reg = nlohmann::json::object();
    }

    if (!reg.is_object()) reg = nlohmann::json::object();
    reg["version"] = 1;
    reg["created_by"] = cfg_.created_by;
    if (!reg.contains("streams") || !reg["streams"].is_object()) {
      reg["streams"] = nlohmann::json::object();
    }

    reg["streams"][cfg_.stream_id] = {
        {"shm_name", cfg_.shm_name},
        {"kind", "POSE"},
        {"frame_id", cfg_.frame_id},
        {"width", 0},
        {"height", 0},
        {"format_code", HMD_POSE_FORMAT_CODE_F64_LE},
        {"format_name", HMD_POSE_FORMAT_NAME},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"slot_header_size", cfg_.slot_header_size},
        {"header_size", cfg_.header_size},
        {"slot_stride", slot_stride_},
        {"payload_schema", "HmdPoseF64"},
        {"payload_version", 1},
        {"coordinate_frame", cfg_.frame_id},
        {"quaternion_order", "wxyz"},
        {"position_units", "m"},
        {"linear_velocity_units", "m/s"},
        {"angular_velocity_units", "rad/s"},
        {"created_by", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"}};

    const auto tmp = path.string() + ".tmp." + std::to_string(::getpid());
    {
      std::ofstream os(tmp);
      if (!os) throw std::runtime_error("failed to write pose registry tmp: " + tmp);
      os << reg.dump(2) << "\n";
      if (!os) throw std::runtime_error("failed to flush pose registry tmp: " + tmp);
    }

    std::filesystem::rename(tmp, path);
  }

  void close() {
    if (data_) {
      ::munmap(data_, total_size_);
      data_ = nullptr;
      total_size_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  HmdPoseShmPublisherConfig cfg_;
  size_t payload_size_ = 0;
  size_t slot_stride_ = 0;
  size_t total_size_ = 0;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  uint64_t sequence_ = 0;
};
#else
class HmdPoseShmPublisher {
 public:
  explicit HmdPoseShmPublisher(HmdPoseShmPublisherConfig) {
    throw std::runtime_error("HmdPoseShmPublisher is not implemented for Windows yet");
  }
  void publish(HmdPoseF64V1) {}
};
#endif

}  // namespace xr_runtime
