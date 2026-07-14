#include "capture_service_cpp/timing/affine_device_clock_mapper.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>

using namespace xr_capture_cpp;

int main() {
  AffineDeviceClockMapper mapper;

  constexpr double kTrueScale = 1.000080;  // host clock vs device clock: +80 ppm.
  constexpr uint64_t kOffsetNs = 5000000000ULL;
  constexpr uint64_t kStepDeviceNs = 2500000ULL;  // 400 Hz.

  uint64_t previous = 0;
  uint64_t last_receive = 0;
  for (uint64_t i = 0; i < 5000; ++i) {
    const uint64_t device_ns = i * kStepDeviceNs;
    const uint64_t true_host = kOffsetNs +
        static_cast<uint64_t>(kTrueScale * static_cast<double>(device_ns));
    // Positive transport jitter with a recurring low-delay observation. The
    // lower-envelope estimator should recover the clock slope, not the jitter.
    const uint64_t transport_delay = 100000ULL + (i % 37) * 25000ULL;
    last_receive = true_host + transport_delay;
    const uint64_t mapped = mapper.map_ns(device_ns, last_receive);
    assert(mapped > previous || i == 0);
    assert(mapped <= last_receive || mapped == previous + 1);
    previous = mapped;
  }

  const auto diagnostics = mapper.diagnostics();
  assert(diagnostics.initialized);
  assert(diagnostics.anchor_count >= 20);
  assert(std::abs(diagnostics.drift_ppm - 80.0) < 25.0);

  // A device timestamp rollback is treated as a new clock epoch.
  const uint64_t reset_mapped = mapper.map_ns(1000ULL, last_receive + 100000000ULL);
  assert(reset_mapped > 0);
  assert(mapper.diagnostics().reset_count >= 1);
  return 0;
}
