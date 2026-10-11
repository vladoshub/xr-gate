#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <deque>
#include <map>
#include <ostream>
#include <set>
#include <utility>

namespace xr_override_controller {

class LinuxEvdevInputProvider final : public InputProvider {
 public:
  explicit LinuxEvdevInputProvider(std::string input_directory = "/dev/input")
      : input_directory_(std::move(input_directory)) {}
  ~LinuxEvdevInputProvider() override;
  LinuxEvdevInputProvider(const LinuxEvdevInputProvider&) = delete;
  LinuxEvdevInputProvider& operator=(const LinuxEvdevInputProvider&) = delete;
  std::string provider_name() const override { return "evdev"; }
  std::vector<DeviceInfo> scan_devices(bool open_readable) override;
  void flush_events(std::vector<DeviceInfo>& devices) override;
  std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices, int timeout_ms, bool include_stdin) override;
  std::string input_name(const DeviceInfo& device, uint16_t type, uint16_t code) const override;
  InputBindingSpec make_input_spec(const DeviceInfo& device, uint16_t type, uint16_t code) const override;
  void close_devices(std::vector<DeviceInfo>& devices) override;
  bool set_device_grab(std::vector<DeviceInfo>& devices,
                       const std::set<size_t>& device_indices,
                       bool enabled,
                       std::ostream* log) override;
 private:
  struct OpenDevice {
    DeviceFingerprint fingerprint;
    uint64_t node_device = 0;
    uint64_t node_inode = 0;
    uint64_t node_rdev = 0;
    bool node_identity_valid = false;
  };
  void close_device(DeviceInfo& device);

  std::string input_directory_;
  // Every returned fd owns one reference to an evdev client. Scan candidates
  // duplicate the live file description rather than opening another client;
  // discarding an unchanged candidate then cannot wait in evdev_release().
  std::map<int, OpenDevice> open_devices_;
  std::deque<InputEvent> pending_events_;
  bool left_ctrl_down_ = false;
  bool right_ctrl_down_ = false;
};

}  // namespace xr_override_controller
