#include "capture_service_cpp/imu_pipeline.hpp"

#include "capture_service_cpp/platform/hid_input_device.hpp"
#include "capture_service_cpp/vendor/xreal_imu_codec.hpp"

#include <array>
#include <iostream>

namespace xr_capture_cpp {

void imu_thread(const RuntimeConfig& cfg, StreamPublishers* publishers) {
  HidInputDevice dev;
  dev.open_interface(kXrealHidVendorId, kXrealHidProductId, kXrealImuInterfaceNumber, "XREAL HID IMU interface 2");

  const auto& start_cmd = xreal_imu_start_command();
  const int wr = dev.write(start_cmd.data(), start_cmd.size());
  std::cerr << "[capture_service_cpp] imu start command write result=" << wr << std::endl;

  const int drop_first = env_int("CPP_CAPTURE_IMU_DROP_FIRST_PACKETS", kXrealDefaultImuDropFirstPackets);
  uint64_t raw_count = 0;
  uint64_t imu_count = 0;
  std::array<uint8_t, kXrealHidPacketSize> packet{};

  while (!g_stop.load()) {
    const int n = dev.read_timeout(packet.data(), packet.size(), 50);
    if (n <= 0) continue;
    const uint64_t ts = steady_ns();
    ++raw_count;
    if (cfg.raw_hid_enabled) publishers->publish("xreal_raw_hid", packet.data(), static_cast<size_t>(n), ts, 0, 0, kFormatBytes, 0, "xreal_raw_hid");
    if (static_cast<int>(raw_count) <= drop_first) continue;

    float payload[6] = {};
    if (!normalize_xreal_imu_packet(packet.data(), static_cast<size_t>(n), payload)) continue;
    publishers->publish("imu0", reinterpret_cast<const uint8_t*>(payload), sizeof(payload), ts, 0, 0, kFormatImuF32Le, 0, "imu0");
    ++imu_count;
    if (imu_count % 1000 == 0) std::cerr << "[capture_service_cpp] imu raw=" << raw_count << " normalized=" << imu_count << std::endl;
  }
}

}  // namespace xr_capture_cpp
