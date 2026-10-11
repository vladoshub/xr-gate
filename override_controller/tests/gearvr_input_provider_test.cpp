#include "gearvr_ble_transport.hpp"
#include "gearvr_input_provider.hpp"
#include "gearvr_imu_stream.hpp"
#include "composite_input_provider.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace xr_override_controller;
using namespace xr_override_controller::gearvr;
using namespace xr_runtime;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

class FakeTransport : public BleTransport {
 public:
  std::vector<BleDeviceSnapshot> snapshots;
  std::deque<BlePacket> packets;
  std::string platform_name() const override { return "test"; }
  void pump(int, bool, bool* stdin_ready) override {
    if (stdin_ready) *stdin_ready = false;
  }
  std::vector<BleDeviceSnapshot> devices() const override { return snapshots; }
  bool pop_packet(BlePacket& packet) override {
    if (packets.empty()) return false;
    packet = std::move(packets.front());
    packets.pop_front();
    return true;
  }
  void enqueue(const std::string& id, uint32_t newest_us, uint64_t host_ns,
               bool rotating = false) {
    BlePacket packet;
    packet.stable_id = id;
    packet.timestamp_ns = host_ns;
    packet.bytes.resize(60);
    for (size_t i = 0; i < 2; ++i) {
      const uint32_t timestamp = newest_us - (1 - i) * 5000;
      for (size_t j = 0; j < 4; ++j) packet.bytes[i * 16 + j] = (timestamp >> (j * 8)) & 0xff;
      packet.bytes[i * 16 + 9] = 8;
      if (rotating) {
        packet.bytes[i * 16 + 14] = 200;
        packet.bytes[i * 16 + 15] = 3;
      }
    }
    packets.push_back(std::move(packet));
  }
};

FakeTransport* transport = nullptr;
namespace xr_override_controller::gearvr {
std::unique_ptr<BleTransport> make_platform_ble_transport(const GearVrOptions&) {
  auto fake = std::make_unique<FakeTransport>();
  transport = fake.get();
  for (const auto* id : {"left", "right"}) {
    BleDeviceSnapshot device;
    device.stable_id = id;
    device.platform = "test";
    device.name = "Gear VR Controller";
    device.connected = device.notifications_active = true;
    fake->snapshots.push_back(device);
  }
  return fake;
}
}  // namespace xr_override_controller::gearvr

uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

void routing_reconnect_and_flush() {
  std::vector<std::unique_ptr<InputProvider>> children;
  children.push_back(std::make_unique<GearVrInputProvider>(ProviderOptionValues{{"initial_scan_ms", "0"}}));
  CompositeInputProvider provider(std::move(children));
  auto devices = provider.scan_devices(true);
  require(devices.size() == 2, "two Gear VR sessions not discovered");
  const uint64_t base_ns = now_ns() - 50'000'000;
  for (uint32_t i = 0; i < 5; ++i) {
    transport->enqueue("left", 10000 + i * 10000, base_ns + i * 10'000'000);
    transport->enqueue("right", 1010000 + i * 10000, base_ns + i * 10'000'000);
  }
  provider.wait_event(devices, 5, false);
  require(provider.imu_state(devices[0]).sequence == 5, "snapshot not latest batch");
  for (uint64_t i = 0; i < 5; ++i) {
    for (size_t side = 0; side < 2; ++side) {
      require(provider.has_pending_imu(devices[side]), "provider drained IMU history prematurely");
      const auto state = provider.consume_imu_state(devices[side]);
      require(state.sequence == i + 1, "composite/provider FIFO order wrong");
      require(state.latest_device_timestamp_ticks == (side ? 1010000 : 10000) + i * 10000,
              "two controllers share device clock state");
    }
  }
  require(!provider.has_pending_imu(devices[0]), "FIFO remains after drain");
  const auto epoch = provider.imu_state(devices[0]).reserved[CONTROLLER_IMU_DIAG_EPOCH];
  transport->enqueue("left", 60000, now_ns());
  provider.wait_event(devices, 5, false);
  transport->snapshots[0].connected = false;
  transport->snapshots[0].notifications_active = false;
  provider.wait_event(devices, 5, false);
  require(!provider.has_pending_imu(devices[0]) &&
          provider.imu_state(devices[0]).status == CONTROLLER_IMU_LOST,
          "disconnect retained queued active samples");
  provider.flush_events(devices);  // Consume/discard queued button releases.
  transport->snapshots[0].connected = transport->snapshots[0].notifications_active = true;
  transport->enqueue("left", 10000, now_ns());
  provider.wait_event(devices, 5, false);
  require(provider.imu_state(devices[0]).reserved[CONTROLLER_IMU_DIAG_EPOCH] != epoch,
          "reconnect did not change clock epoch");
  require(provider.consume_imu_state(devices[0]).latest_device_timestamp_ticks == 10000,
          "reconnect extended previous session's counter");
  transport->enqueue("left", 20000, now_ns());
  provider.flush_events(devices);
  require(!provider.has_pending_imu(devices[0]), "explicit flush retained historical IMU");
}

