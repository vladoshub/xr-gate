#include <xr_override_controller/config_io.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include <xr_runtime/registry/runtime_paths.hpp>

namespace fs = std::filesystem;

namespace xr_override_controller {
namespace {

std::string getenv_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

nlohmann::json fp_to_json(const DeviceFingerprint& fp) {
  return {
      {"platform", fp.platform},
      {"backend", fp.backend},
      {"event_path", fp.event_path},
      {"by_id_path", fp.by_id_path},
      {"by_path", fp.by_path},
      {"name", fp.name},
      {"phys", fp.phys},
      {"uniq", fp.uniq},
      {"bustype", fp.bustype},
      {"vendor", fp.vendor},
      {"product", fp.product},
      {"version", fp.version},
      {"stable_hash", hex_u64(fp.stable_hash)},
  };
}

std::string json_string_or(const nlohmann::json& j, const char* key, const std::string& fallback = {}) {
  if (!j.contains(key) || j.at(key).is_null()) return fallback;
  if (j.at(key).is_string()) return j.at(key).get<std::string>();
  return fallback;
}

uint16_t json_u16_or(const nlohmann::json& j, const char* key, uint16_t fallback = 0) {
  if (!j.contains(key) || j.at(key).is_null()) return fallback;
  if (!j.at(key).is_number_integer() && !j.at(key).is_number_unsigned()) return fallback;
  return static_cast<uint16_t>(j.at(key).get<unsigned int>());
}

uint64_t json_u64_or(const nlohmann::json& j, const char* key, uint64_t fallback = 0) {
  if (!j.contains(key) || j.at(key).is_null()) return fallback;
  const auto& v = j.at(key);
  if (v.is_string()) {
    const std::string raw = v.get<std::string>();
    if (raw.empty()) return fallback;
    try {
      return std::stoull(raw, nullptr, 16);
    } catch (...) {
      return fallback;
    }
  }
  if (v.is_number_integer() || v.is_number_unsigned()) return v.get<uint64_t>();
  return fallback;
}

DeviceFingerprint fp_from_json(const nlohmann::json& j) {
  DeviceFingerprint fp;
  fp.platform = json_string_or(j, "platform", "linux");
  fp.backend = json_string_or(j, "backend", "evdev");
  fp.event_path = json_string_or(j, "event_path", "");
  fp.by_id_path = json_string_or(j, "by_id_path", "");
  fp.by_path = json_string_or(j, "by_path", "");
  fp.name = json_string_or(j, "name", "");
  fp.phys = json_string_or(j, "phys", "");
  fp.uniq = json_string_or(j, "uniq", "");
  fp.bustype = json_u16_or(j, "bustype", 0);
  fp.vendor = json_u16_or(j, "vendor", 0);
  fp.product = json_u16_or(j, "product", 0);
  fp.version = json_u16_or(j, "version", 0);
  fp.stable_hash = json_u64_or(j, "stable_hash", 0);
  return fp;
}

nlohmann::json input_to_json(const InputBindingSpec& in) {
  return {
      {"kind", in.kind == InputKind::Key ? "key" : (in.kind == InputKind::AbsAxis ? "abs_axis" : "rel_axis")},
      {"type", in.type},
      {"code", in.code},
      {"name", in.name},
      {"abs_min", in.abs_min},
      {"abs_max", in.abs_max},
      {"abs_flat", in.abs_flat},
      {"abs_direction", in.abs_direction},
  };
}

InputBindingSpec input_from_json(const nlohmann::json& j) {
  InputBindingSpec in;
  const std::string kind = j.value("kind", "key");
  if (kind == "abs_axis" || kind == "axis" || kind == "abs") {
    in.kind = InputKind::AbsAxis;
  } else if (kind == "rel_axis" || kind == "rel" || kind == "mouse_rel") {
    in.kind = InputKind::RelAxis;
  } else {
    in.kind = InputKind::Key;
  }
  in.type = j.value("type", 0u);
  in.code = j.value("code", 0u);
  in.name = j.value("name", "");
  in.abs_min = j.value("abs_min", 0);
  in.abs_max = j.value("abs_max", 0);
  in.abs_flat = j.value("abs_flat", 0);
  in.abs_direction = j.value("abs_direction", 0);
  return in;
}

std::string trim_copy(std::string v) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
  v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
  return v;
}

std::vector<ControllerAction> parse_action_list_string(const std::string& raw) {
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

std::vector<ControllerAction> action_list_from_json(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) return {};
  const auto& v = j.at(key);
  if (v.is_string()) return parse_action_list_string(v.get<std::string>());
  if (v.is_array()) {
    std::vector<ControllerAction> out;
    for (const auto& item : v) {
      if (item.is_string()) out.push_back(parse_action(item.get<std::string>()));
    }
    return out;
  }
  return {};
}

nlohmann::json action_list_to_json(const std::vector<ControllerAction>& actions) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto action : actions) out.push_back(to_string(action));
  return out;
}

