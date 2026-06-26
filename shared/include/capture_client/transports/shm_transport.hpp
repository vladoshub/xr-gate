#pragma once

// capture_service POSIX shared-memory transport for native backends.
// Matches capture_service/shm_ring.py layout:
//   GLOBAL_HEADER(<8sIIIIIIQQ) + metadata + slots[SLOT_HEADER(<QQqQIIIIII32s)+payload]

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <capture_client/transports/transport.hpp>

namespace capture_client {

namespace detail {
inline uint64_t load_u64(const void* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline int64_t load_i64(const void* p) {
  int64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint32_t load_u32(const void* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
}  // namespace detail

class MappedShm {
 public:
  MappedShm() = default;
  MappedShm(const std::string& shm_name, size_t size) { open(shm_name, size); }
  ~MappedShm() { close(); }

  MappedShm(const MappedShm&) = delete;
  MappedShm& operator=(const MappedShm&) = delete;

  MappedShm(MappedShm&& other) noexcept { move_from(std::move(other)); }
  MappedShm& operator=(MappedShm&& other) noexcept {
    if (this != &other) {
      close();
      move_from(std::move(other));
    }
    return *this;
  }

  void open(const std::string& shm_name, size_t size) {
    close();
    std::string posix_name = shm_name;
    if (posix_name.empty() || posix_name[0] != '/') posix_name = "/" + posix_name;

    fd_ = ::shm_open(posix_name.c_str(), O_RDONLY, 0);
    if (fd_ < 0) {
      throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    }

    size_ = size;
    data_ = static_cast<uint8_t*>(::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      int err = errno;
      ::close(fd_);
      fd_ = -1;
      data_ = nullptr;
      size_ = 0;
      throw std::runtime_error("mmap failed for " + posix_name + ": " + std::strerror(err));
    }
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

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  void move_from(MappedShm&& other) noexcept {
    fd_ = other.fd_;
    data_ = other.data_;
    size_ = other.size_;
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
  }

  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

class ShmStreamReader final : public IStreamReader {
 public:
  explicit ShmStreamReader(StreamInfo info) : info_(std::move(info)) {
    if (info_.slot_stride == 0) info_.slot_stride = info_.slot_header_size + info_.payload_size;
    const size_t total_size = info_.header_size + info_.slot_count * info_.slot_stride;
    shm_.open(info_.shm_name, total_size);
    validate_header();
  }

  const StreamInfo& info() const override { return info_; }

  uint64_t latest_sequence() const override {
    // GLOBAL_HEADER size is 48 bytes; latest_seq is last uint64 at offset 40.
    return detail::load_u64(shm_.data() + 40);
  }

  bool read_latest(RawMessage& out) const override {
    const uint64_t seq = latest_sequence();
    if (seq == 0) return false;
    return read_sequence(seq, out);
  }

  bool read_sequence(uint64_t sequence, RawMessage& out) const override {
    if (sequence == 0) return false;
    const uint64_t latest = latest_sequence();
    if (latest == 0 || sequence > latest) return false;
    if (latest - sequence >= info_.slot_count) return false;

    const uint64_t slot_index = (sequence - 1) % info_.slot_count;
    const size_t slot_off = info_.header_size + slot_index * info_.slot_stride;
    const size_t payload_off = slot_off + info_.slot_header_size;
    const uint8_t* base = shm_.data();
    const uint8_t* h = base + slot_off;

    const uint64_t seq_begin = detail::load_u64(h + 0);
    const uint64_t seq_end = detail::load_u64(h + 8);
    const int64_t timestamp_ns = detail::load_i64(h + 16);
    const uint64_t monotonic_ns = detail::load_u64(h + 24);
    const uint32_t payload_len = detail::load_u32(h + 32);
    const uint32_t width = detail::load_u32(h + 36);
    const uint32_t height = detail::load_u32(h + 40);
    const uint32_t format_code = detail::load_u32(h + 44);
    const uint32_t flags = detail::load_u32(h + 48);
    const char* frame_id_raw = reinterpret_cast<const char*>(h + 60);

    if (seq_begin != seq_end || (seq_begin % 2) != 0 || seq_begin == 0) return false;
    const uint64_t actual_sequence = seq_end / 2;
    if (actual_sequence != sequence) return false;
    if (payload_len > info_.payload_size) return false;

    out.stream_id = info_.stream_id;
    out.sequence = actual_sequence;
    out.timestamp_ns = timestamp_ns;
    out.monotonic_ns = monotonic_ns;
    out.payload_size = payload_len;
    out.width = width;
    out.height = height;
    out.format_code = format_code;
    out.flags = flags;
    out.frame_id = std::string(frame_id_raw, strnlen(frame_id_raw, 32));
    out.payload.resize(payload_len);
    std::memcpy(out.payload.data(), base + payload_off, payload_len);

    // Re-read seq fields after payload copy; if producer touched the slot, reject.
    const uint64_t seq_begin2 = detail::load_u64(h + 0);
    const uint64_t seq_end2 = detail::load_u64(h + 8);
    if (seq_begin2 != seq_begin || seq_end2 != seq_end) return false;

    return true;
  }

 private:
  void validate_header() const {
    const uint8_t* d = shm_.data();
    const char expected[8] = {'C', 'A', 'P', 'S', 'H', 'M', '1', '\0'};
    if (std::memcmp(d, expected, 8) != 0) {
      throw std::runtime_error("bad SHM magic for " + info_.shm_name);
    }
    const uint32_t version = detail::load_u32(d + 8);
    if (version != 1) {
      throw std::runtime_error("unsupported SHM version for " + info_.shm_name);
    }
  }

  StreamInfo info_;
  MappedShm shm_;
};

class CaptureRegistry {
 public:
  explicit CaptureRegistry(const std::string& registry_path) {
    std::ifstream is(registry_path);
    if (!is) throw std::runtime_error("failed to open registry: " + registry_path);
    nlohmann::json j;
    is >> j;
    if (!j.contains("streams") || !j["streams"].is_object()) {
      throw std::runtime_error("registry has no object field: streams");
    }
    for (auto& kv : j["streams"].items()) {
      StreamInfo info;
      const auto& v = kv.value();
      info.stream_id = kv.key();
      info.shm_name = v.value("shm_name", "");
      info.kind = v.value("kind", "UNKNOWN");
      info.frame_id = v.value("frame_id", info.stream_id);
      info.width = v.value("width", 0);
      info.height = v.value("height", 0);
      info.format_code = v.value("format_code", 0);
      info.format_name = v.value("format_name", "UNKNOWN");
      info.payload_size = v.value("payload_size", 0);
      info.slot_count = v.value("slot_count", 0);
      info.slot_header_size = v.value("slot_header_size", 128);
      info.header_size = v.value("header_size", 4096);
      info.slot_stride = v.value("slot_stride", info.slot_header_size + info.payload_size);
      if (info.shm_name.empty() || info.slot_count == 0 || info.payload_size == 0) {
        throw std::runtime_error("bad stream info in registry for stream: " + info.stream_id);
      }
      streams_[info.stream_id] = std::move(info);
    }
  }

  const StreamInfo& stream(const std::string& stream_id) const {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) throw std::runtime_error("stream not found in registry: " + stream_id);
    return it->second;
  }

 private:
  std::unordered_map<std::string, StreamInfo> streams_;
};

class ShmCaptureTransport final : public ICaptureTransport {
 public:
  explicit ShmCaptureTransport(const std::string& registry_path,
                               const std::string& cam0_stream = "camera0",
                               const std::string& cam1_stream = "camera1",
                               const std::string& imu_stream = "imu0")
      : registry_(registry_path),
        cam0_(std::make_unique<ShmStreamReader>(registry_.stream(cam0_stream))),
        cam1_(std::make_unique<ShmStreamReader>(registry_.stream(cam1_stream))),
        imu_(std::make_unique<ShmStreamReader>(registry_.stream(imu_stream))) {}

  const std::string& type() const override {
    static const std::string kType = "shm";
    return kType;
  }

  IStreamReader& cam0() override { return *cam0_; }
  IStreamReader& cam1() override { return *cam1_; }
  IStreamReader& imu() override { return *imu_; }

 private:
  CaptureRegistry registry_;
  std::unique_ptr<ShmStreamReader> cam0_;
  std::unique_ptr<ShmStreamReader> cam1_;
  std::unique_ptr<ShmStreamReader> imu_;
};

// Backward-compatible alias for older local code/scripts.
using CaptureClientShm = ShmCaptureTransport;

}  // namespace capture_client
