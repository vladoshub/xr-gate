#include "frame_model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <CLI/CLI.hpp>

#include <capture_client/sync/recovering_stereo_reader.hpp>
#include <capture_client/transports/shm_transport.hpp>
#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_tracking/contracts/body_tracker_set_contract.hpp>
#include <xr_tracking/publishers/hmd_pose_shm_publisher.hpp>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace xr_tracking;

volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

struct JointTracker {
  const char* name;
  uint32_t role;
};

// BODY_TRACKER_SET_F32_V1 wrist role codes used by the runtime wrist fallback,
// including when it is installed separately from this backend.
constexpr uint32_t kLeftWristRole = 14;
constexpr uint32_t kRightWristRole = 15;
constexpr size_t kLeftHip = 7;
constexpr size_t kRightHip = 11;

constexpr std::array<JointTracker, frame_body::kJointCount + 1> kTrackers{{
    {"frame_neck", BODY_TRACKER_ROLE_GENERIC},
    {"frame_left_shoulder", BODY_TRACKER_ROLE_LEFT_SHOULDER},
    {"frame_left_elbow", BODY_TRACKER_ROLE_LEFT_ELBOW},
    {"frame_left_wrist", kLeftWristRole},
    {"frame_right_shoulder", BODY_TRACKER_ROLE_RIGHT_SHOULDER},
    {"frame_right_elbow", BODY_TRACKER_ROLE_RIGHT_ELBOW},
    {"frame_right_wrist", kRightWristRole},
    {"frame_left_hip", BODY_TRACKER_ROLE_GENERIC},
    {"frame_left_knee", BODY_TRACKER_ROLE_LEFT_KNEE},
    {"frame_left_foot", BODY_TRACKER_ROLE_LEFT_FOOT},
    {"frame_left_toe", BODY_TRACKER_ROLE_GENERIC},
    {"frame_right_hip", BODY_TRACKER_ROLE_GENERIC},
    {"frame_right_knee", BODY_TRACKER_ROLE_RIGHT_KNEE},
    {"frame_right_foot", BODY_TRACKER_ROLE_RIGHT_FOOT},
    {"frame_right_toe", BODY_TRACKER_ROLE_GENERIC},
    {"frame_waist", BODY_TRACKER_ROLE_WAIST},
}};

bool valid_pose(const xr_runtime::HmdPoseF64V1& pose) {
  const Eigen::Vector3d position(pose.px, pose.py, pose.pz);
  const Eigen::Quaterniond orientation(pose.qw, pose.qx, pose.qy, pose.qz);
  return pose.timestamp_ns != 0 && pose.tracking_status == 2 &&
         (pose.flags & xr_runtime::HMD_FLAG_POSE_VALID) != 0 &&
         position.allFinite() && orientation.coeffs().allFinite() &&
         std::abs(orientation.squaredNorm() - 1.0) < 0.01;
}

class PoseReader {
 public:
  PoseReader(std::string registry, std::string stream, uint64_t max_delta_ns)
      : registry_(std::move(registry)), stream_(std::move(stream)), max_delta_ns_(max_delta_ns) {}

  std::optional<Eigen::Isometry3d> at(uint64_t timestamp_ns) {
    if (!reader_) {
      attach();
      if (!reader_) return std::nullopt;
    }
    if (const auto aligned = lookup(timestamp_ns)) return aligned;
    // Basalt can reopen the ring after a capture/backend restart.
    if (Clock::now() - last_update_ >= 1s) attach();
    return std::nullopt;
  }

  uint64_t reset_counter() const { return reset_counter_; }

 private:
  void attach() {
    const auto now = Clock::now();
    if (now < retry_at_) return;
    retry_at_ = now + 1s;
    try {
      const auto input = xr_runtime::stream_info_from_registry(registry_, stream_);
      if (input.format_name != xr_runtime::HMD_POSE_FORMAT_NAME ||
          input.payload_size != sizeof(xr_runtime::HmdPoseF64V1) ||
          input.slot_count == 0 || input.slot_header_size < 88 ||
          input.slot_stride < input.slot_header_size + input.payload_size ||
          input.header_size < 48) {
        throw std::runtime_error("expected a Basalt HMD pose SHM ring");
      }
      // The Basalt pose ring uses CAPSHM1 and the same timestamp and
      // sequence fields as the capture_service reader.
      capture_client::StreamInfo info;
      info.stream_id = input.stream_id;
      info.shm_name = input.shm_name;
      info.slot_count = input.slot_count;
      info.slot_header_size = input.slot_header_size;
      info.slot_stride = input.slot_stride;
      info.header_size = input.header_size;
      info.payload_size = input.payload_size;
      reader_ = std::make_unique<capture_client::ShmStreamReader>(std::move(info));
      latest_sequence_ = 0;
      last_update_ = now;
      last_attach_error_.clear();
    } catch (const std::exception& error) {
      if (error.what() != last_attach_error_) {
        last_attach_error_ = error.what();
        std::cerr << "[frame_body_tracking] waiting for Basalt: " << last_attach_error_ << '\n';
      }
    }
  }

