#include "capture_service_cpp/config/capture_config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace xr_capture_cpp {
namespace {

struct FlatYaml {
  std::unordered_map<std::string, std::string> scalars;
  std::unordered_map<std::string, std::vector<std::string>> sequences;
};

enum class PendingYamlValueKind {
  None,
  Scalar,
  Sequence,
};

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string unquote(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::string strip_comment(const std::string& line) {
  bool single = false;
  bool dbl = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\'' && !dbl) single = !single;
    else if (c == '"' && !single && (i == 0 || line[i - 1] != '\\')) dbl = !dbl;
    else if (c == '#' && !single && !dbl) return line.substr(0, i);
  }
  return line;
}

std::vector<std::string> parse_inline_sequence(std::string value) {
  value = trim(std::move(value));
  if (value.size() < 2 || value.front() != '[' || value.back() != ']') return {};
  value = value.substr(1, value.size() - 2);
  std::vector<std::string> out;
  std::string cur;
  bool single = false;
  bool dbl = false;
  for (char c : value) {
    if (c == '\'' && !dbl) single = !single;
    else if (c == '"' && !single) dbl = !dbl;
    if (c == ',' && !single && !dbl) {
      const std::string item = unquote(cur);
      if (!item.empty()) out.push_back(item);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  const std::string item = unquote(cur);
  if (!item.empty()) out.push_back(item);
  return out;
}

std::string join_path(const std::vector<std::pair<int, std::string>>& stack, const std::string& leaf = {}) {
  std::ostringstream os;
  bool first = true;
  for (const auto& item : stack) {
    if (!first) os << '.';
    os << item.second;
    first = false;
  }
  if (!leaf.empty()) {
    if (!first) os << '.';
    os << leaf;
  }
  return os.str();
}

FlatYaml parse_yaml_subset(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("failed to open config: " + path);

  FlatYaml out;
  std::vector<std::pair<int, std::string>> stack;
  PendingYamlValueKind pending_kind = PendingYamlValueKind::None;
  std::string pending_key;
  int pending_indent = -1;
  bool pending_block_scalar = false;
  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    if (line_number == 1 && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }
    line = strip_comment(line);
    if (trim(line).empty() || trim(line).rfind("%YAML", 0) == 0 || trim(line) == "---") continue;
    if (line.find('\t') != std::string::npos) {
      throw std::runtime_error("tabs are not supported in config " + path + ":" + std::to_string(line_number));
    }

    int indent = 0;
    while (indent < static_cast<int>(line.size()) && line[static_cast<size_t>(indent)] == ' ') ++indent;
    std::string content = trim(line.substr(static_cast<size_t>(indent)));

    // YAML permits a scalar (including a sequence item) to continue on a more
    // deeply indented line. The legacy XREAL profile uses this for a human-
    // readable note. Keep accepting it even though this loader intentionally
    // implements only the configuration-oriented YAML subset needed here.
    if (pending_block_scalar && indent > pending_indent) {
      std::string& target = out.scalars[pending_key];
      if (!target.empty()) target.push_back(' ');
      target += content;
      continue;
    }

    const bool is_sequence_item = content.rfind("- ", 0) == 0 || content == "-";
    bool single = false;
    bool dbl = false;
    size_t colon = std::string::npos;
    if (!is_sequence_item) {
      for (size_t i = 0; i < content.size(); ++i) {
        const char c = content[i];
        if (c == '\'' && !dbl) single = !single;
        else if (c == '"' && !single && (i == 0 || content[i - 1] != '\\')) dbl = !dbl;
        else if (c == ':' && !single && !dbl) {
          colon = i;
          break;
        }
      }
    }

    if (!is_sequence_item && colon == std::string::npos &&
        pending_kind != PendingYamlValueKind::None && indent > pending_indent) {
      if (pending_kind == PendingYamlValueKind::Scalar) {
        std::string& target = out.scalars[pending_key];
        if (!target.empty()) target.push_back(' ');
        target += unquote(content);
      } else {
        auto& values = out.sequences[pending_key];
        if (values.empty()) {
          throw std::runtime_error("invalid YAML continuation in config " + path + ":" +
                                   std::to_string(line_number));
        }
        if (!values.back().empty()) values.back().push_back(' ');
        values.back() += unquote(content);
      }
      continue;
    }

    pending_kind = PendingYamlValueKind::None;
    pending_key.clear();
    pending_indent = -1;
    pending_block_scalar = false;

    while (!stack.empty() && stack.back().first >= indent) stack.pop_back();

    if (is_sequence_item) {
      if (stack.empty()) throw std::runtime_error("top-level YAML sequences are not supported in " + path + ":" + std::to_string(line_number));
      const std::string item = unquote(content.size() > 1 ? content.substr(1) : std::string());
      if (!item.empty()) {
        pending_key = join_path(stack);
        out.sequences[pending_key].push_back(item);
        pending_kind = PendingYamlValueKind::Sequence;
        pending_indent = indent;
      }
      continue;
    }
    if (colon == std::string::npos) {
      throw std::runtime_error("expected key: value in config " + path + ":" + std::to_string(line_number));
    }

    const std::string key = trim(content.substr(0, colon));
    const std::string value = trim(content.substr(colon + 1));
    if (key.empty()) throw std::runtime_error("empty key in config " + path + ":" + std::to_string(line_number));
    const std::string full_key = join_path(stack, key);
    if (value.empty()) {
      stack.emplace_back(indent, key);
      continue;
    }
    if (value == "|" || value == ">" || value == "|-" || value == ">-" ||
        value == "|+" || value == ">+") {
      out.scalars[full_key].clear();
      pending_kind = PendingYamlValueKind::Scalar;
      pending_key = full_key;
      pending_indent = indent;
      pending_block_scalar = true;
      continue;
    }
    const auto seq = parse_inline_sequence(value);
    if (!seq.empty() || value == "[]") {
      out.sequences[full_key] = seq;
    } else {
      out.scalars[full_key] = unquote(value);
      pending_kind = PendingYamlValueKind::Scalar;
      pending_key = full_key;
      pending_indent = indent;
    }
  }

  // Existing device profiles are wrapped in a top-level `capture_service:`
  // mapping. New generic profiles may omit it. Expose both forms to the same
  // mapping code, with an explicitly unwrapped key taking precedence.
  constexpr const char* kWrapper = "capture_service.";
  const size_t wrapper_size = std::char_traits<char>::length(kWrapper);
  std::vector<std::pair<std::string, std::string>> scalar_aliases;
  std::vector<std::pair<std::string, std::vector<std::string>>> sequence_aliases;
  for (const auto& [key, value] : out.scalars) {
    if (key.rfind(kWrapper, 0) == 0) scalar_aliases.emplace_back(key.substr(wrapper_size), value);
  }
  for (const auto& [key, value] : out.sequences) {
    if (key.rfind(kWrapper, 0) == 0) sequence_aliases.emplace_back(key.substr(wrapper_size), value);
  }
  for (auto& [key, value] : scalar_aliases) out.scalars.emplace(std::move(key), std::move(value));
  for (auto& [key, value] : sequence_aliases) out.sequences.emplace(std::move(key), std::move(value));
  return out;
}

bool has(const FlatYaml& yaml, const std::string& key) {
  return yaml.scalars.find(key) != yaml.scalars.end() || yaml.sequences.find(key) != yaml.sequences.end();
}

bool has_prefix(const FlatYaml& yaml, const std::string& prefix) {
  for (const auto& [key, unused] : yaml.scalars) {
    (void)unused;
    if (key.rfind(prefix, 0) == 0) return true;
  }
  for (const auto& [key, unused] : yaml.sequences) {
    (void)unused;
    if (key.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

bool parse_bool_value(const std::string& value) {
  const std::string v = lowercase(trim(value));
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  throw std::runtime_error("invalid boolean value: " + value);
}

int64_t parse_int64_value(const std::string& value) {
  const std::string cleaned = trim(value);
  size_t consumed = 0;
  const long long parsed = std::stoll(cleaned, &consumed, 0);
  if (consumed != cleaned.size()) throw std::runtime_error("invalid integer value: " + value);
  return static_cast<int64_t>(parsed);
}

int64_t parse_camera_control_value(const std::string& value) {
  const std::string lowered = lowercase(trim(value));
  if (lowered == "true" || lowered == "yes" || lowered == "on") return 1;
  if (lowered == "false" || lowered == "no" || lowered == "off") return 0;
  return parse_int64_value(value);
}

int parse_int_value(const std::string& value) {
  size_t consumed = 0;
  const long parsed = std::stol(trim(value), &consumed, 0);
  if (consumed != trim(value).size()) throw std::runtime_error("invalid integer value: " + value);
  return static_cast<int>(parsed);
}

size_t parse_size_value(const std::string& value) {
  size_t consumed = 0;
  const unsigned long long parsed = std::stoull(trim(value), &consumed, 0);
  if (consumed != trim(value).size()) throw std::runtime_error("invalid size value: " + value);
  return static_cast<size_t>(parsed);
}

void set_string(const FlatYaml& y, const std::vector<std::string>& keys, std::string& target) {
  for (const auto& key : keys) {
    const auto it = y.scalars.find(key);
    if (it != y.scalars.end()) {
      target = it->second;
      return;
    }
  }
}

void set_int(const FlatYaml& y, const std::vector<std::string>& keys, int& target) {
  for (const auto& key : keys) {
    const auto it = y.scalars.find(key);
    if (it != y.scalars.end()) {
      target = parse_int_value(it->second);
      return;
    }
  }
}

void set_size(const FlatYaml& y, const std::vector<std::string>& keys, size_t& target) {
  for (const auto& key : keys) {
    const auto it = y.scalars.find(key);
    if (it != y.scalars.end()) {
      target = parse_size_value(it->second);
      return;
    }
  }
}

void set_bool(const FlatYaml& y, const std::vector<std::string>& keys, bool& target) {
  for (const auto& key : keys) {
    const auto it = y.scalars.find(key);
    if (it != y.scalars.end()) {
      target = parse_bool_value(it->second);
      return;
    }
  }
}

std::vector<std::string> sequence_or_csv(const FlatYaml& y, const std::vector<std::string>& keys,
                                         const std::vector<std::string>& fallback) {
  for (const auto& key : keys) {
    const auto seq_it = y.sequences.find(key);
    if (seq_it != y.sequences.end()) return seq_it->second;
    const auto scalar_it = y.scalars.find(key);
    if (scalar_it != y.scalars.end()) return split_publish_modes(scalar_it->second);
  }
  return fallback;
}

std::string platform_key(const std::string& base) {
#ifdef _WIN32
  return base + ".windows";
#else
  return base + ".linux";
#endif
}

std::string current_platform_name() {
#ifdef _WIN32
  return "windows";
#else
  return "linux";
#endif
}

void apply_camera_control_values(const FlatYaml& y,
                                 const std::string& prefix,
                                 CameraControlConfig& controls) {
  const std::string controls_prefix = prefix + ".controls.";

  // First collect platform-neutral values. Only direct children are controls;
  // nested linux/windows mappings are handled in the second pass.
  for (const auto& [key, value] : y.scalars) {
    if (key.rfind(controls_prefix, 0) != 0) continue;
    const std::string name = key.substr(controls_prefix.size());
    if (name.empty() || name.find('.') != std::string::npos) continue;
    controls.values[name] = parse_camera_control_value(value);
  }

  // Platform-specific values override generic ones without changing the public
  // CameraDeviceConfig or the source driver. This is the extension point for a
  // future Windows implementation.
  const std::string platform_prefix = controls_prefix + current_platform_name() + ".";
  for (const auto& [key, value] : y.scalars) {
    if (key.rfind(platform_prefix, 0) != 0) continue;
    const std::string name = key.substr(platform_prefix.size());
    if (name.empty() || name.find('.') != std::string::npos) continue;
    controls.values[name] = parse_camera_control_value(value);
  }

  std::string policy = controls.strict ? "strict" : "best_effort";
  set_string(y, {prefix + ".controls_policy"}, policy);
  policy = lowercase(trim(policy));
  if (policy == "strict") controls.strict = true;
  else if (policy == "best_effort" || policy == "best-effort") controls.strict = false;
  else throw std::runtime_error(prefix + ".controls_policy must be strict or best_effort");
}

void apply_camera_device(const FlatYaml& y, const std::string& prefix, CameraDeviceConfig& device) {
  set_string(y, {platform_key(prefix + ".device"), prefix + ".device", platform_key(prefix + ".path"), prefix + ".path"}, device.device_path);
  set_int(y, {prefix + ".index"}, device.index);
  set_string(y, {platform_key(prefix + ".api"), prefix + ".api"}, device.api);
  set_int(y, {prefix + ".width"}, device.width);
  set_int(y, {prefix + ".height"}, device.height);
  set_int(y, {prefix + ".fps"}, device.fps);
  set_bool(y, {prefix + ".raw_format", prefix + ".raw"}, device.raw_format);
  set_bool(y, {prefix + ".convert_rgb"}, device.convert_rgb);
  set_int(y, {prefix + ".buffer_size"}, device.buffer_size);
  apply_camera_control_values(y, prefix, device.controls);
}

void apply_yaml(const FlatYaml& y, RuntimeConfig& cfg) {
  set_string(y, {platform_key("service.registry_path"), "service.registry_path", platform_key("registry_path"), "registry_path"}, cfg.registry_path);
  set_string(y, {platform_key("service.namespace"), "service.namespace", platform_key("namespace"), "namespace"}, cfg.namespace_name);
  set_string(y, {platform_key("service.profile"), "service.profile", "service.capture_profile",
                 platform_key("profile"), "profile"}, cfg.profile_name);
  set_int(y, {"service.slot_count", "slot_count"}, cfg.slot_count);
  cfg.publish_modes = sequence_or_csv(y, {"service.publish", "publish"}, cfg.publish_modes);
  set_string(y, {"service.tcp.bind_host", "service.tcp_bind_host", "tcp.bind_host", "tcp_bind_host"}, cfg.tcp_bind_host);
  set_int(y, {"service.tcp.port", "service.tcp_port", "tcp.port", "tcp_port"}, cfg.tcp_port);
  set_int(y, {"service.tcp.client_queue_size", "service.tcp_client_queue_size",
              "tcp.client_queue_size", "tcp_client_queue_size"}, cfg.tcp_client_queue_size);
  bool legacy_tcp_enabled = false;
  if (has(y, "service.tcp_enabled")) {
    set_bool(y, {"service.tcp_enabled"}, legacy_tcp_enabled);
    const auto tcp_it = std::find(cfg.publish_modes.begin(), cfg.publish_modes.end(), "tcp");
    if (legacy_tcp_enabled && tcp_it == cfg.publish_modes.end()) cfg.publish_modes.push_back("tcp");
    if (!legacy_tcp_enabled && tcp_it != cfg.publish_modes.end()) cfg.publish_modes.erase(tcp_it);
  }

  const bool legacy_xreal_camera = has_prefix(y, "xreal_linux.camera.");
  if (legacy_xreal_camera && !has(y, "camera.driver")) {
    cfg.camera.driver = "xreal_ultra";
    cfg.camera.layout = "xreal_packed";
  }
  set_bool(y, {"camera.enabled", "xreal_linux.camera.enabled"}, cfg.camera.enabled);
  const std::string previous_camera_driver = cfg.camera.driver;
  set_string(y, {"camera.driver"}, cfg.camera.driver);
  if (previous_camera_driver != cfg.camera.driver && cfg.camera.driver == "opencv") {
    if (!has(y, "camera.layout")) cfg.camera.layout = "side_by_side_horizontal";
    if (!has(y, "camera.transform.left.rotate") && !has(y, "camera.left.rotate")) cfg.camera.left_transform.rotate = "none";
    if (!has(y, "camera.transform.right.rotate") && !has(y, "camera.right.rotate")) cfg.camera.right_transform.rotate = "none";
    if (!has(y, "camera.transform.left.flip") && !has(y, "camera.left.flip")) cfg.camera.left_transform.flip = "none";
    if (!has(y, "camera.transform.right.flip") && !has(y, "camera.right.flip")) cfg.camera.right_transform.flip = "none";
    if (!has(y, "camera.primary.raw_format") && !has(y, "camera.primary.raw") &&
        !has(y, "camera.raw_format") && !has(y, "camera.raw")) cfg.camera.primary.raw_format = false;
    if (!has(y, "camera.primary.convert_rgb") && !has(y, "camera.convert_rgb")) cfg.camera.primary.convert_rgb = true;
    cfg.camera.secondary.raw_format = false;
    cfg.camera.secondary.convert_rgb = true;
  }
  set_string(y, {"camera.layout"}, cfg.camera.layout);
  set_string(y, {"camera.stereo_order", "camera.order"}, cfg.camera.stereo_order);
  apply_camera_device(y, "camera.primary", cfg.camera.primary);
  apply_camera_device(y, "camera.secondary", cfg.camera.secondary);
  // Compact aliases for one-device configurations.
  apply_camera_device(y, "camera", cfg.camera.primary);
  set_string(y, {platform_key("xreal_linux.camera.device"), "xreal_linux.camera.device"}, cfg.camera.primary.device_path);
  set_int(y, {"xreal_linux.camera.raw_width"}, cfg.camera.primary.width);
  set_int(y, {"xreal_linux.camera.raw_height"}, cfg.camera.primary.height);
  set_int(y, {"xreal_linux.camera.raw_fps_num"}, cfg.camera.primary.fps);
  set_string(y, {"camera.output.left_stream", "camera.left_stream", "xreal_linux.camera.left_stream_id"}, cfg.camera.left_stream_id);
  set_string(y, {"camera.output.right_stream", "camera.right_stream", "xreal_linux.camera.right_stream_id"}, cfg.camera.right_stream_id);
  set_string(y, {"camera.output.left_frame", "camera.left_frame"}, cfg.camera.left_frame_id);
  set_string(y, {"camera.output.right_frame", "camera.right_frame"}, cfg.camera.right_frame_id);
  set_int(y, {"camera.output.width", "camera.output_width"}, cfg.camera.output_width);
  set_int(y, {"camera.output.height", "camera.output_height"}, cfg.camera.output_height);
  set_int(y, {"camera.slot_count"}, cfg.camera.slot_count);
  set_string(y, {"camera.transform.left.rotate", "camera.left.rotate", "xreal_linux.camera.post_rotate_left"}, cfg.camera.left_transform.rotate);
  set_string(y, {"camera.transform.left.flip", "camera.left.flip"}, cfg.camera.left_transform.flip);
  set_string(y, {"camera.transform.right.rotate", "camera.right.rotate", "xreal_linux.camera.post_rotate_right"}, cfg.camera.right_transform.rotate);
  set_string(y, {"camera.transform.right.flip", "camera.right.flip"}, cfg.camera.right_transform.flip);
  set_int(y, {"camera.stall_exit_ms"}, cfg.camera.stall_exit_ms);

  const bool legacy_xreal_imu = has_prefix(y, "xreal_linux.imu.");
  if (legacy_xreal_imu && !has(y, "imu.driver")) cfg.imu.driver = "xreal_hid";
  set_bool(y, {"imu.enabled", "xreal_linux.imu.enabled"}, cfg.imu.enabled);
  const std::string previous_driver = cfg.imu.driver;
  set_string(y, {"imu.driver"}, cfg.imu.driver);
  set_string(y, {"imu.output.stream", "imu.stream", "xreal_linux.imu.imu_stream_id"}, cfg.imu.stream_id);
  set_string(y, {"imu.output.frame", "imu.frame"}, cfg.imu.frame_id);
  const bool raw_enabled_explicit = has(y, "imu.raw.enabled") || has(y, "xreal_linux.imu.publish_raw_hid");
  set_bool(y, {"imu.raw.enabled", "xreal_linux.imu.publish_raw_hid"}, cfg.imu.raw_enabled);
  set_string(y, {"imu.raw.stream", "imu.raw.stream_id", "xreal_linux.imu.raw_stream_id"}, cfg.imu.raw_stream_id);
  set_string(y, {"imu.raw.frame", "imu.raw.frame_id"}, cfg.imu.raw_frame_id);
  set_size(y, {"imu.raw.payload_size", "xreal_linux.imu.read_size"}, cfg.imu.raw_payload_size);
  set_int(y, {"imu.slot_count"}, cfg.imu.slot_count);
  set_int(y, {"imu.raw.slot_count"}, cfg.imu.raw_slot_count);
  set_int(y, {"imu.stall_exit_ms"}, cfg.imu.stall_exit_ms);

  int vid = cfg.imu.xreal_hid.vendor_id;
  int pid = cfg.imu.xreal_hid.product_id;
  set_int(y, {"imu.xreal_hid.vendor_id", "xreal_linux.imu.vendor_id"}, vid);
  set_int(y, {"imu.xreal_hid.product_id", "xreal_linux.imu.product_id"}, pid);
  cfg.imu.xreal_hid.vendor_id = static_cast<uint16_t>(vid);
  cfg.imu.xreal_hid.product_id = static_cast<uint16_t>(pid);
  set_int(y, {"imu.xreal_hid.interface", "imu.xreal_hid.interface_number",
              "xreal_linux.imu.interface_number"}, cfg.imu.xreal_hid.interface_number);
  set_int(y, {"imu.xreal_hid.drop_first_packets", "xreal_linux.imu.drop_first_packets"},
          cfg.imu.xreal_hid.drop_first_packets);
  set_int(y, {"imu.xreal_hid.read_timeout_ms", "xreal_linux.imu.read_timeout_ms"},
          cfg.imu.xreal_hid.read_timeout_ms);

  set_string(y, {platform_key("imu.serial.port"), "imu.serial.port"}, cfg.imu.serial.port);
  set_int(y, {"imu.serial.baud_rate", "imu.serial.baud"}, cfg.imu.serial.baud_rate);
  set_string(y, {"imu.serial.protocol"}, cfg.imu.serial.protocol);
  set_string(y, {"imu.serial.timestamp_mode"}, cfg.imu.serial.timestamp_mode);
  set_int(y, {"imu.serial.read_timeout_ms"}, cfg.imu.serial.read_timeout_ms);
  set_size(y, {"imu.serial.max_packet_size"}, cfg.imu.serial.max_packet_size);

  if (previous_driver != cfg.imu.driver && cfg.imu.driver == "serial" && !raw_enabled_explicit) {
    cfg.imu.raw_enabled = false;
    cfg.imu.raw_stream_id = "imu_raw";
    cfg.imu.raw_frame_id = "imu_raw";
    cfg.imu.raw_payload_size = cfg.imu.serial.max_packet_size;
  }
}

}  // namespace

ConfigSelection resolve_config_selection(int argc, char** argv) {
  ConfigSelection out;
  out.exact_path = env_or("XR_CAPTURE_CPP_CONFIG", env_or("CPP_CAPTURE_CONFIG", ""));
  out.directory = env_or("XR_CAPTURE_CPP_CONFIG_DIR", env_or("CPP_CAPTURE_CONFIG_DIR", ""));
  out.name = env_or("XR_CAPTURE_CPP_CONFIG_NAME", env_or("CPP_CAPTURE_CONFIG_NAME", "config.yaml"));
  out.explicit_selection = !out.exact_path.empty() || !out.directory.empty() ||
                           std::getenv("XR_CAPTURE_CPP_CONFIG_NAME") != nullptr ||
                           std::getenv("CPP_CAPTURE_CONFIG_NAME") != nullptr;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (arg == "--config" || arg == "--config-path") {
      out.exact_path = need(arg.c_str());
      out.explicit_selection = true;
    } else if (arg == "--config-dir" || arg == "--config-directory") {
      out.directory = need(arg.c_str());
      out.explicit_selection = true;
    } else if (arg == "--config-name") {
      out.name = need("--config-name");
      out.explicit_selection = true;
    }
  }
  return out;
}

std::string selected_config_path(const ConfigSelection& selection) {
  if (!selection.exact_path.empty()) return expand_user_path(selection.exact_path);
  std::string directory = selection.directory;
  if (directory.empty()) directory = "~/.config/xr_tracking/capture_service_cpp";
  return (std::filesystem::path(expand_user_path(directory)) / selection.name).string();
}

bool load_runtime_config_file(const std::string& path, RuntimeConfig& cfg) {
  if (!std::filesystem::exists(path)) return false;
  const FlatYaml yaml = parse_yaml_subset(path);
  apply_yaml(yaml, cfg);
  cfg.config_path = path;
  return true;
}

void validate_runtime_config(RuntimeConfig& cfg) {
  cfg.camera.driver = lowercase(trim(cfg.camera.driver));
  cfg.camera.layout = lowercase(trim(cfg.camera.layout));
  cfg.camera.stereo_order = lowercase(trim(cfg.camera.stereo_order));
  cfg.imu.driver = lowercase(trim(cfg.imu.driver));
  cfg.imu.serial.protocol = lowercase(trim(cfg.imu.serial.protocol));
  cfg.imu.serial.timestamp_mode = lowercase(trim(cfg.imu.serial.timestamp_mode));
  cfg.profile_name = trim(cfg.profile_name);

  if (!cfg.profile_name.empty()) {
    for (const unsigned char c : cfg.profile_name) {
      if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) {
        throw std::runtime_error("service.profile contains an invalid character: " + cfg.profile_name);
      }
    }
  }

  if (cfg.slot_count <= 0) throw std::runtime_error("service.slot_count must be positive");
  if (cfg.camera.slot_count <= 0) cfg.camera.slot_count = cfg.slot_count;
  if (cfg.imu.slot_count <= 0) cfg.imu.slot_count = cfg.slot_count;
  if (cfg.imu.raw_slot_count <= 0) cfg.imu.raw_slot_count = cfg.imu.slot_count;
  if (cfg.camera.slot_count <= 0) throw std::runtime_error("camera.slot_count must be positive");
  if (cfg.imu.slot_count <= 0) throw std::runtime_error("imu.slot_count must be positive");
  if (cfg.imu.raw_slot_count <= 0) throw std::runtime_error("imu.raw.slot_count must be positive");
  if (cfg.tcp_port <= 0 || cfg.tcp_port > 65535) throw std::runtime_error("tcp port is out of range");
  if (cfg.camera.enabled) {
    if (cfg.camera.driver != "xreal_ultra" && cfg.camera.driver != "opencv") {
      throw std::runtime_error("unsupported camera.driver=" + cfg.camera.driver + "; supported: xreal_ultra, opencv");
    }
    if (cfg.camera.output_width <= 0 || cfg.camera.output_height <= 0) {
      throw std::runtime_error("camera.output width and height must be positive");
    }
    if (cfg.camera.driver == "opencv" && cfg.camera.layout != "side_by_side_horizontal" &&
        cfg.camera.layout != "side_by_side_vertical" && cfg.camera.layout != "interleaved_columns" &&
        cfg.camera.layout != "dual") {
      throw std::runtime_error(
          "opencv camera.layout must be side_by_side_horizontal, side_by_side_vertical, interleaved_columns, or dual");
    }
  }
  if (cfg.camera.enabled && cfg.camera.left_stream_id == cfg.camera.right_stream_id) {
    throw std::runtime_error("camera left and right stream ids must be different");
  }
  if (cfg.imu.enabled) {
    if (cfg.imu.driver != "xreal_hid" && cfg.imu.driver != "serial") {
      throw std::runtime_error("unsupported imu.driver=" + cfg.imu.driver + "; supported: xreal_hid, serial");
    }
    if (cfg.imu.driver == "xreal_hid" && cfg.imu.xreal_hid.read_timeout_ms <= 0) {
      throw std::runtime_error("imu.xreal_hid.read_timeout_ms must be positive");
    }
    if (cfg.imu.driver == "serial") {
      if (cfg.imu.serial.port.empty()) throw std::runtime_error("imu.serial.port is required for imu.driver=serial");
      if (cfg.imu.serial.baud_rate <= 0) throw std::runtime_error("imu.serial.baud_rate must be positive");
      if (cfg.imu.serial.protocol != "xr_imu_v1" && cfg.imu.serial.protocol != "csv_f32") {
        throw std::runtime_error("imu.serial.protocol must be xr_imu_v1 or csv_f32");
      }
      if (cfg.imu.serial.timestamp_mode != "device" && cfg.imu.serial.timestamp_mode != "host_receive") {
        throw std::runtime_error("imu.serial.timestamp_mode must be device or host_receive");
      }
      if (cfg.imu.raw_enabled && cfg.imu.raw_payload_size < cfg.imu.serial.max_packet_size &&
          cfg.imu.serial.protocol == "csv_f32") {
        throw std::runtime_error("imu.raw.payload_size must be >= imu.serial.max_packet_size for csv_f32");
      }
      if (cfg.imu.raw_enabled && cfg.imu.serial.protocol == "xr_imu_v1" && cfg.imu.raw_payload_size < 48) {
        throw std::runtime_error("imu.raw.payload_size must be at least 48 for xr_imu_v1");
      }
    }
    if (cfg.imu.raw_enabled && cfg.imu.raw_stream_id == cfg.imu.stream_id) {
      throw std::runtime_error("normalized and raw IMU stream ids must be different");
    }
  }
  if (cfg.publish_modes.empty()) throw std::runtime_error("at least one publish mode is required");
}

}  // namespace xr_capture_cpp
