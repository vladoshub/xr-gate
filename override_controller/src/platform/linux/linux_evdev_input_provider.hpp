#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <ostream>
#include <set>
#include <deque>

namespace xr_override_controller {

class LinuxEvdevInputProvider final : public InputProvider {
 public:
  std::vector<DeviceInfo> scan_devices(bool open_readable) override;
  void flush_events(std::vector<DeviceInfo>& devices) override;
  std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices, int timeout_ms, bool include_stdin) override;
  std::string input_name(uint16_t type, uint16_t code) const override;
  InputBindingSpec make_input_spec(const DeviceInfo& device, uint16_t type, uint16_t code) const override;
  void close_devices(std::vector<DeviceInfo>& devices) override;
  bool set_device_grab(std::vector<DeviceInfo>& devices,
                       const std::set<size_t>& device_indices,
                       bool enabled,
                       std::ostream* log) override;
 private:
  std::deque<InputEvent> pending_events_;
  bool left_ctrl_down_ = false;
  bool right_ctrl_down_ = false;
};

}  // namespace xr_override_controller
