#include "capture_service_cpp/sources/imu_source.hpp"
#include "capture_service_cpp/protocols/xr_controller_v1.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace xr_capture_cpp {
uint64_t steady_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::unique_ptr<IImuSource> make_serial_imu_source(const RuntimeConfig& cfg);
}

int main() {
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  assert(master >= 0);
  assert(grantpt(master) == 0);
  assert(unlockpt(master) == 0);
  const char* slave = ptsname(master);
  assert(slave != nullptr);

  xr_capture_cpp::XrControllerIdentityV1 identity;
  identity.flags = xr_capture_cpp::kXrControllerIdentityV1DeviceUidValid;
  identity.controller_protocol_version = xr_capture_cpp::kXrControllerV1Version;
  identity.device_uid_size = 8;
  identity.device_uid = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  std::array<uint8_t, xr_capture_cpp::kXrControllerIdentityV1PacketSize>
      identity_packet{};
  assert(xr_capture_cpp::encode_xr_controller_identity_v1(
      identity, identity_packet.data(), identity_packet.size()));

  xr_capture_cpp::RuntimeConfig cfg;
  cfg.imu.driver = "serial";
  cfg.imu.serial.port = slave;
  cfg.imu.serial.baud_rate = 230400;
  cfg.imu.serial.protocol = "xr_controller_v1";
  cfg.imu.serial.protocol_device_uid = "0123456789abcdef";
  cfg.imu.serial.read_timeout_ms = 20;
  auto source = xr_capture_cpp::make_serial_imu_source(cfg);

  std::atomic<bool> probing{true};
  std::thread identity_writer([&]() {
    while (probing.load()) {
      (void)write(master, identity_packet.data(), identity_packet.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });
  source->open();
  probing.store(false);
  identity_writer.join();
  xr_capture_cpp::ImuReadResult result;
  assert(write(master, identity_packet.data(), identity_packet.size()) ==
         static_cast<ssize_t>(identity_packet.size()));
  assert(source->read(result) == xr_capture_cpp::SourceReadStatus::TransportActivity);
  assert(!result.has_sample);

  xr_capture_cpp::XrControllerV1Sample sample;
  sample.flags = xr_capture_cpp::kXrControllerV1TimestampValid;
  sample.sequence = 7;
  sample.device_timestamp_us = 123456;
  sample.gyro_rad_s = {1.0f, 2.0f, 3.0f};
  sample.accel_m_s2 = {4.0f, 5.0f, 6.0f};
  sample.buttons = xr_capture_cpp::XrControllerV1ButtonA;
  std::array<uint8_t, xr_capture_cpp::kXrControllerV1PacketSize> packet{};
  assert(xr_capture_cpp::encode_xr_controller_v1(sample, packet.data(), packet.size()));

  assert(write(master, packet.data(), 10) == 10);
  assert(source->read(result) == xr_capture_cpp::SourceReadStatus::TransportActivity);
  assert(!result.has_sample);

  assert(write(master, packet.data() + 10, packet.size() - 10) ==
         static_cast<ssize_t>(packet.size() - 10));
  assert(source->read(result) == xr_capture_cpp::SourceReadStatus::Data);
  assert(result.has_sample);
  assert(result.sample.source_sequence == 7);
  assert(result.raw_packet.size() == xr_capture_cpp::kXrControllerV1PacketSize);

  const std::array<uint8_t, 12> garbage{{1,2,3,4,5,6,7,8,9,10,11,12}};
  assert(write(master, garbage.data(), garbage.size()) == static_cast<ssize_t>(garbage.size()));
  assert(source->read(result) == xr_capture_cpp::SourceReadStatus::TransportActivity);
  assert(!result.has_sample);

  source->close();
  close(master);
  return 0;
}
