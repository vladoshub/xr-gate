#include <xr_override_controller/config_io.hpp>
#include <xr_override_controller/controller_input_publisher.hpp>
#include <xr_override_controller/input_provider.hpp>
#include <xr_override_controller/types.hpp>
#include <xr_runtime/registry/runtime_paths.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/input.h>
#else
constexpr uint16_t EV_KEY = 1;
constexpr uint16_t EV_REL = 2;
constexpr uint16_t EV_ABS = 3;
constexpr uint16_t KEY_ENTER = 13;
constexpr uint16_t KEY_KPENTER = 13;
#endif

namespace fs = std::filesystem;
using namespace xr_override_controller;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Args {
  fs::path config_dir = default_config_dir();
  fs::path config_path;
  std::string config_name = "default";
  std::string publish_registry;
  std::string publish_stream;
  std::string publish_shm_name;
  std::string publish_transport;
  std::string publish_tcp_bind_host;
  int publish_tcp_port = 0;
  double publish_rate_hz = 0.0;
  uint32_t publish_slot_count = 0;
  bool grab_devices_override_set = false;
  bool grab_devices = false;
  bool reattach_devices_override_set = false;
  bool reattach_devices = true;
  uint32_t reattach_interval_ms = 0;
  uint32_t event_wait_max_ms = 0;
  uint32_t rel_axis_hold_ms = 0;
  uint32_t rel_button_hold_ms = 0;
  uint32_t button_hold_ms = 0;
  uint32_t button_release_grace_ms = 0;
  bool pulse_mode_override_set = false;
  bool pulse_mode = false;
  uint32_t dpad_pulse_gap_ms = 0;
  uint32_t dpad_release_ms = 0;
  uint32_t button_pulse_gap_ms = 0;
  uint32_t button_release_ms = 0;
  uint32_t button_pulse_startup_ms = 0;
  uint32_t button_pulse_startup_release_ms = 0;
  std::vector<ControllerAction> button_pulse_startup_types;
  uint32_t hold_toggle_debounce_ms = 0;
  bool rel_axis_hold_ms_set = false;
  bool rel_button_hold_ms_set = false;
  bool button_hold_ms_set = false;
  bool button_release_grace_ms_set = false;
  bool dpad_pulse_gap_ms_set = false;
  bool dpad_release_ms_set = false;
  bool button_pulse_gap_ms_set = false;
  bool button_release_ms_set = false;
  bool button_pulse_startup_ms_set = false;
  bool button_pulse_startup_release_ms_set = false;
  bool button_pulse_startup_types_set = false;
  bool hold_toggle_debounce_ms_set = false;
  bool train = false;
  bool list_devices = false;
  bool non_interactive = false;
  bool verbose = false;
};

void usage() {
  std::cout <<
      "override_controller\n"
      "  --config-dir <dir>       Config directory. Default: $XDG_CONFIG_HOME/xr_tracking/override_controller\n"
      "  --config <file>          Use one config file directly\n"
      "  --name <name>            New config name when training. Default: default\n"
      "  --train                  Force interactive training\n"
      "  --list-devices           Print readable Linux evdev devices and exit\n"
      "  --publish-registry <p>   Tracking registry path for controller_input SHM\n"
      "  --publish-stream <name>  Stream id written into the registry. Default: controller_input\n"
      "  --publish-shm-name <n>   POSIX SHM name. Default: controller_input\n"
      "  --publish-transport <shm|tcp> Publish transport. Linux default: shm, Windows default: tcp\n"
      "  --publish-tcp-bind-host <host> TCP bind host for --publish-transport tcp. Default: 127.0.0.1\n"
      "  --publish-tcp-port <port> TCP port for --publish-transport tcp. Default: 45672\n"
      "  --publish-rate-hz <hz>   Publish rate override\n"
      "  --publish-slots <n>      Ring slot count override\n"
      "  --grab-devices <bool>    Exclusively capture mapped input devices when running\n"
      "  --no-grab-devices        Disable exclusive input capture override\n"
      "  --reattach-devices <bool> Periodically rescan/re-resolve devices after reconnect\n"
      "  --no-reattach-devices    Disable device reattach/rescan\n"
      "  --reattach-interval-ms <n> Rescan interval. Default: 1000\n"
      "  --event-wait-max-ms <n>  Max event select wait. Higher reduces idle CPU\n"
      "  --rel-axis-hold-ms <n>   Pulse hold for mouse-style REL axes. Default: 160\n"
      "  --rel-button-hold-ms <n> Pulse hold for EV_REL D-pad/buttons. Default: 800\n"
      "  --button-hold-ms <n>    Minimum digital press hold across publish ticks. Default: 120\n"
      "  --button-release-grace-ms <n> Delay digital release to bridge repeated short pulses. Default: 0\n"
      "  --pulse-mode <bool>     Enable generic pulse-source interpretation for pulsing controllers\n"
      "  --dpad-pulse-gap-ms <n> Expected max D-pad inter-pulse gap. Default: 130\n"
      "  --dpad-release-ms <n>   Virtual D-pad release timeout in pulse mode. Default: 140\n"
      "  --button-pulse-gap-ms <n> Expected max button inter-pulse gap. Default: 180\n"
      "  --button-release-ms <n> Virtual button release timeout in pulse mode. Default: 190\n"
      "  --button-pulse-startup-ms <n> Initial button pulse warmup window. Default: 0 disabled\n"
      "  --button-pulse-startup-release-ms <n> Button release timeout during warmup. Default: 0 disabled\n"
      "  --button-pulse-startup-types <list> Comma-separated actions for startup warmup. Empty: all buttons\n"
      "  --non-interactive        Do not prompt; fail if config is missing/ambiguous\n"
      "  --verbose                Print more runtime diagnostics\n"
      "  --help\n";
}

bool parse_bool_arg(std::string v, const char* name) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (v == "1" || v == "true" || v == "yes" || v == "y" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "n" || v == "off") return false;
  throw std::runtime_error(std::string(name) + " expects bool value: 0/1/true/false/on/off");
}

std::string trim_copy(std::string v) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
  v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
  return v;
}

