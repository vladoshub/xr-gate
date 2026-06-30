#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <vector>

#include <xr_override_controller/types.hpp>

namespace xr_override_controller {

struct InputEvent {
  size_t device_index = 0;
  uint16_t type = 0;
  uint16_t code = 0;
  int32_t value = 0;
  int64_t timestamp_ns = 0;

  // Provider-side emergency shutdown event. Linux evdev can exclusively grab a
  // keyboard, which prevents the terminal from receiving its normal SIGINT.
  // The provider detects reserved escape keys before normal binding handling.
  bool stop_requested = false;
};

class InputProvider {
 public:
  virtual ~InputProvider() = default;
  virtual std::vector<DeviceInfo> scan_devices(bool open_readable) = 0;
  virtual void flush_events(std::vector<DeviceInfo>& devices) = 0;
  virtual std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices, int timeout_ms, bool include_stdin) = 0;
  virtual std::string input_name(uint16_t type, uint16_t code) const = 0;
  virtual InputBindingSpec make_input_spec(const DeviceInfo& device, uint16_t type, uint16_t code) const = 0;

  // Release provider-owned OS handles stored in DeviceInfo. This keeps reattach
  // platform-neutral: the core can rescan devices without knowing whether the
  // provider uses POSIX fds, Windows HANDLEs, or another native resource.
  virtual void close_devices(std::vector<DeviceInfo>& devices) {
    (void)devices;
  }

  // Optional platform-specific exclusive input capture. On Linux/evdev this maps
  // to EVIOCGRAB. Platforms that do not support this yet should keep the
  // default no-op behavior and let the caller continue normally.
  virtual bool set_device_grab(std::vector<DeviceInfo>& devices,
                               const std::set<size_t>& device_indices,
                               bool enabled,
                               std::ostream* log) {
    (void)devices;
    (void)device_indices;
    (void)enabled;
    if (log) *log << "[override_controller][WARN] exclusive device grab is not supported on this platform yet\n";
    return false;
  }
};

std::unique_ptr<InputProvider> make_platform_input_provider();

}  // namespace xr_override_controller
