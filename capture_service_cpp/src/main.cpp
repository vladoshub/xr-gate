#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/stream_publishers.hpp"
#include "capture_service_cpp/camera_pipeline.hpp"
#include "capture_service_cpp/imu_pipeline.hpp"
#include "capture_service_cpp/vendor/xreal_stream_specs.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
}

namespace {
void signal_handler(int) { xr_capture_cpp::g_stop.store(true); }
}

int main(int argc, char** argv) {
  using namespace xr_capture_cpp;
  try {
    RuntimeConfig cfg = parse_args(argc, argv);
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    init_xreal_camera_tables();

    const uint32_t slot_count = static_cast<uint32_t>(cfg.slot_count);
    StreamSpec cam0 = make_xreal_camera0_stream(slot_count);
    StreamSpec cam1 = make_xreal_camera1_stream(slot_count);
    StreamSpec rawhid = make_xreal_raw_hid_stream(slot_count);
    StreamSpec imu0 = make_xreal_imu_stream(slot_count);

    StreamPublishers publishers(cfg);
    if (cfg.camera_enabled) {
      publishers.add_stream(cam0);
      publishers.add_stream(cam1);
    }
    if (cfg.imu_enabled) {
      if (cfg.raw_hid_enabled) publishers.add_stream(rawhid);
      publishers.add_stream(imu0);
    }
    publishers.start();
    publishers.write_registry();

    std::vector<std::thread> threads;
    if (cfg.camera_enabled) threads.emplace_back(camera_thread, cfg, &publishers);
    if (cfg.imu_enabled) threads.emplace_back(imu_thread, cfg, &publishers);

    const uint64_t started = steady_ns();
    while (!g_stop.load()) {
      if (cfg.duration_sec > 0 && steady_ns() - started >= static_cast<uint64_t>(cfg.duration_sec) * 1000000000ULL) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    g_stop.store(true);
    for (auto& t : threads) if (t.joinable()) t.join();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[capture_service_cpp][ERROR] " << e.what() << std::endl;
    return 2;
  }
}
