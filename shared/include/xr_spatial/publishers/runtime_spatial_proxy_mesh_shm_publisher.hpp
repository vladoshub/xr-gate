#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>

namespace xr_spatial {

inline int64_t spatial_proxy_mesh_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct RuntimeSpatialProxyMeshShmPublisherConfig {
  std::string registry_path = "/tmp/runtime_tracking_streams.json";
  std::string stream_id = "spatial_proxy_mesh";
  std::string shm_name = "spatial_proxy_mesh";
  std::string frame_id = "tracking_world";
  uint32_t slot_count = 8;
  uint32_t header_size = 4096;
  uint32_t slot_header_size = 128;
  bool unlink_existing = false;
  std::string created_by = "xr_spatial_mapper_backend";
};

#ifndef _WIN32
class RuntimeSpatialProxyMeshShmPublisher {
 public:
  explicit RuntimeSpatialProxyMeshShmPublisher(RuntimeSpatialProxyMeshShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.stream_id.empty()) throw std::runtime_error("spatial proxy mesh stream_id is empty");
    if (cfg_.shm_name.empty()) throw std::runtime_error("spatial proxy mesh shm_name is empty");
    payload_size_ = sizeof(RuntimeSpatialProxyMeshF32V1);
    slot_stride_ = cfg_.slot_header_size + payload_size_;
    total_size_ = cfg_.header_size + static_cast<size_t>(cfg_.slot_count) * slot_stride_;
    open_or_create_shm();
    init_global_header();
    write_registry();
  }

  ~RuntimeSpatialProxyMeshShmPublisher() { close(); }
  RuntimeSpatialProxyMeshShmPublisher(const RuntimeSpatialProxyMeshShmPublisher&) = delete;
  RuntimeSpatialProxyMeshShmPublisher& operator=(const RuntimeSpatialProxyMeshShmPublisher&) = delete;

