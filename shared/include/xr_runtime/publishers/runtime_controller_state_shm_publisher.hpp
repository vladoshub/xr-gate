#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>
#include <xr_runtime/contracts/runtime_pose_stream.hpp>
#include <xr_runtime/registry/runtime_paths.hpp>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xr_runtime {

inline int64_t runtime_controller_state_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct RuntimeControllerStateShmPublisherConfig {
  std::string registry_path = default_runtime_tracking_registry_path();
  std::string stream_id = "runtime_controller_state";
  std::string shm_name = "runtime_controller_state";
  std::string frame_id = "runtime_local";
  uint32_t slot_count = 1024;
  bool unlink_existing = true;
  std::string created_by = "xr_runtime_adapter";
};

#ifndef _WIN32
class RuntimeControllerStateShmPublisher {
 public:
  explicit RuntimeControllerStateShmPublisher(RuntimeControllerStateShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.slot_count == 0) {
      throw std::runtime_error("runtime controller state slot_count must be > 0");
    }

    header_size_ = 4096;
    slot_header_size_ = sizeof(RingSlotHeaderV1);
    payload_size_ = sizeof(RuntimeControllerStateFrameV1);
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

    auto* h = reinterpret_cast<RingHeaderV1*>(data_);
    std::memset(h, 0, sizeof(RingHeaderV1));
    const char magic[8] = {'R', 'T', 'C', 'T', 'R', 'L', '1', '\0'};
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
  }

  ~RuntimeControllerStateShmPublisher() {
    if (data_ != nullptr) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  RuntimeControllerStateShmPublisher(const RuntimeControllerStateShmPublisher&) = delete;
  RuntimeControllerStateShmPublisher& operator=(const RuntimeControllerStateShmPublisher&) = delete;

  uint64_t next_sequence() const { return sequence_ + 1; }

  void publish(RuntimeControllerStateFrameV1 frame) {
    if (!data_) return;

    const uint64_t seq = ++sequence_;
    frame.magic = RUNTIME_CONTROLLER_STATE_MAGIC;
    frame.version = RUNTIME_CONTROLLER_STATE_FORMAT_VERSION;
    frame.size_bytes = sizeof(RuntimeControllerStateFrameV1);
    frame.sequence = seq;
    if (frame.timestamp_ns == 0) frame.timestamp_ns = static_cast<uint64_t>(runtime_controller_state_now_ns());

    const uint64_t committed_seq = seq * 2;
    const uint64_t writing_seq = committed_seq - 1;

    const uint32_t slot_idx = static_cast<uint32_t>((seq - 1) % cfg_.slot_count);
    uint8_t* slot_base = data_ + header_size_ + static_cast<size_t>(slot_idx) * slot_stride_;
    auto* slot = reinterpret_cast<RingSlotHeaderV1*>(slot_base);

    slot->seq_begin = writing_seq;
    slot->seq_end = writing_seq;
    slot->timestamp_ns = frame.timestamp_ns;
    slot->source_timestamp_ns = frame.timestamp_ns;
    slot->payload_size = sizeof(RuntimeControllerStateFrameV1);
    slot->flags = 0;

    std::memcpy(slot_base + slot_header_size_, &frame, sizeof(RuntimeControllerStateFrameV1));

    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);

    auto* h = reinterpret_cast<RingHeaderV1*>(data_);
    h->latest_sequence = seq;
  }

 private:
  static std::string normalize_posix_name(const std::string& name) {
    if (name.empty()) throw std::runtime_error("empty SHM name");
    if (name[0] == '/') return name;
    return "/" + name;
  }

  void write_metadata() {
    nlohmann::json meta = {
        {"stream_id", cfg_.stream_id},
        {"kind", "RUNTIME_CONTROLLER_STATE"},
        {"format_name", RUNTIME_CONTROLLER_STATE_FORMAT_NAME},
        {"format_version", RUNTIME_CONTROLLER_STATE_FORMAT_VERSION},
        {"frame_id", cfg_.frame_id},
        {"payload_schema", "RuntimeControllerStateV1"},
        {"payload_version", 1},
        {"coordinate_frame", cfg_.frame_id},
        {"units", {{"position", "m"}, {"linear_velocity", "m/s"}, {"angular_velocity", "rad/s"}}},
        {"quaternion_order", "xyzw"},
        {"timestamp_clock", "monotonic_ns"},
        {"producer", cfg_.created_by},
        {"consumer_contract", "SteamVR/OpenVR and Monado/OpenXR controller drivers read this stream"}};

    const std::string meta_s = meta.dump();
    const size_t meta_offset = sizeof(RingHeaderV1);
    const size_t max_meta = header_size_ > meta_offset ? header_size_ - meta_offset : 0;
    if (meta_s.size() < max_meta) {
      std::memcpy(data_ + meta_offset, meta_s.data(), meta_s.size());
    }
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
    j["version"] = 1;
    if (!j.contains("streams") || !j["streams"].is_object()) {
      j["streams"] = nlohmann::json::object();
    }

    j["streams"][cfg_.stream_id] = {
        {"shm_name", cfg_.shm_name},
        {"kind", "RUNTIME_CONTROLLER_STATE"},
        {"format_name", RUNTIME_CONTROLLER_STATE_FORMAT_NAME},
        {"format_version", RUNTIME_CONTROLLER_STATE_FORMAT_VERSION},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"header_size", header_size_},
        {"slot_header_size", slot_header_size_},
        {"slot_stride", slot_stride_},
        {"frame_id", cfg_.frame_id},
        {"payload_schema", "RuntimeControllerStateV1"},
        {"payload_version", 1},
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

  RuntimeControllerStateShmPublisherConfig cfg_;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t header_size_ = 4096;
  size_t slot_header_size_ = sizeof(RingSlotHeaderV1);
  size_t payload_size_ = sizeof(RuntimeControllerStateFrameV1);
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
};
#else
class RuntimeControllerStateShmPublisher {
 public:
  explicit RuntimeControllerStateShmPublisher(RuntimeControllerStateShmPublisherConfig cfg) {
    (void)cfg;
    throw std::runtime_error(
        "runtime controller state POSIX SHM publisher is not available in native Windows builds");
  }
  uint64_t next_sequence() const { return 1; }
  void publish(RuntimeControllerStateFrameV1) {}
};
#endif

}  // namespace xr_runtime
