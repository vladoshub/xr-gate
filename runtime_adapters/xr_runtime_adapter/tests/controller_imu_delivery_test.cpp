#include "override_controller.hpp"

#include <xr_override_controller/controller_input_publisher.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

using namespace xr_runtime;
using namespace xr_runtime_adapter::override_controller;
namespace provider = xr_override_controller;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

struct TestStream {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("xr_gate_imu_delivery_" + std::to_string(getpid()));
  provider::PublishConfig config;
  TestStream() {
    std::filesystem::create_directories(directory);
    config.shm_name = "xr_gate_imu_delivery_" + std::to_string(getpid());
    config.registry_path = (directory / "registry.json").string();
    config.slot_count = 16;
  }
  ~TestStream() {
    shm_unlink(("/" + config.shm_name).c_str());
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

provider::OutputState output(uint64_t sequence, uint64_t newest_ns, bool rotated = false) {
  provider::OutputState state;
  state.left.configured = state.left.connected = true;
  state.left.device_id = "gearvr:left";
  auto& imu = state.left.imu;
  imu.status = CONTROLLER_IMU_ACTIVE;
  imu.capability_flags = CONTROLLER_IMU_CAP_DEVICE_TIMESTAMP |
      CONTROLLER_IMU_CAP_MULTI_SAMPLE_PACKET | CONTROLLER_IMU_CAP_STREAM_DIAGNOSTICS;
  imu.data_flags = CONTROLLER_IMU_ORIENTATION_VALID |
      CONTROLLER_IMU_ACCELEROMETER_VALID | CONTROLLER_IMU_HOST_TIME_SYNCED;
  imu.sequence = sequence;
  imu.sample_count = 2;
  imu.latest_sample_timestamp_ns = imu.orientation_timestamp_ns = newest_ns;
  imu.latest_device_timestamp_ticks = sequence * 10000;
  imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = 1;
  imu.reserved[CONTROLLER_IMU_DIAG_TICKS_PER_SECOND] = 1000000;
  if (rotated) {
    imu.orientation_xyzw[1] = std::sqrt(0.5f);
    imu.orientation_xyzw[3] = std::sqrt(0.5f);
  }
  for (uint32_t i = 0; i < 2; ++i) {
    auto& sample = imu.samples[i];
    sample.timestamp_ns = newest_ns - (1 - i) * 5'000'000ULL;
    sample.device_timestamp_ticks = sequence * 10000 - (1 - i) * 5000;
    sample.flags = CONTROLLER_IMU_ACCELEROMETER_VALID | CONTROLLER_IMU_HOST_TIME_SYNCED;
    sample.specific_force_m_s2[rotated ? 2 : 0] = 1.0f;
    sample.specific_force_m_s2[1] = 9.81f;
  }
  return state;
}

int main() {
  static_assert(sizeof(ControllerInputV3) == 1432, "controller V3 ABI changed");
  TestStream stream;
  provider::ControllerInputShmPublisher publisher(stream.config);
  constexpr uint64_t anchor_ns = 1'000'000'000ULL;
  publisher.publish(output(1, anchor_ns));
  const auto info = stream_info_from_registry(stream.config.registry_path, stream.config.stream_id);
  TrackingRingReader<ControllerInputV3> reader(info, CONTROLLER_INPUT_V3_FORMAT_NAME);
  uint64_t cursor = 0;
  auto initial = reader.next(cursor);
  require(initial && cursor == 1, "first SHM attach failed");
  require(initial->source_timestamp_ns == anchor_ns && initial->timestamp_ns != anchor_ns,
          "source and publication timestamps not separated");
  require(initial->left.imu.latest_device_timestamp_ticks == 10000,
          "publisher lost device timestamps");

  RuntimeControllerSynthesisConfig cfg;
  cfg.left_orientation_source = RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
  auto& motion = cfg.left_imu_motion;
  motion.position_prediction_enabled = motion.acceleration_integration_enabled = true;
  motion.prediction_time_decay_enabled = false;
  motion.acceleration_deadband_mps2 = 0;
  motion.max_prediction_velocity_mps = motion.max_prediction_speed_mps = 0;
  motion.max_prediction_path_m = 0;
  HandTrackingFrameF32V2 optical{};
  optical.sequence = 1;
  optical.left.status = 1;
  optical.left.flags = HAND_POSE_VALID;
  optical.left.controller_qw = 1;
  RuntimeControllerSynthesisState runtime;
  compose_runtime_controller_state(1, anchor_ns, cfg, optical, optical, initial,
                                   std::nullopt, 9.81f, &runtime);
  require(runtime.left.has_position_anchor, "optical anchor failed");

  // Eight notifications arrive between runtime ticks; later batches rotate
  // the sensor by 90 degrees. Every batch still represents +X world force.
  for (uint64_t i = 1; i <= 8; ++i) {
    publisher.publish(output(i + 1, anchor_ns + i * 10'000'000, i > 4));
  }
  std::vector<ControllerInputV3> history;
  while (auto frame = reader.next(cursor)) history.push_back(*frame);
  require(history.size() == 8 && cursor == 9, "SHM burst skipped frames");
  for (size_t i = 0; i < history.size(); ++i) {
    require(history[i].sequence == i + 2, "SHM sequence discontinuity");
  }
  const auto latest = history.back();
  compose_runtime_controller_state(2, anchor_ns + 80'000'000, cfg, std::nullopt,
      std::nullopt, latest, std::nullopt, 9.81f, &runtime, &history);
  require(std::abs(runtime.left.acceleration_velocity_delta_mps[0] - 0.08f) < 1e-5f,
          "runtime lost acceleration batches or used latest orientation for old batches");
  require(std::abs(runtime.left.acceleration_velocity_delta_mps[2]) < 1e-5f,
          "per-batch acceleration orientation not retained");
  require(std::abs(runtime.left.acceleration_position_delta_m[0] - 0.0032f) < 1e-6f,
          "runtime acceleration integration duration incorrect");
  compose_runtime_controller_state(3, anchor_ns + 85'000'000, cfg, std::nullopt,
      std::nullopt, latest, std::nullopt, 9.81f, &runtime, &history);
  require(std::abs(runtime.left.acceleration_velocity_delta_mps[0] - 0.08f) < 1e-5f,
          "duplicate history integrated twice");

  // Old epochs must not be fused into a restarted controller's history.
  auto restarted = latest;
  restarted.left.imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = 2;
  compose_runtime_controller_state(4, anchor_ns + 90'000'000, cfg, std::nullopt,
      std::nullopt, restarted, std::nullopt, 9.81f, &runtime, &history);
  require(!runtime.left.has_position_anchor &&
          runtime.left.last_acceleration_sample_timestamp_ns == 0,
          "reconnect continued old integration");

  for (uint64_t i = 0; i < 17; ++i) publisher.publish(output(10 + i, anchor_ns + (10 + i) * 10'000'000));
  bool overrun = false;
  try { reader.next(cursor); } catch (const std::runtime_error&) { overrun = true; }
  require(overrun, "ring overrun silently skipped input frames");
  auto restart_config = stream.config;
  restart_config.unlink_existing = false;
  provider::ControllerInputShmPublisher restarted_publisher(restart_config);
  restarted_publisher.publish(output(1, anchor_ns + 500'000'000));
  bool reset = false;
  try { reader.next(cursor); } catch (const std::runtime_error&) { reset = true; }
  require(reset, "publisher restart not detected");
  std::cout << "SHM burst delivery, runtime IMU history, duplicate/reset/overrun handling: OK\n";
}
