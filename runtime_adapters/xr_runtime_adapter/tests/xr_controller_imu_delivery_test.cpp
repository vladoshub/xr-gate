#include "override_controller.hpp"
#include "xiao_nrf54l15_input_provider.hpp"
#include "xiao_nrf54l15_serial_transport.hpp"
#include "capture_service_cpp/protocols/xr_controller_v1.hpp"

#include <xr_override_controller/controller_input_publisher.hpp>
#include <xr_override_controller/imu/controller_imu_calibration.hpp>

#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace oc = xr_override_controller;
namespace ctl = oc::xiao_nrf54l15;
namespace cap = xr_capture_cpp;
using namespace xr_runtime;
using namespace xr_runtime_adapter::override_controller;

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint64_t test_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

class FakeSerial final : public ctl::SerialTransport {
 public:
  std::vector<ctl::SerialDeviceSnapshot> snapshots;
  std::deque<ctl::SerialBytes> queue;
  std::string platform_name() const override { return "test"; }
  std::vector<ctl::SerialDeviceSnapshot> devices() const override { return snapshots; }
  void pump(int, bool, bool* ready) override { if (ready) *ready = false; }
  bool pop_bytes(ctl::SerialBytes& bytes) override {
    if (queue.empty()) return false;
    bytes = std::move(queue.front()); queue.pop_front(); return true;
  }
  void enqueue(const std::string& id, uint32_t sequence, uint64_t device_us,
                uint64_t rx, size_t count = 3, uint8_t flags = 1, float gyro = 0,
                std::array<float, 3> force = {1, 9.81f, 0}) {
    ctl::SerialBytes bytes;
    bytes.stable_id = id; bytes.host_timestamp_ns = rx;
    for (size_t i = 0; i < count; ++i) {
      cap::XrControllerV1Sample s;
      s.sequence = sequence + static_cast<uint32_t>(i);
      s.device_timestamp_us = device_us + i * 4808;
      s.flags = flags; s.gyro_rad_s[2] = gyro;
      s.accel_m_s2 = force;
      std::array<uint8_t, cap::kXrControllerV1PacketSize> packet{};
      require(cap::encode_xr_controller_v1(s, packet.data(), packet.size()), "encode serial packet");
      bytes.bytes.insert(bytes.bytes.end(), packet.begin(), packet.end());
    }
    queue.push_back(std::move(bytes));
  }
};
FakeSerial* serial;
namespace xr_override_controller::xiao_nrf54l15 {
std::unique_ptr<SerialTransport> make_platform_serial_transport(const XiaoNrf54l15Options&) {
  auto result = std::make_unique<FakeSerial>(); serial = result.get();
  for (const char* id : {"left", "right"}) {
    SerialDeviceSnapshot s;
    s.stable_id = id; s.port_path = std::string("/dev/test-") + id;
    s.explicit_candidate = s.connected = true;
    result->snapshots.push_back(s);
  }
  return result;
}
}

struct TestStream {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("xr_controller_delivery_" + std::to_string(getpid()));
  oc::PublishConfig config;
  TestStream() {
    std::filesystem::create_directories(directory);
    config.shm_name = "xr_controller_delivery_" + std::to_string(getpid());
    config.registry_path = (directory / "registry.json").string();
    config.slot_count = 512;
  }
  ~TestStream() {
    shm_unlink(("/" + config.shm_name).c_str());
    std::error_code ec; std::filesystem::remove_all(directory, ec);
  }
};
oc::OutputState output(const ControllerImuStateV1& imu) {
  oc::OutputState out;
  out.left.configured = out.left.connected = true;
  out.left.device_id = "xctl:left"; out.left.imu = imu;
  return out;
}

