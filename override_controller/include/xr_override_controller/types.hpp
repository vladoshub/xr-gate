#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <xr_runtime/contracts/controller_input_contract.hpp>

namespace xr_override_controller {

constexpr uint64_t kButtonTrigger = 1ull << 0;
constexpr uint64_t kButtonGrip = 1ull << 1;
constexpr uint64_t kButtonMenu = 1ull << 2;
constexpr uint64_t kButtonA = 1ull << 3;
constexpr uint64_t kButtonB = 1ull << 4;
constexpr uint64_t kButtonThumbstick = 1ull << 5;
constexpr uint64_t kButtonDpadUp = 1ull << 6;
constexpr uint64_t kButtonDpadDown = 1ull << 7;
constexpr uint64_t kButtonDpadLeft = 1ull << 8;
constexpr uint64_t kButtonDpadRight = 1ull << 9;
constexpr uint64_t kButtonDpadCenter = 1ull << 10;
constexpr uint64_t kButtonX = 1ull << 11;
constexpr uint64_t kButtonY = 1ull << 12;
constexpr uint64_t kButtonSystem = 1ull << 13;

struct DeviceFingerprint {
  std::string platform = "linux";
  std::string backend = "evdev";
  std::string event_path;
  std::string by_id_path;
  std::string by_path;
  std::string name;
  std::string phys;
  std::string uniq;
  uint16_t bustype = 0;
  uint16_t vendor = 0;
  uint16_t product = 0;
  uint16_t version = 0;
  uint64_t stable_hash = 0;
};

struct DeviceInfo {
  DeviceFingerprint fingerprint;

  // Runtime-only routing metadata used by CompositeInputProvider. It is never
  // serialized into the user config; stable identity remains in fingerprint.
  size_t provider_slot = 0;
  size_t provider_device_index = 0;

  // A provider may know a stable device identity while its transport is
  // disconnected (paired BLE controller). Such a device remains matchable to
  // config bindings, but `readable` stays false until input is flowing.
  bool identity_known = false;

  // Native handle used by some providers (Linux evdev). External/BLE providers
  // can remain readable with fd == -1.
  int fd = -1;
  bool readable = false;
  std::string open_error;
};

enum class InputKind { Key, AbsAxis, RelAxis };

struct InputBindingSpec {
  InputKind kind = InputKind::Key;
  uint16_t type = 0;
  uint16_t code = 0;
  std::string name;
  int abs_min = 0;
  int abs_max = 0;
  int abs_flat = 0;
  int abs_direction = 0;  // -1/0/+1 for axis-backed button bindings.
};

enum class ControllerSide { Left, Right };

enum class ControllerAction {
  Trigger,
  Grip,
  Menu,
  A,
  B,
  X,
  Y,
  System,
  ThumbstickClick,
  ThumbstickTouch,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  DpadCenter,
  ThumbstickX,
  ThumbstickY,
};

struct DeviceInputConfig {
  // Per-physical-device pulse/hold behavior. Every field defaults to zero
  // (or false/empty), so devices without an explicit input block use raw
  // input semantics with no synthetic hold, grace, pulse bridging, or debounce.
  uint32_t rel_axis_hold_ms = 0;
  uint32_t rel_button_hold_ms = 0;
  uint32_t button_hold_ms = 0;
  uint32_t button_release_grace_ms = 0;

  bool pulse_mode = false;
  uint32_t dpad_pulse_gap_ms = 0;
  uint32_t dpad_release_ms = 0;
  uint32_t button_pulse_gap_ms = 0;
  uint32_t button_release_ms = 0;

  uint32_t button_pulse_startup_ms = 0;
  uint32_t button_pulse_startup_release_ms = 0;
  std::vector<ControllerAction> button_pulse_startup_types;

  uint32_t hold_toggle_debounce_ms = 0;
};

struct OrientationBasisRotationConfig {
  double rx_deg = 0.0;
  double ry_deg = 0.0;
  double rz_deg = 0.0;
};

struct OrientationTransformConfig {
  bool enabled = false;
  bool invert_x = false;
  bool invert_y = false;
  bool invert_z = false;
  OrientationBasisRotationConfig basis_rotation;
};

struct ConfigDevice {
  int id = 0;
  DeviceFingerprint fingerprint;
  DeviceInputConfig input;
  OrientationTransformConfig orientation_transform;

