#include "runtime_body_tracker_reader.hpp"

#ifndef _WIN32
#include <xr_tracking/readers/posix_tracking_ring_reader.hpp>
#endif

namespace xr::openvr_driver {

#ifndef _WIN32
namespace {

class PosixBodyTrackerReader final : public IRuntimeBodyTrackerReader {
 public:
  explicit PosixBodyTrackerReader(const std::string& name)
      : shm_(xr_tracking::normalize_body_tracker_posix_shm_name(name)) {}

  bool read_latest(RuntimeBodyTrackerSample& out, std::string* error) override {
    using namespace xr_tracking;
    if (!shm_.ensure_mapping(error)) return false;
    constexpr char magic[8] = {'B', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
    if (!shm_.validate_header_magic<BodyTrackerSetRingHeaderV1>(
            magic, sizeof(out.frame), sizeof(BodyTrackerSetRingSlotHeaderV1), error)) {
      shm_.close_mapping();
      return false;
    }

    uint64_t sequence = 0;
    if (!read_latest_ring_payload<BodyTrackerSetRingHeaderV1, BodyTrackerSetRingSlotHeaderV1>(
            shm_.data(), shm_.mapped_size(), out.frame, sequence, error)) {
      return false;
    }
    const auto& frame = out.frame;
    const auto now = static_cast<uint64_t>(posix_tracking_now_ns());
    if (frame.version != BODY_TRACKER_SET_FORMAT_VERSION || frame.size_bytes != sizeof(frame) ||
        frame.tracker_count > BODY_TRACKER_MAX_TRACKERS ||
        frame.timestamp_ns == 0 || frame.timestamp_ns > now) {
      if (error) *error = "invalid runtime body tracker frame";
      return false;
    }
    shm_.observe_sequence(sequence);
    out.age_ns = static_cast<int64_t>(now - frame.timestamp_ns);
    return true;
  }

 private:
  xr_tracking::MappedPosixShm shm_;
};

}  // namespace
#endif

std::unique_ptr<IRuntimeBodyTrackerReader> create_runtime_body_tracker_reader(
    const RuntimeBodyTrackerReaderConfig& cfg) {
#ifdef _WIN32
  return create_tcp_runtime_body_tracker_reader(cfg.tcp);
#else
  return std::make_unique<PosixBodyTrackerReader>(cfg.shm_name);
#endif
}

}  // namespace xr::openvr_driver