std::vector<ControllerAction> parse_action_list_arg(const std::string& raw) {
  std::vector<ControllerAction> out;
  size_t start = 0;
  while (start <= raw.size()) {
    const size_t comma = raw.find(',', start);
    const std::string token = trim_copy(raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!token.empty()) out.push_back(parse_action(token));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string action_list_to_string(const std::vector<ControllerAction>& actions) {
  if (actions.empty()) return "<all>";
  std::string out;
  for (const auto action : actions) {
    if (!out.empty()) out += ",";
    out += to_string(action);
  }
  return out;
}

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string v = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string(name) + " requires a value");
      return argv[++i];
    };
    if (v == "--help" || v == "-h") { usage(); std::exit(0); }
    else if (v == "--config-dir") a.config_dir = need("--config-dir");
    else if (v == "--config") a.config_path = need("--config");
    else if (v == "--name") a.config_name = need("--name");
    else if (v == "--publish-registry" || v == "--controller-input-registry") {
      a.publish_registry = need(v.c_str());
    } else if (v == "--publish-stream" || v == "--controller-input-stream") {
      a.publish_stream = need(v.c_str());
    } else if (v == "--publish-shm-name" || v == "--controller-input-shm-name") {
      a.publish_shm_name = need(v.c_str());
    } else if (v == "--publish-transport" || v == "--controller-input-transport") {
      a.publish_transport = need(v.c_str());
    } else if (v == "--publish-tcp-bind-host" || v == "--controller-input-tcp-bind-host") {
      a.publish_tcp_bind_host = need(v.c_str());
    } else if (v == "--publish-tcp-port" || v == "--controller-input-tcp-port") {
      a.publish_tcp_port = std::stoi(need(v.c_str()));
    } else if (v == "--publish-rate-hz" || v == "--controller-input-rate-hz") {
      a.publish_rate_hz = std::stod(need(v.c_str()));
    } else if (v == "--publish-slots" || v == "--controller-input-slots") {
      a.publish_slot_count = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--grab-devices") {
      a.grab_devices_override_set = true;
      a.grab_devices = parse_bool_arg(need(v.c_str()), v.c_str());
    } else if (v == "--no-grab-devices") {
      a.grab_devices_override_set = true;
      a.grab_devices = false;
    } else if (v == "--reattach-devices") {
      a.reattach_devices_override_set = true;
      a.reattach_devices = parse_bool_arg(need(v.c_str()), v.c_str());
    } else if (v == "--no-reattach-devices") {
      a.reattach_devices_override_set = true;
      a.reattach_devices = false;
    } else if (v == "--reattach-interval-ms") {
      a.reattach_interval_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--event-wait-max-ms") {
      a.event_wait_max_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--rel-axis-hold-ms") {
      a.rel_axis_hold_ms_set = true;
      a.rel_axis_hold_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--rel-button-hold-ms") {
      a.rel_button_hold_ms_set = true;
      a.rel_button_hold_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-hold-ms") {
      a.button_hold_ms_set = true;
      a.button_hold_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-release-grace-ms") {
      a.button_release_grace_ms_set = true;
      a.button_release_grace_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--pulse-mode") {
      a.pulse_mode_override_set = true;
      a.pulse_mode = parse_bool_arg(need(v.c_str()), v.c_str());
    } else if (v == "--dpad-pulse-gap-ms") {
      a.dpad_pulse_gap_ms_set = true;
      a.dpad_pulse_gap_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--dpad-release-ms") {
      a.dpad_release_ms_set = true;
      a.dpad_release_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-pulse-gap-ms") {
      a.button_pulse_gap_ms_set = true;
      a.button_pulse_gap_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-release-ms") {
      a.button_release_ms_set = true;
      a.button_release_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-pulse-startup-ms") {
      a.button_pulse_startup_ms_set = true;
      a.button_pulse_startup_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-pulse-startup-release-ms") {
      a.button_pulse_startup_release_ms_set = true;
      a.button_pulse_startup_release_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--button-pulse-startup-types") {
      a.button_pulse_startup_types_set = true;
      a.button_pulse_startup_types = parse_action_list_arg(need(v.c_str()));
    } else if (v == "--hold-toggle-debounce-ms") {
      a.hold_toggle_debounce_ms_set = true;
      a.hold_toggle_debounce_ms = static_cast<uint32_t>(std::stoul(need(v.c_str())));
    } else if (v == "--train") a.train = true;
    else if (v == "--list-devices") a.list_devices = true;
    else if (v == "--non-interactive") a.non_interactive = true;
    else if (v == "--verbose") a.verbose = true;
    else throw std::runtime_error("unknown argument: " + v);
  }
  return a;
}

std::vector<ControllerAction> default_actions() {
  return {
      ControllerAction::Trigger,
      ControllerAction::Grip,
      ControllerAction::Menu,
      ControllerAction::A,
      ControllerAction::B,
      ControllerAction::X,
      ControllerAction::Y,
      ControllerAction::System,
      ControllerAction::ThumbstickClick,
      ControllerAction::DpadUp,
      ControllerAction::DpadDown,
      ControllerAction::DpadLeft,
      ControllerAction::DpadRight,
      ControllerAction::DpadCenter,
      ControllerAction::ThumbstickX,
      ControllerAction::ThumbstickY,
  };
}

bool ask_yes_no(const std::string& prompt, bool def) {
  std::cout << prompt << (def ? " [Y/n]: " : " [y/N]: ") << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) return def;
  if (line.empty()) return def;
  const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
  return c == 'y' || line.rfind("д", 0) == 0 || line.rfind("Д", 0) == 0;
}

std::string prompt_line(const std::string& prompt, const std::string& def) {
  std::cout << prompt;
  if (!def.empty()) std::cout << " [" << def << "]";
  std::cout << ": " << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) return def;
  return line.empty() ? def : line;
}

void print_devices(const std::vector<DeviceInfo>& devices) {
  std::cout << "Detected input devices:\n";
  for (size_t i = 0; i < devices.size(); ++i) {
    const auto& d = devices[i];
    std::cout << "  [" << i << "] " << short_device_label(d.fingerprint)
              << " readable=" << (d.readable ? "yes" : "no");
    if (!d.open_error.empty()) std::cout << " error=" << d.open_error;
    std::cout << "\n";
  }
}

struct TrainingInputFilter {
  std::set<size_t> allow_devices;
  bool has_allow_devices = false;
  bool block_enter_keys = true;
};

std::vector<std::string> split_tokens(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool is_integer_token(const std::string& s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool device_matches_token(const DeviceInfo& d, const std::string& token) {
  const std::string t = lower_copy(token);
  const auto& fp = d.fingerprint;
  const std::string hay = lower_copy(short_device_label(fp) + " " + fp.event_path + " " + fp.by_id_path + " " +
                                     fp.by_path + " " + fp.name + " " + fp.phys + " " + fp.uniq + " " +
                                     hex_u64(fp.stable_hash));
  return !t.empty() && hay.find(t) != std::string::npos;
}

TrainingInputFilter prompt_training_input_filter(const std::vector<DeviceInfo>& devices) {
  TrainingInputFilter filter;
  std::cout << "\nTraining input device whitelist.\n"
            << "  Empty = listen to every readable input device.\n"
            << "  Recommended = enter your device indexes, e.g. 10,11, so Enter/keyboard cannot be captured.\n"
            << "  You may use indexes or text tokens or a uniq value.\n";
  std::cout << "Use only devices [empty=all readable]: " << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) return filter;
  line = trim_copy(line);
  if (line.empty()) {
    std::cout << "  using all readable devices; KEY_ENTER/KEY_KPENTER are still blacklisted for skip.\n";
    return filter;
  }

  for (const auto& token : split_tokens(line)) {
    if (is_integer_token(token)) {
      try {
        const size_t idx = static_cast<size_t>(std::stoul(token));
        if (idx < devices.size()) filter.allow_devices.insert(idx);
      } catch (...) {}
      continue;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
      if (device_matches_token(devices[i], token)) filter.allow_devices.insert(i);
    }
  }
  filter.has_allow_devices = !filter.allow_devices.empty();
  if (!filter.has_allow_devices) {
    std::cout << "  no whitelist matches; falling back to all readable devices.\n";
  } else {
    std::cout << "  whitelisted devices:\n";
    for (const size_t idx : filter.allow_devices) {
      if (idx < devices.size()) std::cout << "    [" << idx << "] " << short_device_label(devices[idx].fingerprint) << "\n";
    }
  }
  return filter;
}

bool is_default_blacklisted_training_input(const InputProvider& provider, const InputEvent& ev) {
#if defined(__linux__)
  if (ev.type == EV_KEY && (ev.code == KEY_ENTER || ev.code == KEY_KPENTER)) return true;
#endif
  const std::string name = provider.input_name(ev.type, ev.code);
  return name == "KEY_ENTER" || name == "KEY_KPENTER";
}

bool training_filter_accepts(const TrainingInputFilter& filter,
                             const InputProvider& provider,
                             const std::vector<DeviceInfo>& devices,
                             const InputEvent& ev) {
  if (ev.device_index == std::numeric_limits<size_t>::max()) return true;  // stdin skip sentinel.
  if (ev.device_index >= devices.size()) return false;
  if (filter.has_allow_devices && filter.allow_devices.count(ev.device_index) == 0) return false;
  if (filter.block_enter_keys && is_default_blacklisted_training_input(provider, ev)) return false;
  return true;
}

int axis_direction(const InputBindingSpec& spec, int value) {
  if (spec.abs_max <= spec.abs_min) return value < 0 ? -1 : (value > 0 ? 1 : 0);
  const double center = 0.5 * (static_cast<double>(spec.abs_min) + static_cast<double>(spec.abs_max));
  const double half = std::max(1.0, 0.5 * (static_cast<double>(spec.abs_max) - static_cast<double>(spec.abs_min)));
  const double signed_v = (static_cast<double>(value) - center) / half;
  if (signed_v > 0.30) return 1;
  if (signed_v < -0.30) return -1;
  return 0;
}

int rel_direction(int value) {
  if (value > 0) return 1;
  if (value < 0) return -1;
  return 0;
}


int64_t monotonic_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool rel_axis_matches_action(ControllerAction action, uint16_t code) {
#if defined(__linux__)
  switch (action) {
    case ControllerAction::DpadLeft:
    case ControllerAction::DpadRight:
    case ControllerAction::ThumbstickX:
      return code == REL_X || code == REL_RX;
    case ControllerAction::DpadUp:
    case ControllerAction::DpadDown:
    case ControllerAction::ThumbstickY:
      return code == REL_Y || code == REL_RY;
    default:
      return true;
  }
#else
  (void)action;
  (void)code;
  return true;
#endif
}

bool rel_direction_matches_action(ControllerAction action, int dir) {
  switch (action) {
    case ControllerAction::DpadLeft:
      return dir < 0;
    case ControllerAction::DpadRight:
      return dir > 0;
    case ControllerAction::DpadUp:
      // Linux evdev mouse-style REL_Y is normally negative when moving up.
      return dir < 0;
    case ControllerAction::DpadDown:
      return dir > 0;
    default:
      return dir != 0;
  }
}

void wait_for_input_quiet(InputProvider& provider,
                          std::vector<DeviceInfo>& devices,
                          int quiet_ms = 180,
                          int max_wait_ms = 1800) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  while (std::chrono::steady_clock::now() < deadline && !g_stop) {
    auto ev = provider.wait_event(devices, quiet_ms, false);
    if (!ev) return;
  }
}

