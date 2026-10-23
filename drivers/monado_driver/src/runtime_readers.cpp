#include "xr_monado_driver/runtime_readers.hpp"
#include "runtime_tcp_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

namespace xr::monado_driver {

std::unique_ptr<IRuntimePoseReader>
create_posix_runtime_pose_reader(const RuntimePoseReaderConfig &cfg);
std::unique_ptr<IRuntimeControllerStateReader>
create_posix_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig &cfg);
std::unique_ptr<IRuntimeHandReader>
create_posix_runtime_hand_reader(const RuntimeHandReaderConfig &cfg);

std::unique_ptr<IRuntimePoseReader>
create_windows_runtime_pose_reader(const RuntimePoseReaderConfig &cfg);
std::unique_ptr<IRuntimeControllerStateReader>
create_windows_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig &cfg);
std::unique_ptr<IRuntimeHandReader>
create_windows_runtime_hand_reader(const RuntimeHandReaderConfig &cfg);

namespace {

std::string lower_ascii(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return v;
}

bool fresh_by_age_ns(int64_t age_ns, uint32_t max_age_ms) {
  if (max_age_ms == 0)
    return true;
  if (age_ns < 0)
    return true; // Different monotonic clock domain.
  const int64_t max_age_ns = static_cast<int64_t>(max_age_ms) * 1000LL * 1000LL;
  return age_ns <= max_age_ns;
}

} // namespace

int64_t monotonic_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool runtime_pose_is_valid(const xr_runtime::RuntimeHmdPoseF64V1 &pose) {
  if (pose.version != xr_runtime::RUNTIME_HMD_POSE_FORMAT_VERSION)
    return false;
  if (pose.size_bytes != sizeof(xr_runtime::RuntimeHmdPoseF64V1))
    return false;
  if ((pose.flags & xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID) == 0u)
    return false;
  if (pose.tracking_status != 2u)
    return false;
  if (!std::isfinite(pose.confidence) || pose.confidence <= 0.0f)
    return false;
  if (!std::isfinite(pose.px) || !std::isfinite(pose.py) ||
      !std::isfinite(pose.pz))
    return false;
  const double q2 = pose.qx * pose.qx + pose.qy * pose.qy + pose.qz * pose.qz +
                    pose.qw * pose.qw;
  if (!std::isfinite(q2) || q2 < 1e-12)
    return false;
  return true;
}

bool runtime_pose_sample_is_fresh(const RuntimePoseSample &sample,
                                  uint32_t max_age_ms) {
  return fresh_by_age_ns(sample.age_ns, max_age_ms);
}

bool runtime_controller_state_frame_is_valid(
    const xr_runtime::RuntimeControllerStateFrameV1 &frame) {
  if (frame.magic != xr_runtime::RUNTIME_CONTROLLER_STATE_MAGIC)
    return false;
  if (frame.version != xr_runtime::RUNTIME_CONTROLLER_STATE_FORMAT_VERSION)
    return false;
  if (frame.size_bytes != sizeof(xr_runtime::RuntimeControllerStateFrameV1))
    return false;
  return true;
}

bool runtime_controller_state_sample_is_fresh(
    const RuntimeControllerStateSample &sample, uint32_t max_age_ms) {
  return fresh_by_age_ns(sample.age_ns, max_age_ms);
}

bool runtime_controller_side_has_pose(
    const xr_runtime::RuntimeControllerSideStateV1 &side, bool left) {
  const uint32_t expected_role = left ? xr_runtime::CONTROLLER_SIDE_LEFT
                                      : xr_runtime::CONTROLLER_SIDE_RIGHT;
  if (side.role != expected_role)
    return false;
  if ((side.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) == 0u)
    return false;
  if ((side.flags & xr_runtime::RUNTIME_CONTROLLER_CONNECTED) == 0u)
    return false;
  return true;
}

bool runtime_hand_frame_is_valid(
    const xr_tracking::HandTrackingFrameF32V2 &frame) {
  if (frame.version != xr_tracking::HAND_TRACKING_FORMAT_VERSION_V2)
    return false;
  if (frame.size_bytes != sizeof(xr_tracking::HandTrackingFrameF32V2))
    return false;
  return true;
}

