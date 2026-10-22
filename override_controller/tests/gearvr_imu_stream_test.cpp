#include "gearvr_imu_stream.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace xr_override_controller::gearvr;
using namespace xr_runtime;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

DecodedPacket packet(uint32_t first_us, uint32_t last_us) {
  // Exercise the real BLE decoder, including wrapping little-endian ticks.
  std::array<uint8_t, 60> bytes{};
  for (size_t i = 0; i < 2; ++i) {
    const uint32_t tick = i == 0 ? first_us : last_us;
    for (size_t j = 0; j < 4; ++j) bytes[i * 16 + j] = (tick >> (8 * j)) & 0xff;
    bytes[i * 16 + 9] = 8;  // +1 g on Z.
    bytes[i * 16 + 14] = 200;  // Nonstationary gyro: no startup bias subtraction.
    bytes[i * 16 + 15] = 3;
  }
  const auto decoded = decode_packet(bytes.data(), bytes.size());
  require(decoded.has_value(), "synthetic BLE packet did not decode");
  return *decoded;
}

void wrap_reset_and_fifo() {
  GearVrImuStream stream(0.0f);
  const uint64_t base_ns = 10'000'000'000ULL;
  const uint32_t first = 0xfffff000u;
  require(stream.push(packet(first, first + 5000u), base_ns), "wrap packet rejected");
  require(!stream.push(packet(first, first + 5000u), base_ns + 1000), "duplicate accepted");
  require(stream.latest.samples[1].device_timestamp_ticks == uint64_t(first) + 5000,
          "wrap inside packet was not extended");
  for (uint32_t i = 1; i < 20; ++i) {
    require(stream.push(packet(first + i * 10000, first + i * 10000 + 5000),
                        base_ns + uint64_t(i) * 10'000'000), "packet rejected");
  }
  uint64_t last_host = 0;
  for (uint64_t i = 0; i < 20; ++i) {
    require(stream.pending(), "notification lost before publication");
    const auto state = stream.consume();
    require(state.sequence == i + 1 && state.sample_count == 2, "FIFO order/count wrong");
    require(state.latest_device_timestamp_ticks == uint64_t(first) + i * 10000 + 5000,
            "extended device timestamp was lost");
    require((state.capability_flags & CONTROLLER_IMU_CAP_DEVICE_TIMESTAMP) != 0,
            "device clock capability missing");
    for (size_t j = 0; j < 2; ++j) {
      require(state.samples[j].timestamp_ns > last_host, "host sample order reversed");
      require(state.samples[j].timestamp_ns <= base_ns + i * 10'000'000, "future sample");
      last_host = state.samples[j].timestamp_ns;
    }
  }
  require(!stream.pending(), "FIFO not drained");
  const auto epoch = stream.latest.reserved[CONTROLLER_IMU_DIAG_EPOCH];
  require(stream.push(packet(100, 5100), base_ns + 300'000'000), "MCU reset rejected");
  require(stream.latest.latest_device_timestamp_ticks == 5100, "reset retained old clock epoch");
  require(stream.latest.reserved[CONTROLLER_IMU_DIAG_EPOCH] != epoch, "reset not reported");
  stream.reset();
  require(!stream.pending() && stream.latest.sample_count == 0, "reconnect retained old samples");
  require(stream.push(packet(1000, 6000), base_ns + 400'000'000), "reconnect failed");
  require(stream.latest.samples[0].timestamp_ns == base_ns + 395'000'000, "reconnect clock not reanchored");
}