bool capture_binding(InputProvider& provider,
                     std::vector<DeviceInfo>& devices,
                     const TrainingInputFilter& filter,
                     ControllerSide side,
                     ControllerAction action,
                     BindingConfig& out) {
  // Drain stale release/bounce/noise before the prompt is shown. Draining after
  // the prompt can accidentally eat a fast button press from the user.
  provider.flush_events(devices);
  std::cout << "\n[learn] " << to_string(side) << "." << to_string(action)
            << ": press/move input now, or press Enter to skip.\n";

  while (!g_stop) {
    auto ev = provider.wait_event(devices, 25000, true);
    if (!ev) {
      std::cout << "  timeout; skipped.\n";
      return false;
    }
    if (!training_filter_accepts(filter, provider, devices, *ev)) {
      continue;
    }
    if (ev->device_index == std::numeric_limits<size_t>::max()) {
      std::cout << "  skipped.\n";
      return false;
    }
    if (ev->device_index >= devices.size()) continue;
    const auto& dev = devices[ev->device_index];
    if (!dev.readable) continue;

    if (ev->type == EV_KEY && ev->value != 1) continue;
    if (ev->type == EV_ABS) {
      InputBindingSpec tmp = provider.make_input_spec(dev, ev->type, ev->code);
      const int dir = axis_direction(tmp, ev->value);
      if (!is_axis_action(action) && dir == 0) continue;
      tmp.abs_direction = dir;
      out.side = side;
      out.action = action;
      out.device = dev.fingerprint;
      out.input = tmp;
    } else if (ev->type == EV_REL) {
      InputBindingSpec tmp = provider.make_input_spec(dev, ev->type, ev->code);
      const int dir = rel_direction(ev->value);
      if (dir == 0) continue;
      if (!rel_axis_matches_action(action, ev->code)) continue;
      if (!is_axis_action(action) && !rel_direction_matches_action(action, dir)) continue;
      tmp.abs_direction = dir;
      out.side = side;
      out.action = action;
      out.device = dev.fingerprint;
      out.input = tmp;
    } else if (ev->type == EV_KEY) {
      out.side = side;
      out.action = action;
      out.device = dev.fingerprint;
      out.input = provider.make_input_spec(dev, ev->type, ev->code);
    } else {
      continue;
    }

    std::cout << "  mapped " << to_string(side) << "." << to_string(action)
              << " <= " << out.input.name << " on " << short_device_label(out.device) << "\n";
    wait_for_input_quiet(provider, devices);
    return true;
  }
  return false;
}


void train_hold_toggle_bindings(InputProvider& provider,
                                std::vector<DeviceInfo>& devices,
                                const TrainingInputFilter& training_filter,
                                AppConfig& cfg) {
  std::cout << "\nOptional long-press toggle emulation.\n"
            << "  You can configure a mode where a single click starts a virtual long press "
            << "for the selected action, and the next click releases it.\n"
            << "  If you use an already configured button on the same controller, this "
            << "long-press toggle binding has priority over the normal binding.\n";
  if (!ask_yes_no("Configure long-press toggle emulation now?", false)) return;

  for (ControllerSide side : {ControllerSide::Left, ControllerSide::Right}) {
    const bool has_normal_side_bindings =
        std::any_of(cfg.bindings.begin(), cfg.bindings.end(), [side](const BindingConfig& b) { return b.side == side; });
    if (!ask_yes_no("Configure " + to_string(side) + " long-press toggle bindings?", has_normal_side_bindings)) {
      continue;
    }

    std::cout << "\n=== " << to_string(side) << " long-press toggle bindings ===\n";
    for (const auto action : default_actions()) {
      BindingConfig b;
      if (capture_binding(provider, devices, training_filter, side, action, b)) {
        cfg.hold_toggle_bindings.push_back(std::move(b));
      }
    }
  }
}

void apply_publish_overrides(AppConfig& cfg, const Args& args) {
  if (!args.publish_registry.empty()) cfg.publish.registry_path = args.publish_registry;
  if (!args.publish_stream.empty()) cfg.publish.stream_id = args.publish_stream;
  if (!args.publish_shm_name.empty()) cfg.publish.shm_name = args.publish_shm_name;
  if (!args.publish_transport.empty()) cfg.publish.transport = args.publish_transport;
  if (!args.publish_tcp_bind_host.empty()) cfg.publish.tcp_bind_host = args.publish_tcp_bind_host;
  if (args.publish_tcp_port > 0) cfg.publish.tcp_port = args.publish_tcp_port;
  if (args.publish_rate_hz > 0.0) cfg.publish.rate_hz = args.publish_rate_hz;
  if (args.publish_slot_count > 0) cfg.publish.slot_count = args.publish_slot_count;
  if (args.grab_devices_override_set) cfg.input.grab_devices = args.grab_devices;
  if (args.reattach_devices_override_set) cfg.input.reattach_devices = args.reattach_devices;
  if (args.reattach_interval_ms > 0) cfg.input.reattach_interval_ms = args.reattach_interval_ms;
  if (args.event_wait_max_ms > 0) cfg.input.event_wait_max_ms = args.event_wait_max_ms;
  if (args.rel_axis_hold_ms_set) cfg.input.rel_axis_hold_ms = args.rel_axis_hold_ms;
  if (args.rel_button_hold_ms_set) cfg.input.rel_button_hold_ms = args.rel_button_hold_ms;
  if (args.button_hold_ms_set) cfg.input.button_hold_ms = args.button_hold_ms;
  if (args.button_release_grace_ms_set) cfg.input.button_release_grace_ms = args.button_release_grace_ms;
  if (args.pulse_mode_override_set) cfg.input.pulse_mode = args.pulse_mode;
  if (args.dpad_pulse_gap_ms_set) cfg.input.dpad_pulse_gap_ms = args.dpad_pulse_gap_ms;
  if (args.dpad_release_ms_set) cfg.input.dpad_release_ms = args.dpad_release_ms;
  if (args.button_pulse_gap_ms_set) cfg.input.button_pulse_gap_ms = args.button_pulse_gap_ms;
  if (args.button_release_ms_set) cfg.input.button_release_ms = args.button_release_ms;
  if (args.button_pulse_startup_ms_set) cfg.input.button_pulse_startup_ms = args.button_pulse_startup_ms;
  if (args.button_pulse_startup_release_ms_set) cfg.input.button_pulse_startup_release_ms = args.button_pulse_startup_release_ms;
  if (args.button_pulse_startup_types_set) cfg.input.button_pulse_startup_types = args.button_pulse_startup_types;
  if (args.hold_toggle_debounce_ms_set) cfg.input.hold_toggle_debounce_ms = args.hold_toggle_debounce_ms;
}