void serial_gravity_reference_controls_orientation_validity() {
  ctl::XiaoNrf54l15InputProvider provider({{"initial_scan_ms", "0"}, {"madgwick_beta", "0.04"}});
  auto devices = provider.scan_devices(true);
  const uint64_t receive_ns = test_now_ns() - 40'000'000;
  const std::array<float, 3> gravity{0, 8.492808f, -4.903325f};
  serial->enqueue("left", 0, 1'000'000, receive_ns, 1, 1, 1.0f, gravity);
  provider.wait_event(devices, 5, false);
  const auto moving = provider.consume_imu_state(devices[0]);
  require((moving.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) == 0u,
          "serial moving startup advertised an uninitialized gravity reference");
  require(moving.sample_count == 1 && moving.latest_device_timestamp_ticks == 1'000'000,
          "serial gravity gate discarded timed raw measurements");
  serial->enqueue("left", 1, 1'004'808, receive_ns + 4'808'000, 1, 1, 0, gravity);
  provider.wait_event(devices, 5, false);
  auto quiet = provider.consume_imu_state(devices[0]);
  require((quiet.data_flags & (CONTROLLER_IMU_ORIENTATION_VALID |
                              CONTROLLER_IMU_GYROSCOPE_CALIBRATED)) == 0u,
          "serial single quiet sample bypassed gyro startup confirmation");
  // Advance simulated acquisition time through the default 1500 ms window.
  // 312 intervals of 4808 us span 1500.096 ms; no wall-clock wait is needed.
  constexpr uint32_t last_sequence = 313;
  for (uint32_t sequence = 2; sequence <= last_sequence; ++sequence) {
    serial->enqueue("left", sequence, 1'000'000 + uint64_t(sequence) * 4808,
                    receive_ns + uint64_t(sequence) * 4'808'000, 1, 1, 0, gravity);
    provider.wait_event(devices, 0, false);
    require(provider.has_pending_imu(devices[0]),
            "serial gyro warmup discarded timed raw measurements");
    quiet = provider.consume_imu_state(devices[0]);
    require(quiet.latest_device_timestamp_ticks == 1'000'000 + uint64_t(sequence) * 4808 &&
            quiet.reserved[CONTROLLER_IMU_DIAG_EPOCH] == moving.reserved[CONTROLLER_IMU_DIAG_EPOCH],
            "serial gyro warmup changed acquisition timing");
    if (sequence < last_sequence) {
      require((quiet.data_flags & (CONTROLLER_IMU_ORIENTATION_VALID |
                                  CONTROLLER_IMU_GYROSCOPE_CALIBRATED)) == 0u,
              "serial partial stationary window advertised calibrated orientation");
    }
  }
  require((quiet.data_flags & CONTROLLER_IMU_GYROSCOPE_CALIBRATED) != 0u,
          "serial stationary window did not calibrate gyro");
  require((quiet.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) != 0u,
          "serial quiet gravity did not initialize orientation");
  require(std::abs(quiet.orientation_xyzw[0] - 0.8660254f) < 0.0001f &&
          std::abs(quiet.orientation_xyzw[3] - 0.5f) < 0.0001f,
          "serial tilted startup uses an arbitrary orientation");
  require(quiet.latest_device_timestamp_ticks == 1'000'000 + uint64_t(last_sequence) * 4808 &&
          quiet.reserved[CONTROLLER_IMU_DIAG_EPOCH] == moving.reserved[CONTROLLER_IMU_DIAG_EPOCH],
          "serial orientation initialization changed acquisition timing");
  serial->snapshots[0].connected = false;
  provider.wait_event(devices, 5, false);
  serial->snapshots[0].connected = true;
  const uint64_t reconnect_ns = receive_ns + uint64_t(last_sequence) * 4'808'000 + 10'000'000;
  serial->enqueue("left", 0, 0, reconnect_ns, 1, 1, 1.0f, gravity);
  provider.wait_event(devices, 5, false);
  require((provider.imu_state(devices[0]).data_flags & CONTROLLER_IMU_ORIENTATION_VALID) == 0u,
          "serial reconnect retained an initialized old gravity reference");
  serial->enqueue("left", 1, 4808, reconnect_ns + 4'808'000, 1, 1, 0, gravity);
  provider.wait_event(devices, 0, false);
  require((provider.imu_state(devices[0]).data_flags &
           (CONTROLLER_IMU_ORIENTATION_VALID | CONTROLLER_IMU_GYROSCOPE_CALIBRATED)) == 0u,
          "serial reconnect retained the old gyro calibration");
}