void jitter_drift_and_ahrs() {
  GearVrImuStream jittered(0.0f), clean(0.0f);
  uint64_t previous_ns = 0;
  for (uint32_t i = 0; i < 5000; ++i) {
    const uint32_t first_us = 10000 + i * 10000;
    const auto decoded = packet(first_us, first_us + 5000);
    const uint64_t ideal_ns = 20'000'000'000ULL +
        uint64_t(std::llround((first_us + 5000) * 1000.0 * 1.000080));
    const uint64_t jitter_ns = i % 25 == 0 ? 0 : ((i * 7919) % 1'500'000);
    require(jittered.push(decoded, ideal_ns + jitter_ns), "jitter packet rejected");
    require(clean.push(decoded, ideal_ns), "clean packet rejected");
    const auto state = jittered.consume();
    clean.consume();
    require(state.samples[0].timestamp_ns > previous_ns, "mapped packets overlap");
    require(state.latest_sample_timestamp_ns <= ideal_ns + jitter_ns, "mapped time after receipt");
    previous_ns = state.latest_sample_timestamp_ns;
    for (size_t j = 0; j < 4; ++j) {
      require(std::abs(jittered.latest.orientation_xyzw[j] - clean.latest.orientation_xyzw[j]) < 1e-6f,
              "AHRS follows host receive jitter");
    }
    if (i > 4000) {
      require(std::abs(double(state.latest_sample_timestamp_ns) - double(ideal_ns)) < 100000,
              "mapped clock retains BLE jitter");
    }
  }
  require(std::abs(jittered.clock_diagnostics().drift_ppm - 80.0) < 25,
          "device drift estimate inaccurate");
}

