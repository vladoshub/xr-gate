#include "runtime_pose_reader.hpp"

#ifndef _WIN32

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xr_tracking/types/tracking_types.hpp>

namespace xr::openvr_driver {
namespace {

std::string normalize_posix_shm_name(const std::string& name) {
  if (name.empty()) return "/runtime_hmd_pose";
  if (name.front() == '/') return name;
  return "/" + name;
}

class PosixRuntimePoseReader final : public IRuntimePoseReader {
 public:
  explicit PosixRuntimePoseReader(RuntimePoseReaderConfig cfg)
      : cfg_(std::move(cfg)), shm_name_(normalize_posix_shm_name(cfg_.shm_name)) {}

  ~PosixRuntimePoseReader() override { close_mapping(); }

  bool read_latest(RuntimePoseSample& out, std::string* error) override {
    if (!ensure_mapping(error)) return false;

    auto* header = reinterpret_cast<const xr_runtime::RingHeaderV1*>(data_);
    const uint64_t latest = header->latest_sequence;
    if (latest == 0) {
      if (error) *error = "runtime pose ring has no committed frames yet";
      return false;
    }

    if (header->slot_count == 0 || header->slot_stride == 0 || header->header_size == 0) {
      if (error) *error = "runtime pose ring header is invalid";
      return false;
    }

    const uint32_t slot_idx = static_cast<uint32_t>((latest - 1) % header->slot_count);
    const size_t slot_offset = static_cast<size_t>(header->header_size) +
                               static_cast<size_t>(slot_idx) * header->slot_stride;
    const size_t payload_offset = slot_offset + header->slot_header_size;
    if (payload_offset + sizeof(xr_runtime::RuntimeHmdPoseF64V1) > mapped_size_) {
      if (error) *error = "runtime pose slot is outside mapped SHM size";
      return false;
    }

    const auto* slot = reinterpret_cast<const xr_runtime::RingSlotHeaderV1*>(data_ + slot_offset);
    xr_runtime::RuntimeHmdPoseF64V1 pose{};

    for (int attempt = 0; attempt < 3; ++attempt) {
      const uint64_t begin = slot->seq_begin;
      std::atomic_thread_fence(std::memory_order_acquire);
      std::memcpy(&pose, data_ + payload_offset, sizeof(pose));
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint64_t end = slot->seq_end;

      if (begin == end && (begin % 2u) == 0u && begin != 0 && pose.sequence * 2u == begin) {
        out.pose = pose;
        out.read_time_ns = monotonic_now_ns();
        const int64_t pose_ts = pose.timestamp_ns != 0 ? static_cast<int64_t>(pose.timestamp_ns)
                                                       : static_cast<int64_t>(pose.target_timestamp_ns);
        out.age_ns = pose_ts > 0 ? out.read_time_ns - pose_ts : 0;
        return true;
      }
    }

    if (error) *error = "runtime pose slot changed while reading";
    return false;
  }

  const char* transport_name() const override { return "posix_shm"; }

 private:
  bool ensure_mapping(std::string* error) {
    if (data_ != nullptr) return true;

    fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0);
    if (fd_ < 0) {
      if (error) *error = "shm_open failed for " + shm_name_ + ": " + std::strerror(errno);
      return false;
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0 || st.st_size <= 0) {
      if (error) *error = "fstat failed for " + shm_name_ + ": " + std::strerror(errno);
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

    if (!validate_header(error)) {
      close_mapping();
      return false;
    }
    return true;
  }

  bool validate_header(std::string* error) const {
    if (mapped_size_ < sizeof(xr_runtime::RingHeaderV1)) {
      if (error) *error = "runtime pose SHM is smaller than ring header";
      return false;
    }
    const auto* h = reinterpret_cast<const xr_runtime::RingHeaderV1*>(data_);
    const char expected[8] = {'R', 'T', 'P', 'O', 'S', 'E', '1', '\0'};
    if (std::memcmp(h->magic, expected, sizeof(expected)) != 0) {
      if (error) *error = "runtime pose SHM magic mismatch";
      return false;
    }
    if (h->payload_size != sizeof(xr_runtime::RuntimeHmdPoseF64V1)) {
      if (error) *error = "runtime pose payload size mismatch";
      return false;
    }
    if (h->slot_header_size != sizeof(xr_runtime::RingSlotHeaderV1)) {
      if (error) *error = "runtime pose slot header size mismatch";
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
  }

  RuntimePoseReaderConfig cfg_;
  std::string shm_name_;
  int fd_ = -1;
  const uint8_t* data_ = nullptr;
  size_t mapped_size_ = 0;
};

}  // namespace

std::unique_ptr<IRuntimePoseReader> create_posix_runtime_pose_reader(const RuntimePoseReaderConfig& cfg) {
  return std::make_unique<PosixRuntimePoseReader>(cfg);
}

}  // namespace xr::openvr_driver

#endif  // !_WIN32
