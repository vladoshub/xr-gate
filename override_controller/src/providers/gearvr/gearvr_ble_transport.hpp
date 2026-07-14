#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace xr_override_controller::gearvr {

struct BleDeviceSnapshot {
  std::string stable_id;
  std::string address;
  std::string name;
  std::string platform;
  bool paired = false;
  bool trusted = false;
  bool connected = false;
  bool notifications_active = false;
  std::string error;
};

struct BlePacket {
  std::string stable_id;
  std::vector<uint8_t> bytes;
  uint64_t timestamp_ns = 0;
};

class BleTransport {
 public:
  virtual ~BleTransport() = default;

  virtual std::string platform_name() const = 0;
  virtual void pump(int timeout_ms, bool include_stdin, bool* stdin_ready) = 0;
  virtual std::vector<BleDeviceSnapshot> devices() const = 0;
  virtual bool pop_packet(BlePacket& packet) = 0;
};

// Exactly one platform translation unit defines this factory. Adding Windows
// support only requires a Windows transport implementing BleTransport.
std::unique_ptr<BleTransport> make_platform_ble_transport(const InputProviderOptions& options);

}  // namespace xr_override_controller::gearvr