  void publish(RuntimeSpatialProxyMeshF32V1 mesh) {
    if (!data_) return;
    const uint64_t seq = ++sequence_;
    mesh.version = RUNTIME_SPATIAL_PROXY_MESH_FORMAT_VERSION;
    mesh.size_bytes = sizeof(RuntimeSpatialProxyMeshF32V1);
    if (mesh.sequence == 0) mesh.sequence = seq;
    if (mesh.timestamp_ns == 0) mesh.timestamp_ns = static_cast<uint64_t>(spatial_proxy_mesh_now_ns());

    const uint64_t seq_odd = seq * 2 - 1;
    const uint64_t seq_even = seq * 2;
    const uint64_t slot_index = (seq - 1) % cfg_.slot_count;
    uint8_t* slot = data_ + cfg_.header_size + slot_index * slot_stride_;
    uint8_t* payload = slot + cfg_.slot_header_size;

    auto store_u64 = [](uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); };
    auto store_i64 = [](uint8_t* p, int64_t v) { std::memcpy(p, &v, sizeof(v)); };
    auto store_u32 = [](uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); };

    store_u64(slot + 0, seq_odd);
    store_u64(slot + 8, seq_odd);
    std::atomic_thread_fence(std::memory_order_release);
    store_i64(slot + 16, static_cast<int64_t>(mesh.timestamp_ns));
    store_u64(slot + 24, static_cast<uint64_t>(spatial_proxy_mesh_now_ns()));
    store_u32(slot + 32, static_cast<uint32_t>(payload_size_));
    store_u32(slot + 36, 0);
    store_u32(slot + 40, 0);
    store_u32(slot + 44, 0);
    store_u32(slot + 48, mesh.status_flags);
    store_u32(slot + 52, 0);
    std::memset(slot + 60, 0, 32);
    const size_t frame_len = std::min<size_t>(cfg_.frame_id.size(), 31);
    std::memcpy(slot + 60, cfg_.frame_id.data(), frame_len);
    std::memcpy(payload, &mesh, sizeof(mesh));
    std::atomic_thread_fence(std::memory_order_release);
    store_u64(slot + 8, seq_even);
    store_u64(slot + 0, seq_even);
    // RingHeaderV1 is #pragma pack(push, 1); latest_sequence is not at a stable
    // hand-written +40 offset.  Use offsetof so readers see the correct sequence.
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(data_ + offsetof(xr_runtime::RingHeaderV1, latest_sequence), &seq, sizeof(seq));
  }

 private:
  void open_or_create_shm() {
    std::string posix_name = cfg_.shm_name;
    if (posix_name.empty() || posix_name[0] != '/') posix_name = "/" + posix_name;
    if (cfg_.unlink_existing) ::shm_unlink(posix_name.c_str());
    fd_ = ::shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    if (::ftruncate(fd_, static_cast<off_t>(total_size_)) != 0) {
      throw std::runtime_error("ftruncate failed for " + posix_name + ": " + std::strerror(errno));
    }
    data_ = static_cast<uint8_t*>(::mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      throw std::runtime_error("mmap failed for " + posix_name + ": " + std::strerror(errno));
    }
  }

  void init_global_header() {
    std::memset(data_, 0, total_size_);
    xr_runtime::RingHeaderV1 h{};
    const char magic[8] = {'S','P','M','S','H','M','1','\0'};
    std::memcpy(h.magic, magic, sizeof(magic));
    h.version = 1;
    h.header_size = cfg_.header_size;
    h.slot_count = cfg_.slot_count;
    h.slot_stride = static_cast<uint32_t>(slot_stride_);
    h.slot_header_size = cfg_.slot_header_size;
    h.payload_size = static_cast<uint32_t>(payload_size_);
    h.latest_sequence = 0;
    std::memcpy(data_, &h, sizeof(h));
  }

  void write_registry() {
    namespace fs = std::filesystem;
    fs::path path(cfg_.registry_path);
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    nlohmann::json reg = nlohmann::json::object();
    if (fs::exists(path)) {
      std::ifstream is(path);
      if (is) {
        try { is >> reg; } catch (...) { reg = nlohmann::json::object(); }
      }
    }
    if (!reg.contains("streams") || !reg["streams"].is_object()) reg["streams"] = nlohmann::json::object();
    reg["streams"][cfg_.stream_id] = {
        {"stream_id", cfg_.stream_id},
        {"shm_name", cfg_.shm_name},
        {"kind", "SPATIAL_PROXY_MESH"},
        {"frame_id", cfg_.frame_id},
        {"format_name", RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME},
        {"format_version", RUNTIME_SPATIAL_PROXY_MESH_FORMAT_VERSION},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"slot_header_size", cfg_.slot_header_size},
        {"header_size", cfg_.header_size},
        {"slot_stride", slot_stride_},
        {"payload_schema", "RuntimeSpatialProxyMeshF32V1"},
        {"coordinate_frame", cfg_.frame_id},
        {"position_units", "m"},
        {"created_by", cfg_.created_by},
        {"timestamp_clock", "monotonic_ns"}};

    const fs::path lock_path = path.string() + ".lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) throw std::runtime_error("failed to open spatial proxy registry lock: " + lock_path.string());
    struct Guard { int fd; ~Guard() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } } } guard{lock_fd};
    if (::flock(lock_fd, LOCK_EX) != 0) throw std::runtime_error("failed to lock spatial proxy registry");
    const auto tmp = path.string() + ".tmp." + std::to_string(::getpid());
    { std::ofstream os(tmp); if (!os) throw std::runtime_error("failed to write registry tmp"); os << reg.dump(2) << "\n"; }
    fs::rename(tmp, path);
  }

  void close() {
    if (data_) { ::munmap(data_, total_size_); data_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }

  RuntimeSpatialProxyMeshShmPublisherConfig cfg_;
  size_t payload_size_ = 0;
  size_t slot_stride_ = 0;
  size_t total_size_ = 0;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  uint64_t sequence_ = 0;
};
#else
class RuntimeSpatialProxyMeshShmPublisher {
 public:
  explicit RuntimeSpatialProxyMeshShmPublisher(RuntimeSpatialProxyMeshShmPublisherConfig) {
    throw std::runtime_error("spatial proxy mesh POSIX SHM publisher is unavailable on native Windows");
  }
  void publish(RuntimeSpatialProxyMeshF32V1) {}
};
#endif

}  // namespace xr_spatial
