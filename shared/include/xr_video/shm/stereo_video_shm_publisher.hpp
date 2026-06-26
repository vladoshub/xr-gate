#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

#include <xr_video/contracts/stereo_video_contract.hpp>

namespace xr_video {

struct StereoVideoShmPublisherConfig {
  std::string registry_path = "/tmp/xr_video_streams.json";
  std::string stream_id = "stereo_video";
  std::string shm_name = "/xr_stereo_video_v1";
  std::string frame_id = "camera_stereo";
  uint32_t slot_count = 8;
  uint32_t header_size = 4096;
  uint32_t slot_header_size = sizeof(StereoVideoSlotHeaderV1);
  uint32_t width = 640;
  uint32_t height = 480;
  StereoVideoPixelFormat pixel_format = StereoVideoPixelFormat::Gray8;
  bool unlink_existing = true;
  std::string created_by = "xr_video_backend";
};

#ifndef _WIN32
class StereoVideoShmPublisher {
 public:
  explicit StereoVideoShmPublisher(StereoVideoShmPublisherConfig cfg)
      : cfg_(std::move(cfg)) {
    if (cfg_.slot_count == 0) throw std::runtime_error("video slot_count must be > 0");
    if (cfg_.stream_id.empty()) throw std::runtime_error("video stream_id is empty");
    if (cfg_.shm_name.empty()) throw std::runtime_error("video shm_name is empty");

    payload_size_ = stereo_video_payload_size_for(cfg_.width, cfg_.height, cfg_.pixel_format);
    slot_stride_ = static_cast<size_t>(cfg_.slot_header_size) + payload_size_;
    size_ = static_cast<size_t>(cfg_.header_size) + static_cast<size_t>(cfg_.slot_count) * slot_stride_;

    const std::string posix_name = normalize_posix_name(cfg_.shm_name);
    if (cfg_.unlink_existing) {
      ::shm_unlink(posix_name.c_str());
    }

    fd_ = ::shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    }
    if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
      const std::string err = std::strerror(errno);
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("ftruncate failed for " + posix_name + ": " + err);
    }

    data_ = static_cast<uint8_t*>(::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      const std::string err = std::strerror(errno);
      data_ = nullptr;
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("mmap failed for " + posix_name + ": " + err);
    }

    std::memset(data_, 0, size_);
    auto* h = reinterpret_cast<StereoVideoRingHeaderV1*>(data_);
    *h = StereoVideoRingHeaderV1{};
    h->header_size = cfg_.header_size;
    h->slot_count = cfg_.slot_count;
    h->slot_stride = static_cast<uint32_t>(slot_stride_);
    h->slot_header_size = cfg_.slot_header_size;
    h->payload_size = static_cast<uint32_t>(payload_size_);

    update_registry();
  }

  ~StereoVideoShmPublisher() { close(); }

  StereoVideoShmPublisher(const StereoVideoShmPublisher&) = delete;
  StereoVideoShmPublisher& operator=(const StereoVideoShmPublisher&) = delete;

  void publish(StereoVideoFrame frame) {
    if (!data_) return;

    validate_frame(frame, payload_size_);
    if (frame.header.width != cfg_.width || frame.header.height != cfg_.height) {
      throw std::runtime_error("XR stereo video frame dimensions changed after SHM publisher init");
    }

    const uint64_t seq = ++sequence_;
    frame.header.sequence = seq;
    if (frame.header.timestamp_ns == 0) frame.header.timestamp_ns = monotonic_now_ns();
    if (frame.header.publish_timestamp_ns == 0) frame.header.publish_timestamp_ns = monotonic_now_ns();
    if (frame.header.source_timestamp_ns == 0) frame.header.source_timestamp_ns = frame.header.timestamp_ns;

    const uint64_t committed_seq = seq * 2;
    const uint64_t writing_seq = committed_seq - 1;

    const uint32_t slot_idx = static_cast<uint32_t>((seq - 1) % cfg_.slot_count);
    uint8_t* slot_base = data_ + cfg_.header_size + static_cast<size_t>(slot_idx) * slot_stride_;
    auto* slot = reinterpret_cast<StereoVideoSlotHeaderV1*>(slot_base);

    slot->seq_begin = writing_seq;
    slot->seq_end = writing_seq;
    slot->timestamp_ns = frame.header.timestamp_ns;
    slot->source_timestamp_ns = frame.header.source_timestamp_ns;
    slot->payload_size = static_cast<uint32_t>(sizeof(StereoVideoFrameHeaderV1) + frame.left.size() + frame.right.size());
    slot->width = frame.header.width;
    slot->height = frame.header.height;
    slot->format_code = XR_STEREO_VIDEO_FORMAT_CODE_V1;
    slot->flags = frame.header.flags;

    write_frame_payload(slot_base + cfg_.slot_header_size, frame);

    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = committed_seq;
    std::atomic_thread_fence(std::memory_order_release);

    auto* h = reinterpret_cast<StereoVideoRingHeaderV1*>(data_);
    h->latest_sequence = seq;
  }

  const StereoVideoShmPublisherConfig& config() const { return cfg_; }

 private:
  static std::string normalize_posix_name(const std::string& name) {
    if (name.empty()) throw std::runtime_error("empty SHM name");
    if (name[0] == '/') return name;
    return "/" + name;
  }

  void close() {
    if (data_) {
      ::munmap(data_, size_);
      data_ = nullptr;
      size_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void update_registry() const {
    namespace fs = std::filesystem;
    const fs::path registry_path(cfg_.registry_path);
    const fs::path parent = registry_path.parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    const fs::path lock_path = registry_path.string() + ".lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) throw std::runtime_error("failed to open video registry lock: " + lock_path.string());
    if (::flock(lock_fd, LOCK_EX) != 0) {
      ::close(lock_fd);
      throw std::runtime_error("failed to lock video registry: " + lock_path.string());
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
    j["created_by"] = cfg_.created_by;
    if (!j.contains("streams") || !j["streams"].is_object()) j["streams"] = nlohmann::json::object();

    j["streams"][cfg_.stream_id] = {
        {"shm_name", cfg_.shm_name},
        {"kind", "STEREO_VIDEO"},
        {"frame_id", cfg_.frame_id},
        {"format_name", XR_STEREO_VIDEO_FORMAT_NAME_V1},
        {"format_version", XR_STEREO_VIDEO_FORMAT_VERSION_V1},
        {"format_code", XR_STEREO_VIDEO_FORMAT_CODE_V1},
        {"payload_size", payload_size_},
        {"slot_count", cfg_.slot_count},
        {"header_size", cfg_.header_size},
        {"slot_header_size", cfg_.slot_header_size},
        {"slot_stride", slot_stride_},
        {"width", cfg_.width},
        {"height", cfg_.height},
        {"pixel_format", "GRAY8"},
        {"layout", "separate_left_right"},
        {"timestamp_clock", "monotonic_ns"},
    };

    const fs::path tmp_path = registry_path.string() + ".tmp." + std::to_string(::getpid());
    {
      std::ofstream out(tmp_path);
      out << j.dump(2) << "\n";
    }
    fs::rename(tmp_path, registry_path);

    ::flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
  }

  StereoVideoShmPublisherConfig cfg_;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t payload_size_ = 0;
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
};
#else
class StereoVideoShmPublisher {
 public:
  explicit StereoVideoShmPublisher(StereoVideoShmPublisherConfig) {
    throw std::runtime_error("XR stereo video POSIX SHM publisher is not available on native Windows; use --output tcp");
  }
  void publish(StereoVideoFrame) {}
};
#endif

}  // namespace xr_video
