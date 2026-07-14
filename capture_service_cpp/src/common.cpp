#include "capture_service_cpp/common.hpp"

#include "capture_service_cpp/config/capture_config.hpp"
#include "capture_service_cpp/platform/runtime_defaults.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace xr_capture_cpp {

uint64_t steady_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t wall_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void request_stop_with_exit_code(int exit_code) {
  if (exit_code != kExitOk) {
    int expected = kExitOk;
    g_exit_code.compare_exchange_strong(expected, exit_code);
  }
  g_stop.store(true);
}

std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

int env_int(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  return std::stoi(v, nullptr, 0);
}

bool env_bool(const char* name, bool fallback) {
  const std::string v = env_or(name, fallback ? "1" : "0");
  return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES" || v == "on" || v == "ON";
}

std::string json_escape(const std::string& s) {
  std::ostringstream os;
  for (char c : s) {
    switch (c) {
      case '\\': os << "\\\\"; break;
      case '"': os << "\\\""; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default: os << c; break;
    }
  }
  return os.str();
}

std::string sanitize_shm_name(const std::string& value) {
  std::string out = "cap_";
  for (char ch : value) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') out.push_back(ch);
    else out.push_back('_');
    if (out.size() >= 184) break;
  }
  return out;
}

std::vector<std::string> split_publish_modes(const std::string& value) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&]() {
    if (cur.empty()) return;
    std::transform(cur.begin(), cur.end(), cur.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (cur == "shm" || cur == "tcp") out.push_back(cur);
    cur.clear();
  };
  for (char c : value) {
    if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) flush();
    else cur.push_back(c);
  }
  flush();
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string expand_user_path(const std::string& value) {
  const std::string home = env_or("HOME", env_or("USERPROFILE", "~"));
  if (value == "~") return home;
  if (value.rfind("~/", 0) == 0) return (std::filesystem::path(home) / value.substr(2)).string();
  return value;
}

void usage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " [--config PATH | --config-path PATH | --config-dir DIR --config-name NAME]"
            << " [--registry PATH] [--namespace NAME] [--publish shm|tcp|shm,tcp]"
            << " [--tcp-bind HOST] [--tcp-port PORT]"
            << " [--camera-driver xreal_ultra|opencv]"
            << " [--camera-layout side_by_side_horizontal|side_by_side_vertical|dual]";
  print_platform_camera_usage(std::cerr);
  std::cerr << " [--secondary-video-device PATH] [--secondary-camera-index N]"
            << " [--imu-driver xreal_hid|serial] [--serial-port PORT] [--serial-baud RATE]"
            << " [--no-camera] [--no-imu] [--duration SEC]\n"
            << "Default config: ~/.config/xr_tracking/capture_service_cpp/config.yaml. "
            << "If it does not exist, the built-in XREAL Ultra profile is used.\n";
}

