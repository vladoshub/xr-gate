#include <xr_override_controller/config_io.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

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

DeviceFingerprint fp_from_json(const nlohmann::json& j) {
  DeviceFingerprint fp;
  fp.platform = j.value("platform", "linux");
  fp.backend = j.value("backend", "evdev");
  fp.event_path = j.value("event_path", "");
  fp.by_id_path = j.value("by_id_path", "");
  fp.by_path = j.value("by_path", "");
  fp.name = j.value("name", "");
  fp.phys = j.value("phys", "");
  fp.uniq = j.value("uniq", "");
  fp.bustype = j.value("bustype", 0u);
  fp.vendor = j.value("vendor", 0u);
  fp.product = j.value("product", 0u);
  fp.version = j.value("version", 0u);
  if (j.contains("stable_hash")) {
    if (j["stable_hash"].is_string()) {
      fp.stable_hash = std::stoull(j["stable_hash"].get<std::string>(), nullptr, 16);
    } else {
      fp.stable_hash = j.value("stable_hash", 0ull);
    }
  }
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
  cfg.input.rel_axis_hold_ms = input.value("rel_axis_hold_ms", 160u);
  cfg.input.rel_button_hold_ms = input.value("rel_button_hold_ms", 800u);
  cfg.input.button_hold_ms = input.value("button_hold_ms", 120u);
  cfg.input.button_release_grace_ms = input.value("button_release_grace_ms", 0u);
  cfg.input.pulse_mode = input.value("pulse_mode", false);
  cfg.input.dpad_pulse_gap_ms = input.value("dpad_pulse_gap_ms", 130u);
  cfg.input.dpad_release_ms = input.value("dpad_release_ms", 140u);
  cfg.input.button_pulse_gap_ms = input.value("button_pulse_gap_ms", 180u);
  cfg.input.button_release_ms = input.value("button_release_ms", 190u);
  cfg.input.button_pulse_startup_ms = input.value("button_pulse_startup_ms", 0u);
  cfg.input.button_pulse_startup_release_ms = input.value("button_pulse_startup_release_ms", 0u);
  cfg.input.button_pulse_startup_types = action_list_from_json(input, "button_pulse_startup_types");
  cfg.input.hold_toggle_debounce_ms = input.value("hold_toggle_debounce_ms", 1500u);

  const auto binding_from_json = [](const nlohmann::json& bj) {
    BindingConfig b;
    b.side = parse_side(bj.value("side", "left"));
    b.action = parse_action(bj.value("action", "trigger"));
    b.device = fp_from_json(bj.at("device"));
    b.input = input_from_json(bj.at("input"));
    return b;
  };

  for (const auto& bj : j.value("bindings", nlohmann::json::array())) {
    cfg.bindings.push_back(binding_from_json(bj));
  }

  for (const auto& bj : j.value("hold_toggle_bindings", nlohmann::json::array())) {
    cfg.hold_toggle_bindings.push_back(binding_from_json(bj));
  }
  return cfg;
}

void save_config_file(const AppConfig& cfg, const fs::path& path) {
  nlohmann::json j;
  j["version"] = 1;
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
      {"rel_axis_hold_ms", cfg.input.rel_axis_hold_ms},
      {"rel_button_hold_ms", cfg.input.rel_button_hold_ms},
      {"button_hold_ms", cfg.input.button_hold_ms},
      {"button_release_grace_ms", cfg.input.button_release_grace_ms},
      {"pulse_mode", cfg.input.pulse_mode},
      {"dpad_pulse_gap_ms", cfg.input.dpad_pulse_gap_ms},
      {"dpad_release_ms", cfg.input.dpad_release_ms},
      {"button_pulse_gap_ms", cfg.input.button_pulse_gap_ms},
      {"button_release_ms", cfg.input.button_release_ms},
      {"button_pulse_startup_ms", cfg.input.button_pulse_startup_ms},
      {"button_pulse_startup_release_ms", cfg.input.button_pulse_startup_release_ms},
      {"button_pulse_startup_types", action_list_to_json(cfg.input.button_pulse_startup_types)},
      {"hold_toggle_debounce_ms", cfg.input.hold_toggle_debounce_ms},
  };
  const auto binding_to_json = [](const BindingConfig& b) {
    return nlohmann::json({
        {"side", to_string(b.side)},
        {"action", to_string(b.action)},
        {"device", fp_to_json(b.device)},
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
