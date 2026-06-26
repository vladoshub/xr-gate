#include "capture_service_cpp/common.hpp"

#include "capture_service_cpp/platform/runtime_defaults.hpp"

#include <algorithm>
#include <cctype>
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

std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

int env_int(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  return std::stoi(v);
}

bool env_bool(const char* name, bool fallback) {
  const std::string v = env_or(name, fallback ? "1" : "0");
  return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
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

void usage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " [--registry PATH] [--namespace NAME] [--publish shm|tcp|shm,tcp]"
            << " [--tcp-bind HOST] [--tcp-port PORT]";
  print_platform_camera_usage(std::cerr);
  std::cerr << " [--no-camera] [--no-imu] [--duration SEC]\n"
            << "Experimental. Default production path remains capture_service_python.\n";
}


RuntimeConfig parse_args(int argc, char** argv) {
  RuntimeConfig cfg;
  apply_platform_runtime_defaults(cfg);
  cfg.video_device = env_or("CPP_CAPTURE_VIDEO_DEVICE", env_or("VIDEO_DEVICE", cfg.video_device));
  cfg.camera_index = env_int("CPP_CAPTURE_CAMERA_INDEX", cfg.camera_index);
  cfg.camera_api = env_or("CPP_CAPTURE_CAMERA_API", cfg.camera_api);
  cfg.left_rotate = env_or("CPP_CAPTURE_LEFT_ROTATE", cfg.left_rotate);
  cfg.right_rotate = env_or("CPP_CAPTURE_RIGHT_ROTATE", cfg.right_rotate);
  cfg.left_flip = env_or("CPP_CAPTURE_LEFT_FLIP", cfg.left_flip);
  cfg.right_flip = env_or("CPP_CAPTURE_RIGHT_FLIP", cfg.right_flip);
  cfg.slot_count = env_int("CPP_CAPTURE_SLOT_COUNT", cfg.slot_count);
  cfg.imu_enabled = env_bool("CPP_CAPTURE_IMU_ENABLED", cfg.imu_enabled);
  cfg.camera_enabled = env_bool("CPP_CAPTURE_CAMERA_ENABLED", cfg.camera_enabled);
  cfg.tcp_bind_host = env_or("TCP_BIND_HOST", env_or("CPP_CAPTURE_TCP_BIND_HOST", cfg.tcp_bind_host));
  cfg.tcp_port = env_int("TCP_PORT", env_int("CPP_CAPTURE_TCP_PORT", cfg.tcp_port));
  cfg.tcp_client_queue_size = env_int("TCP_CLIENT_QUEUE_SIZE", env_int("CPP_CAPTURE_TCP_CLIENT_QUEUE_SIZE", cfg.tcp_client_queue_size));
  if (env_bool("TCP_ENABLED", false)) {
    if (std::find(cfg.publish_modes.begin(), cfg.publish_modes.end(), "tcp") == cfg.publish_modes.end()) cfg.publish_modes.push_back("tcp");
  }

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (a == "--help" || a == "-h") { usage(argv[0]); std::exit(0); }
    else if (a == "--registry") cfg.registry_path = need("--registry");
    else if (a == "--namespace") cfg.namespace_name = need("--namespace");
    else if (a == "--config") cfg.config_path = need("--config");
    else if (a == "--publish") cfg.publish_modes = split_publish_modes(need("--publish"));
    else if (a == "--tcp-bind") cfg.tcp_bind_host = need("--tcp-bind");
    else if (a == "--tcp-port") cfg.tcp_port = std::stoi(need("--tcp-port"));
    else if (a == "--tcp-client-queue-size") cfg.tcp_client_queue_size = std::stoi(need("--tcp-client-queue-size"));
    else if (a == "--video-device" || a == "--device") cfg.video_device = need(a.c_str());
    else if (a == "--camera-index") cfg.camera_index = std::stoi(need("--camera-index"));
    else if (a == "--camera-api") cfg.camera_api = need("--camera-api");
    else if (a == "--no-camera") cfg.camera_enabled = false;
    else if (a == "--no-imu") cfg.imu_enabled = false;
    else if (a == "--duration") cfg.duration_sec = std::stoi(need("--duration"));
    else throw std::runtime_error("unknown argument: " + a);
  }

  finalize_platform_runtime_config(cfg);
  return cfg;
}

}  // namespace xr_capture_cpp
