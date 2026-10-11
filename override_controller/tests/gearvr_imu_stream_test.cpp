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
  require((stream.latest.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) != 0u,
          "initialized gravity reference was not published");
  require(stream.latest.reserved[CONTROLLER_IMU_DIAG_EPOCH] == 1,
          "initializing unadvertised orientation changed the clock epoch");
  stream.reset();
  require(stream.push(moving, start_ns + 20'000'000), "post-reset packet rejected");
  require((stream.latest.data_flags & CONTROLLER_IMU_ORIENTATION_VALID) == 0u,
          "reset published an old gravity reference as valid");
}

int main() {
  gravity_reference_controls_orientation_validity();
  wrap_reset_and_fifo();
  jitter_drift_and_ahrs();
  overflow_and_overlap();
  missing_measurements_preserve_ahrs_reference();
  std::cout << "Gear VR timestamps, drift, AHRS intervals, FIFO and measurement gaps: OK\n";
}