  std::optional<Eigen::Isometry3d> lookup(uint64_t timestamp_ns) {
    const uint64_t latest = reader_->latest_sequence();
    if (latest != latest_sequence_) {
      latest_sequence_ = latest;
      last_update_ = Clock::now();
    }
    if (latest == 0) return std::nullopt;

    const uint64_t slots = reader_->info().slot_count;
    const uint64_t first = latest > slots ? latest - slots + 1 : 1;
    std::optional<xr_runtime::HmdPoseF64V1> next;
    capture_client::RawMessage raw;
    for (uint64_t seq = latest; seq >= first; --seq) {
      if (!reader_->read_sequence(seq, raw) ||
          raw.payload.size() != sizeof(xr_runtime::HmdPoseF64V1)) {
        return std::nullopt;
      }
      xr_runtime::HmdPoseF64V1 pose{};
      std::memcpy(&pose, raw.payload.data(), sizeof(pose));
      if (pose.version != 1 || pose.size_bytes != sizeof(pose)) return std::nullopt;
      if (seq == latest) reset_counter_ = pose.reset_counter;
      if (pose.reset_counter != reset_counter_ || !valid_pose(pose)) return std::nullopt;
      if (pose.timestamp_ns > timestamp_ns) {
        next = pose;
        continue;
      }

      const Eigen::Vector3d position(pose.px, pose.py, pose.pz);
      const Eigen::Quaterniond orientation(pose.qw, pose.qx, pose.qy, pose.qz);
      auto result = Eigen::Isometry3d::Identity();
      if (pose.timestamp_ns == timestamp_ns) {
        result.translation() = position;
        result.linear() = orientation.normalized().toRotationMatrix();
        return result;
      }
      if (!next || timestamp_ns - pose.timestamp_ns > max_delta_ns_ ||
          next->timestamp_ns - timestamp_ns > max_delta_ns_) return std::nullopt;

      const double blend = double(timestamp_ns - pose.timestamp_ns) /
                           double(next->timestamp_ns - pose.timestamp_ns);
      const Eigen::Vector3d next_position(next->px, next->py, next->pz);
      const Eigen::Quaterniond next_orientation(next->qw, next->qx, next->qy, next->qz);
      result.translation() = position + blend * (next_position - position);
      result.linear() = orientation.normalized().slerp(
          blend, next_orientation.normalized()).toRotationMatrix();
      return result;
    }
    return std::nullopt;
  }

  std::string registry_;
  std::string stream_;
  uint64_t max_delta_ns_;
  uint64_t latest_sequence_ = 0;
  uint64_t reset_counter_ = 0;
  Clock::time_point retry_at_{};
  Clock::time_point last_update_{};
  std::string last_attach_error_;
  std::unique_ptr<capture_client::ShmStreamReader> reader_;
};

BodyTrackerSetFrameF32V1 lost_frame(uint64_t camera_timestamp_ns, uint64_t reset) {
  BodyTrackerSetFrameF32V1 frame;
  frame.source_timestamp_ns = camera_timestamp_ns;
  frame.reset_counter = reset;
  frame.space = BODY_TRACKER_SPACE_TRACKING_WORLD;
  frame.source = BODY_TRACKER_SOURCE_FRAME;
  frame.tracker_count = kTrackers.size();
  for (size_t i = 0; i < kTrackers.size(); ++i) {
    auto& tracker = frame.trackers[i];
    tracker.tracker_index = static_cast<uint32_t>(i);
    tracker.role = kTrackers[i].role;
    tracker.status = BODY_TRACKER_STATUS_LOST;
    tracker.flags = BODY_TRACKER_FLAG_CONNECTED | BODY_TRACKER_FLAG_ROLE_VALID;
    const auto name_size = std::strlen(kTrackers[i].name);
    std::memcpy(tracker.tracker_id, kTrackers[i].name, name_size);
  }
  return frame;
}

