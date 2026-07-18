#pragma once

#include "xiao_nrf54l15_options.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xr_override_controller::xiao_nrf54l15 {

struct SerialDeviceSnapshot {
  std::string stable_id;
  std::string port_path;
  std::string by_id_path;
  std::string by_path;
  std::string name;
  std::string phys;
  std::string serial;
  uint16_t vendor = 0;
  uint16_t product = 0;
  uint16_t version = 0;
  bool explicit_candidate = false;
  bool connected = false;
  std::string error;
};

struct SerialBytes {
  std::string stable_id;
  std::vector<uint8_t> bytes;
  uint64_t host_timestamp_ns = 0;
};

class SerialTransport {
 public:
  virtual ~SerialTransport() = default;
  virtual std::string platform_name() const = 0;
  virtual std::vector<SerialDeviceSnapshot> devices() const = 0;
  virtual void pump(int timeout_ms, bool include_stdin,
                    bool* stdin_ready = nullptr) = 0;
  virtual bool pop_bytes(SerialBytes& bytes) = 0;
};

std::unique_ptr<SerialTransport> make_platform_serial_transport(
    const XiaoNrf54l15Options& options);

}  // namespace xr_override_controller::xiao_nrf54l15
