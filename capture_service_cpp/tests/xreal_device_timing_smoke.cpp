#include "capture_service_cpp/timing/xreal_device_clock.hpp"
#include "capture_service_cpp/vendor/xreal_camera_metadata.hpp"
#include "capture_service_cpp/vendor/xreal_imu_codec.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace xr_capture_cpp;

namespace {
void require(bool value) {
  if (!value) throw std::runtime_error("XREAL device timing check failed");
}
void put_le(uint8_t* p, uint64_t value, size_t size) {
  for (size_t i = 0; i < size; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
}

int main() {
  constexpr uint64_t device_epoch = 1000000000000ULL;
  constexpr uint64_t host_epoch = 9000000000000ULL;
  std::array<uint8_t, 256> report{};
  report[0] = 1;
  report[1] = 2;
  put_le(report.data() + 4, device_epoch, 8);
  put_le(report.data() + 18, 0xffffff, 3);  // Signed -1 gyro count.
  put_le(report.data() + 33, 524288, 3);  // +1 g.
  uint64_t decoded = 0;
  float imu64[6]{}, imu256[6]{};
  require(decode_xreal_imu_timestamp(report.data(), 64, decoded) && decoded == device_epoch);
  require(decode_xreal_imu_timestamp(report.data(), 256, decoded) && decoded == device_epoch);
  require(normalize_xreal_imu_packet(report.data(), 64, 9.80665, imu64));
  require(normalize_xreal_imu_packet(report.data(), 256, 9.80665, imu256));
  require(imu64[0] < 0 && std::abs(imu64[3] - 9.80665) < 1e-5);
  for (int i = 0; i < 6; ++i) require(imu64[i] == imu256[i]);
  require(!decode_xreal_imu_timestamp(report.data(), 63, decoded));
  require(!normalize_xreal_imu_packet(report.data(), 65, 9.80665, imu64));
  report[0] = 0xaa;  // Command reply is not a motion sample.
  require(!decode_xreal_imu_timestamp(report.data(), 64, decoded));
  require(!normalize_xreal_imu_packet(report.data(), 256, 9.80665, imu64));
  report[0] = 1;
  put_le(report.data() + 4, 0, 8);
  require(!decode_xreal_imu_timestamp(report.data(), 256, decoded));
  put_le(report.data() + 4, std::numeric_limits<uint64_t>::max(), 8);
  require(!decode_xreal_imu_timestamp(report.data(), 256, decoded));

  std::array<uint8_t, 1280> footer{};
  put_le(footer.data(), device_epoch, 8);
  put_le(footer.data() + 18, 65535, 2);
  footer[59] = 1;
  put_le(footer.data() + 62, device_epoch + 9965000, 8);
  XrealCameraMetadata meta;
  require(decode_xreal_camera_metadata(footer.data(), footer.size(), meta));
  require(meta.t0_ns == device_epoch && meta.t62_ns == device_epoch + 9965000);
  require(meta.eye == 1 && meta.pair_counter == 65535);
  require(!decode_xreal_camera_metadata(footer.data(), 69, meta));
  footer[59] = 2;
  require(!decode_xreal_camera_metadata(footer.data(), footer.size(), meta));
  footer[59] = 1;
  put_le(footer.data() + 62, 0, 8);
  require(!decode_xreal_camera_metadata(footer.data(), footer.size(), meta));

  using PairResult = XrealStereoTimestampPairer::Result;
  XrealStereoTimestampPairer pairs;
  auto eye = [](uint64_t t, uint16_t counter, uint8_t side) {
    return XrealCameraMetadata{t - 100, t, counter, side};
  };
  require(pairs.push(eye(1000, 65535, 1)) == PairResult::Pending);
  require(pairs.push(eye(1000, 65535, 1)) == PairResult::Discard);  // Duplicate.
  require(pairs.push(eye(1000, 65535, 0)) == PairResult::Complete);  // Right first.
  require(pairs.push(eye(1000, 65535, 0)) == PairResult::Discard);
  require(pairs.push(eye(2000, 0, 0)) == PairResult::Pending);  // Counter wraps.
  require(pairs.push(eye(3000, 1, 0)) == PairResult::Pending);  // Missing eye at 2000.
  require(pairs.push(eye(2000, 0, 1)) == PairResult::Discard);  // Late old eye.
  require(pairs.push(eye(3000, 2, 1)) == PairResult::Discard);  // Same t, wrong counter.
  require(pairs.push(eye(3000, 1, 1)) == PairResult::Complete);
  require(pairs.dropped() >= 5);

  XrealDeviceClock clock;
  require(!clock.map_camera_ns(device_epoch, host_epoch));
  require(!clock.map_imu_ns(0, host_epoch));
  const auto first = clock.map_imu_ns(device_epoch, host_epoch + 100000);
  require(first && *first == host_epoch + 100000);
  const auto earlier = clock.map_camera_ns(device_epoch - 3000000, host_epoch + 8000000);
  const auto delayed = clock.map_camera_ns(device_epoch - 3000000, host_epoch + 30000000);
  require(earlier && delayed && *earlier == *delayed && *first - *earlier == 3000000);
  require(clock.diagnostics().reset_count == 0);
  require(!clock.map_camera_ns(1000, host_epoch + 30000000));  // Wrong epoch/units.
  require(!clock.map_imu_ns(device_epoch, host_epoch + 1000000));  // Duplicate.

  constexpr double scale = 1.000080;
  for (uint64_t i = 1; i < 12000; ++i) {
    const uint64_t step = i * 1000000ULL;
    const uint64_t d = device_epoch + step;
    const uint64_t h = host_epoch + static_cast<uint64_t>(scale * step);
    const auto mapped_imu = clock.map_imu_ns(d, h + 100000 + (i % 37) * 25000);
    const auto mapped_camera = clock.map_camera_ns(d - 3000000, h + 8000000);
    require(mapped_imu && mapped_camera);
    const double sensor_delta = static_cast<double>(*mapped_imu - *mapped_camera);
    require(std::abs(sensor_delta - scale * 3000000) < 2000);
  }
  const auto diagnostics = clock.diagnostics();
  require(diagnostics.anchor_count >= 20 && std::abs(diagnostics.drift_ppm - 80) < 25);
  require(diagnostics.reset_count == 0);  // Late cameras must never reset the HID model.

  // Both source threads can access one model; reads must not mutate it.
  std::thread reader([&] {
    for (int i = 0; i < 1000; ++i) require(clock.diagnostics().initialized);
  });
  for (int i = 0; i < 1000; ++i) {
    require(clock.map_camera_ns(device_epoch + 11999000000ULL, host_epoch + 12010000000ULL).has_value());
  }
  reader.join();
  bool rollback_failed = false;
  try { clock.map_imu_ns(device_epoch, host_epoch + 13000000000ULL); }
  catch (const std::runtime_error&) { rollback_failed = true; }
  require(rollback_failed);
  return 0;
}