DeviceInputConfig device_input_from_json(const nlohmann::json& device_json) {
  DeviceInputConfig out;
  if (!device_json.contains("input") || !device_json.at("input").is_object()) return out;
  const auto& input = device_json.at("input");
  out.rel_axis_hold_ms = input.value("rel_axis_hold_ms", 0u);
  out.rel_button_hold_ms = input.value("rel_button_hold_ms", 0u);
  out.button_hold_ms = input.value("button_hold_ms", 0u);
  out.button_release_grace_ms = input.value("button_release_grace_ms", 0u);
  out.pulse_mode = input.value("pulse_mode", false);
  out.dpad_pulse_gap_ms = input.value("dpad_pulse_gap_ms", 0u);
  out.dpad_release_ms = input.value("dpad_release_ms", 0u);
  out.button_pulse_gap_ms = input.value("button_pulse_gap_ms", 0u);
  out.button_release_ms = input.value("button_release_ms", 0u);
  out.button_pulse_startup_ms = input.value("button_pulse_startup_ms", 0u);
  out.button_pulse_startup_release_ms = input.value("button_pulse_startup_release_ms", 0u);
  out.button_pulse_startup_types = action_list_from_json(input, "button_pulse_startup_types");
  out.hold_toggle_debounce_ms = input.value("hold_toggle_debounce_ms", 0u);
  return out;
}

nlohmann::json device_input_to_json(const DeviceInputConfig& input) {
  return {
      {"rel_axis_hold_ms", input.rel_axis_hold_ms},
      {"rel_button_hold_ms", input.rel_button_hold_ms},
      {"button_hold_ms", input.button_hold_ms},
      {"button_release_grace_ms", input.button_release_grace_ms},
      {"pulse_mode", input.pulse_mode},
      {"dpad_pulse_gap_ms", input.dpad_pulse_gap_ms},
      {"dpad_release_ms", input.dpad_release_ms},
      {"button_pulse_gap_ms", input.button_pulse_gap_ms},
      {"button_release_ms", input.button_release_ms},
      {"button_pulse_startup_ms", input.button_pulse_startup_ms},
      {"button_pulse_startup_release_ms", input.button_pulse_startup_release_ms},
      {"button_pulse_startup_types", action_list_to_json(input.button_pulse_startup_types)},
      {"hold_toggle_debounce_ms", input.hold_toggle_debounce_ms},
  };
}

nlohmann::json config_device_to_json(const ConfigDevice& d) {
  nlohmann::json j = fp_to_json(d.fingerprint);
  j["id"] = d.id;
  j["input"] = device_input_to_json(d.input);
  j["imu_side"] = d.imu_side ? to_string(*d.imu_side) : "none";
  return j;
}

ConfigDevice config_device_from_json(const nlohmann::json& j, int fallback_id) {
  ConfigDevice d;
  d.id = j.value("id", fallback_id);
  d.fingerprint = fp_from_json(j);
  d.input = device_input_from_json(j);
  d.imu_side_explicit = j.contains("imu_side");
  const std::string imu_side = json_string_or(j, "imu_side", "none");
  if (imu_side == "left" || imu_side == "right") {
    d.imu_side = parse_side(imu_side);
  } else if (imu_side != "none" && !imu_side.empty()) {
    throw std::runtime_error("invalid devices[].imu_side '" + imu_side +
                             "'; expected left, right, or none");
  }
  return d;
}

DeviceFingerprint* find_config_device(AppConfig& cfg, int id) {
  for (auto& d : cfg.devices) {
    if (d.id == id) return &d.fingerprint;
  }
  return nullptr;
}

const DeviceFingerprint* find_config_device(const AppConfig& cfg, int id) {
  for (const auto& d : cfg.devices) {
    if (d.id == id) return &d.fingerprint;
  }
  return nullptr;
}

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