  // Explicit IMU routing is independent from button/axis bindings. This lets
  // an IMU-only provider (for example MPU-6050) feed one controller side
  // without exposing any input buttons. Missing/"none" means unassigned.
  std::optional<ControllerSide> imu_side;

  // Runtime-only migration marker: old configs did not contain imu_side.
  // Explicit "none" must remain distinct from a missing legacy field.
  bool imu_side_explicit = false;
};

struct BindingConfig {
  ControllerSide side = ControllerSide::Left;
  ControllerAction action = ControllerAction::Trigger;
  int device_id = 0;
  DeviceFingerprint device;
  InputBindingSpec input;
};

struct LayoutSwitchConfig {
  bool enabled = false;
  int device_id = 0;
  DeviceFingerprint device;
  InputBindingSpec input;
};

struct PublishConfig {
#if defined(_WIN32)
  std::string transport = "tcp";
#else
  std::string transport = "shm";
#endif
  std::string registry_path;
  std::string stream_id = "controller_input";
  std::string shm_name = "controller_input";
  std::string tcp_bind_host = "127.0.0.1";
  int tcp_port = 45672;
  uint32_t slot_count = 32;
  double rate_hz = 90.0;
  bool unlink_existing = true;
};

struct InputConfig {
  // Linux evdev: use EVIOCGRAB on devices that matched configured bindings.
  // EVIOCGRAB is device-wide; it cannot grab only individual bound keys.
  bool grab_devices = false;

  // Allow bindings for both virtual controller sides to resolve to the same
  // physical input device.
  bool allow_shared_physical_device_sides = true;

  // Periodically rescan/re-resolve configured devices after reconnect.
  bool reattach_devices = false;
  uint32_t reattach_interval_ms = 1000;

  // Event wait cap for the service loop. Publish cadence is configured
  // independently in PublishConfig.
  uint32_t event_wait_max_ms = 20;
};

struct AppConfig {
  std::string name = "default";

  // Runtime-only: load_config_file inferred imu_side for a legacy config and
  // the caller may persist the migrated schema atomically.
  bool migrated_imu_side = false;
  bool migrated_orientation_transform = false;
  bool migrated_gearvr_touch_bindings = false;
  PublishConfig publish;
  InputConfig input;
  std::vector<ConfigDevice> devices;
  LayoutSwitchConfig layout_switch;
  std::vector<BindingConfig> bindings;
  std::vector<BindingConfig> alternative_bindings;

  // Optional alternative bindings where one click toggles a virtual long press
  // until the next click. These bindings have priority over normal bindings
  // when they use the same physical input on the same controller side.
  std::vector<BindingConfig> hold_toggle_bindings;
  std::vector<BindingConfig> alternative_hold_toggle_bindings;
};

struct SideOutputState {
  bool configured = false;
  bool connected = false;
  uint64_t buttons = 0;
  uint64_t touches = 0;
  uint64_t changed_buttons = 0;
  float trigger = 0.0f;
  float grip = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  uint32_t press_counters[32] = {};
  uint32_t release_counters[32] = {};
  std::string device_id;

  // Defaults to CONTROLLER_IMU_NOT_SUPPORTED. A motion-controller provider
  // sets this independently for each side so V3 consumers can distinguish a
  // buttons-only controller from an IMU-capable controller even while its IMU
  // data is configured, connected, stale, or lost.
  xr_runtime::ControllerImuStateV1 imu;
};

struct OutputState {
  SideOutputState left;
  SideOutputState right;
};

std::string to_string(ControllerSide side);
std::string to_string(ControllerAction action);
ControllerSide parse_side(const std::string& s);
ControllerAction parse_action(const std::string& s);
uint64_t button_bit_for_action(ControllerAction action);
bool is_axis_action(ControllerAction action);

uint64_t stable_hash64(const std::string& s);
std::string hex_u64(uint64_t v);
std::string short_device_label(const DeviceFingerprint& fp);
int fingerprint_match_score(const DeviceFingerprint& wanted, const DeviceFingerprint& candidate);

}  // namespace xr_override_controller