void serial_config_calibration_resets_once_and_reaches_native_samples() {
  ctl::XiaoNrf54l15InputProvider provider({{"initial_scan_ms", "0"}});
  auto devices = provider.scan_devices(true);
  const uint64_t receive_ns = test_now_ns() - 40'000'000;
  serial->enqueue("left", 0, 1'000'000, receive_ns, 1);
  provider.wait_event(devices, 1, false);
  oc::AppConfig cfg;
  oc::ConfigDevice d;
  d.id = 12; d.fingerprint = devices[0].fingerprint;
  cfg.devices.push_back(d);
  cfg.imu_calibrations.push_back({12, {true, {.1f,.2f,.3f}, {.99f,1.01f,1.02f}}});
  oc::imu::configure_imu_calibrations(provider, cfg, devices);
  require(!provider.has_pending_imu(devices[0]) && provider.imu_state(devices[0]).sample_count == 0,
          "serial calibration retained queued uncorrected samples");
  serial->enqueue("left", 1, 1'004'808, receive_ns + 4'808'000, 1);
  provider.wait_event(devices, 1, false);
  auto state = provider.imu_state(devices[0]);
  const auto epoch = state.reserved[CONTROLLER_IMU_DIAG_EPOCH];
  require(state.data_flags & CONTROLLER_IMU_ACCELEROMETER_CALIBRATED,
          "serial ignored device_id calibration");
  require(std::abs(state.samples[0].specific_force_m_s2[1] - (9.81f - .2f) * 1.01f) < 1e-5f,
          "serial applied calibration after basis transform or more than once");
  oc::imu::configure_imu_calibrations(provider, cfg, devices);
  require(provider.has_pending_imu(devices[0]) &&
          provider.imu_state(devices[0]).reserved[CONTROLLER_IMU_DIAG_EPOCH] == epoch,
          "serial restarted AHRS for unchanged calibration");
  serial->snapshots[0].connected = false;
  provider.wait_event(devices, 1, false);
  serial->snapshots[0].connected = true;
  serial->enqueue("left", 0, 0, receive_ns + 10'000'000, 1);
  provider.wait_event(devices, 1, false);
  require(provider.imu_state(devices[0]).data_flags & CONTROLLER_IMU_ACCELEROMETER_CALIBRATED,
          "serial reconnect lost coefficients");
  cfg.imu_calibrations.clear();
  oc::imu::configure_imu_calibrations(provider, cfg, devices);
  serial->enqueue("left", 1, 4808, receive_ns + 14'808'000, 1);
  provider.wait_event(devices, 1, false);
  state = provider.imu_state(devices[0]);
  require(!(state.data_flags & CONTROLLER_IMU_ACCELEROMETER_CALIBRATED),
          "serial retained coefficients absent from selected config");
  require(std::abs(state.samples[0].specific_force_m_s2[1] - 9.81f) < 1e-5f,
          "serial disabled correction still changed force");
}