void require_same_orientation(const ControllerImuStateV1& a,
                              const ControllerImuStateV1& b) {
  for (size_t j = 0; j < 4; ++j) {
    require(std::abs(a.orientation_xyzw[j] - b.orientation_xyzw[j]) < 1e-6f,
            "delivery pause reset or changed device-timed AHRS");
  }
}

void delayed_delivery_preserves_ahrs() {
  GearVrInputProvider provider({{"initial_scan_ms", "0"}, {"madgwick_beta", "0"}});
  auto devices = provider.scan_devices(true);
  GearVrImuStream clean(0.0f);
  const uint64_t base_ns = now_ns() - 100'000'000;
  for (uint32_t i = 0; i < 8; ++i) {
    // The controller keeps sampling at 200 Hz; a host stall delays receipt
    // from the fourth notification onward by 300 ms.
    if (i == 3) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t ideal_ns = base_ns + uint64_t(i) * 10'000'000;
    const uint64_t received_ns = ideal_ns + (i >= 3 ? 300'000'000 : 0);
    transport->enqueue("left", 10000 + i * 10000, received_ns, true);
    const auto decoded = decode_packet(transport->packets.back().bytes.data(), 60);
    require(clean.push(*decoded, ideal_ns), "reference AHRS packet rejected");
    provider.wait_event(devices, 1, false);
    const auto state = provider.imu_state(devices[0]);
    require(state.reserved[CONTROLLER_IMU_DIAG_EPOCH] == 1,
            "host receive pause changed the IMU epoch");
    require_same_orientation(state, clean.latest);
  }
  for (uint64_t i = 1; i <= 8; ++i) {
    require(provider.consume_imu_state(devices[0]).sequence == i,
            "host receive pause discarded queued continuous measurements");
  }
}

void stale_output_preserves_filter_and_bias() {
  GearVrInputProvider provider({{"initial_scan_ms", "0"}, {"madgwick_beta", "0"}});
  auto devices = provider.scan_devices(true);
  // Older receive timestamps make the provider stale deterministically,
  // without waiting for a wall-clock timeout in the test.
  const uint64_t base_ns = now_ns() - 1'000'000'000;
  for (uint32_t i = 0; i < 30; ++i) {
    transport->enqueue("left", 10000 + i * 10000,
                       base_ns + uint64_t(i) * 10'000'000);
    provider.wait_event(devices, 1, false);
  }
  const auto stale = provider.imu_state(devices[0]);
  require(stale.status == CONTROLLER_IMU_STALE && stale.data_flags == 0 &&
          stale.sample_count == 0 && !provider.has_pending_imu(devices[0]),
          "stale provider advertises historical samples as active");
  require(stale.reserved[CONTROLLER_IMU_DIAG_EPOCH] == 1,
          "stale output reset the AHRS epoch");
  transport->enqueue("left", 310000, now_ns());
  provider.wait_event(devices, 1, false);
  const auto resumed = provider.imu_state(devices[0]);
  require(resumed.status == CONTROLLER_IMU_ACTIVE &&
          (resumed.data_flags & CONTROLLER_IMU_GYROSCOPE_CALIBRATED) != 0,
          "delivery recovery restarted gyro bias calibration");
  require(resumed.reserved[CONTROLLER_IMU_DIAG_EPOCH] == 1,
          "delivery recovery reset the IMU epoch a second time");
  require_same_orientation(resumed, stale);
}

int main() {
  routing_reconnect_and_flush();
  delayed_delivery_preserves_ahrs();
  stale_output_preserves_filter_and_bias();
  std::cout << "Gear VR routing, reconnect, flush, delayed delivery and stale recovery: OK\n";
}
