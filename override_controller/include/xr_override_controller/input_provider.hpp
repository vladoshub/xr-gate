#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
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

struct InputProviderOptions {
  // Ordered list of enabled providers. Unknown names are rejected instead of
  // silently falling back to platform input, so release launchers cannot appear
  // to enable a provider that is actually inactive.
  std::vector<std::string> providers;

  // Native Gear VR provider settings shared by all OS transports. Linux uses
  // BlueZ/system D-Bus; a future Windows backend can use C++/WinRT without
  // changing packet decoding, touchpad handling, AHRS, training, or bindings.
  uint32_t gearvr_initial_scan_ms = 1500;
  uint32_t gearvr_reconnect_ms = 1000;
  std::string gearvr_touchpad_mode = "relative_stick";
  double gearvr_touchpad_deadzone = 0.12;
  double gearvr_touchpad_radius = 90.0;
  bool gearvr_touchpad_invert_x = false;
  bool gearvr_touchpad_invert_y = true;
  double gearvr_madgwick_beta = 0.04;
};

class InputProvider {
 public:
  virtual ~InputProvider() = default;
  virtual std::string provider_name() const = 0;

  // Providers backed by an external transport may need polling even while no
  // currently configured device is readable (for example, BLE reconnect).
  virtual bool requires_polling() const { return false; }
  virtual std::vector<DeviceInfo> scan_devices(bool open_readable) = 0;
  virtual void flush_events(std::vector<DeviceInfo>& devices) = 0;
  virtual std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices, int timeout_ms, bool include_stdin) = 0;
  virtual std::string input_name(uint16_t type, uint16_t code) const = 0;
  virtual InputBindingSpec make_input_spec(const DeviceInfo& device, uint16_t type, uint16_t code) const = 0;

  // Return the current per-device IMU state. Buttons-only providers keep the
  // default NOT_SUPPORTED state. The composite provider routes the request to
  // the provider that owns DeviceInfo.
  virtual xr_runtime::ControllerImuStateV1 imu_state(const DeviceInfo& device) const {
    (void)device;
    return {};
  }

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
    if (log) *log << "[override_controller][WARN] exclusive device grab is not supported by provider '"
                  << provider_name() << "'\n";
    return false;
  }
};

std::unique_ptr<InputProvider> make_input_provider(const InputProviderOptions& options);

}  // namespace xr_override_controller