void set_position(BodyTrackerF32V1& tracker, const frame_body::JointObservation& joint,
                  const Eigen::Isometry3d& world_from_imu) {
  const Eigen::Vector3f position = (world_from_imu * joint.position_imu).cast<float>();
  if (!position.allFinite()) return;
  tracker.pose.px = position.x();
  tracker.pose.py = position.y();
  tracker.pose.pz = position.z();
  tracker.status = BODY_TRACKER_STATUS_TRACKING;
  tracker.flags |= BODY_TRACKER_FLAG_POSITION_VALID;
  tracker.confidence = joint.confidence;
}
}  // namespace

int main(int argc, char** argv) {
  std::string model_path;
  std::string calibration_path;
  std::string capture_registry = xr_runtime::default_capture_service_registry_path();
  std::string tracking_registry = xr_runtime::default_tracking_registry_path();
  std::string camera0;
  std::string camera1;
  std::string stereo_pair;
  std::string pose_stream = "hmd_pose";
  std::string output_stream = "body_trackers";
  std::string output_shm = "track_frame_body_trackers";
  int ort_threads = 4;
  double pitch_down = 0.0;
  double max_camera_pitch = 0.0;
  double max_disagreement_m = 0.24;
  double max_fps = 20.0;
  double pose_wait_ms = 80.0;
  double max_pose_delta_ms = 50.0;
  int64_t clock_offset_ns = 0;
  double duration_s = 0.0;
  frame_body::ObservationFilterConfig filters;

  CLI::App app{"FRAME stereo body joint backend (ONNX Runtime CPU)"};
  app.add_option("--model", model_path, "Exported FRAME backbone ONNX model")->required()->take_last();
  app.add_option("--calib", calibration_path, "Basalt calibration used by the same capture profile")->required()->take_last();
  app.add_option("--capture-registry", capture_registry, "Capture service SHM registry")->take_last();
  app.add_option("--tracking-registry", tracking_registry, "Basalt input and body tracker output SHM registry");
  app.add_option("--stereo-pair", stereo_pair, "Stereo pair name in the capture registry")->take_last();
  app.add_option("--cam0-stream", camera0, "Override the left GRAY8 stream")->take_last();
  app.add_option("--cam1-stream", camera1, "Override the right GRAY8 stream")->take_last();
  app.add_option("--pose-stream", pose_stream, "Timestamped Basalt T_world_imu stream");
  app.add_option("--output-stream", output_stream, "Body tracker stream ID");
  app.add_option("--output-shm", output_shm, "Body tracker SHM name");
  app.add_option("--ort-threads", ort_threads, "Number of ONNX Runtime CPU threads")->check(CLI::Range(1, 256));
  app.add_option("--pitch-down-deg", pitch_down, "Downward pitch of virtual fisheye camera")
      ->check(CLI::Range(-90.0, 90.0));
  app.add_option("--max-camera-pitch-deg", max_camera_pitch,
                 "Highest physical camera pitch for inference (0=horizontal, -1=always infer)")
      ->envname("XR_FRAME_MAX_CAMERA_PITCH_DEG")
      ->check(CLI::Range(-90.0, 90.0));
  app.add_option("--max-stereo-disagreement-m", max_disagreement_m, "Maximum distance between the two 3D estimates")
      ->check(CLI::Range(0.001, 1.0));
  app.add_option("--check-field-of-view", filters.check_field_of_view,
                 "Require joint estimates to lie inside both camera images")
      ->envname("XR_FRAME_CHECK_FIELD_OF_VIEW");
  app.add_option("--check-positive-depth", filters.check_positive_depth,
                 "Require positive Z in each virtual camera's 3D joint estimate")
      ->envname("XR_FRAME_CHECK_POSITIVE_DEPTH");
  app.add_option("--check-joint-distance", filters.check_joint_distance,
                 "Apply the configured minimum and maximum joint distances")
      ->envname("XR_FRAME_CHECK_JOINT_DISTANCE");
  app.add_option("--check-stereo-disagreement", filters.check_stereo_disagreement,
                 "Reject joint estimates that disagree between the cameras")
      ->envname("XR_FRAME_CHECK_STEREO_DISAGREEMENT");
  app.add_option("--check-camera-pitch", filters.check_camera_pitch,
                 "Skip inference above the physical camera pitch limit")
      ->envname("XR_FRAME_CHECK_CAMERA_PITCH");
  for (size_t joint = 0; joint < frame_body::kJointCount; ++joint) {
    std::string option_name = kTrackers[joint].name + std::strlen("frame_");
    std::string env_name = option_name;
    std::replace(option_name.begin(), option_name.end(), '_', '-');
    std::transform(env_name.begin(), env_name.end(), env_name.begin(),
                   [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    auto& range = filters.joint_ranges[joint];
    app.add_option("--" + option_name + "-min-m", range.min_m, "Minimum joint distance from either camera (m)")
        ->envname("XR_FRAME_" + env_name + "_MIN_M")->check(CLI::NonNegativeNumber);
    app.add_option("--" + option_name + "-max-m", range.max_m, "Maximum joint distance from either camera (m)")
        ->envname("XR_FRAME_" + env_name + "_MAX_M")->check(CLI::NonNegativeNumber);
  }
  app.add_option("--max-fps", max_fps, "Cap inference rate, zero uses every available latest pair")
      ->check(CLI::Range(0.0, 240.0));
  app.add_option("--pose-wait-ms", pose_wait_ms, "Wait for the time-matched Basalt pose")
      ->check(CLI::Range(0.0, 1000.0));
  app.add_option("--max-pose-delta-ms", max_pose_delta_ms, "Maximum distance to either interpolation sample in time")
      ->check(CLI::Range(0.0, 1000.0));
  const auto* offset_option = app.add_option(
      "--camera-time-offset-ns", clock_offset_ns, "Override the calibration camera-to-IMU time offset");
  app.add_option("--duration", duration_s, "Run duration in seconds; zero waits for a stop signal")
      ->check(CLI::NonNegativeNumber);
  CLI11_PARSE(app, argc, argv);

  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  try {
    filters.validate();
    const auto calibration = frame_body::read_calibration(calibration_path);
    const auto offset = offset_option->count() ? clock_offset_ns : calibration.camera_time_offset_ns;
    const bool check_camera_pitch = filters.check_camera_pitch && max_camera_pitch != -1.0;
    const double max_forward_z = std::sin(max_camera_pitch * std::acos(-1.0) / 180.0);
    frame_body::FrameModel model(model_path, calibration, pitch_down, ort_threads, max_disagreement_m, filters);
    capture_client::RecoveringStereoReader capture(
        [&] {
          auto transport = std::make_unique<capture_client::ShmCaptureTransport>(
              capture_registry, camera0, camera1, "imu0", false, stereo_pair);
          const std::array<const capture_client::StreamInfo*, 2> streams{
              &transport->cam0().info(), &transport->cam1().info()};
          if (streams[0]->shm_name == streams[1]->shm_name) {
            throw std::runtime_error("FRAME needs two distinct camera streams in " + capture_registry);
          }
          for (size_t i = 0; i < streams.size(); ++i) {
            const auto& stream = *streams[i];
            const auto& camera = calibration.cameras[i];
            if (stream.format_name != "GRAY8" || stream.width != camera.width || stream.height != camera.height) {
              throw std::runtime_error("FRAME camera " + stream.stream_id + " must publish GRAY8 " +
                  std::to_string(camera.width) + "x" + std::to_string(camera.height) +
                  " to match calibration " + calibration_path);
            }
          }
          std::cout << "[frame_body_tracking] capture=" << capture_registry
                    << ", left=" << streams[0]->stream_id << ", right=" << streams[1]->stream_id << std::endl;
          return transport;
        }, 1'000'000, true, 8, capture_client::StereoPairing::Timestamp);
    PoseReader poses(tracking_registry, pose_stream, static_cast<uint64_t>(max_pose_delta_ms * 1e6));
    BodyTrackerSetShmPublisherConfig output;
    output.registry_path = tracking_registry;
    output.stream_id = output_stream;
    output.shm_name = output_shm;
    output.frame_id = "tracking_world";
    output.slot_count = 128;
    output.space = BODY_TRACKER_SPACE_TRACKING_WORLD;
    output.source = BODY_TRACKER_SOURCE_FRAME;
    output.created_by = "frame_body_tracking";
    BodyTrackerSetShmPublisher publisher(std::move(output));
    publisher.publish(lost_frame(0, 0));
    std::cout << "[frame_body_tracking] ONNX Runtime CPU " << Ort::GetVersionString()
              << ", model=" << model_path << ", calib=" << calibration_path
              << ", pose=" << pose_stream << ", output=" << output_stream
              << ", camera_time_offset_ns=" << offset << ", pitch_down_deg=" << pitch_down
              << ", max_camera_pitch_deg=" << max_camera_pitch << std::endl;
    std::cout << "[frame_body_tracking] filters field_of_view=" << filters.check_field_of_view
              << " joint_distance=" << filters.check_joint_distance
              << " stereo_disagreement=" << filters.check_stereo_disagreement
              << " camera_pitch=" << check_camera_pitch
              << " positive_depth=" << filters.check_positive_depth << std::endl;

    const auto wait_for_pose = [&](uint64_t timestamp) {
      const auto wait_start = Clock::now();
      auto pose = poses.at(timestamp);
      while (!pose && !stopping &&
             Clock::now() - wait_start < std::chrono::duration<double, std::milli>(pose_wait_ms)) {
        std::this_thread::sleep_for(2ms);
        pose = poses.at(timestamp);
      }
      return pose;
    };

    auto next_inference = Clock::time_point{};
    const auto period = max_fps >= 0.01
        ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / max_fps))
        : Clock::duration::zero();
    const auto started = Clock::now();
    uint64_t last_camera_timestamp_ns = 0;
    uint64_t processed = 0;
    uint64_t inferences = 0;
    uint64_t missing_poses = 0;
    uint64_t pitch_gated = 0;
    while (!stopping && (duration_s <= 0 || Clock::now() - started < std::chrono::duration<double>(duration_s))) {
      const auto pair = capture.read_next(0.10);
      if (!pair) {
        publisher.publish(lost_frame(last_camera_timestamp_ns, poses.reset_counter()));
        continue;
      }
      const auto frame_start = Clock::now();
      if (frame_start < next_inference) continue;
      next_inference = frame_start + period;
      const auto timestamp = static_cast<uint64_t>(
          capture_client::shift_camera_timestamp_ns(pair->timestamp_ns, offset));
      last_camera_timestamp_ns = timestamp;
      // Check the exposure-time camera pitch before doing any neural inference.
      // With the gate disabled, retain inference followed by Basalt pose lookup.
      auto pose = check_camera_pitch ? wait_for_pose(timestamp) : std::optional<Eigen::Isometry3d>{};
      bool allow_inference = !check_camera_pitch;
      if (check_camera_pitch && pose) {
        // Camera +Z is the physical optical axis and Basalt world +Z is up.
        // Express the pitch limit as the vertical component of the unit ray.
        allow_inference = std::all_of(
            calibration.cameras.begin(), calibration.cameras.end(), [&](const auto& camera) {
              const Eigen::Vector3d forward = pose->linear() * camera.imu_from_camera.linear().col(2);
              return forward.z() <= max_forward_z + 1e-9;
            });
        pitch_gated += !allow_inference;
      }
      frame_body::Observations joints;
      double inference_ms = 0.0;
      if (allow_inference && !stopping) {
        const auto inference_start = Clock::now();
        joints = model.infer(pair->cam0, pair->cam1);
        inference_ms = std::chrono::duration<double, std::milli>(Clock::now() - inference_start).count();
        ++inferences;
      }
      if (!check_camera_pitch) pose = wait_for_pose(timestamp);

      if (!pose) ++missing_poses;
      auto frame = lost_frame(timestamp, poses.reset_counter());
      unsigned tracked = 0;
      if (pose && !stopping) {
        for (size_t i = 0; i < joints.size(); ++i) {
          if (!joints[i]) continue;
          set_position(frame.trackers[i], *joints[i], *pose);
          tracked += frame.trackers[i].status == BODY_TRACKER_STATUS_TRACKING;
        }
        if (joints[kLeftHip] && joints[kRightHip]) {
          const frame_body::JointObservation waist{
              (joints[kLeftHip]->position_imu + joints[kRightHip]->position_imu) * 0.5,
              std::min(joints[kLeftHip]->confidence, joints[kRightHip]->confidence)};
          set_position(frame.trackers[kTrackers.size() - 1], waist, *pose);
        }
      }
      publisher.publish(frame);
      if (++processed == 1 || processed % 60 == 0) {
        std::cout << "[frame_body_tracking] frames=" << processed << " tracked_joints=" << tracked
                  << " inferences=" << inferences << " missing_poses=" << missing_poses
                  << " pitch_gated=" << pitch_gated << " inference_ms=" << inference_ms
                  << std::endl;
      }
    }
    publisher.publish(lost_frame(last_camera_timestamp_ns, poses.reset_counter()));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "[frame_body_tracking] " << error.what() << '\n';
    return 1;
  }
}