AppConfig train_config(InputProvider& provider, const fs::path& config_path, const std::string& name) {
  auto devices = provider.scan_devices(true);
  print_devices(devices);
  if (devices.empty()) throw std::runtime_error("no readable input devices found");
  const auto readable_count = std::count_if(devices.begin(), devices.end(), [](const DeviceInfo& d) { return d.readable; });
  if (readable_count == 0) {
#if defined(_WIN32)
    throw std::runtime_error(
        "no readable input devices. Windows keyboard polling provider should normally expose one device; "
        "check that the process can access the desktop/session input state.");
#else
    throw std::runtime_error(
        "no readable /dev/input/event* devices. On Linux, add the current user to the input group "
        "and re-login: sudo usermod -aG input $USER. For a quick temporary test, run the start script "
        "with USE_SUDO=1, or grant a temporary ACL: sudo setfacl -m u:$USER:rw /dev/input/event*.");
#endif
  }

  TrainingInputFilter training_filter = prompt_training_input_filter(devices);

  AppConfig cfg;
  cfg.name = prompt_line("Config name", name.empty() ? "default" : name);
  cfg.publish.registry_path = xr_runtime::default_tracking_registry_path();
  cfg.publish.stream_id = "controller_input";
  cfg.publish.shm_name = "controller_input";
#if defined(_WIN32)
  cfg.publish.transport = "tcp";
#else
  cfg.publish.transport = "shm";
#endif
  cfg.publish.tcp_bind_host = "127.0.0.1";
  cfg.publish.tcp_port = 45672;
  cfg.publish.rate_hz = 90.0;
  cfg.publish.slot_count = 32;

  std::cout << "\nTraining creates independent bindings per action. Actions may come from different devices.\n";
  std::cout << "Each binding stores the captured device fingerprint for matching after reboot/reconnect.\n";

  for (ControllerSide side : {ControllerSide::Left, ControllerSide::Right}) {
    if (!ask_yes_no("Configure " + to_string(side) + " controller?", true)) continue;
    std::cout << "\n=== " << to_string(side) << " controller ===\n";
    for (const auto action : default_actions()) {
      BindingConfig b;
      if (capture_binding(provider, devices, training_filter, side, action, b)) {
        cfg.bindings.push_back(std::move(b));
      }
    }
  }

  train_hold_toggle_bindings(provider, devices, training_filter, cfg);

  save_config_file(cfg, config_path);
  std::cout << "\nSaved config: " << config_path << "\n";
  return cfg;
}

fs::path choose_config_interactive(const std::vector<fs::path>& files) {
  std::cout << "Available override_controller configs:\n";
  for (size_t i = 0; i < files.size(); ++i) {
    std::cout << "  [" << (i + 1) << "] " << files[i] << "\n";
  }
  while (true) {
    std::cout << "Choose config [1-" << files.size() << "]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("no config selected");
    try {
      const size_t idx = static_cast<size_t>(std::stoul(line));
      if (idx >= 1 && idx <= files.size()) return files[idx - 1];
    } catch (...) {}
  }
}

struct RuntimeBinding {
  BindingConfig cfg;
  int device_index = -1;
  int match_score = 0;
  float value = 0.0f;
  float physical_value = 0.0f;
  int64_t active_until_ns = 0;
  int64_t last_event_ns = 0;
  int64_t pulse_series_start_ns = 0;
  int64_t pulse_series_until_ns = 0;
  int64_t toggle_debounce_until_ns = 0;
  bool connected = false;
};

float normalize_abs_signed(const InputBindingSpec& spec, int value) {
  if (spec.abs_max <= spec.abs_min) return 0.0f;
  const double center = 0.5 * (static_cast<double>(spec.abs_min) + static_cast<double>(spec.abs_max));
  const double half = std::max(1.0, 0.5 * (static_cast<double>(spec.abs_max) - static_cast<double>(spec.abs_min)));
  double v = (static_cast<double>(value) - center) / half;
  if (std::abs(v) < 0.05) v = 0.0;
  return static_cast<float>(std::clamp(v, -1.0, 1.0));
}

