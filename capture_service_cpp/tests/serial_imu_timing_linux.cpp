#include "capture_service_cpp/sources/imu_source.hpp"
#include "capture_service_cpp/protocols/xr_controller_v1.hpp"
#include "capture_service_cpp/protocols/xr_imu_v1.hpp"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace xr_capture_cpp {
uint64_t receipt_override_ns = 0;
uint64_t steady_ns() {
  if (receipt_override_ns) return receipt_override_ns;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::unique_ptr<IImuSource> make_serial_imu_source(const RuntimeConfig& cfg);
}
using namespace xr_capture_cpp;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

std::vector<uint8_t> bytes(const std::string& protocol, uint32_t sequence,
                           uint64_t start_us, uint8_t flags = 1) {
  std::vector<uint8_t> out;
  for (uint32_t i = 0; i < 3; ++i) {
    if (protocol == "xr_controller_v1") {
      XrControllerV1Sample s;
      s.sequence = sequence + i; s.device_timestamp_us = start_us + i * 4808;
      s.flags = flags; s.accel_m_s2[2] = 9.81f;
      std::array<uint8_t, kXrControllerV1PacketSize> packet{};
      require(encode_xr_controller_v1(s, packet.data(), packet.size()), "encode XCTL");
      out.insert(out.end(), packet.begin(), packet.end());
    } else {
      XrImuV1Sample s;
      s.sequence = sequence + i; s.device_timestamp_us = start_us + i * 4808;
      s.flags = flags; s.accel_m_s2[2] = 9.81f;
      std::array<uint8_t, kXrImuV1PacketSize> packet{};
      require(encode_xr_imu_v1(s, packet.data(), packet.size()), "encode XIMU");
      out.insert(out.end(), packet.begin(), packet.end());
    }
  }
  return out;
}

void run(const std::string& protocol) {
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  require(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "PTY open");
  RuntimeConfig cfg;
  cfg.imu.serial.port = ptsname(master);
  cfg.imu.serial.protocol = protocol;
  cfg.imu.serial.timestamp_mode = "device";
  cfg.imu.serial.baud_rate = 230400;
  cfg.imu.serial.read_timeout_ms = 20;
  auto source = make_serial_imu_source(cfg);
  source->open();
  const uint64_t start_us = steady_ns() / 1000 + 10'000'000;  // Negative offset.
  const uint64_t first_rx_ns = steady_ns();
  auto first = bytes(protocol, 0xfffffffeu, start_us);
  // Fragment the first packet and corrupt an earlier complete packet: neither
  // transport activity nor invalid CRC may anchor the acquisition clock.
  auto corrupt = first;
  corrupt[20] ^= 1;
  require(write(master, corrupt.data(), protocol == "xr_controller_v1" ? 64 : 48) > 0, "write CRC");
  ImuReadResult result;
  require(source->read(result) == SourceReadStatus::TransportActivity, "CRC sample published");
  require(write(master, first.data(), 10) == 10, "write fragment");
  require(source->read(result) == SourceReadStatus::TransportActivity, "partial sample published");
  receipt_override_ns = first_rx_ns;
  require(write(master, first.data() + 10, first.size() - 10) == static_cast<ssize_t>(first.size() - 10), "write batch");
  std::array<ImuReadResult, 3> samples;
  for (auto& s : samples) require(source->read(s) == SourceReadStatus::Data, "read batch");
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto& s = samples[i];
    require(s.sample.source_timestamp_valid && s.sample.source_sequence == static_cast<uint32_t>(0xfffffffeu + i), "flags/sequence wrap");
    require(s.sample.timestamp_ns <= s.receive_timestamp_ns, "future sample");
    require(s.receive_timestamp_ns == samples[0].receive_timestamp_ns, "PTY split complete batch");
    require(s.raw_packet == std::vector<uint8_t>(first.begin() + i * s.raw_packet.size(), first.begin() + (i + 1) * s.raw_packet.size()), "raw packet changed");
    if (i) require(s.sample.timestamp_ns - samples[i - 1].sample.timestamp_ns == 4'808'000, "first batch timing collapsed");
  }
  const auto packet_size = samples[2].raw_packet.size();
  require(write(master, first.data() + 2 * packet_size, packet_size) == static_cast<ssize_t>(packet_size), "write duplicate");
  require(source->read(result) == SourceReadStatus::TransportActivity, "duplicate published");
  // Zero time is valid at reboot; all three samples must remain spaced.
  receipt_override_ns = first_rx_ns + 20'000'000;
  auto reboot = bytes(protocol, 0, 0);
  require(write(master, reboot.data(), reboot.size()) == static_cast<ssize_t>(reboot.size()), "write reboot");
  for (auto& s : samples) require(source->read(s) == SourceReadStatus::Data, "read reboot");
  require(samples[1].sample.timestamp_ns - samples[0].sample.timestamp_ns == 4'808'000, "reboot spacing");
  // Another epoch arriving too soon overlaps the old capture stream. Keep
  // only its non-overlapping tail, preserving monotonic VIO input.
  receipt_override_ns = first_rx_ns + 20'100'000;
  require(write(master, reboot.data(), reboot.size()) == static_cast<ssize_t>(reboot.size()), "write overlapping reboot");
  require(source->read(result) == SourceReadStatus::Data && result.sample.source_sequence == 2 &&
          result.sample.timestamp_ns > samples[2].sample.timestamp_ns, "capture epoch emitted backwards time");
  receipt_override_ns = first_rx_ns + 30'000'000;
  auto unsynced = bytes(protocol, 3, 123, 0);
  require(write(master, unsynced.data(), unsynced.size()) == static_cast<ssize_t>(unsynced.size()), "write no clock");
  require(source->read(result) == SourceReadStatus::Data && !result.sample.source_timestamp_valid && result.sample.source_sequence == 5, "invalid clock falsely synced");
  source->close();
  close(master);
  receipt_override_ns = 0;
}

int main() {
  run("xr_controller_v1");
  run("xr_imu_v1");
  std::cout << "Real serial/PTY batch time, raw bytes, CRC, fragmentation, wrap, duplicate and reboot: OK\n";
}
