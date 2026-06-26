#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <xr_video/contracts/stereo_video_contract.hpp>

namespace xr_video {

struct StereoVideoStreamInfo {
  std::string stream_id;
  std::string shm_name;
  std::string format_name;
  uint32_t format_version = 1;
  uint32_t payload_size = 0;
  uint32_t slot_count = 0;
  uint32_t header_size = 4096;
  uint32_t slot_header_size = 128;
  uint32_t slot_stride = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string frame_id = "camera_stereo";
};

inline StereoVideoStreamInfo stereo_video_stream_from_registry(const std::string& registry_path,
                                                               const std::string& stream_id) {
  std::ifstream in(registry_path);
  if (!in) throw std::runtime_error("failed to open XR video registry: " + registry_path);
  nlohmann::json j;
  in >> j;
  if (!j.contains("streams") || !j["streams"].contains(stream_id)) {
    throw std::runtime_error("video stream '" + stream_id + "' not found in registry " + registry_path);
  }
  const auto& s = j["streams"][stream_id];
  StereoVideoStreamInfo info;
  info.stream_id = stream_id;
  info.shm_name = s.value("shm_name", "");
  info.format_name = s.value("format_name", s.value("format", ""));
  info.format_version = s.value("format_version", 1);
  info.payload_size = s.value("payload_size", 0u);
  info.slot_count = s.value("slot_count", 0u);
  info.header_size = s.value("header_size", 4096u);
  info.slot_header_size = s.value("slot_header_size", 128u);
  info.slot_stride = s.value("slot_stride", info.slot_header_size + info.payload_size);
  info.width = s.value("width", 0u);
  info.height = s.value("height", 0u);
  info.frame_id = s.value("frame_id", "camera_stereo");
  if (info.shm_name.empty() || info.payload_size == 0 || info.slot_count == 0 || info.slot_stride == 0) {
    throw std::runtime_error("bad XR stereo video stream metadata in registry");
  }
  if (info.format_name != XR_STEREO_VIDEO_FORMAT_NAME_V1) {
    throw std::runtime_error("stream '" + stream_id + "' has format '" + info.format_name + "', expected XR_STEREO_VIDEO_V1");
  }
  return info;
}

#ifndef _WIN32
class StereoVideoShmReader {
 public:
  explicit StereoVideoShmReader(StereoVideoStreamInfo info) : info_(std::move(info)) {
    const std::string posix_name = normalize_posix_name(info_.shm_name);
    fd_ = ::shm_open(posix_name.c_str(), O_RDONLY, 0666);
    if (fd_ < 0) throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    size_ = static_cast<size_t>(info_.header_size) + static_cast<size_t>(info_.slot_count) * info_.slot_stride;
    data_ = static_cast<const uint8_t*>(::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      const std::string err = std::strerror(errno);
      data_ = nullptr;
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("mmap failed for " + posix_name + ": " + err);
    }
    validate_header();
  }

  ~StereoVideoShmReader() { close(); }

  StereoVideoShmReader(const StereoVideoShmReader&) = delete;
  StereoVideoShmReader& operator=(const StereoVideoShmReader&) = delete;

  uint64_t latest_sequence() const {
    const auto* h = reinterpret_cast<const StereoVideoRingHeaderV1*>(data_);
    return h->latest_sequence;
  }

  std::optional<StereoVideoFrame> latest() const {
    const uint64_t seq = latest_sequence();
    if (seq == 0) return std::nullopt;
    return read_sequence(seq);
  }

  std::optional<StereoVideoFrame> read_sequence(uint64_t seq) const {
    if (seq == 0) return std::nullopt;
    const uint64_t latest = latest_sequence();
    if (latest == 0 || seq > latest || latest - seq >= info_.slot_count) return std::nullopt;

    const uint64_t slot_index = (seq - 1) % info_.slot_count;
    const size_t slot_off = static_cast<size_t>(info_.header_size) + slot_index * info_.slot_stride;
    const uint8_t* slot_base = data_ + slot_off;
    const auto* slot = reinterpret_cast<const StereoVideoSlotHeaderV1*>(slot_base);

    const uint64_t seq_begin = slot->seq_begin;
    const uint64_t seq_end = slot->seq_end;
    if (seq_begin != seq_end || (seq_begin % 2) != 0 || seq_begin == 0) return std::nullopt;
    const uint64_t actual_sequence = seq_end / 2;
    if (actual_sequence != seq) return std::nullopt;
    if (slot->payload_size == 0 || slot->payload_size > info_.payload_size) return std::nullopt;

    std::vector<uint8_t> payload(slot->payload_size);
    std::memcpy(payload.data(), slot_base + info_.slot_header_size, payload.size());

    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot->seq_begin != seq_begin || slot->seq_end != seq_end) return std::nullopt;

    return read_frame_payload(payload.data(), payload.size());
  }

  const StereoVideoStreamInfo& info() const { return info_; }

 private:
  static std::string normalize_posix_name(const std::string& name) {
    if (name.empty()) throw std::runtime_error("empty SHM name");
    if (name[0] == '/') return name;
    return "/" + name;
  }

  void validate_header() const {
    const auto* h = reinterpret_cast<const StereoVideoRingHeaderV1*>(data_);
    const char expected[8] = {'X', 'S', 'V', 'S', 'H', 'M', '1', '\0'};
    if (std::memcmp(h->magic, expected, 8) != 0) throw std::runtime_error("bad XR stereo video SHM magic");
    if (h->version != 1) throw std::runtime_error("unsupported XR stereo video SHM version");
  }

  void close() {
    if (data_) {
      ::munmap(const_cast<uint8_t*>(data_), size_);
      data_ = nullptr;
      size_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  StereoVideoStreamInfo info_;
  int fd_ = -1;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};
#else
class StereoVideoShmReader {
 public:
  explicit StereoVideoShmReader(StereoVideoStreamInfo) {
    throw std::runtime_error("XR stereo video POSIX SHM reader is not available on native Windows; use --spatial/video TCP or UDP input");
  }
  std::optional<StereoVideoFrame> latest() { return std::nullopt; }
};
#endif

}  // namespace xr_video