void overflow_and_overlap() {
  GearVrImuStream stream(0.0f);
  for (uint32_t i = 0; i < GearVrImuStream::kMaxPendingBatches + 3; ++i) {
    stream.push(packet(10000 + i * 10000, 15000 + i * 10000),
                30'000'000'000ULL + uint64_t(i) * 10'000'000);
  }
  const auto first = stream.consume();
  require(first.sequence == 4 && first.reserved[CONTROLLER_IMU_DIAG_DROPPED_BATCHES] == 3,
          "overflow not reported or oldest batches not dropped");
  stream.clear_pending();
  const auto last_raw = uint32_t(stream.latest.latest_device_timestamp_ticks);
  require(stream.push(packet(last_raw, last_raw + 5000), 33'000'000'000ULL), "overlap rejected");
  require(stream.latest.sample_count == 1, "overlapping sample processed twice");
}

void missing_measurements_preserve_ahrs_reference() {
  GearVrImuStream stream(0.0f);
  const uint64_t base_ns = 40'000'000'000ULL;
  for (uint32_t i = 0; i < 30; ++i) {
    require(stream.push(packet(10000 + i * 10000, 15000 + i * 10000),
                        base_ns + uint64_t(i) * 10'000'000), "warmup packet rejected");
  }
  const auto before = stream.latest;
  require(std::abs(before.orientation_xyzw[2]) > 0.01f,
          "gap test did not build a non-identity orientation");
  const uint32_t last_us = static_cast<uint32_t>(before.latest_device_timestamp_ticks);
  GearVrImuStream reference = stream;
  reference.clear_pending();
  require(reference.push(packet(last_us + 5000, last_us + 10000),
                         before.latest_sample_timestamp_ns + 10'000'000),
          "reference packet rejected");
  require(stream.push(packet(last_us + 995000, last_us + 1000000),
                      before.latest_sample_timestamp_ns + 1'000'000'000),
          "post-gap packet rejected");
  require(stream.latest.reserved[CONTROLLER_IMU_DIAG_EPOCH] ==
              before.reserved[CONTROLLER_IMU_DIAG_EPOCH],
          "missing measurements changed the AHRS epoch");
  require(stream.dropped_batches() == 31, "missing interval/abandoned history not reported");
  for (size_t j = 0; j < 4; ++j) {
    require(std::abs(stream.latest.orientation_xyzw[j] - reference.latest.orientation_xyzw[j]) < 1e-6f,
            "post-gap AHRS reset or integrated gyro over the unobserved interval");
  }
  const auto returned = stream.consume();
  require(returned.sequence == 31 && !stream.pending(),
          "pre-gap history remained eligible for position integration");
}

void gravity_reference_controls_orientation_validity() {
  GearVrImuStream stream(0.04f);
  const uint64_t start_ns = 60'000'000'000ULL;
  auto moving = packet(10000, 15000);
  require(stream.push(moving, start_ns), "moving startup packet rejected");
  require((stream.latest.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) == 0u,
          "uninitialized startup orientation was published as valid");
  require(stream.latest.sample_count == 2 &&
          (stream.latest.data_flags & CONTROLLER_IMU_ACCELEROMETER_VALID) != 0u,
          "gravity startup gate discarded timed raw measurements");
  auto quiet = packet(20000, 25000);
  for (auto& sample : quiet.imu_samples) {
    sample.gyro_rad_s = {};
    sample.accel_m_s2 = {0, 8.492808f, -4.903325f};
  }
  require(stream.push(quiet, start_ns + 10'000'000), "quiet startup packet rejected");
  require((stream.latest.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) == 0u,
          "single quiet packet bypassed gyro startup confirmation");
  for (uint32_t i = 1; i <= 160; ++i) {
    for (auto& sample : quiet.imu_samples) sample.device_timestamp_us += 10000;
    require(stream.push(quiet, start_ns + uint64_t(i + 1) * 10'000'000),
            "stationary gyro warmup rejected");
  }
  require((stream.latest.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) != 0u,
          "initialized gravity reference was not published");
  require(stream.latest.reserved[CONTROLLER_IMU_DIAG_EPOCH] == 1,
          "initializing unadvertised orientation changed the clock epoch");
  stream.reset();
  require(stream.push(moving, start_ns + 2'000'000'000), "post-reset packet rejected");
  require((stream.latest.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) == 0u,
          "reset published an old gravity reference as valid");
}

void accelerometer_calibration_survives_clock_reset() {
  GearVrImuStream stream(0.04f);
  xr_override_controller::imu::AccelerometerCalibration calibration;
  calibration.enabled = true;
  calibration.bias_m_s2 = {0.1f, -0.2f, 0.4f};
  calibration.scale = {0.99f, 1.01f, 0.98f};
  stream.set_accelerometer_calibration(calibration);
  const uint64_t base_ns = 70'000'000'000ULL;
  auto biased_packet = [](uint32_t first_us) {
    auto decoded = packet(first_us, first_us + 5000);
    for (auto& sample : decoded.imu_samples) {
      sample.gyro_rad_s = {};
      sample.accel_m_s2 = {0.1f, -0.2f, 9.80665f / 0.98f + 0.4f};
    }
    return decoded;
  };
  for (int reset = 0; reset < 2; ++reset) {
    for (uint32_t i = 0; i <= 160; ++i) {
      require(stream.push(biased_packet((reset == 0 ? 10000 : 100) + i * 10000),
                          base_ns + uint64_t(reset) * 2'000'000'000 + uint64_t(i) * 10'000'000),
              "calibrated packet rejected");
      if (i < 160) stream.consume();
    }
    const auto state = stream.consume();
    require((state.data_flags & CONTROLLER_IMU_ACCELEROMETER_CALIBRATED) != 0,
            "batch calibration flag missing");
    require((state.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) != 0,
            "calibrated gravity did not initialize AHRS");
    for (size_t i = 0; i < state.sample_count; ++i) {
      const auto& sample = state.samples[i];
      require((sample.flags & CONTROLLER_IMU_ACCELEROMETER_CALIBRATED) != 0,
              "timed sample calibration flag missing");
      require(std::abs(sample.specific_force_m_s2[0]) < 1e-5f &&
              std::abs(sample.specific_force_m_s2[1]) < 1e-5f &&
              std::abs(sample.specific_force_m_s2[2] - 9.80665f) < 1e-4f,
              "published timed force lost device calibration on reset");
    }
    require(state.reserved[CONTROLLER_IMU_DIAG_EPOCH] == uint32_t(reset + 1),
            "clock reset did not create a new epoch");
  }
}

void captured_firmware_rollovers_keep_ahrs_epoch_and_history() {
  struct Event { uint32_t before, after; uint64_t gap_ns; };
  // Exact counter transitions and receive gaps from the 2026-10-13 capture.
  const Event events[] = {{419394225,7000,41'927'100},
                         {419429475,27850,23'136'600},
                         {419421425,34200,40'917'900},
                         {419420300,18675,28'540'100}};
  for (const auto& event : events) {
    GearVrImuStream wrapped(0.0f), reference(0.0f);
    const uint64_t host = 90'000'000'000ULL;
    require(wrapped.push(packet(event.before - 5000, event.before), host), "pre-wrap packet rejected");
    require(reference.push(packet(event.before - 5000, event.before), host), "reference pre-wrap rejected");
    const uint64_t extended = uint64_t(kTimestampWrapUs) + event.after;
    require(wrapped.push(packet(event.after - 5000, event.after), host + event.gap_ns),
            "captured Gear VR counter rollover rejected");
    require(reference.push(packet(uint32_t(extended - 5000), uint32_t(extended)), host + event.gap_ns),
            "continuous reference rejected");
    require(wrapped.epoch() == 1 && wrapped.clock_wraps() == 1,
            "normal 419430400-us rollover restarted AHRS epoch");
    require(wrapped.latest.latest_device_timestamp_ticks == extended && wrapped.dropped_batches() == 0,
            "rollover did not extend time or invented a measurement gap");
    for (size_t axis = 0; axis < 4; ++axis) {
      require(std::abs(wrapped.latest.orientation_xyzw[axis] - reference.latest.orientation_xyzw[axis]) < 1e-6f,
              "rollover reset/integrated a wrong AHRS interval");
    }
    require(wrapped.consume().sequence == 1 && wrapped.consume().sequence == 2 && !wrapped.pending(),
            "rollover discarded unpublished history");
    require(!wrapped.push(packet(event.after - 5000, event.after), host + event.gap_ns + 1000),
            "duplicate boundary packet counted twice");
    require(wrapped.clock_wraps() == 1, "duplicate incremented rollover count");
  }
}

void firmware_rollover_inside_notification_and_at_startup() {
  for (bool startup : {false, true}) {
    GearVrImuStream wrapped(0.0f), reference(0.0f);
    const uint64_t host = 95'000'000'000ULL;
    if (!startup) {
      require(wrapped.push(packet(kTimestampWrapUs - 11000, kTimestampWrapUs - 6000), host),
              "pre-boundary packet rejected");
      require(reference.push(packet(kTimestampWrapUs - 11000, kTimestampWrapUs - 6000), host),
              "reference packet rejected");
    }
    const auto boundary = packet(kTimestampWrapUs - 1975, 3025);
    require(wrapped.push(boundary, host + 10'000'000), "rollover inside BLE notification rejected");
    require(reference.push(packet(kTimestampWrapUs - 1975, kTimestampWrapUs + 3025), host + 10'000'000),
            "reference boundary packet rejected");
    require(wrapped.epoch() == 1 && wrapped.clock_wraps() == 1 && wrapped.latest.sample_count == 2,
            "intra-notification rollover changed epoch/sample count");
    require(wrapped.latest.samples[0].device_timestamp_ticks == kTimestampWrapUs - 1975 &&
            wrapped.latest.samples[1].device_timestamp_ticks == uint64_t(kTimestampWrapUs) + 3025,
            "boundary samples lost their chronological acquisition times");
    for (size_t axis = 0; axis < 4; ++axis) {
      require(std::abs(wrapped.latest.orientation_xyzw[axis] - reference.latest.orientation_xyzw[axis]) < 1e-6f,
              "intra-notification rollover changed AHRS");
    }
  }
}

void two_firmware_periods_preserve_clock_mapping_and_filter() {
  GearVrImuStream wrapped(0.0f), reference(0.0f);
  const uint64_t host = 100'000'000'000ULL;
  uint64_t last_host = 0;
  const uint64_t end = uint64_t(kTimestampWrapUs) * 2 + 50'000;
  for (uint64_t first = 10000; first < end; first += 10000) {
    const uint64_t receive = host + (first - 10000) * 1000;
    require(wrapped.push(packet(uint32_t(first % kTimestampWrapUs),
                                uint32_t((first + 5000) % kTimestampWrapUs)), receive),
            "continuous two-period firmware stream rejected a packet");
    require(reference.push(packet(uint32_t(first), uint32_t(first + 5000)), receive),
            "two-period reference rejected a packet");
    const auto state = wrapped.consume(); reference.consume();
    require(wrapped.epoch() == 1 && wrapped.dropped_batches() == 0,
            "long-running counter rollover changed epoch or dropped data");
    require(state.latest_device_timestamp_ticks == first + 5000,
            "second firmware rollover lost the accumulated device time");
    for (size_t i = 0; i < state.sample_count; ++i) {
      require(state.samples[i].timestamp_ns > last_host && state.samples[i].timestamp_ns <= receive,
              "rollover produced reversed/future host sample time");
      last_host = state.samples[i].timestamp_ns;
    }
    for (size_t axis = 0; axis < 4; ++axis) {
      require(std::abs(state.orientation_xyzw[axis] - reference.latest.orientation_xyzw[axis]) < 1e-6f,
              "long-running rollover changed the AHRS trajectory");
    }
  }
  require(wrapped.clock_wraps() == 2, "two firmware counter periods were not detected exactly once");
  require(std::abs(wrapped.clock_diagnostics().scale - reference.clock_diagnostics().scale) < 1e-10,
          "firmware rollover restarted/changed the affine clock model");
}

void boundary_overlap_before_startup_origin_skips_only_the_old_record() {
  GearVrImuStream wrapped(0.0f), reference(0.0f);
  const uint64_t host = 105'000'000'000ULL;
  require(wrapped.push(packet(1000,6000),host), "post-wrap startup rejected");
  require(reference.push(packet(1000,6000),host), "overlap reference startup rejected");
  require(wrapped.push(packet(kTimestampWrapUs - 3000,7000),host + 1'000'000),
          "older pre-startup boundary record prevented fresh sample delivery");
  require(reference.push(packet(6000,7000),host + 1'000'000), "overlap reference rejected");
  require(wrapped.epoch() == 1 && wrapped.clock_wraps() == 0 && wrapped.latest.sample_count == 1 &&
          wrapped.latest.samples[0].device_timestamp_ticks == 7000,
          "pre-startup overlap underflowed time or reprocessed old data");
  for (size_t axis = 0; axis < 4; ++axis) {
    require(std::abs(wrapped.latest.orientation_xyzw[axis] - reference.latest.orientation_xyzw[axis]) < 1e-6f,
            "pre-startup overlap invented a huge AHRS interval");
  }
}

void real_restart_and_measurement_gap_near_firmware_boundary() {
  const uint64_t host = 110'000'000'000ULL;
  GearVrImuStream restarted(0.0f);
  require(restarted.push(packet(kTimestampWrapUs - 1000000, kTimestampWrapUs - 995000), host),
          "pre-restart packet rejected");
  require(restarted.push(packet(50000,55000), host + 10'000'000), "real reboot packet rejected");
  require(restarted.epoch() == 2 && restarted.clock_wraps() == 0 &&
          restarted.latest.latest_device_timestamp_ticks == 55000,
          "real counter reboot was incorrectly treated as continuous rollover");
  GearVrImuStream gap(0.0f), reference(0.0f);
  const uint32_t previous = kTimestampWrapUs - 100000;
  require(gap.push(packet(previous - 5000, previous), host), "pre-gap packet rejected");
  require(reference.push(packet(previous - 5000, previous), host), "reference pre-gap rejected");
  require(gap.push(packet(295000,300000), host + 400'000'000), "gap across rollover rejected");
  require(reference.push(packet(previous + 5000, previous + 10000), host + 10'000'000),
          "gap reference rejected");
  require(gap.epoch() == 1 && gap.clock_wraps() == 1 && gap.dropped_batches() == 2,
          "rollover concealed a real missing interval or restarted AHRS");
  for (size_t axis = 0; axis < 4; ++axis) {
    require(std::abs(gap.latest.orientation_xyzw[axis] - reference.latest.orientation_xyzw[axis]) < 1e-6f,
            "missing interval at rollover was integrated or reset AHRS");
  }
  const auto epoch = gap.epoch();
  require(!gap.push(packet(kTimestampWrapUs - 1000,200000), host + 410'000'000),
          "malformed boundary notification was accepted");
  require(gap.epoch() == epoch, "rejected malformed packet changed epoch");
}

int main() {
  captured_firmware_rollovers_keep_ahrs_epoch_and_history();
  firmware_rollover_inside_notification_and_at_startup();
  two_firmware_periods_preserve_clock_mapping_and_filter();
  boundary_overlap_before_startup_origin_skips_only_the_old_record();
  real_restart_and_measurement_gap_near_firmware_boundary();
  accelerometer_calibration_survives_clock_reset();
  gravity_reference_controls_orientation_validity();
  wrap_reset_and_fifo();
  jitter_drift_and_ahrs();
  overflow_and_overlap();
  missing_measurements_preserve_ahrs_reference();
  std::cout << "Gear VR timestamps, drift, AHRS intervals, FIFO and measurement gaps: OK\n";
}
