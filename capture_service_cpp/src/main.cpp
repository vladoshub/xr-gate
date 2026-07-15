#include "capture_service_cpp/camera_pipeline.hpp"
#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/imu_pipeline.hpp"
#include "capture_service_cpp/sources/camera_source.hpp"
#include "capture_service_cpp/sources/imu_source.hpp"
#include "capture_service_cpp/stream_publishers.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{kExitOk};
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

    std::unique_ptr<ICameraSource> camera_source;
    std::unique_ptr<IImuSource> imu_source;
    if (cfg.camera.enabled) camera_source = create_camera_source(cfg);
    if (cfg.imu.enabled) imu_source = create_imu_source(cfg);

    StreamPublishers publishers(cfg);
    if (camera_source) {
      for (const auto& spec : camera_source->stream_specs(static_cast<uint32_t>(cfg.camera.slot_count))) {
        publishers.add_stream(spec);
      }
    }
    if (imu_source) {
      for (const auto& spec : imu_source->stream_specs(static_cast<uint32_t>(cfg.imu.slot_count),
                                                       static_cast<uint32_t>(cfg.imu.raw_slot_count))) {
        publishers.add_stream(spec);
      }
    }
    publishers.start();
    publishers.write_registry();

    std::cerr << "[capture_service_cpp] config=" << cfg.config_path
              << " profile=" << (cfg.profile_name.empty() ? "<none>" : cfg.profile_name)
              << " camera=" << (cfg.camera.enabled ? cfg.camera.driver : "disabled")
              << " imu=" << (cfg.imu.enabled ? cfg.imu.driver : "disabled") << std::endl;

    std::vector<std::thread> threads;
    if (camera_source) {
      threads.emplace_back(camera_thread, std::cref(cfg), std::move(camera_source), &publishers);
    }
    if (imu_source) {
      threads.emplace_back(imu_thread, std::cref(cfg), std::move(imu_source), &publishers);
    }

    const uint64_t started = steady_ns();
    while (!g_stop.load()) {
      if (cfg.duration_sec > 0 &&
          steady_ns() - started >= static_cast<uint64_t>(cfg.duration_sec) * 1000000000ULL) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    g_stop.store(true);
    for (auto& thread : threads) if (thread.joinable()) thread.join();
    return g_exit_code.load();
  } catch (const std::exception& e) {
    std::cerr << "[capture_service_cpp][ERROR] " << e.what() << std::endl;
    return kExitRuntimeError;
  }
}
