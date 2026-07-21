#include "capture_service_cpp/backend_control.hpp"
#include "capture_service_cpp/vendor/xreal_imu_codec.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

void write_s24le(std::array<uint8_t, xr_capture_cpp::kXrealHidPacketSize>& packet,
                 size_t offset,
                 int32_t value) {
  const uint32_t encoded = static_cast<uint32_t>(value) & 0x00ffffffU;
  packet[offset + 0] = static_cast<uint8_t>(encoded & 0xffU);
  packet[offset + 1] = static_cast<uint8_t>((encoded >> 8U) & 0xffU);
  packet[offset + 2] = static_cast<uint8_t>((encoded >> 16U) & 0xffU);
}

bool close_enough(double a, double b, double tolerance = 1.0e-5) {
  return std::abs(a - b) <= tolerance;
}

}  // namespace

int main() {
  using namespace xr_capture_cpp;

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "capture_service_backend_control_smoke.json";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  BackendControlReader reader(path, 1);
  reader.poll_if_due(1000000ULL);
  if (!close_enough(reader.snapshot().gravity_magnitude,
                    kDefaultGravityMagnitudeMps2)) {
    std::cerr << "missing-file fallback gravity mismatch\n";
    return 1;
  }

  {
    std::ofstream out(path);
    out << "{\"gravity_magnitude\":10.0,\"reset_counter\":7}\n";
  }
  reader.poll_if_due(2000000ULL);
  if (!close_enough(reader.snapshot().gravity_magnitude, 10.0) ||
      reader.snapshot().reset_counter != 7) {
    std::cerr << "late backend-control reload failed\n";
    return 2;
  }

  std::array<uint8_t, kXrealHidPacketSize> packet{};
  // XREAL scale is +/-16 g over signed 24-bit full scale. 524288 counts is 1 g.
  write_s24le(packet, 33, 524288);

  float payload[6]{};
  if (!normalize_xreal_imu_packet(packet.data(), packet.size(),
                                  reader.snapshot().gravity_magnitude, payload)) {
    std::cerr << "packet normalization failed\n";
    return 3;
  }
  if (!close_enough(payload[3], 10.0)) {
    std::cerr << "dynamic gravity scaling failed: " << payload[3] << "\n";
    return 4;
  }

  std::filesystem::remove(path, ec);
  return 0;
}