bool runtime_hand_sample_is_fresh(const RuntimeHandSample &sample,
                                  uint32_t max_age_ms) {
  return fresh_by_age_ns(sample.age_ns, max_age_ms);
}

bool runtime_hand_side_is_valid(
    const xr_tracking::HandTrackingFrameF32V2 &frame,
    const xr_tracking::HandSideF32V2 &side, bool left) {
  if (!runtime_hand_frame_is_valid(frame))
    return false;

  const uint32_t expected_handedness = left ? 1u : 2u;
  const uint32_t frame_side_flag = left ? xr_tracking::HAND_FLAG_LEFT_VALID
                                        : xr_tracking::HAND_FLAG_RIGHT_VALID;

  if (side.handedness != expected_handedness)
    return false;
  if ((frame.flags & frame_side_flag) == 0u)
    return false;
  if ((side.flags & xr_tracking::HAND_POSE_VALID) == 0u)
    return false;
  if (side.confidence <= 0.0f)
    return false;

  const uint32_t tracking =
      static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Tracking);
  const uint32_t degraded =
      static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Degraded);
  if (side.status != tracking && side.status != degraded)
    return false;

  return true;
}

namespace {
template <class LocalBase, class LocalSample, class RemoteBase,
          class RemoteSample>
class TcpReaderAdapter final : public LocalBase {
public:
  explicit TcpReaderAdapter(std::unique_ptr<RemoteBase> reader)
      : reader_(std::move(reader)) {}
  bool read_latest(LocalSample &out, std::string *error) override {
    RemoteSample sample{};
    if (!reader_->read_latest(sample, error)) {
      out = {};
      return false;
    }
    copy(out, sample);
    return true;
  }
  const char *transport_name() const override { return "app_tcp"; }

private:
  static void copy(RuntimePoseSample &d,
                   const xr::openvr_driver::RuntimePoseSample &s) {
    d.pose = s.pose;
    d.read_time_ns = s.read_time_ns;
    d.age_ns = s.age_ns;
  }
  static void copy(RuntimeHandSample &d,
                   const xr::openvr_driver::RuntimeHandSample &s) {
    d.frame = s.frame;
    d.read_time_ns = s.read_time_ns;
    d.age_ns = s.age_ns;
    // Monado interprets native-hand and input timestamps in the CLIENT clock.
    // Rebase by age/deltas within the server domain, never by comparing clocks.
    d.frame.source_timestamp_ns =
        local_stamp(s.frame.source_timestamp_ns, s.frame.timestamp_ns,
                    s.read_time_ns, s.age_ns);
    d.frame.timestamp_ns = local_stamp(
        s.frame.timestamp_ns, s.frame.timestamp_ns, s.read_time_ns, s.age_ns);
  }
  static void copy(RuntimeControllerStateSample &d,
                   const xr::openvr_driver::RuntimeControllerStateSample &s) {
    d.frame = s.frame;
    d.read_time_ns = s.read_time_ns;
    d.age_ns = s.age_ns;
    for (auto *side : {&d.frame.left, &d.frame.right}) {
      side->last_input_ns = local_stamp(
          side->last_input_ns, s.frame.timestamp_ns, s.read_time_ns, s.age_ns);
      side->last_pose_ns = local_stamp(side->last_pose_ns, s.frame.timestamp_ns,
                                       s.read_time_ns, s.age_ns);
    }
    d.frame.timestamp_ns = local_stamp(
        s.frame.timestamp_ns, s.frame.timestamp_ns, s.read_time_ns, s.age_ns);
  }
  static uint64_t local_stamp(uint64_t stamp, uint64_t published, int64_t now,
                              int64_t initial_age) {
    if (!stamp)
      return 0;
    const uint64_t delta = published > stamp ? published - stamp : 0;
    const uint64_t age =
        static_cast<uint64_t>(std::max(int64_t{0}, initial_age));
    if (age >= uint64_t(now) || delta >= uint64_t(now) - age)
      return 1;
    return uint64_t(now) - age - delta;
  }
  std::unique_ptr<RemoteBase> reader_;
};
std::unique_ptr<IRuntimePoseReader>
create_tcp_pose_reader(const RuntimePoseReaderConfig &cfg) {
  xr::openvr_driver::RuntimePoseReaderConfig remote;
  remote.tcp = cfg.tcp;
  remote.max_age_ms = cfg.max_age_ms;
  return std::make_unique<
      TcpReaderAdapter<IRuntimePoseReader, RuntimePoseSample,
                       xr::openvr_driver::IRuntimePoseReader,
                       xr::openvr_driver::RuntimePoseSample>>(
      xr::openvr_driver::create_tcp_runtime_pose_reader(remote));
}
std::unique_ptr<IRuntimeControllerStateReader>
create_tcp_controller_state_reader(
    const RuntimeControllerStateReaderConfig &cfg) {
  xr::openvr_driver::RuntimeControllerStateReaderConfig remote;
  remote.tcp = cfg.tcp;
  remote.max_age_ms = cfg.max_age_ms;
  return std::make_unique<TcpReaderAdapter<
      IRuntimeControllerStateReader, RuntimeControllerStateSample,
      xr::openvr_driver::IRuntimeControllerStateReader,
      xr::openvr_driver::RuntimeControllerStateSample>>(
      xr::openvr_driver::create_tcp_runtime_controller_state_reader(remote));
}
std::unique_ptr<IRuntimeHandReader>
create_tcp_hand_reader(const RuntimeHandReaderConfig &cfg) {
  xr::openvr_driver::RuntimeHandReaderConfig remote;
  remote.tcp = cfg.tcp;
  remote.max_age_ms = cfg.max_age_ms;
  return std::make_unique<
      TcpReaderAdapter<IRuntimeHandReader, RuntimeHandSample,
                       xr::openvr_driver::IRuntimeHandReader,
                       xr::openvr_driver::RuntimeHandSample>>(
      xr::openvr_driver::create_tcp_runtime_hand_reader(remote));
}
} // namespace

