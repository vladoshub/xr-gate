#include "xr_monado_driver/runtime_readers.hpp"

#ifndef _WIN32

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xr::monado_driver {
namespace {

std::string normalize_posix_shm_name(const std::string& name, const char* fallback) {
  if (name.empty()) return std::string("/") + fallback;
  if (name.front() == '/') return name;
  return "/" + name;
}

template <typename HeaderT, typename SlotT, typename PayloadT>
bool read_latest_ring_payload(const uint8_t* data,
                              size_t mapped_size,
                              PayloadT& out_payload,
                              uint64_t& out_sequence,
                              std::string* error) {
  auto* header = reinterpret_cast<const HeaderT*>(data);
  const uint64_t latest = header->latest_sequence;
  if (latest == 0) {
    if (error) *error = "ring has no committed frames yet";
    return false;
  }

  if (header->slot_count == 0 || header->slot_stride == 0 || header->header_size == 0) {
    if (error) *error = "ring header is invalid";
    return false;
  }

  const uint32_t slot_idx = static_cast<uint32_t>((latest - 1) % header->slot_count);
  const size_t slot_offset = static_cast<size_t>(header->header_size) +
                             static_cast<size_t>(slot_idx) * header->slot_stride;
  const size_t payload_offset = slot_offset + header->slot_header_size;
  if (payload_offset + sizeof(PayloadT) > mapped_size) {
    if (error) *error = "ring slot is outside mapped SHM size";
    return false;
  }

  const auto* slot = reinterpret_cast<const SlotT*>(data + slot_offset);
  PayloadT payload{};

  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint64_t begin = slot->seq_begin;
    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(&payload, data + payload_offset, sizeof(payload));
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t end = slot->seq_end;

    if (begin == end && (begin % 2u) == 0u && begin != 0 && payload.sequence * 2u == begin) {
      out_payload = payload;
      out_sequence = payload.sequence;
      return true;
    }
  }

  if (error) *error = "ring slot changed while reading";
  return false;
}

class MappedPosixShm {
 public:
  explicit MappedPosixShm(std::string shm_name) : shm_name_(std::move(shm_name)) {}
  ~MappedPosixShm() { close_mapping(); }

  bool ensure_mapping(std::string* error) {
    const int64_t now = monotonic_now_ns();
    if (data_ != nullptr) {
      // mmap keeps an unlinked object alive after the publisher recreates the
      // same name. Inspect the descriptor before accessing the mapped pages,
      // including when the old sample is still fresh or sequence restarts at 1.
      struct stat current {};
      if (fstat(fd_, &current) != 0 || current.st_nlink == 0 ||
          current.st_size <= 0 || static_cast<size_t>(current.st_size) != mapped_size_ ||
          now - last_progress_ns_ >= kReattachAfterStallNs) {
        close_mapping();
        next_open_attempt_ns_ = 0;
      } else {
        return true;
      }
    }

    // Missing/partially initialized streams must not cause shm_open/mmap work
    // at the Monado callback rate. No sleep or polling thread is needed.
    if (now < next_open_attempt_ns_) {
      if (error) *error = "waiting to retry SHM " + shm_name_;
      return false;
    }
    next_open_attempt_ns_ = now + kOpenRetryIntervalNs;

    fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0);
    if (fd_ < 0) {
      if (error) *error = "shm_open failed for " + shm_name_ + ": " + std::strerror(errno);
      return false;
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0 || st.st_nlink == 0 || st.st_size <= 0) {
      if (error) *error = "SHM is unavailable or not initialized: " + shm_name_;
      close_mapping();
      return false;
    }
    mapped_size_ = static_cast<size_t>(st.st_size);

    void* mapped = mmap(nullptr, mapped_size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
      if (error) *error = "mmap failed for " + shm_name_ + ": " + std::strerror(errno);
      data_ = nullptr;
      close_mapping();
      return false;
    }
    data_ = static_cast<const uint8_t*>(mapped);
    last_progress_ns_ = now;
    return true;
  }

  void observe_sequence(uint64_t sequence) {
    // Any change is progress: a producer restart may lower the sequence.
    if (sequence != last_sequence_) {
      last_sequence_ = sequence;
      last_progress_ns_ = monotonic_now_ns();
    }
  }

  template <typename HeaderT>
  bool validate_header_magic(const char (&expected)[8], uint32_t payload_size, uint32_t slot_header_size,
                             std::string* error) const {
    if (mapped_size_ < sizeof(HeaderT)) {
      if (error) *error = "SHM is smaller than ring header";
      return false;
    }
    const auto* h = reinterpret_cast<const HeaderT*>(data_);
    if (std::memcmp(h->magic, expected, sizeof(expected)) != 0) {
      if (error) *error = "SHM magic mismatch";
      return false;
    }
    if (h->payload_size != payload_size) {
      if (error) *error = "SHM payload size mismatch";
      return false;
    }
    if (h->slot_header_size != slot_header_size) {
      if (error) *error = "SHM slot header size mismatch";
      return false;
    }
    // A replacement may be visible before its header is fully initialized.
    // Check the entire ring layout without overflow before reading any slot.
    if (h->version != 1 || h->header_size < sizeof(HeaderT) ||
        h->header_size > mapped_size_ || h->slot_count == 0 ||
        static_cast<size_t>(h->slot_stride) < static_cast<size_t>(slot_header_size) + payload_size ||
        h->slot_count > (mapped_size_ - h->header_size) / h->slot_stride) {
      if (error) *error = "SHM ring layout is invalid";
      return false;
    }
    return true;
  }

  void close_mapping() {
    if (data_ != nullptr) {
      munmap(const_cast<uint8_t*>(data_), mapped_size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    mapped_size_ = 0;
    last_sequence_ = 0;
    last_progress_ns_ = 0;
  }

  const uint8_t* data() const { return data_; }
  size_t mapped_size() const { return mapped_size_; }

 private:
  static constexpr int64_t kOpenRetryIntervalNs = 100LL * 1000 * 1000;
  static constexpr int64_t kReattachAfterStallNs = 500LL * 1000 * 1000;
  std::string shm_name_;
  int fd_ = -1;
  const uint8_t* data_ = nullptr;
  size_t mapped_size_ = 0;
  uint64_t last_sequence_ = 0;
  int64_t last_progress_ns_ = 0;
  int64_t next_open_attempt_ns_ = 0;
};

class PosixRuntimePoseReader final : public IRuntimePoseReader {
 public:
  explicit PosixRuntimePoseReader(RuntimePoseReaderConfig cfg)
      : cfg_(std::move(cfg)), shm_(normalize_posix_shm_name(cfg_.shm_name, "runtime_hmd_pose")) {}

  bool read_latest(RuntimePoseSample& out, std::string* error) override {
    out = {};
    if (!shm_.ensure_mapping(error)) return false;
    const char expected[8] = {'R', 'T', 'P', 'O', 'S', 'E', '1', '\0'};
    if (!shm_.validate_header_magic<xr_runtime::RingHeaderV1>(
            expected,
            sizeof(xr_runtime::RuntimeHmdPoseF64V1),
            sizeof(xr_runtime::RingSlotHeaderV1),
            error)) {
      shm_.close_mapping();
      return false;
    }

    xr_runtime::RuntimeHmdPoseF64V1 pose{};
    uint64_t sequence = 0;
    if (!read_latest_ring_payload<xr_runtime::RingHeaderV1, xr_runtime::RingSlotHeaderV1>(
            shm_.data(), shm_.mapped_size(), pose, sequence, error)) {
      return false;
    }

    shm_.observe_sequence(sequence);
    out.pose = pose;
    out.read_time_ns = monotonic_now_ns();
    const int64_t pose_ts = pose.timestamp_ns != 0 ? static_cast<int64_t>(pose.timestamp_ns)
                                                   : static_cast<int64_t>(pose.target_timestamp_ns);
    out.age_ns = pose_ts > 0 ? out.read_time_ns - pose_ts : 0;
    return true;
  }

  const char* transport_name() const override { return "posix_shm"; }

 private:
  RuntimePoseReaderConfig cfg_;
  MappedPosixShm shm_;
};

class PosixRuntimeControllerStateReader final : public IRuntimeControllerStateReader {
 public:
  explicit PosixRuntimeControllerStateReader(RuntimeControllerStateReaderConfig cfg)
      : cfg_(std::move(cfg)), shm_(normalize_posix_shm_name(cfg_.shm_name, "runtime_controller_state")) {}

  bool read_latest(RuntimeControllerStateSample& out, std::string* error) override {
    out = {};
    if (!shm_.ensure_mapping(error)) return false;
    const char expected[8] = {'R', 'T', 'C', 'T', 'R', 'L', '1', '\0'};
    if (!shm_.validate_header_magic<xr_runtime::RingHeaderV1>(
            expected,
            sizeof(xr_runtime::RuntimeControllerStateFrameV1),
            sizeof(xr_runtime::RingSlotHeaderV1),
            error)) {
      shm_.close_mapping();
      return false;
    }

    xr_runtime::RuntimeControllerStateFrameV1 frame{};
    uint64_t sequence = 0;
    if (!read_latest_ring_payload<xr_runtime::RingHeaderV1, xr_runtime::RingSlotHeaderV1>(
            shm_.data(), shm_.mapped_size(), frame, sequence, error)) {
      return false;
    }

    shm_.observe_sequence(sequence);
    out.frame = frame;
    out.read_time_ns = monotonic_now_ns();
    const int64_t frame_ts = frame.timestamp_ns != 0 ? static_cast<int64_t>(frame.timestamp_ns)
                                                     : out.read_time_ns;
    out.age_ns = frame_ts > 0 ? out.read_time_ns - frame_ts : 0;
    return true;
  }

  const char* transport_name() const override { return "posix_shm"; }

 private:
  RuntimeControllerStateReaderConfig cfg_;
  MappedPosixShm shm_;
};

class PosixRuntimeHandReader final : public IRuntimeHandReader {
 public:
  explicit PosixRuntimeHandReader(RuntimeHandReaderConfig cfg)
      : cfg_(std::move(cfg)), shm_(normalize_posix_shm_name(cfg_.shm_name, "runtime_hand_tracking")) {}

  bool read_latest(RuntimeHandSample& out, std::string* error) override {
    out = {};
    if (!shm_.ensure_mapping(error)) return false;
    const char expected[8] = {'H', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
    if (!shm_.validate_header_magic<xr_tracking::TrackingRingHeaderV1>(
            expected,
            sizeof(xr_tracking::HandTrackingFrameF32V2),
            sizeof(xr_tracking::TrackingSlotHeaderV1),
            error)) {
      shm_.close_mapping();
      return false;
    }

    xr_tracking::HandTrackingFrameF32V2 frame{};
    uint64_t sequence = 0;
    if (!read_latest_ring_payload<xr_tracking::TrackingRingHeaderV1, xr_tracking::TrackingSlotHeaderV1>(
            shm_.data(), shm_.mapped_size(), frame, sequence, error)) {
      return false;
    }

    shm_.observe_sequence(sequence);
    out.frame = frame;
    out.read_time_ns = monotonic_now_ns();
    const int64_t frame_ts = frame.timestamp_ns != 0 ? static_cast<int64_t>(frame.timestamp_ns)
                                                     : static_cast<int64_t>(frame.source_timestamp_ns);
    out.age_ns = frame_ts > 0 ? out.read_time_ns - frame_ts : 0;
    return true;
  }

  const char* transport_name() const override { return "posix_shm"; }

 private:
  RuntimeHandReaderConfig cfg_;
  MappedPosixShm shm_;
};

}  // namespace

std::unique_ptr<IRuntimePoseReader> create_posix_runtime_pose_reader(const RuntimePoseReaderConfig& cfg) {
  return std::make_unique<PosixRuntimePoseReader>(cfg);
}

std::unique_ptr<IRuntimeControllerStateReader> create_posix_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& cfg) {
  return std::make_unique<PosixRuntimeControllerStateReader>(cfg);
}

std::unique_ptr<IRuntimeHandReader> create_posix_runtime_hand_reader(const RuntimeHandReaderConfig& cfg) {
  return std::make_unique<PosixRuntimeHandReader>(cfg);
}

}  // namespace xr::monado_driver

#endif  // !_WIN32
