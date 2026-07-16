#pragma once

#include <map>
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

using ProviderOptionValues = std::map<std::string, std::string>;

struct InputProviderOptions {
  // Ordered list of enabled providers. Unknown names are rejected instead of
  // silently falling back to platform input, so release launchers cannot appear
  // to enable a provider that is actually inactive.
  std::vector<std::string> providers;

  // Provider-owned string options keyed by canonical or alias provider name.
  // The core only parses `provider.key=value`; each provider validates and
  // converts its own values. Adding a provider therefore does not add fields or
  // provider-specific branches to main.cpp or the common interface.
  std::map<std::string, ProviderOptionValues> provider_options;
};

struct ConfigMigrationResult {
  bool changed = false;
  std::vector<std::string> notes;
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
  virtual std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices,
                                                int timeout_ms,
                                                bool include_stdin) = 0;

  // Device is part of the name lookup so a composite provider can route the
  // request to the child that owns the event instead of assuming the first
  // child understands every provider's input codes.
  virtual std::string input_name(const DeviceInfo& device,
                                 uint16_t type,
                                 uint16_t code) const = 0;
  virtual InputBindingSpec make_input_spec(const DeviceInfo& device,
                                           uint16_t type,
                                           uint16_t code) const = 0;

  // Provider-owned migrations keep protocol/input-code knowledge out of the
  // generic JSON loader. The composite provider invokes every enabled child.
  virtual ConfigMigrationResult migrate_config(AppConfig& cfg) const {
    (void)cfg;
    return {};
  }

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
