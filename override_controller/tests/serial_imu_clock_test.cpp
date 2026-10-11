#include <xr_tracking/timing/serial_imu_clock.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using Clock = xr_tracking::SerialImuClock;
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main() {
  Clock clock;
  // MCU uptime exceeds host uptime: the clock offset must be negative.
  constexpr uint64_t device_us = 9'000'000'000ULL;
  constexpr uint64_t receive_ns = 5'000'000'000ULL;
  auto batch = clock.map_batch({{0xfffffffeu, device_us, true},
      {0xffffffffu, device_us + 4808, true}, {0, device_us + 9616, true}}, receive_ns);
  require(batch.size() == 3, "first read lost samples or mistook sequence wrap for reset");
  require(batch[1].host_ns - batch[0].host_ns == 4'808'000 &&
          batch[2].host_ns - batch[1].host_ns == 4'808'000, "batch intervals collapsed");
  require(batch.back().host_ns == receive_ns, "batch timestamp occurs after receipt");
  const auto epoch = clock.epoch();
  require(clock.map_batch({{0, device_us + 9616, true}}, receive_ns).empty(), "duplicate sequence accepted");
  require(clock.map_batch({{1, device_us + 9616, true}}, receive_ns).empty(), "duplicate acquisition tick accepted");
  batch = clock.map_batch({{2, device_us + 14424, true}}, receive_ns + 4'808'000);
  require(batch.size() == 1 && clock.epoch() == epoch, "duplicates changed clock epoch");
  require(std::abs(batch[0].device_interval_seconds - 0.004808f) < 1e-7f, "device AHRS interval wrong");
  batch = clock.map_batch({{4, device_us + 24040, true}}, receive_ns + 14'424'000);
  require(clock.dropped_samples() == 1, "sequence gap not reported");

  // A reset inside one read discards records from its old clock epoch.
  batch = clock.map_batch({{5, device_us + 28848, true}, {0, 0, true},
                          {1, 4808, true}}, receive_ns + 24'040'000);
  require(batch.size() == 2 && batch[0].index == 1 && clock.epoch() != epoch,
          "mixed reboot epochs fused");
  require(batch[0].device_time_valid && batch[0].device_interval_seconds == 0,
          "valid zero acquisition timestamp rejected");
  require(batch[1].host_ns - batch[0].host_ns == 4'808'000, "reboot batch spacing wrong");
  batch = clock.map_batch({{2, 111, false}, {3, 222, false}}, receive_ns + 30'000'000);
  require(batch.size() == 1 && batch[0].index == 1 && !batch[0].device_time_valid &&
          batch[0].host_ns == receive_ns + 30'000'000, "fallback pretends acquisition timing is known");
  require(clock.map_batch({{4, std::numeric_limits<uint64_t>::max(), true}}, receive_ns + 40'000'000).empty(),
          "microsecond conversion overflow accepted");

  Clock rejected;
  rejected.map_batch({{100, 100'000, true}}, receive_ns);
  require(rejected.map_batch({{101, std::numeric_limits<uint64_t>::max(), true}},
                            receive_ns + 4'808'000).empty(), "overflow packet not rejected");
  rejected.map_batch({{101, std::numeric_limits<uint64_t>::max(), true}}, receive_ns + 4'808'000);
  batch = rejected.map_batch({{102, 109'616, true}}, receive_ns + 9'616'000);
  require(batch.size() == 1 && rejected.dropped_samples() == 1 && rejected.epoch() == 1,
          "rejected packet counted twice or reset clock");

  Clock compressed;
  compressed.map_batch({{0, 0, true}, {1, 4808, true}}, receive_ns);
  const auto compressed_epoch = compressed.epoch();
  batch = compressed.map_batch({{2, 9616, true}, {3, 14424, true}}, receive_ns + 100'000);
  require(batch.size() == 2 && compressed.epoch() != compressed_epoch &&
          batch[1].host_ns - batch[0].host_ns == 4'808'000 &&
          batch.back().host_ns <= receive_ns + 100'000 && batch[0].device_interval_seconds == 0,
          "arrival discontinuity compressed intervals or emitted future time");

  Clock fit;
  uint64_t previous = 0;
  for (uint32_t read = 0; read < 6000; ++read) {
    const uint64_t newest_us = device_us + read * 14424ULL;
    const uint64_t true_ns = receive_ns + static_cast<uint64_t>(read * 14'424'000.0 * 1.000080);
    const uint64_t rx = true_ns + 100'000 + (read % 37) * 25'000;
    auto mapped = fit.map_batch({{read * 3, newest_us, true},
        {read * 3 + 1, newest_us + 4808, true}, {read * 3 + 2, newest_us + 9616, true}}, rx + 9'616'769);
    require(mapped.size() == 3, "drift/jitter lost batch");
    for (const auto& sample : mapped) {
      require(sample.host_ns > previous && sample.host_ns <= rx + 9'616'769,
              "mapped stream not monotonic or exceeds receipt");
      previous = sample.host_ns;
    }
  }
  require(std::abs(fit.diagnostics().drift_ppm - 80) < 25, "drift fit failed");
  require(fit.epoch() == 1 && fit.dropped_samples() == 0, "normal jitter resets stream");
  std::cout << "Serial batch intervals, negative offset, wrap/reset, duplicates, fallback and drift: OK\n";
}
