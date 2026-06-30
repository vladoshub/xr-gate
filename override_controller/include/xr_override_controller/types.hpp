#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  DpadCenter,
  ThumbstickX,
  ThumbstickY,
};

struct BindingConfig {
  ControllerSide side = ControllerSide::Left;
  ControllerAction action = ControllerAction::Trigger;
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
  // This prevents mapped HID/media/mouse events from also reaching the desktop/Steam.
  // Other platforms should implement this through their platform input provider.
  bool grab_devices = false;

  // Allow bindings for both virtual controller sides to resolve to the same
  // physical input device. When true, the same physical key/button may drive
  // both left and right if it is explicitly bound for both sides.
  bool allow_shared_physical_device_sides = true;

  // Periodically rescan/re-resolve configured devices. This lets Bluetooth HID
  // devices recover after disconnect/reconnect because /dev/input/eventX often
  // changes while stable fingerprint fields such as uniq/by-id remain usable.
  bool reattach_devices = false;
  uint32_t reattach_interval_ms = 1000;

  // Event wait cap for the service loop. Larger values reduce idle CPU wakeups;
  // publish cadence still follows PublishConfig::rate_hz.
  uint32_t event_wait_max_ms = 20;

  // Mouse-style EV_REL stick axes are deltas, not held states. Keep them
  // active for this short pulse window after the latest delta event.
  uint32_t rel_axis_hold_ms = 160;

  // Mouse-style EV_REL D-pad/button actions are also deltas, but games often
  // need enough hold time to detect press-and-hold actions reliably.
  uint32_t rel_button_hold_ms = 800;

  // Minimum visible hold for digital/button-like actions. This prevents very
  // short Bluetooth/evdev press+release pairs from disappearing between two
  // 90 Hz publish ticks.
  uint32_t button_hold_ms = 120;

  // Some Bluetooth/media/mouse-style controllers emit repeated short key
  // pulses while the user physically holds a button. Keep a released button
  // logically active for this grace window so the next repeat pulse can bridge
  // into the same controller_input hold. Default 0 keeps normal keys
  // responsive; set 1000-1500 ms for pulse-only devices during diagnostics.
  uint32_t button_release_grace_ms = 0;

  // Generic pulse-source mode for remote/mouse-style controllers that emit a
  // stream of short pulses instead of stable button down/up state. When enabled,
  // D-pad and button bindings get separate short release windows to bridge the
  // expected inter-pulse gaps without using long, sticky global holds.
  bool pulse_mode = false;
  uint32_t dpad_pulse_gap_ms = 130;
  uint32_t dpad_release_ms = 140;
  uint32_t button_pulse_gap_ms = 180;
  uint32_t button_release_ms = 190;

  // Some pulse-style controllers have a slow/irregular repeat cadence during
  // the first seconds after a physical button is held, then become stable.
  // When non-zero, button pulse-mode uses this larger release timeout only
  // during the initial per-binding warmup window. Keep defaults disabled.
  // If button_pulse_startup_types is empty, the warmup applies to all
  // non-D-pad button-like actions for backward compatibility. Otherwise it
  // applies only to listed action names, e.g. ["trigger"].
  uint32_t button_pulse_startup_ms = 0;
  uint32_t button_pulse_startup_release_ms = 0;
  std::vector<ControllerAction> button_pulse_startup_types;

  // Hold-toggle bindings are driven by click edges. Pulse-style controllers may
  // emit repeated edges while the physical button is still held, sometimes with
  // large early gaps. Keep the toggle disarmed for this window after each pulse
  // so one physical hold/click cannot toggle on/off repeatedly.
  uint32_t hold_toggle_debounce_ms = 1500;
};

struct AppConfig {
  std::string name = "default";
  PublishConfig publish;
  InputConfig input;
  std::vector<BindingConfig> bindings;

  // Optional alternative bindings where one click toggles a virtual long press
  // until the next click. These bindings have priority over normal bindings
  // when they use the same physical input on the same controller side.
  std::vector<BindingConfig> hold_toggle_bindings;
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