RuntimeConfig parse_args(int argc, char** argv) {
  RuntimeConfig cfg;
  apply_platform_runtime_defaults(cfg);

  const ConfigSelection selection = resolve_config_selection(argc, argv);
  const std::string config_path = selected_config_path(selection);
  cfg.config_dir = selection.directory.empty() ? "~/.config/xr_tracking/capture_service_cpp" : selection.directory;
  cfg.config_name = selection.name;
  cfg.config_explicit = selection.explicit_selection;
  if (!load_runtime_config_file(config_path, cfg) && selection.explicit_selection) {
    throw std::runtime_error("config file does not exist: " + config_path);
  }

  // Environment variables override YAML while preserving the original runtime
  // environment contract used by the XREAL launch scripts.
  cfg.registry_path = env_or("REGISTRY_PATH", cfg.registry_path);
  cfg.namespace_name = env_or("NAMESPACE", cfg.namespace_name);
  const std::string publish_env = env_or("PUBLISH", "");
  if (!publish_env.empty()) cfg.publish_modes = split_publish_modes(publish_env);
  const char* global_slots_env = std::getenv("CPP_CAPTURE_SLOT_COUNT");
  if (global_slots_env && *global_slots_env) {
    const int slots = std::stoi(global_slots_env, nullptr, 0);
    cfg.slot_count = slots;
    cfg.camera.slot_count = slots;
    cfg.imu.slot_count = slots;
    cfg.imu.raw_slot_count = slots;
  }
  cfg.camera.slot_count = env_int("CPP_CAPTURE_CAMERA_SLOT_COUNT", cfg.camera.slot_count);
  cfg.imu.slot_count = env_int("CPP_CAPTURE_IMU_SLOT_COUNT", cfg.imu.slot_count);
  cfg.imu.raw_slot_count = env_int("CPP_CAPTURE_IMU_RAW_SLOT_COUNT", cfg.imu.raw_slot_count);
  cfg.camera.enabled = env_bool("CPP_CAPTURE_CAMERA_ENABLED", cfg.camera.enabled);
  cfg.imu.enabled = env_bool("CPP_CAPTURE_IMU_ENABLED", cfg.imu.enabled);
  const std::string camera_driver_env = env_or("CPP_CAPTURE_CAMERA_DRIVER", "");
  if (!camera_driver_env.empty() && camera_driver_env != cfg.camera.driver) {
    cfg.camera.driver = camera_driver_env;
    if (cfg.camera.driver == "opencv") {
      cfg.camera.primary.raw_format = false;
      cfg.camera.primary.convert_rgb = true;
      cfg.camera.secondary.raw_format = false;
      cfg.camera.secondary.convert_rgb = true;
      cfg.camera.left_transform = ImageTransformConfig{"none", "none"};
      cfg.camera.right_transform = ImageTransformConfig{"none", "none"};
      if (cfg.camera.layout == "xreal_packed") cfg.camera.layout = "side_by_side_horizontal";
    }
  }
  const std::string imu_driver_env = env_or("CPP_CAPTURE_IMU_DRIVER", "");
  if (!imu_driver_env.empty() && imu_driver_env != cfg.imu.driver) {
    cfg.imu.driver = imu_driver_env;
    if (cfg.imu.driver == "serial") {
      cfg.imu.raw_enabled = false;
      cfg.imu.raw_stream_id = "imu_raw";
      cfg.imu.raw_frame_id = "imu_raw";
      cfg.imu.raw_payload_size = cfg.imu.serial.max_packet_size;
    }
  }
  cfg.camera.primary.device_path = env_or("CPP_CAPTURE_VIDEO_DEVICE", env_or("VIDEO_DEVICE", cfg.camera.primary.device_path));
  cfg.camera.primary.index = env_int("CPP_CAPTURE_CAMERA_INDEX", cfg.camera.primary.index);
  cfg.camera.primary.api = env_or("CPP_CAPTURE_CAMERA_API", cfg.camera.primary.api);
  cfg.camera.left_transform.rotate = env_or("XR_CAPTURE_CPP_LEFT_ROTATE", env_or("CPP_CAPTURE_LEFT_ROTATE", cfg.camera.left_transform.rotate));
  cfg.camera.right_transform.rotate = env_or("XR_CAPTURE_CPP_RIGHT_ROTATE", env_or("CPP_CAPTURE_RIGHT_ROTATE", cfg.camera.right_transform.rotate));
  cfg.camera.left_transform.flip = env_or("XR_CAPTURE_CPP_LEFT_FLIP", env_or("CPP_CAPTURE_LEFT_FLIP", cfg.camera.left_transform.flip));
  cfg.camera.right_transform.flip = env_or("XR_CAPTURE_CPP_RIGHT_FLIP", env_or("CPP_CAPTURE_RIGHT_FLIP", cfg.camera.right_transform.flip));
  cfg.camera.stall_exit_ms = env_int("CPP_CAPTURE_CAMERA_STALL_EXIT_MS", cfg.camera.stall_exit_ms);
  cfg.imu.stall_exit_ms = env_int("CPP_CAPTURE_IMU_STALL_EXIT_MS", cfg.imu.stall_exit_ms);
  cfg.imu.xreal_hid.drop_first_packets = env_int("CPP_CAPTURE_IMU_DROP_FIRST_PACKETS", cfg.imu.xreal_hid.drop_first_packets);
  cfg.imu.raw_enabled = env_bool("CPP_CAPTURE_RAW_HID_ENABLED", cfg.imu.raw_enabled);
  cfg.imu.serial.port = env_or("CPP_CAPTURE_IMU_SERIAL_PORT", cfg.imu.serial.port);
  cfg.imu.serial.baud_rate = env_int("CPP_CAPTURE_IMU_SERIAL_BAUD", cfg.imu.serial.baud_rate);
  cfg.tcp_bind_host = env_or("TCP_BIND_HOST", env_or("CPP_CAPTURE_TCP_BIND_HOST", cfg.tcp_bind_host));
  cfg.tcp_port = env_int("TCP_PORT", env_int("CPP_CAPTURE_TCP_PORT", cfg.tcp_port));
  cfg.tcp_client_queue_size = env_int("TCP_CLIENT_QUEUE_SIZE", env_int("CPP_CAPTURE_TCP_CLIENT_QUEUE_SIZE", cfg.tcp_client_queue_size));
  if (env_bool("TCP_ENABLED", false) &&
      std::find(cfg.publish_modes.begin(), cfg.publish_modes.end(), "tcp") == cfg.publish_modes.end()) {
    cfg.publish_modes.push_back("tcp");
  }

  // CLI is the highest-precedence layer. Config location options were already
  // consumed by resolve_config_selection and are skipped here.
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (a == "--help" || a == "-h") { usage(argv[0]); std::exit(0); }
    else if (a == "--config" || a == "--config-path" || a == "--config-dir" || a == "--config-directory" || a == "--config-name") { (void)need(a.c_str()); }
    else if (a == "--registry") cfg.registry_path = need("--registry");
    else if (a == "--namespace") cfg.namespace_name = need("--namespace");
    else if (a == "--publish") cfg.publish_modes = split_publish_modes(need("--publish"));
    else if (a == "--tcp-bind") cfg.tcp_bind_host = need("--tcp-bind");
    else if (a == "--tcp-port") cfg.tcp_port = std::stoi(need("--tcp-port"));
    else if (a == "--tcp-client-queue-size") cfg.tcp_client_queue_size = std::stoi(need("--tcp-client-queue-size"));
    else if (a == "--camera-driver") {
      const std::string previous = cfg.camera.driver;
      cfg.camera.driver = need("--camera-driver");
      if (previous != cfg.camera.driver && cfg.camera.driver == "opencv") {
        cfg.camera.primary.raw_format = false;
        cfg.camera.primary.convert_rgb = true;
        cfg.camera.secondary.raw_format = false;
        cfg.camera.secondary.convert_rgb = true;
        cfg.camera.left_transform = ImageTransformConfig{"none", "none"};
        cfg.camera.right_transform = ImageTransformConfig{"none", "none"};
        if (cfg.camera.layout == "xreal_packed") cfg.camera.layout = "side_by_side_horizontal";
      }
    }
    else if (a == "--camera-layout") cfg.camera.layout = need("--camera-layout");
    else if (a == "--video-device" || a == "--device") cfg.camera.primary.device_path = need(a.c_str());
    else if (a == "--camera-index") cfg.camera.primary.index = std::stoi(need("--camera-index"));
    else if (a == "--camera-api") cfg.camera.primary.api = need("--camera-api");
    else if (a == "--secondary-video-device") cfg.camera.secondary.device_path = need("--secondary-video-device");
    else if (a == "--secondary-camera-index") cfg.camera.secondary.index = std::stoi(need("--secondary-camera-index"));
    else if (a == "--imu-driver") {
      const std::string previous = cfg.imu.driver;
      cfg.imu.driver = need("--imu-driver");
      if (previous != cfg.imu.driver && cfg.imu.driver == "serial") {
        cfg.imu.raw_enabled = false;
        cfg.imu.raw_stream_id = "imu_raw";
        cfg.imu.raw_frame_id = "imu_raw";
        cfg.imu.raw_payload_size = cfg.imu.serial.max_packet_size;
      }
    }
    else if (a == "--serial-port") cfg.imu.serial.port = need("--serial-port");
    else if (a == "--serial-baud") cfg.imu.serial.baud_rate = std::stoi(need("--serial-baud"));
    else if (a == "--raw-imu") cfg.imu.raw_enabled = true;
    else if (a == "--no-raw-imu") cfg.imu.raw_enabled = false;
    else if (a == "--no-camera") cfg.camera.enabled = false;
    else if (a == "--no-imu") cfg.imu.enabled = false;
    else if (a == "--duration") cfg.duration_sec = std::stoi(need("--duration"));
    else throw std::runtime_error("unknown argument: " + a);
  }

  finalize_platform_runtime_config(cfg);
  validate_runtime_config(cfg);
  return cfg;
}

}  // namespace xr_capture_cpp
