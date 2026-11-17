#include "xr_monado_driver/runtime_readers.hpp"

#ifndef _WIN32

#include <xr_tracking/readers/posix_tracking_ring_reader.hpp>

#include <memory>
#include <string>
#include <utility>

namespace xr::monado_driver {
namespace {

std::string normalize_posix_shm_name(const std::string& name, const char* fallback) {
  if (name.empty()) return std::string("/") + fallback;
  if (name.front() == '/') return name;
  return "/" + name;
}

using xr_tracking::MappedPosixShm;
using xr_tracking::read_latest_ring_payload;

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

class PosixBodyTrackerReader final : public IRuntimeBodyTrackerReader {
 public:
  explicit PosixBodyTrackerReader(const std::string& shm_name)
      : shm_(normalize_posix_shm_name(shm_name, "runtime_body_trackers")) {}

  bool read_latest(RuntimeBodyTrackerSample& out, std::string* error) override {
    using namespace xr_tracking;
    out = {};
    if (!shm_.ensure_mapping(error)) return false;
    const char expected[8] = {'B', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
    if (!shm_.validate_header_magic<BodyTrackerSetRingHeaderV1>(
            expected, sizeof(BodyTrackerSetFrameF32V1), sizeof(BodyTrackerSetRingSlotHeaderV1), error)) {
      shm_.close_mapping();
      return false;
    }

    BodyTrackerSetFrameF32V1 frame{};
    uint64_t sequence = 0;
    if (!read_latest_ring_payload<BodyTrackerSetRingHeaderV1, BodyTrackerSetRingSlotHeaderV1>(
            shm_.data(), shm_.mapped_size(), frame, sequence, error)) {
      return false;
    }
    if (frame.version != BODY_TRACKER_SET_FORMAT_VERSION || frame.size_bytes != sizeof(frame) ||
        frame.tracker_count > BODY_TRACKER_MAX_TRACKERS) {
      if (error) *error = "invalid runtime body tracker frame";
      return false;
    }
    shm_.observe_sequence(sequence);
    const auto now_ns = static_cast<uint64_t>(monotonic_now_ns());
    if (frame.timestamp_ns == 0 || frame.timestamp_ns > now_ns) {
      if (error) *error = "invalid runtime body tracker timestamp";
      return false;
    }
    out.frame = frame;
    out.age_ns = static_cast<int64_t>(now_ns - frame.timestamp_ns);
    return true;
  }

  const char* transport_name() const override { return "posix_shm"; }

 private:
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

std::unique_ptr<IRuntimeBodyTrackerReader> create_posix_body_tracker_reader(const std::string& shm_name) {
  return std::make_unique<PosixBodyTrackerReader>(shm_name);
}

}  // namespace xr::monado_driver

#endif  // !_WIN32
