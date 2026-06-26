#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace xr_capture_cpp {

extern std::atomic<bool> g_stop;

constexpr uint32_t kKindImage = 1;
constexpr uint32_t kKindImu = 2;
constexpr uint32_t kKindBytes = 3;
constexpr uint32_t kFormatGray8 = 1;
constexpr uint32_t kFormatImuF32Le = 101;
constexpr uint32_t kFormatBytes = 255;
constexpr size_t kHeaderSize = 4096;
constexpr size_t kSlotHeaderSize = 128;
constexpr const char* kTcpProtocolName = "capture_net_v1_json_payload";

struct StreamSpec {
  std::string stream_id;
  uint32_t kind = 0;
  std::string kind_name;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_code = 0;
  std::string format_name;
  uint32_t payload_size = 0;
  uint32_t slot_count = 256;
  std::string frame_id;
  std::string description;
};

struct RuntimeConfig {
  std::string registry_path = "/tmp/capture_service_streams.json";
  std::string namespace_name = "xreal_air2ultra_linux";
  std::string config_path = "<native-cpp>";
  std::string video_device = "/dev/video0";
  int camera_index = 0;
  std::string camera_api = "auto";
  int slot_count = 256;
  bool camera_enabled = true;
  bool imu_enabled = true;
  bool raw_hid_enabled = true;
  int duration_sec = 0;
  std::string left_rotate = "ccw90";
  std::string right_rotate = "ccw90";
  std::string left_flip = "none";
  std::string right_flip = "xy";
  std::vector<std::string> publish_modes;
  std::string tcp_bind_host = "0.0.0.0";
  int tcp_port = 45660;
  int tcp_client_queue_size = 256;
};

uint64_t steady_ns();
uint64_t wall_ns();
std::string env_or(const char* name, const std::string& fallback);
int env_int(const char* name, int fallback);
bool env_bool(const char* name, bool fallback);
std::string json_escape(const std::string& s);
std::string sanitize_shm_name(const std::string& value);
std::vector<std::string> split_publish_modes(const std::string& value);
RuntimeConfig parse_args(int argc, char** argv);
void usage(const char* argv0);

}  // namespace xr_capture_cpp
