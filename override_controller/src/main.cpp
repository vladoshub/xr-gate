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
#else
#define NOMINMAX
#include <windows.h>
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
  bool allow_shared_physical_device_sides_override_set = false;
  bool allow_shared_physical_device_sides = true;
  bool reattach_devices_override_set = false;
  bool reattach_devices = true;
  uint32_t reattach_interval_ms = 0;
  uint32_t event_wait_max_ms = 0;
  bool train = false;
  bool list_devices = false;
  bool connect_devices = false;
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
      "  --connect-devices        Reconnect/update configured devices, save config, and exit\n"
      "  --publish-registry <p>   Tracking registry path for controller_input SHM\n"
      "  --publish-stream <name>  Stream id written into the registry. Default: controller_input\n"
      "  --publish-shm-name <n>   POSIX SHM name. Default: controller_input\n"
      "  --publish-transport <shm|tcp> Publish transport. Linux default: shm, Windows default: tcp\n"
      "  --publish-tcp-bind-host <host> TCP bind host for --publish-transport tcp. Default: 127.0.0.1\n"
      "  --publish-tcp-port <port> TCP port for --publish-transport tcp. Default: 45672\n"
      "  --publish-rate-hz <hz>   Publish rate override\n"
      "  --publish-slots <n>      Ring slot count override\n"
      "  --grab-devices <bool>    Exclusively capture whole mapped input devices when running\n"
      "  --no-grab-devices        Disable exclusive input capture override\n"
      "  --allow-shared-physical-device-sides <bool> Allow one physical device/key to drive both sides\n"
      "  --reattach-devices <bool> Periodically rescan/re-resolve devices after reconnect\n"
      "  --no-reattach-devices    Disable device reattach/rescan\n"
      "  --reattach-interval-ms <n> Rescan interval. Default: 1000\n"
      "  --event-wait-max-ms <n>  Max event select wait. Higher reduces idle CPU\n"
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
    } else if (v == "--allow-shared-physical-device-sides") {
      a.allow_shared_physical_device_sides_override_set = true;
      a.allow_shared_physical_device_sides = parse_bool_arg(need(v.c_str()), v.c_str());
    } else if (v == "--no-shared-physical-device-sides") {
      a.allow_shared_physical_device_sides_override_set = true;
      a.allow_shared_physical_device_sides = false;
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
    } else if (v == "--train") a.train = true;
    else if (v == "--list-devices") a.list_devices = true;
    else if (v == "--connect-devices" || v == "connect-devices") a.connect_devices = true;
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


bool fingerprint_same_config_device(const DeviceFingerprint& a, const DeviceFingerprint& b) {
  // Config device IDs must represent concrete physical input nodes, not just
  // device models. Two identical controllers often share stable_hash, name,
  // vendor/product/bustype, so those fields must not be used for de-dup here.
  //
  // Linux/Bluetooth note: evdev `phys` may identify the host adapter path rather
  // than the concrete controller. Two VR-PARK controllers can therefore have the
  // same phys value while their `uniq` values differ. Treat phys only as a weak
  // fallback when no stronger field conflicts.
  bool strong_conflict = false;
  if (!a.uniq.empty() && !b.uniq.empty()) {
    if (a.uniq == b.uniq) return true;
    strong_conflict = true;
  }
  if (!a.by_id_path.empty() && !b.by_id_path.empty()) {
    if (a.by_id_path == b.by_id_path) return true;
    strong_conflict = true;
  }
  if (!a.by_path.empty() && !b.by_path.empty()) {
    if (a.by_path == b.by_path) return true;
    strong_conflict = true;
  }
  if (!a.event_path.empty() && !b.event_path.empty()) {
    if (a.event_path == b.event_path) return true;
    strong_conflict = true;
  }

  if (strong_conflict) return false;
  if (!a.phys.empty() && !b.phys.empty() && a.phys == b.phys) return true;

  // No stable per-device identity is available. Keep devices separate instead
  // of merging same-name/same-model controllers. --connect-devices can later
  // update name-only template devices with real fingerprints.
  return false;
}