bool infer_legacy_gearvr_imu_sides(AppConfig& cfg) {
  bool changed = false;
  const auto collect = [&](int device_id, std::set<ControllerSide>& sides,
                           const std::vector<BindingConfig>& bindings) {
    for (const auto& b : bindings) {
      if (b.device_id == device_id) sides.insert(b.side);
    }
  };

  for (auto& device : cfg.devices) {
    if (device.imu_side_explicit || device.fingerprint.backend != "gearvr_ble") continue;
    std::set<ControllerSide> sides;
    collect(device.id, sides, cfg.bindings);
    collect(device.id, sides, cfg.hold_toggle_bindings);
    collect(device.id, sides, cfg.alternative_bindings);
    collect(device.id, sides, cfg.alternative_hold_toggle_bindings);
    if (sides.size() == 1) {
      device.imu_side = *sides.begin();
      device.imu_side_explicit = true;
      changed = true;
    }
  }
  return changed;
}

void hydrate_binding_device(AppConfig& cfg, BindingConfig& b) {
  if (b.device_id > 0) {
    if (DeviceFingerprint* fp = find_config_device(cfg, b.device_id)) {
      b.device = *fp;
      return;
    }
  }
  b.device_id = ensure_config_device(cfg, b.device);
}

void hydrate_switch_device(AppConfig& cfg) {
  if (!cfg.layout_switch.enabled) return;
  if (cfg.layout_switch.device_id > 0) {
    if (DeviceFingerprint* fp = find_config_device(cfg, cfg.layout_switch.device_id)) {
      cfg.layout_switch.device = *fp;
      return;
    }
  }
  cfg.layout_switch.device_id = ensure_config_device(cfg, cfg.layout_switch.device);
}

}  // namespace

fs::path default_config_dir() {
  const std::string explicit_dir = getenv_or_empty("XR_OVERRIDE_CONTROLLER_CONFIG_DIR");
  if (!explicit_dir.empty()) return fs::path(explicit_dir);

#ifndef _WIN32
  const std::string xdg = getenv_or_empty("XDG_CONFIG_HOME");
  if (!xdg.empty()) return fs::path(xdg) / "xr_tracking" / "override_controller";
  const std::string home = getenv_or_empty("HOME");
  if (!home.empty()) return fs::path(home) / ".config" / "xr_tracking" / "override_controller";
  return fs::path(".") / "override_controller_configs";
#else
  const std::string local = getenv_or_empty("LOCALAPPDATA");
  if (!local.empty()) return fs::path(local) / "XrTracking" / "override_controller";
  return fs::path(".") / "override_controller_configs";
#endif
}

std::vector<fs::path> list_config_files(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!fs::exists(dir)) return out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".json") out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

AppConfig load_config_file(const fs::path& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("failed to open override controller config: " + path.string());
  nlohmann::json j;
  in >> j;

  AppConfig cfg;
  cfg.name = j.value("name", path.stem().string());
  const auto pub = j.value("publish", nlohmann::json::object());
  cfg.publish.transport = pub.value("transport", cfg.publish.transport);
  cfg.publish.registry_path = pub.value("registry", pub.value("registry_path", xr_runtime::default_tracking_registry_path()));
  cfg.publish.stream_id = pub.value("stream", pub.value("stream_id", "controller_input"));
  cfg.publish.shm_name = pub.value("shm_name", "controller_input");
  cfg.publish.tcp_bind_host = pub.value("tcp_bind_host", cfg.publish.tcp_bind_host);
  cfg.publish.tcp_port = pub.value("tcp_port", cfg.publish.tcp_port);
  cfg.publish.slot_count = pub.value("slot_count", 1024u);
  cfg.publish.rate_hz = pub.value("rate_hz", 90.0);
  cfg.publish.unlink_existing = pub.value("unlink_existing", true);

  const auto input = j.value("input", nlohmann::json::object());
  cfg.input.grab_devices = input.value("grab_devices", false);
  cfg.input.allow_shared_physical_device_sides = input.value("allow_shared_physical_device_sides", true);
  cfg.input.reattach_devices = input.value("reattach_devices", true);
  cfg.input.reattach_interval_ms = input.value("reattach_interval_ms", 1000u);
  cfg.input.event_wait_max_ms = input.value("event_wait_max_ms", 20u);

  int fallback_device_id = 1;
  for (const auto& dj : j.value("devices", nlohmann::json::array())) {
    ConfigDevice d = config_device_from_json(dj, fallback_device_id++);
    if (d.id > 0) cfg.devices.push_back(std::move(d));
  }

  const auto binding_from_json = [&](const nlohmann::json& bj) {
    BindingConfig b;
    b.side = parse_side(bj.value("side", "left"));
    b.action = parse_action(bj.value("action", "trigger"));
    b.device_id = bj.value("device_id", 0);
    if (bj.contains("device")) {
      b.device = fp_from_json(bj.at("device"));
    }
    b.input = input_from_json(bj.at("input"));
    hydrate_binding_device(cfg, b);
    return b;
  };

  if (j.contains("layout_switch") && j.at("layout_switch").is_object()) {
    const auto& sj = j.at("layout_switch");
    cfg.layout_switch.enabled = sj.value("enabled", false);
    cfg.layout_switch.device_id = sj.value("device_id", 0);
    if (sj.contains("device")) cfg.layout_switch.device = fp_from_json(sj.at("device"));
    if (sj.contains("input")) cfg.layout_switch.input = input_from_json(sj.at("input"));
    hydrate_switch_device(cfg);
  }

  for (const auto& bj : j.value("bindings", nlohmann::json::array())) {
    cfg.bindings.push_back(binding_from_json(bj));
  }

  for (const auto& bj : j.value("hold_toggle_bindings", nlohmann::json::array())) {
    cfg.hold_toggle_bindings.push_back(binding_from_json(bj));
  }

  for (const auto& bj : j.value("alternative_bindings", nlohmann::json::array())) {
    cfg.alternative_bindings.push_back(binding_from_json(bj));
  }

  for (const auto& bj : j.value("alternative_hold_toggle_bindings", nlohmann::json::array())) {
    cfg.alternative_hold_toggle_bindings.push_back(binding_from_json(bj));
  }
  cfg.migrated_imu_side = infer_legacy_gearvr_imu_sides(cfg);
  return cfg;
}