int main() {
  serial_config_calibration_resets_once_and_reaches_native_samples();
  serial_gravity_reference_controls_orientation_validity();
  static_assert(sizeof(ControllerInputV3) == 1432, "controller ABI changed");
  // Transport/integration starts with calibrated controllers. The full default
  // startup window is exercised separately above.
  ctl::XiaoNrf54l15InputProvider provider({{"initial_scan_ms", "0"}, {"madgwick_beta", "0"},
                                        {"gyro_bias.stable_ms", "1"},
                                        {"gyro_bias.min_samples", "2"}});
  auto devices = provider.scan_devices(true);
  require(devices.size() == 2, "two serial controllers not found");
  const uint64_t rx = test_now_ns() - 20'000'000;
  const uint64_t device_us = rx / 1000 + 10'000'000;
  // Each read's receive time belongs to its newest packet. Warmup ends one
  // interval before sequence 10/40, the oldest packets of the next reads.
  serial->enqueue("left", 8, device_us - 2 * 4808, rx - 3 * 4'808'000, 2);
  serial->enqueue("right", 38, device_us + 1'000'000 - 2 * 4808, rx - 3 * 4'808'000, 2);
  provider.wait_event(devices, 0, false);
  for (const auto& device : devices) {
    require((provider.imu_state(device).data_flags & CONTROLLER_IMU_ORIENTATION_VALID) != 0u,
            "serial transport test started before gyro calibration completed");
  }
  provider.flush_events(devices);
  serial->enqueue("left", 10, device_us, rx);
  serial->enqueue("right", 40, device_us + 1'000'000, rx);
  provider.wait_event(devices, 5, false);
  require(provider.imu_state(devices[0]).sequence == 12, "snapshot not newest");
  auto first = provider.consume_imu_state(devices[0]);
  require(first.sequence == 10 && first.sample_count == 1, "FIFO lost first packet");
  require(first.latest_device_timestamp_ticks == device_us &&
          first.samples[0].device_timestamp_ticks == device_us, "raw hardware timestamp lost");
  require((first.capability_flags & CONTROLLER_IMU_CAP_DEVICE_TIMESTAMP) &&
          (first.data_flags & CONTROLLER_IMU_HOST_TIME_SYNCED), "valid timing not advertised");
  const uint32_t epoch = first.reserved[CONTROLLER_IMU_DIAG_EPOCH];

  TestStream stream;
  oc::ControllerInputShmPublisher publisher(stream.config);
  publisher.publish(output(first));
  auto info = stream_info_from_registry(stream.config.registry_path, stream.config.stream_id);
  TrackingRingReader<ControllerInputV3> reader(info, CONTROLLER_INPUT_V3_FORMAT_NAME);
  uint64_t cursor = 0;
  auto initial = reader.next(cursor);
  require(initial && initial->left.imu.latest_device_timestamp_ticks == device_us &&
          initial->source_timestamp_ns == first.latest_sample_timestamp_ns, "SHM changed acquisition time");
  const uint64_t anchor_ns = first.latest_sample_timestamp_ns;
  RuntimeControllerSynthesisConfig cfg;
  cfg.left_orientation_source = RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
  auto& motion = cfg.left_imu_motion;
  motion.position_prediction_enabled = motion.acceleration_integration_enabled = true;
  motion.prediction_time_decay_enabled = false;
  motion.acceleration_deadband_mps2 = 0;
  motion.max_prediction_velocity_mps = motion.max_prediction_speed_mps = 0;
  motion.max_prediction_path_m = 0;
  HandTrackingFrameF32V2 optical{};
  optical.sequence = 1; optical.left.status = 1;
  optical.left.flags = HAND_POSE_VALID; optical.left.controller_qw = 1;
  RuntimeControllerSynthesisState runtime;
  // Test transport/integration independently of optical motion calibration.
  runtime.left.acceleration_world_yaw_valid = true;
  compose_runtime_controller_state(1, anchor_ns, cfg, optical, optical, initial,
      std::nullopt, 9.81f, &runtime);

  serial->enqueue("left", 13, device_us + 3 * 4808, rx + 3 * 4'808'000);
  provider.wait_event(devices, 5, false);
  for (uint32_t sequence = 11; sequence <= 15; ++sequence) {
    require(provider.has_pending_imu(devices[0]), "queued IMU packet lost");
    auto imu = provider.consume_imu_state(devices[0]);
    require(imu.sequence == sequence && imu.samples[0].device_timestamp_ticks == device_us + (sequence - 10) * 4808,
            "FIFO order or raw tick wrong");
    publisher.publish(output(imu));
  }
  require(!provider.has_pending_imu(devices[0]), "FIFO not drained");
  std::vector<ControllerInputV3> history;
  while (auto frame = reader.next(cursor)) history.push_back(*frame);
  require(history.size() == 5, "SHM skipped serial burst");
  const auto latest = history.back();
  const float dt = 5 * 0.004808f;
  compose_runtime_controller_state(2, latest.source_timestamp_ns, cfg, std::nullopt,
      std::nullopt, latest, std::nullopt, 9.81f, &runtime, &history);
  require(std::abs(runtime.left.acceleration_velocity_delta_mps[0] - dt) < 1e-5f,
          "runtime dropped/integrated serial packets with wrong intervals");
  require(std::abs(runtime.left.acceleration_position_delta_m[0] - 0.5f * dt * dt) < 1e-6f,
          "runtime serial position integration wrong");
  compose_runtime_controller_state(3, latest.source_timestamp_ns, cfg, std::nullopt,
      std::nullopt, latest, std::nullopt, 9.81f, &runtime, &history);
  require(std::abs(runtime.left.acceleration_velocity_delta_mps[0] - dt) < 1e-5f,
          "runtime integrated duplicate history");

  // Wire sequence numbers wrap after 0xffffffff. Vary acceleration so a
  // skipped historical sample cannot be hidden by constant-force integration.
  auto wrapped_initial = *initial;
  wrapped_initial.left.imu.sequence = 0xfffffffdu;
  auto wrapped_history = history;
  for (size_t i = 0; i < wrapped_history.size(); ++i) {
    wrapped_history[i].left.imu.sequence = 0xfffffffeu + static_cast<uint32_t>(i);
    wrapped_history[i].left.imu.samples[0].specific_force_m_s2[0] = static_cast<float>(i + 1);
  }
  const auto wrapped_latest = wrapped_history.back();
  RuntimeControllerSynthesisState wrapped_runtime;
  wrapped_runtime.left.acceleration_world_yaw_valid = true;
  compose_runtime_controller_state(1, anchor_ns, cfg, optical, optical, wrapped_initial,
      std::nullopt, 9.81f, &wrapped_runtime);
  compose_runtime_controller_state(2, wrapped_latest.source_timestamp_ns, cfg, std::nullopt,
      std::nullopt, wrapped_latest, std::nullopt, 9.81f, &wrapped_runtime, &wrapped_history);
  require(std::abs(wrapped_runtime.left.acceleration_velocity_delta_mps[0] - 15 * 0.004808f) < 1e-5f,
          "runtime lost history across wire sequence wrap");
  require(std::abs(wrapped_runtime.left.acceleration_position_delta_m[0] - 27.5f * 0.004808f * 0.004808f) < 1e-6f,
          "runtime position wrong across wire sequence wrap");
  for (uint32_t sequence = 40; sequence < 43; ++sequence) {
    auto imu = provider.consume_imu_state(devices[1]);
    require(imu.sequence == sequence && imu.latest_device_timestamp_ticks == device_us + 1'000'000 + (sequence - 40) * 4808,
            "two controllers share clocks or FIFO");
  }
  serial->enqueue("left", 15, device_us + 5 * 4808, test_now_ns(), 1);
  provider.wait_event(devices, 5, false);
  require(!provider.has_pending_imu(devices[0]), "duplicate packet queued");

  serial->enqueue("left", 0, 0, test_now_ns());
  provider.wait_event(devices, 5, false);
  auto reboot = provider.consume_imu_state(devices[0]);
  require(reboot.reserved[CONTROLLER_IMU_DIAG_EPOCH] != epoch && reboot.latest_device_timestamp_ticks == 0,
          "MCU reboot did not reset epoch");
  publisher.publish(output(reboot));
  auto restarted = reader.next(cursor);
  compose_runtime_controller_state(4, restarted->source_timestamp_ns, cfg, std::nullopt,
      std::nullopt, restarted, std::nullopt, 9.81f, &runtime, &history);
  require(!runtime.left.has_position_anchor, "runtime continued across MCU reboot");

  {
    // Several ordinary serial reads must fill the IMU FIFO without exceeding
    // the stream decoder's separate, bounded byte buffer.
    auto* previous_serial = serial;
    ctl::XiaoNrf54l15InputProvider overflow_provider({{"initial_scan_ms", "0"}});
    auto overflow_devices = overflow_provider.scan_devices(true);
    const uint64_t last_rx = test_now_ns();
    for (uint32_t i = 0; i < 300; i += 30) {
      serial->enqueue("left", 3 + i, 14424 + i * 4808,
          last_rx - (300 - i - 30) * 4'808'000ULL, 30);
    }
    overflow_provider.wait_event(overflow_devices, 5, false);
    auto overflow = overflow_provider.consume_imu_state(overflow_devices[0]);
    require(overflow.sequence == 47 && overflow.reserved[CONTROLLER_IMU_DIAG_DROPPED_BATCHES] == 44,
            "bounded FIFO overflow not reported on next consumed frame");
    overflow_provider.flush_events(overflow_devices);
    require(!overflow_provider.has_pending_imu(overflow_devices[0]), "flush retained IMU queue");
    serial = previous_serial;
  }
  provider.flush_events(devices);

  serial->snapshots[0].connected = false;
  provider.wait_event(devices, 5, false);
  require(provider.imu_state(devices[0]).status == CONTROLLER_IMU_LOST &&
          !provider.has_pending_imu(devices[0]), "disconnect retained active samples");
  serial->snapshots[0].connected = true;
  serial->enqueue("left", 0, 0, test_now_ns());
  provider.wait_event(devices, 5, false);
  require(provider.imu_state(devices[0]).reserved[CONTROLLER_IMU_DIAG_EPOCH] != reboot.reserved[CONTROLLER_IMU_DIAG_EPOCH],
          "reconnect did not reset clock epoch");
  provider.flush_events(devices);

  // Host jitter must not alter the AHRS integration step on either controller.
  const auto jitter_base = test_now_ns();
  for (uint32_t i = 0; i < 100; ++i) {
    serial->enqueue("left", 1000 + i, 1'000'000 + i * 4808, jitter_base + i * 4'808'000, 1, 1, 1.0f);
    serial->enqueue("right", 1000 + i, 2'000'000 + i * 4808, jitter_base + i * 4'808'000 + (i % 7) * 100'000, 1, 1, 1.0f);
  }
  provider.wait_event(devices, 5, false);
  const auto left = provider.imu_state(devices[0]);
  const auto right = provider.imu_state(devices[1]);
  for (size_t i = 0; i < 4; ++i) require(std::abs(left.orientation_xyzw[i] - right.orientation_xyzw[i]) < 1e-6f,
                                        "AHRS follows receipt jitter");
  provider.flush_events(devices);
  const auto fallback_rx = test_now_ns();
  serial->enqueue("left", 1100, 123, fallback_rx, 3, 0);
  provider.wait_event(devices, 5, false);
  auto fallback = provider.consume_imu_state(devices[0]);
  require(fallback.sequence == 1102 && fallback.latest_sample_timestamp_ns == fallback_rx &&
          !(fallback.data_flags & CONTROLLER_IMU_HOST_TIME_SYNCED) &&
          !(fallback.capability_flags & CONTROLLER_IMU_CAP_DEVICE_TIMESTAMP) &&
          !provider.has_pending_imu(devices[0]), "invalid acquisition time falsely synced/future");
  serial->enqueue("left", 0, 0, test_now_ns() - 300'000'000);
  provider.wait_event(devices, 5, false);
  require(provider.imu_state(devices[0]).status == CONTROLLER_IMU_STALE &&
          provider.imu_state(devices[0]).data_flags == 0 && !provider.has_pending_imu(devices[0]),
          "stale IMU queue retained");
  std::cout << "Serial provider -> SHM -> runtime, two clocks, burst/duplicate/reset/overflow/fallback/AHRS: OK\n";
}