ConfigDevice* find_config_device(AppConfig& cfg, int id) {
  for (auto& d : cfg.devices) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

const ConfigDevice* find_config_device(const AppConfig& cfg, int id) {
  for (const auto& d : cfg.devices) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

int ensure_config_device(AppConfig& cfg, const DeviceFingerprint& fp) {
  for (const auto& d : cfg.devices) {
    if (fingerprint_same_config_device(d.fingerprint, fp)) return d.id;
  }
  int next_id = 1;
  for (const auto& d : cfg.devices) next_id = std::max(next_id, d.id + 1);
  ConfigDevice d;
  d.id = next_id;
  d.fingerprint = fp;
  cfg.devices.push_back(d);
  return d.id;
}

void sync_binding_device_from_registry(AppConfig& cfg, BindingConfig& b) {
  if (b.device_id <= 0) {
    b.device_id = ensure_config_device(cfg, b.device);
  }
  if (ConfigDevice* d = find_config_device(cfg, b.device_id)) {
    b.device = d->fingerprint;
  }
}

void sync_devices_from_registry(AppConfig& cfg) {
  for (auto& b : cfg.bindings) sync_binding_device_from_registry(cfg, b);
  for (auto& b : cfg.hold_toggle_bindings) sync_binding_device_from_registry(cfg, b);
  for (auto& b : cfg.alternative_bindings) sync_binding_device_from_registry(cfg, b);
  for (auto& b : cfg.alternative_hold_toggle_bindings) sync_binding_device_from_registry(cfg, b);
  if (cfg.layout_switch.enabled) {
    if (cfg.layout_switch.device_id <= 0) {
      cfg.layout_switch.device_id = ensure_config_device(cfg, cfg.layout_switch.device);
    }
    if (ConfigDevice* d = find_config_device(cfg, cfg.layout_switch.device_id)) {
      cfg.layout_switch.device = d->fingerprint;
    }
  }
}

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


bool fingerprint_same_strict_input_device(const DeviceFingerprint& wanted, const DeviceFingerprint& candidate) {
  // During training and at runtime, the layout-switch input must be scoped to
  // one concrete input device, not just to a similar device model. Two identical
  // controllers often share name/vendor/product/stable_hash, so do not use
  // fuzzy scoring or stable_hash here.
  //
  // Linux/Bluetooth note: evdev `phys` can be shared by two controllers on the
  // same host adapter. If a stronger field exists and conflicts, do not fall
  // back to phys, or the switch button on one VR-PARK can match the other.
  bool strong_conflict = false;
  if (!wanted.uniq.empty() && !candidate.uniq.empty()) {
    if (wanted.uniq == candidate.uniq) return true;
    strong_conflict = true;
  }
  if (!wanted.by_id_path.empty() && !candidate.by_id_path.empty()) {
    if (wanted.by_id_path == candidate.by_id_path) return true;
    strong_conflict = true;
  }
  if (!wanted.by_path.empty() && !candidate.by_path.empty()) {
    if (wanted.by_path == candidate.by_path) return true;
    strong_conflict = true;
  }
  if (!wanted.event_path.empty() && !candidate.event_path.empty()) {
    if (wanted.event_path == candidate.event_path) return true;
    strong_conflict = true;
  }

  if (strong_conflict) return false;
  if (!wanted.phys.empty() && !candidate.phys.empty() && wanted.phys == candidate.phys) return true;

  // No strict identity is available. Treat it as not the same device instead
  // of reserving/switching the same button code on every identical controller.
  return false;
}

bool input_event_matches_spec(const InputProvider& provider,
                              const std::vector<DeviceInfo>& devices,
                              const InputEvent& ev,
                              const DeviceFingerprint& wanted_device,
                              const InputBindingSpec& wanted_input) {
  if (ev.device_index >= devices.size()) return false;
  if (!fingerprint_same_strict_input_device(wanted_device, devices[ev.device_index].fingerprint)) {
    return false;
  }
  if (wanted_input.type != ev.type || wanted_input.code != ev.code) return false;
  if (wanted_input.kind == InputKind::AbsAxis) {
    InputBindingSpec tmp = provider.make_input_spec(devices[ev.device_index], ev.type, ev.code);
    const int dir = axis_direction(tmp, ev.value);
    return wanted_input.abs_direction == 0 || dir == wanted_input.abs_direction;
  }
  if (wanted_input.kind == InputKind::RelAxis) {
    const int dir = rel_direction(ev.value);
    return wanted_input.abs_direction == 0 || dir == wanted_input.abs_direction;
  }
  return true;
}

bool event_matches_layout_switch_training(const InputProvider& provider,
                                          const std::vector<DeviceInfo>& devices,
                                          const LayoutSwitchConfig* sw,
                                          const InputEvent& ev) {
  if (!sw || !sw->enabled) return false;
  return input_event_matches_spec(provider, devices, ev, sw->device, sw->input);
}

bool capture_layout_switch_button(InputProvider& provider,
                                  std::vector<DeviceInfo>& devices,
                                  const TrainingInputFilter& filter,
                                  AppConfig& cfg) {
  provider.flush_events(devices);
  std::cout << "\nOptional alternative layout switch button.\n"
            << "  If you want to switch to an alternative layout while the service is running, choose one button on one of your input devices.\n"
            << "  If you choose a button, after the default layout is configured you will be asked whether to configure an alternative layout.\n"
            << "  This is recommended when your input device has a limited number of physical buttons.\n";
  if (!ask_yes_no("Configure an alternative layout switch button now?", false)) return false;
  std::cout << "\n[learn] layout_switch: press one button now, or press Enter to skip.\n";
  while (!g_stop) {
    auto ev = provider.wait_event(devices, 25000, true);
    if (!ev) {
      std::cout << "  timeout; skipped.\n";
      return false;
    }
    if (!training_filter_accepts(filter, provider, devices, *ev)) continue;
    if (ev->device_index == std::numeric_limits<size_t>::max()) {
      std::cout << "  skipped.\n";
      return false;
    }
    if (ev->device_index >= devices.size()) continue;
    const auto& dev = devices[ev->device_index];
    if (!dev.readable) continue;

    InputBindingSpec tmp;
    if (ev->type == EV_KEY) {
      if (ev->value != 1) continue;
      tmp = provider.make_input_spec(dev, ev->type, ev->code);
    } else if (ev->type == EV_ABS) {
      tmp = provider.make_input_spec(dev, ev->type, ev->code);
      const int dir = axis_direction(tmp, ev->value);
      if (dir == 0) continue;
      tmp.abs_direction = dir;
    } else if (ev->type == EV_REL) {
      tmp = provider.make_input_spec(dev, ev->type, ev->code);
      const int dir = rel_direction(ev->value);
      if (dir == 0) continue;
      tmp.abs_direction = dir;
    } else {
      continue;
    }

    cfg.layout_switch.enabled = true;
    cfg.layout_switch.device = dev.fingerprint;
    cfg.layout_switch.device_id = ensure_config_device(cfg, dev.fingerprint);
    cfg.layout_switch.input = tmp;
    std::cout << "  mapped layout_switch <= " << cfg.layout_switch.input.name
              << " on device_id=" << cfg.layout_switch.device_id << " "
              << short_device_label(cfg.layout_switch.device) << "\n";
    wait_for_input_quiet(provider, devices);
    return true;
  }
  return false;
}

bool capture_binding(InputProvider& provider,
                     std::vector<DeviceInfo>& devices,
                     const TrainingInputFilter& filter,
                     AppConfig& cfg,
                     ControllerSide side,
                     ControllerAction action,
                     BindingConfig& out,
                     const LayoutSwitchConfig* reserved_switch = nullptr) {
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
    if (event_matches_layout_switch_training(provider, devices, reserved_switch, *ev)) {
      std::cout << "  layout switch button is reserved; choose another input for this mapping.\n";
      continue;
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
      out.device_id = ensure_config_device(cfg, dev.fingerprint);
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
      out.device_id = ensure_config_device(cfg, dev.fingerprint);
      out.input = tmp;
    } else if (ev->type == EV_KEY) {
      out.side = side;
      out.action = action;
      out.device = dev.fingerprint;
      out.device_id = ensure_config_device(cfg, dev.fingerprint);
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
                                AppConfig& cfg,
                                const std::vector<BindingConfig>& normal_bindings,
                                std::vector<BindingConfig>& target_bindings,
                                const std::string& layout_label,
                                const LayoutSwitchConfig* reserved_switch) {
  std::cout << "\nOptional " << layout_label << " long-press toggle emulation.\n"
            << "  You can configure a mode where a single click starts a virtual long press "
            << "for the selected action, and the next click releases it.\n"
            << "  If you use an already configured button on the same controller, this "
            << "long-press toggle binding has priority over the normal binding.\n";
  if (!ask_yes_no("Configure " + layout_label + " long-press toggle emulation now?", false)) return;

  for (ControllerSide side : {ControllerSide::Left, ControllerSide::Right}) {
    const bool has_normal_side_bindings =
        std::any_of(normal_bindings.begin(), normal_bindings.end(), [side](const BindingConfig& b) { return b.side == side; });
    if (!ask_yes_no("Configure " + to_string(side) + " " + layout_label + " long-press toggle bindings?",
                    has_normal_side_bindings)) {
      continue;
    }

    std::cout << "\n=== " << to_string(side) << " " << layout_label << " long-press toggle bindings ===\n";
    for (const auto action : default_actions()) {
      BindingConfig b;
      if (capture_binding(provider, devices, training_filter, cfg, side, action, b, reserved_switch)) {
        target_bindings.push_back(std::move(b));
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
  if (args.allow_shared_physical_device_sides_override_set) {
    cfg.input.allow_shared_physical_device_sides = args.allow_shared_physical_device_sides;
  }
  if (args.reattach_devices_override_set) cfg.input.reattach_devices = args.reattach_devices;
  if (args.reattach_interval_ms > 0) cfg.input.reattach_interval_ms = args.reattach_interval_ms;
  if (args.event_wait_max_ms > 0) cfg.input.event_wait_max_ms = args.event_wait_max_ms;
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
  std::cout << "Each binding refers to a config device_id; device fingerprints are stored once in the top-level devices list.\n";

  capture_layout_switch_button(provider, devices, training_filter, cfg);
  const LayoutSwitchConfig* reserved_switch = cfg.layout_switch.enabled ? &cfg.layout_switch : nullptr;

  for (ControllerSide side : {ControllerSide::Left, ControllerSide::Right}) {
    if (!ask_yes_no("Configure " + to_string(side) + " controller?", true)) continue;
    std::cout << "\n=== " << to_string(side) << " controller ===\n";
    for (const auto action : default_actions()) {
      BindingConfig b;
      if (capture_binding(provider, devices, training_filter, cfg, side, action, b, reserved_switch)) {
        cfg.bindings.push_back(std::move(b));
      }
    }
  }

  train_hold_toggle_bindings(provider, devices, training_filter, cfg,
                             cfg.bindings, cfg.hold_toggle_bindings, "default layout", reserved_switch);

  if (cfg.layout_switch.enabled && ask_yes_no("Configure alternative layout now?", true)) {
    std::cout << "\n=== alternative layout ===\n";
    for (ControllerSide side : {ControllerSide::Left, ControllerSide::Right}) {
      const bool has_default_side =
          std::any_of(cfg.bindings.begin(), cfg.bindings.end(), [side](const BindingConfig& b) { return b.side == side; });
      if (!ask_yes_no("Configure " + to_string(side) + " alternative controller?", has_default_side)) continue;
      std::cout << "\n=== " << to_string(side) << " alternative controller ===\n";
      for (const auto action : default_actions()) {
        BindingConfig b;
        if (capture_binding(provider, devices, training_filter, cfg, side, action, b, reserved_switch)) {
          cfg.alternative_bindings.push_back(std::move(b));
        }
      }
    }
    train_hold_toggle_bindings(provider, devices, training_filter, cfg,
                               cfg.alternative_bindings,
                               cfg.alternative_hold_toggle_bindings,
                               "alternative layout",
                               reserved_switch);
  }

  sync_devices_from_registry(cfg);

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

std::vector<fs::path> unique_sorted_paths(std::vector<fs::path> files) {
  for (auto& f : files) {
    f = f.lexically_normal();
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  return files;
}

fs::path current_executable_dir(const char* argv0) {
#if defined(__linux__)
  char buf[4096];
  const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return fs::path(buf).parent_path();
  }
#endif
#if defined(_WIN32)
  char buf[MAX_PATH];
  const DWORD len = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
  if (len > 0 && len < sizeof(buf)) {
    return fs::path(buf).parent_path();
  }
#endif
  if (argv0 && *argv0) {
    fs::path p(argv0);
    if (p.has_parent_path()) return fs::absolute(p).parent_path();
  }
  return fs::current_path();
}

std::vector<fs::path> connect_device_config_candidates(const fs::path& config_dir,
                                                       const fs::path& executable_dir) {
  const fs::path default_path = config_dir / "default.json";
  if (fs::is_regular_file(default_path)) return {default_path};

  std::vector<fs::path> files = list_config_files(config_dir);
  const auto saved_files = list_config_files(config_dir / "configs");
  files.insert(files.end(), saved_files.begin(), saved_files.end());

  // Bundled preset configs copied by CMake/install next to the executable.
  // Example release layout: bin/override_controller/configs/*.json
  const auto bundled_files = list_config_files(executable_dir / "configs");
  files.insert(files.end(), bundled_files.begin(), bundled_files.end());

  return unique_sorted_paths(std::move(files));
}

struct RuntimeBinding {
  BindingConfig cfg;
  DeviceInputConfig device_input;
  int device_index = -1;
  int match_score = 0;
  float value = 0.0f;
  float physical_value = 0.0f;
  int64_t active_until_ns = 0;
  int64_t last_event_ns = 0;
  int64_t pulse_series_start_ns = 0;
  int64_t pulse_series_until_ns = 0;
  int64_t toggle_debounce_until_ns = 0;
  bool clear_after_publish = false;
  bool connected = false;
};

struct RuntimeLayoutSwitch {
  LayoutSwitchConfig cfg;
  int device_index = -1;
  int match_score = 0;
  bool pressed = false;
  int64_t debounce_until_ns = 0;
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

int64_t pulse_release_ns_for_binding(const DeviceInputConfig& input, const BindingConfig& cfg) {
  if (!input.pulse_mode || !binding_uses_min_hold(cfg)) return 0;
  if (is_dpad_action(cfg.action)) {
    return pulse_window_ns(input.dpad_pulse_gap_ms, input.dpad_release_ms);
  }
  return pulse_window_ns(input.button_pulse_gap_ms, input.button_release_ms);
}

bool button_pulse_startup_applies_to_action(const DeviceInputConfig& input, ControllerAction action) {
  if (input.button_pulse_startup_types.empty()) return true;
  return std::find(input.button_pulse_startup_types.begin(), input.button_pulse_startup_types.end(), action) !=
         input.button_pulse_startup_types.end();
}

int64_t button_pulse_grace_ns_for_event(const DeviceInputConfig& input, RuntimeBinding& b, int64_t event_ns, float value, int64_t normal_grace_ns) {
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

  // EV_REL inputs never have a physical release state. With a non-zero hold
  // window they remain active until the configured timeout. With a zero hold
  // window they are still published for exactly one controller_input frame,
  // then cleared immediately after that frame.
  if (b.cfg.input.kind == InputKind::RelAxis) {
    b.physical_value = 0.0f;
    const bool active = is_axis_action(b.cfg.action) ? std::abs(value) > 0.0f : value >= 0.50f;
    if (active) {
      b.value = value;
      if (hold_ns > 0) {
        b.active_until_ns = std::max(b.active_until_ns, event_ns + hold_ns);
        b.clear_after_publish = false;
      } else {
        b.active_until_ns = 0;
        b.clear_after_publish = true;
      }
    } else if (hold_ns <= 0 || event_ns > b.active_until_ns) {
      b.value = 0.0f;
      b.active_until_ns = 0;
      b.clear_after_publish = false;
    }
    return;
  }

  if (!binding_uses_min_hold(b.cfg) || (hold_ns <= 0 && release_grace_ns <= 0)) {
    b.value = value;
    b.physical_value = value;
    b.active_until_ns = 0;
    b.clear_after_publish = false;
    return;
  }

  b.physical_value = value;
  b.clear_after_publish = false;
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


std::vector<RuntimeBinding> resolve_binding_configs(const AppConfig& app_config,
                                                   const std::vector<BindingConfig>& configs,
                                                   const std::vector<DeviceInfo>& devices,
                                                   bool log_warnings = true,
                                                   const char* label = "binding") {
  std::vector<RuntimeBinding> out;
  for (const auto& b : configs) {
    RuntimeBinding rb;
    rb.cfg = b;
    if (const ConfigDevice* device = find_config_device(app_config, b.device_id)) {
      rb.device_input = device->input;
    }
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
  return resolve_binding_configs(cfg, cfg.bindings, devices, log_warnings, "binding");
}

std::vector<RuntimeBinding> resolve_hold_toggle_bindings(const AppConfig& cfg,
                                                         const std::vector<DeviceInfo>& devices,
                                                         bool log_warnings = true) {
  return resolve_binding_configs(cfg, cfg.hold_toggle_bindings, devices, log_warnings, "hold-toggle binding");
}


std::vector<RuntimeBinding> resolve_alternative_bindings(const AppConfig& cfg,
                                                         const std::vector<DeviceInfo>& devices,
                                                         bool log_warnings = true) {
  return resolve_binding_configs(cfg, cfg.alternative_bindings, devices, log_warnings, "alternative binding");
}

std::vector<RuntimeBinding> resolve_alternative_hold_toggle_bindings(const AppConfig& cfg,
                                                                     const std::vector<DeviceInfo>& devices,
                                                                     bool log_warnings = true) {
  return resolve_binding_configs(cfg, cfg.alternative_hold_toggle_bindings, devices, log_warnings,
                                 "alternative hold-toggle binding");
}

void collect_layout_switch_candidate_indices(const std::vector<RuntimeBinding>& source,
                                             int config_device_id,
                                             std::set<int>& out_indices) {
  if (config_device_id <= 0) return;
  for (const auto& b : source) {
    if (b.cfg.device_id != config_device_id) continue;
    if (b.device_index < 0 || !b.connected) continue;
    out_indices.insert(b.device_index);
  }
}

RuntimeLayoutSwitch resolve_layout_switch(const AppConfig& cfg,
                                          const std::vector<DeviceInfo>& devices,
                                          const std::vector<RuntimeBinding>& bindings,
                                          const std::vector<RuntimeBinding>& hold_toggle_bindings,
                                          const std::vector<RuntimeBinding>& alternative_bindings,
                                          const std::vector<RuntimeBinding>& alternative_hold_toggle_bindings,
                                          bool log_warnings = true) {
  RuntimeLayoutSwitch out;
  out.cfg = cfg.layout_switch;
  if (!cfg.layout_switch.enabled) return out;

  // First resolve through the explicit config device id. This is stronger than
  // fuzzy matching and more useful than requiring a fresh event_path: normal
  // controller bindings for the same device_id already resolved the physical
  // input node, so the layout switch must use that same node.
  std::set<int> indices_from_bindings;
  collect_layout_switch_candidate_indices(bindings, cfg.layout_switch.device_id, indices_from_bindings);
  collect_layout_switch_candidate_indices(hold_toggle_bindings, cfg.layout_switch.device_id, indices_from_bindings);
  collect_layout_switch_candidate_indices(alternative_bindings, cfg.layout_switch.device_id, indices_from_bindings);
  collect_layout_switch_candidate_indices(alternative_hold_toggle_bindings, cfg.layout_switch.device_id, indices_from_bindings);

  if (indices_from_bindings.size() == 1) {
    out.device_index = *indices_from_bindings.begin();
    if (out.device_index >= 0 && static_cast<size_t>(out.device_index) < devices.size()) {
      out.match_score = fingerprint_match_score(cfg.layout_switch.device, devices[out.device_index].fingerprint);
    }
    if (log_warnings) {
      std::cout << "[override_controller] layout switch resolved via device_id="
                << cfg.layout_switch.device_id << " to device_index=" << out.device_index
                << " input=" << cfg.layout_switch.input.name << "\n";
    }
    return out;
  }

  if (indices_from_bindings.size() > 1) {
    out.device_index = -1;
    if (log_warnings) {
      std::cerr << "[override_controller][ERROR] ambiguous layout switch device_id="
                << cfg.layout_switch.device_id
                << "; bindings for this config device resolved to multiple live input nodes. "
                << "Re-run --connect-devices for this config.\n";
    }
    return out;
  }

  // Fallback for a switch-only device: use a concrete identity field, but never
  // name/vendor/product/stable_hash. Those are model-level and can be identical
  // for left/right copies of the same controller.
  int strict_matches = 0;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (!devices[i].readable) continue;
    if (!fingerprint_same_strict_input_device(cfg.layout_switch.device, devices[i].fingerprint)) continue;
    out.device_index = static_cast<int>(i);
    out.match_score = fingerprint_match_score(cfg.layout_switch.device, devices[i].fingerprint);
    ++strict_matches;
  }

  if (strict_matches == 1) {
    if (log_warnings) {
      std::cout << "[override_controller] layout switch resolved via strict identity to device_index="
                << out.device_index << " input=" << cfg.layout_switch.input.name << "\n";
    }
    return out;
  }

  out.device_index = -1;
  if (strict_matches > 1) {
    if (log_warnings) {
      std::cerr << "[override_controller][ERROR] ambiguous strict device match for layout switch; refusing to bind; wanted="
                << short_device_label(cfg.layout_switch.device) << " matches=" << strict_matches << "\n";
    }
  } else if (log_warnings) {
    std::cerr << "[override_controller][WARN] layout switch unresolved: device_id="
              << cfg.layout_switch.device_id << " input=" << cfg.layout_switch.input.name
              << " wanted=" << short_device_label(cfg.layout_switch.device)
              << ". Re-run --connect-devices or --train if the device identity changed.\n";
  }
  return out;
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
                          CounterState& counters,
                          bool allow_shared_physical_device_sides) {
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


  if (!allow_shared_physical_device_sides) {
    bool side_device_overlap = false;
    for (const auto& device_id : left_devices) {
      if (right_devices.count(device_id) != 0) {
        side_device_overlap = true;
        break;
      }
    }
    if (side_device_overlap) {
      std::cerr << "[override_controller][ERROR] same physical input device resolved for both left and right; "
                   "suppressing both controller sides until bindings are retrained or sharing is enabled\n";
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

void decay_relative_axis_bindings(std::vector<RuntimeBinding>& bindings, int64_t now) {
  for (auto& b : bindings) {
    if (b.cfg.input.kind != InputKind::RelAxis || !is_axis_action(b.cfg.action)) continue;
    if (b.clear_after_publish) continue;
    if (b.value == 0.0f && b.physical_value == 0.0f) continue;
    const int64_t hold_ns = static_cast<int64_t>(b.device_input.rel_axis_hold_ms) * 1000000LL;
    if (b.last_event_ns == 0 || now - b.last_event_ns > hold_ns) {
      b.value = 0.0f;
      b.physical_value = 0.0f;
    }
  }
}

void decay_button_hold_bindings(std::vector<RuntimeBinding>& bindings, int64_t now) {
  for (auto& b : bindings) {
    if (!binding_uses_min_hold(b.cfg)) continue;
    if (b.clear_after_publish) continue;
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

void clear_one_frame_relative_bindings(std::vector<RuntimeBinding>& bindings) {
  for (auto& b : bindings) {
    if (!b.clear_after_publish) continue;
    b.value = 0.0f;
    b.physical_value = 0.0f;
    b.active_until_ns = 0;
    b.clear_after_publish = false;
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
    b.clear_after_publish = false;
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
  for (const auto& b : cfg.alternative_bindings) mark_configured(b);
  for (const auto& b : cfg.alternative_hold_toggle_bindings) mark_configured(b);
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


bool event_matches_layout_switch(const RuntimeLayoutSwitch& sw, const InputEvent& ev) {
  return sw.cfg.enabled && sw.device_index == static_cast<int>(ev.device_index) &&
         sw.cfg.input.type == ev.type && sw.cfg.input.code == ev.code;
}

float value_for_layout_switch(const RuntimeLayoutSwitch& sw, int raw_value) {
  BindingConfig tmp;
  tmp.input = sw.cfg.input;
  tmp.action = ControllerAction::Menu;
  return value_for_binding(tmp, raw_value);
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
    dst.clear_after_publish = it->clear_after_publish;
  }
}

bool any_runtime_binding_active(const std::vector<RuntimeBinding>& bindings) {
  return std::any_of(bindings.begin(), bindings.end(), [](const RuntimeBinding& b) {
    return b.value >= 0.50f || b.physical_value >= 0.50f || b.active_until_ns != 0 ||
           b.pulse_series_until_ns != 0 || b.clear_after_publish;
  });
}

void clear_runtime_binding_value(RuntimeBinding& b) {
  b.value = 0.0f;
  b.physical_value = 0.0f;
  b.active_until_ns = 0;
  b.pulse_series_start_ns = 0;
  b.pulse_series_until_ns = 0;
  b.clear_after_publish = false;
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


std::string configured_device_usage(const AppConfig& cfg, int device_id) {
  std::set<ControllerSide> sides;
  bool has_switch = false;
  auto scan = [&](const std::vector<BindingConfig>& bindings) {
    for (const auto& b : bindings) {
      if (b.device_id == device_id) sides.insert(b.side);
    }
  };
  scan(cfg.bindings);
  scan(cfg.hold_toggle_bindings);
  scan(cfg.alternative_bindings);
  scan(cfg.alternative_hold_toggle_bindings);
  if (cfg.layout_switch.enabled && cfg.layout_switch.device_id == device_id) has_switch = true;

  std::string out;
  if (!sides.empty()) {
    out += "(";
    bool first = true;
    for (const auto side : sides) {
      if (!first) out += ",";
      first = false;
      out += to_string(side);
    }
    out += ")";
  }
  if (has_switch) {
    if (!out.empty()) out += " ";
    out += "layout-switch";
  }
  return out.empty() ? "unused" : out;
}

bool capture_device_fingerprint(InputProvider& provider,
                                std::vector<DeviceInfo>& devices,
                                DeviceFingerprint& out,
                                const std::string& prompt) {
  provider.flush_events(devices);
  std::cout << prompt << "\n";
  while (!g_stop) {
    auto ev = provider.wait_event(devices, 60000, false);
    if (!ev) {
      std::cout << "  timeout; device unchanged.\n";
      return false;
    }
    if (ev->stop_requested) return false;
    if (ev->device_index >= devices.size()) continue;
    const auto& dev = devices[ev->device_index];
    if (!dev.readable) continue;

    bool accept = false;
    if (ev->type == EV_KEY) {
      accept = ev->value == 1;
    } else if (ev->type == EV_ABS) {
      InputBindingSpec tmp = provider.make_input_spec(dev, ev->type, ev->code);
      accept = axis_direction(tmp, ev->value) != 0;
    } else if (ev->type == EV_REL) {
      accept = rel_direction(ev->value) != 0;
    }
    if (!accept) continue;

    out = dev.fingerprint;
    std::cout << "  captured [" << ev->device_index << "] " << short_device_label(out)
              << " via " << provider.input_name(ev->type, ev->code) << "\n";
    wait_for_input_quiet(provider, devices);
    return true;
  }
  return false;
}

void connect_config_devices(InputProvider& provider, AppConfig& cfg, const fs::path& config_path) {
  sync_devices_from_registry(cfg);
  auto devices = provider.scan_devices(true);
  print_devices(devices);
  if (cfg.devices.empty()) {
    std::cout << "[override_controller] config has no devices to reconnect.\n";
    return;
  }

  std::cout << "\nConfigured devices in config:\n";
  std::vector<int> ids;
  for (const auto& d : cfg.devices) {
    ids.push_back(d.id);
  }
  std::sort(ids.begin(), ids.end());
  for (const int id : ids) {
    const ConfigDevice* d = find_config_device(cfg, id);
    if (!d) continue;
    std::cout << "  device_id=" << id << " " << short_device_label(d->fingerprint)
              << " " << configured_device_usage(cfg, id) << "\n";
  }

  std::cout << "\nReconnect each configured device. For each prompt, press any button/move any control on that physical device.\n";
  for (const int id : ids) {
    ConfigDevice* d = find_config_device(cfg, id);
    if (!d) continue;
    const std::string usage = configured_device_usage(cfg, id);
    std::cout << "\nConfig device_id=" << id << " currently " << short_device_label(d->fingerprint)
              << " " << usage << "\n";
    if (!ask_yes_no("Update this device now?", true)) continue;
    DeviceFingerprint captured;
    if (capture_device_fingerprint(provider, devices, captured,
                                   "[connect-devices] Press any button on config device_id=" +
                                       std::to_string(id) + " " + usage + ".")) {
      d->fingerprint = captured;
    }
  }

  sync_devices_from_registry(cfg);
  save_config_file(cfg, config_path);
  std::cout << "\nSaved updated config: " << config_path << "\n";
}

void run_service(InputProvider& provider, AppConfig cfg, bool verbose) {
  std::vector<DeviceInfo> devices;
  std::vector<RuntimeBinding> bindings;
  std::vector<RuntimeBinding> hold_toggle_bindings;
  std::vector<RuntimeBinding> alternative_bindings;
  std::vector<RuntimeBinding> alternative_hold_toggle_bindings;
  RuntimeLayoutSwitch layout_switch;
  bool alternative_layout_active = false;
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
    alternative_bindings = resolve_alternative_bindings(cfg, devices, log_warnings);
    alternative_hold_toggle_bindings = resolve_alternative_hold_toggle_bindings(cfg, devices, log_warnings);
    layout_switch = resolve_layout_switch(cfg, devices,
                                           bindings,
                                           hold_toggle_bindings,
                                           alternative_bindings,
                                           alternative_hold_toggle_bindings,
                                           log_warnings);
    const size_t resolved = count_connected_bindings(bindings, devices);
    const size_t toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
    const size_t alt_resolved = count_connected_bindings(alternative_bindings, devices);
    const size_t alt_toggle_resolved = count_connected_bindings(alternative_hold_toggle_bindings, devices);
    const size_t switch_resolved = layout_switch.device_index >= 0 ? 1u : 0u;
    const size_t total_resolved = resolved + toggle_resolved + alt_resolved + alt_toggle_resolved + switch_resolved;
    if (verbose || total_resolved != last_logged_resolved) {
      std::cout << "[override_controller] " << reason << ": "
                << cfg.bindings.size() << " bindings, " << resolved
                << " resolved, hold_toggle_bindings=" << cfg.hold_toggle_bindings.size()
                << ", hold_toggle_resolved=" << toggle_resolved
                << ", alternative_bindings=" << cfg.alternative_bindings.size()
                << ", alternative_resolved=" << alt_resolved
                << ", alternative_hold_toggle_bindings=" << cfg.alternative_hold_toggle_bindings.size()
                << ", alternative_hold_toggle_resolved=" << alt_toggle_resolved
                << ", layout_switch=" << (cfg.layout_switch.enabled ? (switch_resolved ? "resolved" : "unresolved") : "disabled")
                << ", open_devices=" << count_open_devices(devices) << "\n";
      last_logged_resolved = total_resolved;
    }
    grabbed_indices = collect_resolved_device_indices(bindings);
    const auto toggle_grabbed_indices = collect_resolved_device_indices(hold_toggle_bindings);
    grabbed_indices.insert(toggle_grabbed_indices.begin(), toggle_grabbed_indices.end());
    const auto alt_indices = collect_resolved_device_indices(alternative_bindings);
    grabbed_indices.insert(alt_indices.begin(), alt_indices.end());
    const auto alt_toggle_indices = collect_resolved_device_indices(alternative_hold_toggle_bindings);
    grabbed_indices.insert(alt_toggle_indices.begin(), alt_toggle_indices.end());
    if (layout_switch.device_index >= 0) grabbed_indices.insert(static_cast<size_t>(layout_switch.device_index));
    grabbed = false;
    if (cfg.input.grab_devices && !grabbed_indices.empty()) {
      grabbed = provider.set_device_grab(devices, grabbed_indices, true, &std::cerr);
    }
    provider.flush_events(devices);
  };

  attach_devices("initial attach", true);
  const size_t resolved = count_connected_bindings(bindings, devices);
  const size_t toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
  const size_t alt_resolved = count_connected_bindings(alternative_bindings, devices);
  const size_t alt_toggle_resolved = count_connected_bindings(alternative_hold_toggle_bindings, devices);
  std::cout << "[override_controller] loaded config '" << cfg.name << "': "
            << cfg.bindings.size() << " bindings, " << resolved << " resolved"
            << ", hold_toggle_bindings=" << cfg.hold_toggle_bindings.size()
            << ", hold_toggle_resolved=" << toggle_resolved
            << ", alternative_bindings=" << cfg.alternative_bindings.size()
            << ", alternative_resolved=" << alt_resolved
            << ", alternative_hold_toggle_bindings=" << cfg.alternative_hold_toggle_bindings.size()
            << ", alternative_hold_toggle_resolved=" << alt_toggle_resolved
            << ", layout_switch=" << (cfg.layout_switch.enabled ? (layout_switch.device_index >= 0 ? "resolved" : "unresolved") : "disabled")
            << "\n";
  std::cout << "[override_controller] publish SHM stream=" << cfg.publish.stream_id
            << " registry=" << cfg.publish.registry_path
            << " shm=" << cfg.publish.shm_name
            << " rate_hz=" << cfg.publish.rate_hz << "\n";
  std::cout << "[override_controller] input grab_devices=" << (cfg.input.grab_devices ? "true" : "false")
            << " allow_shared_physical_device_sides="
            << (cfg.input.allow_shared_physical_device_sides ? "true" : "false")
            << " reattach_devices=" << (cfg.input.reattach_devices ? "true" : "false")
            << " reattach_interval_ms=" << cfg.input.reattach_interval_ms
            << " event_wait_max_ms=" << cfg.input.event_wait_max_ms << "\n";
  for (const auto& device : cfg.devices) {
    const auto& input = device.input;
    std::cout << "[override_controller] device_input id=" << device.id
              << " device=" << short_device_label(device.fingerprint)
              << " rel_axis_hold_ms=" << input.rel_axis_hold_ms
              << " rel_button_hold_ms=" << input.rel_button_hold_ms
              << " button_hold_ms=" << input.button_hold_ms
              << " button_release_grace_ms=" << input.button_release_grace_ms
              << " pulse_mode=" << (input.pulse_mode ? "true" : "false")
              << " dpad_pulse_gap_ms=" << input.dpad_pulse_gap_ms
              << " dpad_release_ms=" << input.dpad_release_ms
              << " button_pulse_gap_ms=" << input.button_pulse_gap_ms
              << " button_release_ms=" << input.button_release_ms
              << " button_pulse_startup_ms=" << input.button_pulse_startup_ms
              << " button_pulse_startup_release_ms=" << input.button_pulse_startup_release_ms
              << " button_pulse_startup_types=" << action_list_to_string(input.button_pulse_startup_types)
              << " hold_toggle_debounce_ms=" << input.hold_toggle_debounce_ms << "\n";
  }

  ControllerInputPublisher publisher(cfg.publish);
  CounterState counters;

  const double rate_hz = std::max(1.0, cfg.publish.rate_hz);
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / rate_hz));
  const auto reattach_interval = std::chrono::milliseconds(std::max<uint32_t>(100, cfg.input.reattach_interval_ms));
  const int event_wait_cap_ms = static_cast<int>(std::max<uint32_t>(1, cfg.input.event_wait_max_ms));
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

    if (ev && ev->stop_requested) {
      if (ev->device_index < devices.size()) {
        std::cout << "[override_controller] emergency stop requested from input device ["
                  << ev->device_index << "] " << short_device_label(devices[ev->device_index].fingerprint)
                  << " via " << provider.input_name(ev->type, ev->code) << "\n";
      } else {
        std::cout << "[override_controller] emergency stop requested from input device\n";
      }
      g_stop = true;
      continue;
    }

    if (ev && ev->device_index < devices.size()) {
      if (event_matches_layout_switch(layout_switch, *ev)) {
        const float switch_value = value_for_layout_switch(layout_switch, ev->value);
        const bool pressed = switch_value >= 0.50f;

        // LinuxEvdevInputProvider intentionally compacts a ready fd to the latest
        // value per (device,type,code) so SteamVR does not receive a seconds-long
        // backlog after stalls. For a very quick physical key click this can mean
        // the service observes only the final release event (value=0). Normal
        // controller bindings tolerate this through min-hold/release windows, but
        // layout switching was previously edge-only on press and therefore missed
        // many quick clicks. Treat a key release observed while the switch was not
        // already pressed as a compacted click pulse. This mirrors the robust
        // pulse handling used by the earlier triple-combo detector without
        // delaying or suppressing normal input.
        const bool press_edge = pressed && !layout_switch.pressed;
        const bool compacted_key_click = !pressed && !layout_switch.pressed &&
                                         layout_switch.cfg.input.kind == InputKind::Key &&
                                         ev->value == 0;
        if ((press_edge || compacted_key_click) && ev->timestamp_ns > layout_switch.debounce_until_ns) {
          alternative_layout_active = !alternative_layout_active;
          layout_switch.debounce_until_ns = ev->timestamp_ns + 250000000LL;
          clear_runtime_bindings(bindings);
          clear_runtime_bindings(hold_toggle_bindings);
          clear_runtime_bindings(alternative_bindings);
          clear_runtime_bindings(alternative_hold_toggle_bindings);
          std::cout << "[override_controller] alternative layout -> "
                    << (alternative_layout_active ? "on" : "off")
                    << (compacted_key_click ? " (release pulse)" : "") << "\n";
        }
        layout_switch.pressed = pressed;
        continue;
      }

      auto& active_bindings = alternative_layout_active ? alternative_bindings : bindings;
      auto& active_hold_toggle_bindings = alternative_layout_active ? alternative_hold_toggle_bindings : hold_toggle_bindings;
      std::set<size_t> suppressed_normal_bindings;

      for (auto& tb : active_hold_toggle_bindings) {
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
          const int64_t hold_toggle_debounce_ns =
              static_cast<int64_t>(tb.device_input.hold_toggle_debounce_ms) * 1000000LL;
          tb.toggle_debounce_until_ns = ev->timestamp_ns + hold_toggle_debounce_ns;
          if (new_click) {
            tb.value = tb.value >= 0.50f ? 0.0f : 1.0f;
            tb.physical_value = 0.0f;
            tb.active_until_ns = 0;
            if (verbose) {
              std::cout << "[override_controller] "
                        << (alternative_layout_active ? "alternative " : "")
                        << "hold-toggle " << to_string(tb.cfg.side) << "."
                        << to_string(tb.cfg.action) << " -> " << (tb.value >= 0.50f ? "on" : "off") << "\n";
            }
          }
        }

        // Priority rule: a hold-toggle binding consumes the same physical input
        // on the same controller side, even if that input is also configured as
        // a normal binding. This prevents one click from producing both a
        // momentary press and a virtual latched hold.
        for (size_t i = 0; i < active_bindings.size(); ++i) {
          if (same_physical_input_on_side(tb, active_bindings[i])) {
            suppressed_normal_bindings.insert(i);
            clear_runtime_binding_value(active_bindings[i]);
          }
        }
      }

      for (size_t i = 0; i < active_bindings.size(); ++i) {
        if (suppressed_normal_bindings.count(i) != 0) continue;
        auto& b = active_bindings[i];
        if (!event_matches_runtime_binding(b, *ev)) continue;
        const auto& device_input = b.device_input;
        int64_t hold_ns = 0;
        if (b.cfg.input.kind == InputKind::RelAxis && is_axis_action(b.cfg.action)) {
          hold_ns = static_cast<int64_t>(device_input.rel_axis_hold_ms) * 1000000LL;
        } else if (binding_is_rel_button_like(b.cfg)) {
          hold_ns = static_cast<int64_t>(device_input.rel_button_hold_ms) * 1000000LL;
        } else {
          hold_ns = static_cast<int64_t>(device_input.button_hold_ms) * 1000000LL;
        }
        int64_t release_grace_ns = binding_is_rel_button_like(b.cfg)
            ? 0
            : static_cast<int64_t>(device_input.button_release_grace_ms) * 1000000LL;
        const float binding_value = value_for_binding(b.cfg, ev->value);
        if (device_input.pulse_mode) {
          const int64_t pulse_ns = pulse_release_ns_for_binding(device_input, b.cfg);
          if (binding_is_rel_button_like(b.cfg)) {
            // EV_REL inputs are pulses even when they are mapped to non-D-pad
            // virtual actions such as thumbstick_click, A/B, grip, or menu.
            // Use this physical device's D-pad pulse window for every
            // button-like REL mapping.
            hold_ns = pulse_window_ns(device_input.dpad_pulse_gap_ms, device_input.dpad_release_ms);
          } else if (pulse_ns > 0) {
            release_grace_ns = button_pulse_grace_ns_for_event(
                device_input, b, ev->timestamp_ns, binding_value, pulse_ns);
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
      std::vector<RuntimeBinding> candidate_alt_bindings = resolve_alternative_bindings(cfg, candidate_devices, false);
      std::vector<RuntimeBinding> candidate_alt_toggle_bindings = resolve_alternative_hold_toggle_bindings(cfg, candidate_devices, false);
      RuntimeLayoutSwitch candidate_switch = resolve_layout_switch(cfg,
                                                                  candidate_devices,
                                                                  candidate_bindings,
                                                                  candidate_toggle_bindings,
                                                                  candidate_alt_bindings,
                                                                  candidate_alt_toggle_bindings,
                                                                  false);
      const size_t current_resolved = count_connected_bindings(bindings, devices);
      const size_t current_toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
      const size_t current_alt_resolved = count_connected_bindings(alternative_bindings, devices);
      const size_t current_alt_toggle_resolved = count_connected_bindings(alternative_hold_toggle_bindings, devices);
      const size_t current_switch_resolved = layout_switch.device_index >= 0 ? 1u : 0u;
      const size_t candidate_resolved = count_connected_bindings(candidate_bindings, candidate_devices);
      const size_t candidate_toggle_resolved = count_connected_bindings(candidate_toggle_bindings, candidate_devices);
      const size_t candidate_alt_resolved = count_connected_bindings(candidate_alt_bindings, candidate_devices);
      const size_t candidate_alt_toggle_resolved = count_connected_bindings(candidate_alt_toggle_bindings, candidate_devices);
      const size_t candidate_switch_resolved = candidate_switch.device_index >= 0 ? 1u : 0u;
      const bool mapping_changed =
          binding_device_mapping_changed(bindings, devices, candidate_bindings, candidate_devices) ||
          binding_device_mapping_changed(hold_toggle_bindings, devices, candidate_toggle_bindings, candidate_devices) ||
          binding_device_mapping_changed(alternative_bindings, devices, candidate_alt_bindings, candidate_devices) ||
          binding_device_mapping_changed(alternative_hold_toggle_bindings, devices, candidate_alt_toggle_bindings, candidate_devices) ||
          current_switch_resolved != candidate_switch_resolved;
      const size_t current_total = current_resolved + current_toggle_resolved + current_alt_resolved +
                                   current_alt_toggle_resolved + current_switch_resolved;
      const size_t candidate_total = candidate_resolved + candidate_toggle_resolved + candidate_alt_resolved +
                                     candidate_alt_toggle_resolved + candidate_switch_resolved;
      const bool should_reattach = candidate_total != current_total || mapping_changed;
      if (should_reattach) {
        const bool had_active_bindings = any_runtime_binding_active(bindings) ||
                                         any_runtime_binding_active(hold_toggle_bindings) ||
                                         any_runtime_binding_active(alternative_bindings) ||
                                         any_runtime_binding_active(alternative_hold_toggle_bindings);
        preserve_runtime_binding_state(candidate_bindings, bindings);
        preserve_runtime_binding_state(candidate_toggle_bindings, hold_toggle_bindings);
        preserve_runtime_binding_state(candidate_alt_bindings, alternative_bindings);
        preserve_runtime_binding_state(candidate_alt_toggle_bindings, alternative_hold_toggle_bindings);
        candidate_switch.pressed = layout_switch.pressed;
        candidate_switch.debounce_until_ns = layout_switch.debounce_until_ns;
        release_devices();
        devices = std::move(candidate_devices);
        bindings = std::move(candidate_bindings);
        hold_toggle_bindings = std::move(candidate_toggle_bindings);
        alternative_bindings = std::move(candidate_alt_bindings);
        alternative_hold_toggle_bindings = std::move(candidate_alt_toggle_bindings);
        layout_switch = std::move(candidate_switch);
        const size_t new_resolved = count_connected_bindings(bindings, devices);
        const size_t new_toggle_resolved = count_connected_bindings(hold_toggle_bindings, devices);
        const size_t new_alt_resolved = count_connected_bindings(alternative_bindings, devices);
        const size_t new_alt_toggle_resolved = count_connected_bindings(alternative_hold_toggle_bindings, devices);
        const bool preserved_active_bindings = had_active_bindings &&
            (any_runtime_binding_active(bindings) || any_runtime_binding_active(hold_toggle_bindings) ||
             any_runtime_binding_active(alternative_bindings) || any_runtime_binding_active(alternative_hold_toggle_bindings));
        std::cout << "[override_controller] reattach: " << current_resolved << "+" << current_toggle_resolved
                  << "+" << current_alt_resolved << "+" << current_alt_toggle_resolved
                  << "+" << current_switch_resolved
                  << " -> " << new_resolved << "+" << new_toggle_resolved
                  << "+" << new_alt_resolved << "+" << new_alt_toggle_resolved
                  << "+" << (layout_switch.device_index >= 0 ? 1 : 0)
                  << " resolved, open_devices=" << count_open_devices(devices)
                  << " preserved_active=" << (preserved_active_bindings ? "yes" : "no") << "\n";
        grabbed_indices = collect_resolved_device_indices(bindings);
        const auto toggle_indices = collect_resolved_device_indices(hold_toggle_bindings);
        grabbed_indices.insert(toggle_indices.begin(), toggle_indices.end());
        const auto alt_indices = collect_resolved_device_indices(alternative_bindings);
        grabbed_indices.insert(alt_indices.begin(), alt_indices.end());
        const auto alt_toggle_indices = collect_resolved_device_indices(alternative_hold_toggle_bindings);
        grabbed_indices.insert(alt_toggle_indices.begin(), alt_toggle_indices.end());
        if (layout_switch.device_index >= 0) grabbed_indices.insert(static_cast<size_t>(layout_switch.device_index));
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
    auto& active_bindings_for_decay = alternative_layout_active ? alternative_bindings : bindings;
    auto& active_toggle_bindings_for_decay = alternative_layout_active ? alternative_hold_toggle_bindings : hold_toggle_bindings;
    decay_relative_axis_bindings(active_bindings_for_decay, now_ns);
    decay_button_hold_bindings(active_bindings_for_decay, now_ns);
    now = std::chrono::steady_clock::now();
    if (now >= next_publish) {
      const auto out = compose_state(active_bindings_for_decay, active_toggle_bindings_for_decay, devices, counters,
                                     cfg.input.allow_shared_physical_device_sides);
      publisher.publish(out);
      clear_one_frame_relative_bindings(active_bindings_for_decay);
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
  clear_runtime_bindings(alternative_bindings);
  clear_runtime_bindings(alternative_hold_toggle_bindings);
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
    const fs::path executable_dir = current_executable_dir(argc > 0 ? argv[0] : nullptr);
    auto provider = make_platform_input_provider();
    if (!provider) throw std::runtime_error("no input provider for this platform yet");

    if (args.list_devices) {
      auto devices = provider->scan_devices(true);
      print_devices(devices);
      return 0;
    }

    if (args.non_interactive && args.config_path.empty()) {
      const auto files = args.connect_devices
          ? connect_device_config_candidates(args.config_dir, executable_dir)
          : list_config_files(args.config_dir);
      if (files.size() != 1 && !args.train) {
        throw std::runtime_error("--non-interactive requires --config or exactly one config file");
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
      const auto files = args.connect_devices
          ? connect_device_config_candidates(args.config_dir, executable_dir)
          : list_config_files(args.config_dir);
      if (files.empty()) {
        if (args.connect_devices) {
          throw std::runtime_error("--connect-devices requires an existing override_controller config in " +
                                   args.config_dir.string() + ", " +
                                   (args.config_dir / "configs").string() + ", or " +
                                   (executable_dir / "configs").string());
        }
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
        if (args.connect_devices && !fs::is_regular_file(args.config_dir / "default.json")) {
          std::cout << "No default config found at " << (args.config_dir / "default.json")
                    << "; choose a saved config to reconnect devices.\n";
        }
        selected_path = choose_config_interactive(files);
        cfg = load_config_file(selected_path);
      }
    }

    apply_publish_overrides(cfg, args);
    sync_devices_from_registry(cfg);
    if (args.connect_devices) {
      connect_config_devices(*provider, cfg, selected_path);
      return 0;
    }
    std::cout << "[override_controller] using config: " << selected_path << "\n";
    run_service(*provider, cfg, args.verbose);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[override_controller][ERROR] " << e.what() << "\n";
    return 1;
  }
}
