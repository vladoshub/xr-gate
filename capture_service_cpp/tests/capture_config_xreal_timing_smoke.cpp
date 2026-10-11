#include "capture_service_cpp/config/capture_config.hpp"

#include <stdexcept>

using namespace xr_capture_cpp;
namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{kExitOk};
}
namespace {
void require(bool value) {
  if (!value) throw std::runtime_error("XREAL timing config check failed");
}
bool invalid(RuntimeConfig cfg) {
  try { validate_runtime_config(cfg); }
  catch (const std::runtime_error&) { return true; }
  return false;
}
}
int main(int argc, char** argv) {
  RuntimeConfig legacy;
  legacy.publish_modes = {"shm"};
  validate_runtime_config(legacy);
  require(legacy.camera.timestamp_mode == "host_receive");
  require(legacy.imu.xreal_hid.timestamp_mode == "host_receive");

  require(argc == 2);
  RuntimeConfig cfg;
  require(load_runtime_config_file(argv[1], cfg));
  validate_runtime_config(cfg);
  require(cfg.camera.timestamp_mode == "device" && cfg.imu.xreal_hid.timestamp_mode == "device");
  require(cfg.imu.raw_payload_size == 256);
  require(cfg.camera.output_width == 480 && cfg.camera.output_height == 640);
  auto half = cfg;
  half.imu.xreal_hid.timestamp_mode = "host_receive";
  require(invalid(half));
  half = cfg;
  half.camera.timestamp_mode = "host_receive";
  require(invalid(half));
  half = cfg;
  half.imu.enabled = false;
  require(invalid(half));
  half = cfg;
  half.camera.driver = "opencv";
  require(invalid(half));
  half = cfg;
  half.imu.driver = "serial";
  half.imu.serial.port = "/dev/ttyACM0";
  require(invalid(half));
  half = cfg;
  half.camera.enabled = false;  // XREAL IMU alone can still train its clock.
  validate_runtime_config(half);
  half.imu.xreal_hid.timestamp_mode = "invalid";
  require(invalid(half));
  return 0;
}
