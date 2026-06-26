#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace xr_capture_cpp {

class HidInputDevice {
 public:
  HidInputDevice();
  ~HidInputDevice();

  HidInputDevice(const HidInputDevice&) = delete;
  HidInputDevice& operator=(const HidInputDevice&) = delete;

  void open_interface(uint16_t vendor_id, uint16_t product_id, int interface_number, const std::string& label);
  int write(const uint8_t* data, size_t size);
  int read_timeout(uint8_t* data, size_t size, int timeout_ms);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xr_capture_cpp