float normalize_abs_unsigned(const InputBindingSpec& spec, int value) {
  if (spec.abs_max <= spec.abs_min) return 0.0f;
  double v = (static_cast<double>(value) - static_cast<double>(spec.abs_min)) /
             static_cast<double>(spec.abs_max - spec.abs_min);
  if (v < 0.04) v = 0.0;
  return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

float value_for_binding(const BindingConfig& cfg, int raw_value) {
  if (cfg.input.kind == InputKind::Key) {
    return raw_value != 0 ? 1.0f : 0.0f;
  }
  if (cfg.input.kind == InputKind::RelAxis) {
    const int dir = rel_direction(raw_value);
    if (dir == 0) return 0.0f;
    if (is_axis_action(cfg.action)) return static_cast<float>(dir);
    if (cfg.input.abs_direction > 0) return dir > 0 ? 1.0f : 0.0f;
    if (cfg.input.abs_direction < 0) return dir < 0 ? 1.0f : 0.0f;
    return 1.0f;
  }
  if (is_axis_action(cfg.action)) {
    return normalize_abs_signed(cfg.input, raw_value);
  }
  if (cfg.action == ControllerAction::Trigger || cfg.action == ControllerAction::Grip) {
    return normalize_abs_unsigned(cfg.input, raw_value);
  }
  const float signed_v = normalize_abs_signed(cfg.input, raw_value);
  if (cfg.input.abs_direction > 0) return signed_v > 0.50f ? 1.0f : 0.0f;
  if (cfg.input.abs_direction < 0) return signed_v < -0.50f ? 1.0f : 0.0f;
  return std::abs(signed_v) > 0.50f ? 1.0f : 0.0f;
}


bool binding_uses_min_hold(const BindingConfig& cfg) {
  if (is_axis_action(cfg.action)) return false;
  if (cfg.input.kind == InputKind::Key || cfg.input.kind == InputKind::RelAxis) return true;
  // Directional ABS bindings, such as a hat switch mapped as D-pad buttons,
  // are button-like. Pure analog trigger/grip values should not be latched.
  return cfg.input.kind == InputKind::AbsAxis && cfg.input.abs_direction != 0;
}

bool binding_is_rel_button_like(const BindingConfig& cfg) {
  return cfg.input.kind == InputKind::RelAxis && !is_axis_action(cfg.action);
}

bool is_dpad_action(ControllerAction action) {
  return action == ControllerAction::DpadUp || action == ControllerAction::DpadDown ||
         action == ControllerAction::DpadLeft || action == ControllerAction::DpadRight ||
         action == ControllerAction::DpadCenter;
}

int64_t pulse_window_ns(uint32_t gap_ms, uint32_t release_ms) {
  // release_ms is the actual virtual-release timeout. gap_ms is the expected
  // inter-pulse period. Use the larger value so an accidentally smaller release
  // timeout does not reintroduce gaps between normal pulses.
  return static_cast<int64_t>(std::max(gap_ms, release_ms)) * 1000000LL;
}

int64_t pulse_release_ns_for_binding(const InputConfig& input, const BindingConfig& cfg) {
  if (!input.pulse_mode || !binding_uses_min_hold(cfg)) return 0;
  if (is_dpad_action(cfg.action)) {
    return pulse_window_ns(input.dpad_pulse_gap_ms, input.dpad_release_ms);
  }
  return pulse_window_ns(input.button_pulse_gap_ms, input.button_release_ms);
}

bool button_pulse_startup_applies_to_action(const InputConfig& input, ControllerAction action) {
  if (input.button_pulse_startup_types.empty()) return true;
  return std::find(input.button_pulse_startup_types.begin(), input.button_pulse_startup_types.end(), action) !=
         input.button_pulse_startup_types.end();
}

int64_t button_pulse_grace_ns_for_event(const InputConfig& input, RuntimeBinding& b, int64_t event_ns, float value, int64_t normal_grace_ns) {
  if (!input.pulse_mode || binding_is_rel_button_like(b.cfg) || !binding_uses_min_hold(b.cfg)) {
    return normal_grace_ns;
  }
  if (is_dpad_action(b.cfg.action)) return normal_grace_ns;
  if (!button_pulse_startup_applies_to_action(input, b.cfg.action)) return normal_grace_ns;

  const int64_t startup_ns = static_cast<int64_t>(input.button_pulse_startup_ms) * 1000000LL;
  const int64_t startup_grace_ns = static_cast<int64_t>(input.button_pulse_startup_release_ms) * 1000000LL;
  if (startup_ns <= 0 || startup_grace_ns <= normal_grace_ns) return normal_grace_ns;

  // Pulse-only remotes can have very slow button-repeat cadence for the first
  // seconds after a physical hold begins, then settle into the normal cadence.
  // Keep a per-binding pulse series alive with the warmup timeout only while
  // the series age is inside button_pulse_startup_ms. After that, fall back to
  // the normal short button_release_ms so release latency stays responsive.
  if (value >= 0.50f) {
    if (b.pulse_series_start_ns == 0 || event_ns > b.pulse_series_until_ns) {
      b.pulse_series_start_ns = event_ns;
    }
  } else if (b.pulse_series_start_ns == 0 || event_ns > b.pulse_series_until_ns) {
    return normal_grace_ns;
  }

  const bool in_startup = event_ns - b.pulse_series_start_ns <= startup_ns;
  const int64_t grace_ns = in_startup ? startup_grace_ns : normal_grace_ns;
  b.pulse_series_until_ns = event_ns + grace_ns;
  return grace_ns;
}

void update_binding_value_from_event(RuntimeBinding& b, float value, int64_t event_ns, int64_t hold_ns, int64_t release_grace_ns) {
  b.last_event_ns = event_ns;
  if (!binding_uses_min_hold(b.cfg) || (hold_ns <= 0 && release_grace_ns <= 0)) {
    b.value = value;
    b.physical_value = value;
    b.active_until_ns = 0;
    return;
  }

  if (binding_is_rel_button_like(b.cfg)) {
    // EV_REL D-pad/button events are pulses, not physical held states. Keep an
    // active window after each pulse, but do not mark physical_value as held or
    // the button would remain stuck forever without a release event.
    b.physical_value = 0.0f;
    if (value >= 0.50f) {
      b.active_until_ns = std::max(b.active_until_ns, event_ns + hold_ns);
      b.value = 1.0f;
    } else {
      b.value = event_ns <= b.active_until_ns ? 1.0f : 0.0f;
    }
    return;
  }

  b.physical_value = value;
  if (value >= 0.50f) {
    b.active_until_ns = std::max(b.active_until_ns, event_ns + hold_ns);
    b.value = value;
    return;
  }

  // A quick press+release can happen between two publish ticks. Keep a short
  // synthetic hold so the runtime/OpenVR side sees at least several active
  // frames. Some mouse/media/Bluetooth controllers also emit repeated short
  // key pulses while the user physically holds the button. In that case an
  // optional release grace bridges the gap to the next repeat pulse so
  // SteamVR Input sees one continuous hold instead of many short taps.
  if (release_grace_ns > 0) {
    b.active_until_ns = std::max(b.active_until_ns, event_ns + release_grace_ns);
  }
  b.value = event_ns <= b.active_until_ns ? 1.0f : 0.0f;
}

std::vector<RuntimeBinding> resolve_binding_configs(const std::vector<BindingConfig>& configs,
                                                   const std::vector<DeviceInfo>& devices,
                                                   bool log_warnings = true,
                                                   const char* label = "binding") {
  std::vector<RuntimeBinding> out;
  for (const auto& b : configs) {
    RuntimeBinding rb;
    rb.cfg = b;
    int best_score = -1;
    int best_idx = -1;
    bool ambiguous = false;
    for (size_t i = 0; i < devices.size(); ++i) {
      if (!devices[i].readable) continue;
      const int score = fingerprint_match_score(b.device, devices[i].fingerprint);
      if (score > best_score) {
        best_score = score;
        best_idx = static_cast<int>(i);
        ambiguous = false;
      } else if (score == best_score && score > 0) {
        ambiguous = true;
      }
    }
    rb.match_score = best_score;
    if (best_idx >= 0 && best_score >= 55 && !ambiguous) {
      rb.device_index = best_idx;
      rb.connected = true;
    } else if (ambiguous && log_warnings) {
      std::cerr << "[override_controller][ERROR] ambiguous device match for "
                << label << " " << to_string(b.side) << "." << to_string(b.action)
                << "; refusing to bind; wanted=" << short_device_label(b.device)
                << " candidate=" << short_device_label(devices[best_idx].fingerprint)
                << " score=" << best_score << "\n";
    } else if (log_warnings) {
      std::cerr << "[override_controller][WARN] no good device match for "
                << label << " " << to_string(b.side) << "." << to_string(b.action)
                << " score=" << best_score << " wanted=" << short_device_label(b.device) << "\n";
    }
    out.push_back(std::move(rb));
  }
  return out;
}

std::vector<RuntimeBinding> resolve_bindings(const AppConfig& cfg, const std::vector<DeviceInfo>& devices, bool log_warnings = true) {
  return resolve_binding_configs(cfg.bindings, devices, log_warnings, "binding");
}

std::vector<RuntimeBinding> resolve_hold_toggle_bindings(const AppConfig& cfg,
                                                         const std::vector<DeviceInfo>& devices,
                                                         bool log_warnings = true) {
  return resolve_binding_configs(cfg.hold_toggle_bindings, devices, log_warnings, "hold-toggle binding");
}

void apply_action(SideOutputState& side, ControllerAction action, float value) {
  side.configured = true;
  if (button_bit_for_action(action) != 0) {
    if (value >= 0.50f) side.buttons |= button_bit_for_action(action);
  }
  switch (action) {
    case ControllerAction::Trigger:
      side.trigger = std::max(side.trigger, std::clamp(value, 0.0f, 1.0f));
      break;
    case ControllerAction::Grip:
      side.grip = std::max(side.grip, std::clamp(value, 0.0f, 1.0f));
      break;
    case ControllerAction::ThumbstickX:
      side.thumbstick_x = std::clamp(value, -1.0f, 1.0f);
      break;
    case ControllerAction::ThumbstickY:
      side.thumbstick_y = std::clamp(value, -1.0f, 1.0f);
      break;
    default:
      break;
  }
  // D-pad center is also compatible with thumbstick click.
  if (action == ControllerAction::DpadCenter && value >= 0.50f) {
    side.buttons |= kButtonThumbstick;
  }
}

struct CounterState {
  uint64_t prev_left_buttons = 0;
  uint64_t prev_right_buttons = 0;
  uint32_t left_press[32] = {};
  uint32_t left_release[32] = {};
  uint32_t right_press[32] = {};
  uint32_t right_release[32] = {};
};

void update_counters(uint64_t prev, uint64_t now, uint32_t press[32], uint32_t release[32]) {
  for (int i = 0; i < 32; ++i) {
    const uint64_t bit = 1ull << i;
    const bool was = (prev & bit) != 0;
    const bool is = (now & bit) != 0;
    if (!was && is) ++press[i];
    if (was && !is) ++release[i];
  }
}

OutputState compose_state(const std::vector<RuntimeBinding>& bindings,
                          const std::vector<RuntimeBinding>& hold_toggle_bindings,
                          const std::vector<DeviceInfo>& devices,
                          CounterState& counters) {
  OutputState out;
  std::set<std::string> left_devices;
  std::set<std::string> right_devices;
  auto apply_runtime_binding = [&](const RuntimeBinding& b) {
    const bool resolved = b.device_index >= 0 && static_cast<size_t>(b.device_index) < devices.size() && devices[b.device_index].readable && devices[b.device_index].fd >= 0;
    SideOutputState& side = b.cfg.side == ControllerSide::Left ? out.left : out.right;
    side.configured = true;
    if (resolved) {
      side.connected = true;
      const auto id = hex_u64(devices[b.device_index].fingerprint.stable_hash);
      if (b.cfg.side == ControllerSide::Left) left_devices.insert(id);
      else right_devices.insert(id);
    }
    apply_action(side, b.cfg.action, resolved ? b.value : 0.0f);
  };

  for (const auto& b : bindings) {
    apply_runtime_binding(b);
  }
  // Hold-toggle bindings are applied after normal bindings so a latched
  // virtual hold has priority over a normal binding for the same action.
  for (const auto& b : hold_toggle_bindings) {
    apply_runtime_binding(b);
  }
  const auto label = [](const std::set<std::string>& ids) {
    if (ids.empty()) return std::string();
    if (ids.size() == 1) return *ids.begin();
    return std::string("mixed:") + std::to_string(ids.size());
  };
  out.left.device_id = label(left_devices);
  out.right.device_id = label(right_devices);


  bool side_device_overlap = false;
  for (const auto& device_id : left_devices) {
    if (right_devices.count(device_id) != 0) {
      side_device_overlap = true;
      break;
    }
  }
  if (side_device_overlap) {
    std::cerr << "[override_controller][ERROR] same physical input device resolved for both left and right; "
                 "suppressing both controller sides until bindings are retrained or made unique\n";
    out.left.connected = false;
    out.right.connected = false;
    out.left.buttons = 0;
    out.right.buttons = 0;
    out.left.touches = 0;
    out.right.touches = 0;
    out.left.trigger = 0.0f;
    out.right.trigger = 0.0f;
    out.left.grip = 0.0f;
    out.right.grip = 0.0f;
    out.left.thumbstick_x = 0.0f;
    out.left.thumbstick_y = 0.0f;
    out.right.thumbstick_x = 0.0f;
    out.right.thumbstick_y = 0.0f;
    out.left.device_id.clear();
    out.right.device_id.clear();
  }
out.left.changed_buttons = counters.prev_left_buttons ^ out.left.buttons;
  out.right.changed_buttons = counters.prev_right_buttons ^ out.right.buttons;
  update_counters(counters.prev_left_buttons, out.left.buttons, counters.left_press, counters.left_release);
  update_counters(counters.prev_right_buttons, out.right.buttons, counters.right_press, counters.right_release);
  counters.prev_left_buttons = out.left.buttons;
  counters.prev_right_buttons = out.right.buttons;
  std::memcpy(out.left.press_counters, counters.left_press, sizeof(counters.left_press));
  std::memcpy(out.left.release_counters, counters.left_release, sizeof(counters.left_release));
  std::memcpy(out.right.press_counters, counters.right_press, sizeof(counters.right_press));
  std::memcpy(out.right.release_counters, counters.right_release, sizeof(counters.right_release));
  return out;
}

void decay_relative_axis_bindings(std::vector<RuntimeBinding>& bindings, int64_t now, int64_t hold_ns) {
  for (auto& b : bindings) {
    if (b.cfg.input.kind != InputKind::RelAxis || !is_axis_action(b.cfg.action)) continue;
    if (b.value == 0.0f && b.physical_value == 0.0f) continue;
    if (b.last_event_ns == 0 || now - b.last_event_ns > hold_ns) {
      b.value = 0.0f;
      b.physical_value = 0.0f;
    }
  }
}

void decay_button_hold_bindings(std::vector<RuntimeBinding>& bindings, int64_t now) {
  for (auto& b : bindings) {
    if (!binding_uses_min_hold(b.cfg)) continue;
    if (b.physical_value >= 0.50f) {
      b.value = b.physical_value;
      continue;
    }
    if (b.active_until_ns != 0 && now <= b.active_until_ns) {
      b.value = 1.0f;
    } else {
      b.value = 0.0f;
      b.active_until_ns = 0;
      if (b.pulse_series_until_ns != 0 && now > b.pulse_series_until_ns) {
        b.pulse_series_start_ns = 0;
        b.pulse_series_until_ns = 0;
      }
    }
  }
}

void clear_runtime_bindings(std::vector<RuntimeBinding>& bindings) {
  for (auto& b : bindings) {
    b.value = 0.0f;
    b.physical_value = 0.0f;
    b.active_until_ns = 0;
    b.pulse_series_start_ns = 0;
    b.pulse_series_until_ns = 0;
    b.toggle_debounce_until_ns = 0;
  }
}

OutputState compose_neutral_state_from_config(const AppConfig& cfg, CounterState& counters) {
  OutputState out;
  auto mark_configured = [&](const BindingConfig& b) {
    SideOutputState& side = b.side == ControllerSide::Left ? out.left : out.right;
    side.configured = true;
    side.connected = false;
  };
  for (const auto& b : cfg.bindings) mark_configured(b);
  for (const auto& b : cfg.hold_toggle_bindings) mark_configured(b);
  out.left.changed_buttons = counters.prev_left_buttons;
  out.right.changed_buttons = counters.prev_right_buttons;
  if (counters.prev_left_buttons != 0) {
    for (unsigned i = 0; i < 32; ++i) {
      if ((counters.prev_left_buttons & (1ull << i)) != 0ull) counters.left_release[i]++;
    }
  }
  if (counters.prev_right_buttons != 0) {
    for (unsigned i = 0; i < 32; ++i) {
      if ((counters.prev_right_buttons & (1ull << i)) != 0ull) counters.right_release[i]++;
    }
  }
  counters.prev_left_buttons = 0;
  counters.prev_right_buttons = 0;
  std::memcpy(out.left.press_counters, counters.left_press, sizeof(counters.left_press));
  std::memcpy(out.left.release_counters, counters.left_release, sizeof(counters.left_release));
  std::memcpy(out.right.press_counters, counters.right_press, sizeof(counters.right_press));
  std::memcpy(out.right.release_counters, counters.right_release, sizeof(counters.right_release));
  return out;
}

void publish_neutral_frames(ControllerInputPublisher& publisher, const AppConfig& cfg, CounterState& counters) {
  const OutputState neutral = compose_neutral_state_from_config(cfg, counters);
  for (int i = 0; i < 6; ++i) {
    publisher.publish(neutral);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
}


std::set<size_t> collect_resolved_device_indices(const std::vector<RuntimeBinding>& bindings) {
  std::set<size_t> indices;
  for (const auto& b : bindings) {
    if (b.device_index >= 0) indices.insert(static_cast<size_t>(b.device_index));
  }
  return indices;
}

size_t count_open_devices(const std::vector<DeviceInfo>& devices) {
  return static_cast<size_t>(std::count_if(devices.begin(), devices.end(), [](const DeviceInfo& d) {
    return d.readable && d.fd >= 0;
  }));
}

size_t count_connected_bindings(const std::vector<RuntimeBinding>& bindings,
                                const std::vector<DeviceInfo>& devices) {
  return static_cast<size_t>(std::count_if(bindings.begin(), bindings.end(), [&](const RuntimeBinding& b) {
    return b.device_index >= 0 && static_cast<size_t>(b.device_index) < devices.size() &&
           devices[b.device_index].readable && devices[b.device_index].fd >= 0;
  }));
}


bool runtime_binding_is_resolved(const RuntimeBinding& b, const std::vector<DeviceInfo>& devices) {
  return b.device_index >= 0 && static_cast<size_t>(b.device_index) < devices.size() &&
         devices[b.device_index].readable && devices[b.device_index].fd >= 0;
}

bool event_matches_runtime_binding(const RuntimeBinding& b, const InputEvent& ev) {
  return b.device_index == static_cast<int>(ev.device_index) &&
         b.cfg.input.type == ev.type &&
         b.cfg.input.code == ev.code;
}

bool same_physical_input_on_side(const RuntimeBinding& a, const RuntimeBinding& b) {
  return a.cfg.side == b.cfg.side &&
         a.device_index >= 0 && a.device_index == b.device_index &&
         a.cfg.input.type == b.cfg.input.type &&
         a.cfg.input.code == b.cfg.input.code;
}

bool same_runtime_binding_identity(const RuntimeBinding& a, const RuntimeBinding& b) {
  return a.cfg.side == b.cfg.side &&
         a.cfg.action == b.cfg.action &&
         a.cfg.input.kind == b.cfg.input.kind &&
         a.cfg.input.type == b.cfg.input.type &&
         a.cfg.input.code == b.cfg.input.code &&
         a.cfg.input.abs_direction == b.cfg.input.abs_direction &&
         a.cfg.device.stable_hash == b.cfg.device.stable_hash;
}

void preserve_runtime_binding_state(std::vector<RuntimeBinding>& target,
                                    const std::vector<RuntimeBinding>& source) {
  for (auto& dst : target) {
    auto it = std::find_if(source.begin(), source.end(), [&](const RuntimeBinding& src) {
      return same_runtime_binding_identity(src, dst);
    });
    if (it == source.end()) continue;

    dst.value = it->value;
    dst.physical_value = it->physical_value;
    dst.active_until_ns = it->active_until_ns;
    dst.last_event_ns = it->last_event_ns;
    dst.pulse_series_start_ns = it->pulse_series_start_ns;
    dst.pulse_series_until_ns = it->pulse_series_until_ns;
    dst.toggle_debounce_until_ns = it->toggle_debounce_until_ns;
  }
}

bool any_runtime_binding_active(const std::vector<RuntimeBinding>& bindings) {
  return std::any_of(bindings.begin(), bindings.end(), [](const RuntimeBinding& b) {
    return b.value >= 0.50f || b.physical_value >= 0.50f || b.active_until_ns != 0 ||
           b.pulse_series_until_ns != 0;
  });
}

void clear_runtime_binding_value(RuntimeBinding& b) {
  b.value = 0.0f;
  b.physical_value = 0.0f;
  b.active_until_ns = 0;
  b.pulse_series_start_ns = 0;
  b.pulse_series_until_ns = 0;
}

bool binding_device_mapping_changed(const std::vector<RuntimeBinding>& a,
                                    const std::vector<DeviceInfo>& a_devices,
                                    const std::vector<RuntimeBinding>& b,
                                    const std::vector<DeviceInfo>& b_devices) {
  if (a.size() != b.size()) return true;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto device_key = [](const RuntimeBinding& rb, const std::vector<DeviceInfo>& devices) -> std::string {
      if (rb.device_index < 0 || static_cast<size_t>(rb.device_index) >= devices.size()) return {};
      const auto& fp = devices[rb.device_index].fingerprint;
      return hex_u64(fp.stable_hash) + "|" + fp.event_path + "|" + fp.by_id_path + "|" + fp.by_path;
    };
    if (device_key(a[i], a_devices) != device_key(b[i], b_devices)) return true;
  }
  return false;
}

void run_service(InputProvider& provider, AppConfig cfg, bool verbose) {
  std::vector<DeviceInfo> devices;
  std::vector<RuntimeBinding> bindings;
  std::vector<RuntimeBinding> hold_toggle_bindings;
  std::set<size_t> grabbed_indices;
  bool grabbed = false;
  size_t last_logged_resolved = std::numeric_limits<size_t>::max();

  auto release_devices = [&]() {
    if (grabbed) {
      provider.set_device_grab(devices, grabbed_indices, false, &std::cerr);
      grabbed = false;
    }
    grabbed_indices.clear();
    provider.close_devices(devices);
  };

  auto attach_devices = [&](const char* reason, bool log_warnings) {
    release_devices();
    devices = provider.scan_devices(true);
    if (verbose) print_devices(devices);
    bindings = resolve_bindings(cfg, devices, log_warnings);
    hold_toggle_bindings = resolve_hold_toggle_bindings(cfg, devices, log_warnings);
    const size_t resolved = count_connected_bindings(bindings, devices);
    const size_t toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
    if (verbose || resolved + toggle_resolved != last_logged_resolved) {
      std::cout << "[override_controller] " << reason << ": "
                << cfg.bindings.size() << " bindings, " << resolved
                << " resolved, hold_toggle_bindings=" << cfg.hold_toggle_bindings.size()
                << ", hold_toggle_resolved=" << toggle_resolved
                << ", open_devices=" << count_open_devices(devices) << "\n";
      last_logged_resolved = resolved + toggle_resolved;
    }
    grabbed_indices = collect_resolved_device_indices(bindings);
    const auto toggle_grabbed_indices = collect_resolved_device_indices(hold_toggle_bindings);
    grabbed_indices.insert(toggle_grabbed_indices.begin(), toggle_grabbed_indices.end());
    grabbed = false;
    if (cfg.input.grab_devices && !grabbed_indices.empty()) {
      grabbed = provider.set_device_grab(devices, grabbed_indices, true, &std::cerr);
    }
    provider.flush_events(devices);
  };

  attach_devices("initial attach", true);
  const size_t resolved = count_connected_bindings(bindings, devices);
  const size_t toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
  std::cout << "[override_controller] loaded config '" << cfg.name << "': "
            << cfg.bindings.size() << " bindings, " << resolved << " resolved"
            << ", hold_toggle_bindings=" << cfg.hold_toggle_bindings.size()
            << ", hold_toggle_resolved=" << toggle_resolved << "\n";
  std::cout << "[override_controller] publish SHM stream=" << cfg.publish.stream_id
            << " registry=" << cfg.publish.registry_path
            << " shm=" << cfg.publish.shm_name
            << " rate_hz=" << cfg.publish.rate_hz << "\n";
  std::cout << "[override_controller] input grab_devices=" << (cfg.input.grab_devices ? "true" : "false")
            << " reattach_devices=" << (cfg.input.reattach_devices ? "true" : "false")
            << " reattach_interval_ms=" << cfg.input.reattach_interval_ms
            << " event_wait_max_ms=" << cfg.input.event_wait_max_ms
            << " rel_axis_hold_ms=" << cfg.input.rel_axis_hold_ms
            << " rel_button_hold_ms=" << cfg.input.rel_button_hold_ms
            << " button_hold_ms=" << cfg.input.button_hold_ms
            << " button_release_grace_ms=" << cfg.input.button_release_grace_ms
            << " pulse_mode=" << (cfg.input.pulse_mode ? "true" : "false")
            << " dpad_pulse_gap_ms=" << cfg.input.dpad_pulse_gap_ms
            << " dpad_release_ms=" << cfg.input.dpad_release_ms
            << " button_pulse_gap_ms=" << cfg.input.button_pulse_gap_ms
            << " button_release_ms=" << cfg.input.button_release_ms
            << " button_pulse_startup_ms=" << cfg.input.button_pulse_startup_ms
            << " button_pulse_startup_release_ms=" << cfg.input.button_pulse_startup_release_ms
            << " button_pulse_startup_types=" << action_list_to_string(cfg.input.button_pulse_startup_types)
            << " hold_toggle_debounce_ms=" << cfg.input.hold_toggle_debounce_ms << "\n";

  ControllerInputPublisher publisher(cfg.publish);
  CounterState counters;

  const double rate_hz = std::max(1.0, cfg.publish.rate_hz);
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / rate_hz));
  const auto reattach_interval = std::chrono::milliseconds(std::max<uint32_t>(100, cfg.input.reattach_interval_ms));
  const int event_wait_cap_ms = static_cast<int>(std::max<uint32_t>(1, cfg.input.event_wait_max_ms));
  const int64_t rel_axis_hold_ns = static_cast<int64_t>(std::max<uint32_t>(1, cfg.input.rel_axis_hold_ms)) * 1000000LL;
  const int64_t rel_button_hold_ns = static_cast<int64_t>(std::max<uint32_t>(1, cfg.input.rel_button_hold_ms)) * 1000000LL;
  const int64_t button_hold_ns = static_cast<int64_t>(cfg.input.button_hold_ms) * 1000000LL;
  const int64_t button_release_grace_ns = static_cast<int64_t>(cfg.input.button_release_grace_ms) * 1000000LL;
  const int64_t dpad_pulse_window_ns = pulse_window_ns(cfg.input.dpad_pulse_gap_ms, cfg.input.dpad_release_ms);
  const int64_t hold_toggle_debounce_ns =
      static_cast<int64_t>(cfg.input.hold_toggle_debounce_ms) * 1000000LL;
  auto next_publish = std::chrono::steady_clock::now();
  auto next_reattach_check = std::chrono::steady_clock::now() + reattach_interval;

  while (!g_stop) {
    const auto before_wait = std::chrono::steady_clock::now();
    int wait_ms = 1;
    if (next_publish > before_wait) {
      wait_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next_publish - before_wait).count());
      wait_ms = std::clamp(wait_ms, 1, event_wait_cap_ms);
    }

    std::optional<InputEvent> ev;
    if (count_open_devices(devices) > 0) {
      ev = provider.wait_event(devices, wait_ms, false);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }

    if (ev && ev->device_index < devices.size()) {
      std::set<size_t> suppressed_normal_bindings;

      for (auto& tb : hold_toggle_bindings) {
        if (!event_matches_runtime_binding(tb, *ev)) continue;

        const float toggle_input_value = value_for_binding(tb.cfg, ev->value);
        const bool active_edge = toggle_input_value >= 0.50f;
        if (active_edge) {
          // Treat repeated pulses close to each other as one physical click.
          // A new click is recognized only after the input has been quiet for
          // hold_toggle_debounce_ns. This avoids toggling on/off repeatedly
          // when a pulse-style controller emits repeats while held; some
          // Bluetooth remotes have early repeat gaps close to one second.
          const bool new_click = ev->timestamp_ns > tb.toggle_debounce_until_ns;
          tb.toggle_debounce_until_ns = ev->timestamp_ns + hold_toggle_debounce_ns;
          if (new_click) {
            tb.value = tb.value >= 0.50f ? 0.0f : 1.0f;
            tb.physical_value = 0.0f;
            tb.active_until_ns = 0;
            if (verbose) {
              std::cout << "[override_controller] hold-toggle " << to_string(tb.cfg.side) << "."
                        << to_string(tb.cfg.action) << " -> " << (tb.value >= 0.50f ? "on" : "off") << "\n";
            }
          }
        }

        // Priority rule: a hold-toggle binding consumes the same physical input
        // on the same controller side, even if that input is also configured as
        // a normal binding. This prevents one click from producing both a
        // momentary press and a virtual latched hold.
        for (size_t i = 0; i < bindings.size(); ++i) {
          if (same_physical_input_on_side(tb, bindings[i])) {
            suppressed_normal_bindings.insert(i);
            clear_runtime_binding_value(bindings[i]);
          }
        }
      }

      for (size_t i = 0; i < bindings.size(); ++i) {
        if (suppressed_normal_bindings.count(i) != 0) continue;
        auto& b = bindings[i];
        if (!event_matches_runtime_binding(b, *ev)) continue;
        int64_t hold_ns = binding_is_rel_button_like(b.cfg) ? rel_button_hold_ns : button_hold_ns;
        int64_t release_grace_ns = binding_is_rel_button_like(b.cfg) ? 0 : button_release_grace_ns;
        const float binding_value = value_for_binding(b.cfg, ev->value);
        if (cfg.input.pulse_mode) {
          const int64_t pulse_ns = pulse_release_ns_for_binding(cfg.input, b.cfg);
          if (binding_is_rel_button_like(b.cfg) && is_dpad_action(b.cfg.action)) {
            hold_ns = dpad_pulse_window_ns;
          } else if (!binding_is_rel_button_like(b.cfg) && pulse_ns > 0) {
            release_grace_ns = button_pulse_grace_ns_for_event(cfg.input, b, ev->timestamp_ns, binding_value, pulse_ns);
          }
        }
        update_binding_value_from_event(b, binding_value, ev->timestamp_ns, hold_ns, release_grace_ns);
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (cfg.input.reattach_devices && now >= next_reattach_check) {
      next_reattach_check = now + reattach_interval;

      std::vector<DeviceInfo> candidate_devices = provider.scan_devices(true);
      std::vector<RuntimeBinding> candidate_bindings = resolve_bindings(cfg, candidate_devices, false);
      std::vector<RuntimeBinding> candidate_toggle_bindings = resolve_hold_toggle_bindings(cfg, candidate_devices, false);
      const size_t current_resolved = count_connected_bindings(bindings, devices);
      const size_t current_toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
      const size_t candidate_resolved = count_connected_bindings(candidate_bindings, candidate_devices);
      const size_t candidate_toggle_resolved = count_connected_bindings(candidate_toggle_bindings, candidate_devices);
      const bool mapping_changed =
          binding_device_mapping_changed(bindings, devices, candidate_bindings, candidate_devices) ||
          binding_device_mapping_changed(hold_toggle_bindings, devices, candidate_toggle_bindings, candidate_devices);
      const bool should_reattach = candidate_resolved + candidate_toggle_resolved != current_resolved + current_toggle_resolved || mapping_changed;
      if (should_reattach) {
        const bool had_active_bindings = any_runtime_binding_active(bindings) ||
                                         any_runtime_binding_active(hold_toggle_bindings);
        preserve_runtime_binding_state(candidate_bindings, bindings);
        preserve_runtime_binding_state(candidate_toggle_bindings, hold_toggle_bindings);
        release_devices();
        devices = std::move(candidate_devices);
        bindings = std::move(candidate_bindings);
        hold_toggle_bindings = std::move(candidate_toggle_bindings);
        const size_t new_resolved = count_connected_bindings(bindings, devices);
        const size_t new_toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
        const bool preserved_active_bindings = had_active_bindings &&
            (any_runtime_binding_active(bindings) || any_runtime_binding_active(hold_toggle_bindings));
        std::cout << "[override_controller] reattach: " << current_resolved << "+" << current_toggle_resolved
                  << " -> " << new_resolved << "+" << new_toggle_resolved
                  << " resolved, open_devices=" << count_open_devices(devices)
                  << " preserved_active=" << (preserved_active_bindings ? "yes" : "no") << "\n";
        grabbed_indices = collect_resolved_device_indices(bindings);
        const auto toggle_indices = collect_resolved_device_indices(hold_toggle_bindings);
        grabbed_indices.insert(toggle_indices.begin(), toggle_indices.end());
        grabbed = false;
        if (cfg.input.grab_devices && !grabbed_indices.empty()) {
          grabbed = provider.set_device_grab(devices, grabbed_indices, true, &std::cerr);
        }
        provider.flush_events(devices);
      } else {
        provider.close_devices(candidate_devices);
      }
    }

    const int64_t now_ns = monotonic_now_ns();
    decay_relative_axis_bindings(bindings, now_ns, rel_axis_hold_ns);
    decay_button_hold_bindings(bindings, now_ns);
    now = std::chrono::steady_clock::now();
    if (now >= next_publish) {
      const auto out = compose_state(bindings, hold_toggle_bindings, devices, counters);
      publisher.publish(out);
      do {
        next_publish += period;
      } while (next_publish <= now);
    }
  }

  // Publish several neutral frames before releasing EVIOCGRAB. This prevents
  // SteamVR/OpenVR from keeping the last non-zero D-pad/stick/button sample
  // after the override service is stopped while a control is still active.
  clear_runtime_bindings(bindings);
  clear_runtime_bindings(hold_toggle_bindings);
  publish_neutral_frames(publisher, cfg, counters);
  provider.flush_events(devices);
  release_devices();
  std::cout << "[override_controller] stopped, frames=" << publisher.sequence() << "\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Args args = parse_args(argc, argv);
    auto provider = make_platform_input_provider();
    if (!provider) throw std::runtime_error("no input provider for this platform yet");

    if (args.list_devices) {
      auto devices = provider->scan_devices(true);
      print_devices(devices);
      return 0;
    }

    if (args.non_interactive && args.config_path.empty()) {
      const auto files = list_config_files(args.config_dir);
      if (files.size() != 1 && !args.train) {
        throw std::runtime_error("--non-interactive requires --config or exactly one config file in --config-dir");
      }
    }

    AppConfig cfg;
    fs::path selected_path;
    if (args.train) {
      selected_path = args.config_path.empty()
          ? choose_or_create_config_path(args.config_dir, args.config_name)
          : args.config_path;
      cfg = train_config(*provider, selected_path, args.config_name);
    } else if (!args.config_path.empty()) {
      selected_path = args.config_path;
      cfg = load_config_file(selected_path);
    } else {
      const auto files = list_config_files(args.config_dir);
      if (files.empty()) {
        if (args.non_interactive || !isatty(STDIN_FILENO)) {
          throw std::runtime_error("no override_controller config found in " + args.config_dir.string());
        }
        std::cout << "No override_controller config found in " << args.config_dir << "\n";
        if (!ask_yes_no("Start interactive training now?", true)) return 1;
        selected_path = choose_or_create_config_path(args.config_dir, args.config_name);
        cfg = train_config(*provider, selected_path, args.config_name);
      } else if (files.size() == 1) {
        selected_path = files[0];
        cfg = load_config_file(selected_path);
      } else {
        if (args.non_interactive || !isatty(STDIN_FILENO)) {
          throw std::runtime_error("multiple override_controller configs found; pass --config explicitly");
        }
        selected_path = choose_config_interactive(files);
        cfg = load_config_file(selected_path);
      }
    }

    apply_publish_overrides(cfg, args);
    std::cout << "[override_controller] using config: " << selected_path << "\n";
    run_service(*provider, cfg, args.verbose);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[override_controller][ERROR] " << e.what() << "\n";
    return 1;
  }
}
