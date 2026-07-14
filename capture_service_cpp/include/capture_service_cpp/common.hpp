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
extern std::atomic<int> g_exit_code;

constexpr int kExitOk = 0;
constexpr int kExitRuntimeError = 2;
constexpr int kExitDeviceLost = 3;

void request_stop_with_exit_code(int exit_code);

constexpr uint32_t kKindImage = 1;
constexpr uint32_t kKindImu = 2;
constexpr uint32_t kKindBytes = 3;
constexpr uint32_t kFormatGray8 = 1;
constexpr uint32_t kFormatImuF32Le = 101;
constexpr uint32_t kFormatBytes = 255;
constexpr size_t kHeaderSize = 4096;
constexpr size_t kSlotHeaderSize = 128;
constexpr const char* kTcpProtocolName = "capture_net_v1_json_payload";

enum class SourceReadStatus {
  Data,
  // Bytes were received, but no complete valid logical sample was produced.
  // This must not reset device-stall detection.
  TransportActivity,
  Timeout,
  EndOfStream,
};

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

struct CameraDeviceConfig {
  // Linux normally uses device_path; Windows normally uses index. Drivers may
  // use either field on either platform when the backend supports it.
  std::string device_path;
  int index = 0;
  std::string api = "auto";
  int width = 0;
  int height = 0;
  int fps = 0;
  bool raw_format = false;
  bool convert_rgb = true;
  int buffer_size = 1;
};

struct ImageTransformConfig {
  std::string rotate = "none";
  std::string flip = "none";
};

struct CameraSourceConfig {
  bool enabled = true;
  std::string driver = "xreal_ultra";

  // xreal_packed: one vendor-packed XREAL stream.
  // side_by_side_horizontal / side_by_side_vertical: one normal OpenCV frame.
  // interleaved_columns: one raw GRAY8 stereo frame arranged L0,R0,L1,R1,...
  // dual: two independent OpenCV camera devices.
  std::string layout = "xreal_packed";
  std::string stereo_order = "left_right";
  CameraDeviceConfig primary;
  CameraDeviceConfig secondary;

  std::string left_stream_id = "camera0";
  std::string right_stream_id = "camera1";
  std::string left_frame_id = "camera0";
  std::string right_frame_id = "camera1";
  int output_width = 480;
  int output_height = 640;
  int slot_count = 0;  // 0 means inherit service.slot_count
  ImageTransformConfig left_transform{"ccw90", "none"};
  ImageTransformConfig right_transform{"ccw90", "xy"};
  int stall_exit_ms = 2000;
};

struct SerialImuConfig {
  std::string port;
  int baud_rate = 921600;
  std::string protocol = "xr_imu_v1";
  std::string timestamp_mode = "device";  // device | host_receive
  int read_timeout_ms = 50;
  size_t max_packet_size = 256;
};

struct XrealHidImuConfig {
  uint16_t vendor_id = 0x3318;
  uint16_t product_id = 0x0426;
  int interface_number = 2;
  int drop_first_packets = 500;
  int read_timeout_ms = 50;
};

struct ImuSourceConfig {
  bool enabled = true;
  std::string driver = "xreal_hid";
  std::string stream_id = "imu0";
  std::string frame_id = "imu0";
  bool raw_enabled = true;
  std::string raw_stream_id = "xreal_raw_hid";
  std::string raw_frame_id = "xreal_raw_hid";
  size_t raw_payload_size = 64;
  int slot_count = 0;      // 0 means inherit service.slot_count
  int raw_slot_count = 0;  // 0 means inherit imu.slot_count
  int stall_exit_ms = 2000;
  XrealHidImuConfig xreal_hid;
  SerialImuConfig serial;
};

struct RuntimeConfig {
  std::string registry_path = "/tmp/capture_service_streams.json";
  std::string namespace_name = "xreal_air2ultra_linux";
  std::string config_path = "<built-in:xreal_ultra>";
  std::string config_dir;
  std::string config_name = "config.yaml";
  bool config_explicit = false;

  int slot_count = 256;
  int duration_sec = 0;
  std::vector<std::string> publish_modes;
  std::string tcp_bind_host = "0.0.0.0";
  int tcp_port = 45660;
  int tcp_client_queue_size = 256;

  CameraSourceConfig camera;
  ImuSourceConfig imu;
};

uint64_t steady_ns();
uint64_t wall_ns();
std::string env_or(const char* name, const std::string& fallback);
int env_int(const char* name, int fallback);
bool env_bool(const char* name, bool fallback);
std::string json_escape(const std::string& s);
std::string sanitize_shm_name(const std::string& value);
std::vector<std::string> split_publish_modes(const std::string& value);
std::string expand_user_path(const std::string& value);
RuntimeConfig parse_args(int argc, char** argv);
void usage(const char* argv0);

}  // namespace xr_capture_cpp
