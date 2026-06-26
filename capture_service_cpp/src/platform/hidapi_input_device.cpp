#include "capture_service_cpp/platform/hid_input_device.hpp"

#include <hidapi/hidapi.h>

#include <stdexcept>

namespace xr_capture_cpp {

struct HidInputDevice::Impl {
  hid_device* dev = nullptr;
};

HidInputDevice::HidInputDevice() : impl_(new Impl()) {
  if (hid_init() != 0) throw std::runtime_error("hid_init failed");
}

HidInputDevice::~HidInputDevice() {
  if (impl_ && impl_->dev) hid_close(impl_->dev);
  hid_exit();
}

void HidInputDevice::open_interface(uint16_t vendor_id, uint16_t product_id, int interface_number, const std::string& label) {
  hid_device_info* infos = hid_enumerate(vendor_id, product_id);
  hid_device_info* cur = infos;
  const char* selected = nullptr;
  while (cur) {
    if (cur->interface_number == interface_number && cur->path) {
      selected = cur->path;
      break;
    }
    cur = cur->next;
  }
  if (selected) impl_->dev = hid_open_path(selected);
  hid_free_enumeration(infos);
  if (!impl_->dev) throw std::runtime_error("failed to open " + label);
}

int HidInputDevice::write(const uint8_t* data, size_t size) {
  if (!impl_->dev) throw std::runtime_error("HID device is not open");
  return hid_write(impl_->dev, data, size);
}

int HidInputDevice::read_timeout(uint8_t* data, size_t size, int timeout_ms) {
  if (!impl_->dev) throw std::runtime_error("HID device is not open");
  return hid_read_timeout(impl_->dev, data, size, timeout_ms);
}

}  // namespace xr_capture_cpp