std::unique_ptr<IRuntimePoseReader>
create_runtime_pose_reader(const RuntimePoseReaderConfig &cfg) {
  const std::string transport =
      lower_ascii(cfg.transport.empty() ? "auto" : cfg.transport);
  if (transport == "tcp" || transport == "app_tcp")
    return create_tcp_pose_reader(cfg);
#if defined(_WIN32)
  if (transport == "auto" || transport == "windows_shm" ||
      transport == "named_shm") {
    return create_windows_runtime_pose_reader(cfg);
  }
#else
  if (transport == "auto" || transport == "posix_shm" || transport == "shm") {
    return create_posix_runtime_pose_reader(cfg);
  }
#endif
  return nullptr;
}

std::unique_ptr<IRuntimeControllerStateReader>
create_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig &cfg) {
  const std::string transport =
      lower_ascii(cfg.transport.empty() ? "auto" : cfg.transport);
  if (transport == "tcp" || transport == "app_tcp")
    return create_tcp_controller_state_reader(cfg);
#if defined(_WIN32)
  if (transport == "auto" || transport == "windows_shm" ||
      transport == "named_shm") {
    return create_windows_runtime_controller_state_reader(cfg);
  }
#else
  if (transport == "auto" || transport == "posix_shm" || transport == "shm") {
    return create_posix_runtime_controller_state_reader(cfg);
  }
#endif
  return nullptr;
}

std::unique_ptr<IRuntimeHandReader>
create_runtime_hand_reader(const RuntimeHandReaderConfig &cfg) {
  const std::string transport =
      lower_ascii(cfg.transport.empty() ? "auto" : cfg.transport);
  if (transport == "tcp" || transport == "app_tcp")
    return create_tcp_hand_reader(cfg);
#if defined(_WIN32)
  if (transport == "auto" || transport == "windows_shm" ||
      transport == "named_shm") {
    return create_windows_runtime_hand_reader(cfg);
  }
#else
  if (transport == "auto" || transport == "posix_shm" || transport == "shm") {
    return create_posix_runtime_hand_reader(cfg);
  }
#endif
  return nullptr;
}

} // namespace xr::monado_driver