void save_config_file(const AppConfig& cfg, const fs::path& path) {
  nlohmann::json j;
  j["version"] = 3;
  j["name"] = cfg.name;
  j["publish"] = {
      {"transport", cfg.publish.transport},
      {"registry", cfg.publish.registry_path},
      {"stream", cfg.publish.stream_id},
      {"shm_name", cfg.publish.shm_name},
      {"tcp_bind_host", cfg.publish.tcp_bind_host},
      {"tcp_port", cfg.publish.tcp_port},
      {"slot_count", cfg.publish.slot_count},
      {"rate_hz", cfg.publish.rate_hz},
      {"unlink_existing", cfg.publish.unlink_existing},
  };
  j["input"] = {
      {"grab_devices", cfg.input.grab_devices},
      {"allow_shared_physical_device_sides", cfg.input.allow_shared_physical_device_sides},
      {"reattach_devices", cfg.input.reattach_devices},
      {"reattach_interval_ms", cfg.input.reattach_interval_ms},
      {"event_wait_max_ms", cfg.input.event_wait_max_ms},
  };
  j["devices"] = nlohmann::json::array();
  for (const auto& d : cfg.devices) {
    j["devices"].push_back(config_device_to_json(d));
  }

  if (cfg.layout_switch.enabled) {
    j["layout_switch"] = {
        {"enabled", true},
        {"device_id", cfg.layout_switch.device_id},
        {"input", input_to_json(cfg.layout_switch.input)},
    };
  } else {
    j["layout_switch"] = {{"enabled", false}};
  }

  const auto binding_to_json = [](const BindingConfig& b) {
    return nlohmann::json({
        {"side", to_string(b.side)},
        {"action", to_string(b.action)},
        {"device_id", b.device_id},
        {"input", input_to_json(b.input)},
    });
  };

  j["bindings"] = nlohmann::json::array();
  for (const auto& b : cfg.bindings) {
    j["bindings"].push_back(binding_to_json(b));
  }

  j["hold_toggle_bindings"] = nlohmann::json::array();
  for (const auto& b : cfg.hold_toggle_bindings) {
    j["hold_toggle_bindings"].push_back(binding_to_json(b));
  }

  j["alternative_bindings"] = nlohmann::json::array();
  for (const auto& b : cfg.alternative_bindings) {
    j["alternative_bindings"].push_back(binding_to_json(b));
  }

  j["alternative_hold_toggle_bindings"] = nlohmann::json::array();
  for (const auto& b : cfg.alternative_hold_toggle_bindings) {
    j["alternative_hold_toggle_bindings"].push_back(binding_to_json(b));
  }

  if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("failed to write config: " + tmp.string());
    out << j.dump(2) << "\n";
  }
  fs::rename(tmp, path);
}

fs::path choose_or_create_config_path(const fs::path& dir, const std::string& preferred_name) {
  fs::create_directories(dir);
  std::string name = preferred_name.empty() ? "default" : preferred_name;
  for (char& c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
  }
  return dir / (name + ".json");
}

}  // namespace xr_override_controller
