#pragma once

#include <xr_override_controller/input_provider.hpp>

namespace xr_override_controller {

class WindowsKeyboardInputProvider final : public InputProvider {
 public:
  std::vector<DeviceInfo> scan_devices(bool open_readable) override;
  void flush_events(std::vector<DeviceInfo>& devices) override;
  std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices, int timeout_ms, bool include_stdin) override;
  std::string input_name(uint16_t type, uint16_t code) const override;
  InputBindingSpec make_input_spec(const DeviceInfo& device, uint16_t type, uint16_t code) const override;

 private:
  bool previous_[256]{};
};

}  // namespace xr_override_controller
