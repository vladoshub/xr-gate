#include "capture_service_cpp/additional_camera_streams.hpp"
#include "capture_service_cpp/camera_pipeline.hpp"
#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/extra_capture.hpp"
#include "capture_service_cpp/imu_pipeline.hpp"
#include "capture_service_cpp/sources/camera_source.hpp"
#include "capture_service_cpp/sources/imu_source.hpp"
#include "capture_service_cpp/stream_publishers.hpp"
#include "capture_service_cpp/timing/xreal_device_clock.hpp"

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
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    if (argc > 1 && std::string(argv[1]) == kExtraCaptureWorkerArg) {
      try {
        return run_extra_capture(argc, argv);
      } catch (const std::exception& e) {
        std::cerr << "[capture_service_cpp][extra][ERROR] " << e.what() << std::endl;
        return kExitRuntimeError;
      }
    }
    RuntimeConfig cfg = parse_args(argc, argv);

    std::unique_ptr<ICameraSource> camera_source;
    std::unique_ptr<IImuSource> imu_source;
    auto xreal_clock = std::make_shared<XrealDeviceClock>();
    if (cfg.camera.enabled) camera_source = create_camera_source(cfg, xreal_clock);
    if (cfg.imu.enabled) imu_source = create_imu_source(cfg, xreal_clock);

    StreamPublishers publishers(cfg);
    if (camera_source && cfg.camera.stereo_enabled) {
      for (const auto& spec : camera_source->stream_specs(static_cast<uint32_t>(cfg.camera.slot_count))) {
        publishers.add_stream(spec);
      }
      publishers.add_stereo_pair("primary", cfg.camera.left_stream_id, cfg.camera.right_stream_id);
    }
    if (imu_source) {
      for (const auto& spec : imu_source->stream_specs(static_cast<uint32_t>(cfg.imu.slot_count),
                                                       static_cast<uint32_t>(cfg.imu.raw_slot_count))) {
        publishers.add_stream(spec);
      }
    }
    if ((camera_source && cfg.camera.stereo_enabled) || imu_source || cfg.camera.additional_cameras.empty()) {
      publishers.start();
      publishers.write_registry();
    }

    std::unique_ptr<AdditionalCameraStreams> additional;
    std::atomic<int> view_capture_exit{kExitOk};
    if (camera_source && !cfg.camera.additional_cameras.empty()) {
      try {
        additional = std::make_unique<AdditionalCameraStreams>(cfg, *camera_source, publishers);
      } catch (const std::exception& e) {
        std::cerr << "[capture_service_cpp][additional_cameras][ERROR] " << e.what() << std::endl;
        if (!cfg.camera.stereo_enabled) {
          cfg.camera.enabled = false;
          view_capture_exit.store(kExitRuntimeError);
        }
      }
    }
    std::cerr << "[capture_service_cpp] config=" << cfg.config_path
              << " profile=" << (cfg.profile_name.empty() ? "<none>" : cfg.profile_name)
              << " camera=" << (cfg.camera.enabled ? cfg.camera.driver : "disabled")
              << " camera_timestamp_mode=" << cfg.camera.timestamp_mode
              << " imu=" << (cfg.imu.enabled ? cfg.imu.driver : "disabled")
              << " xreal_imu_timestamp_mode=" << cfg.imu.xreal_hid.timestamp_mode
              << " backend_control_file=" << cfg.backend_control_file
              << " backend_control_poll_ms=" << cfg.backend_control_poll_ms << std::endl;

    std::vector<std::thread> threads;
    threads.reserve(2);
    ExtraCaptureProcess extra;
    if (!cfg.extra_config_path.empty()) {
      try {
        start_extra_capture(extra, cfg, publishers, additional ? &additional->publishers() : nullptr);
      } catch (const std::exception& e) {
        std::cerr << "[capture_service_cpp][extra][ERROR] " << e.what() << std::endl;
      }
    }
    try {
      if (camera_source && (cfg.camera.stereo_enabled || additional)) {
        threads.emplace_back(camera_thread, std::cref(cfg), std::move(camera_source), &publishers,
                             additional.get(), std::ref(view_capture_exit));
      }
      if (imu_source) {
        threads.emplace_back(imu_thread, std::cref(cfg), std::move(imu_source), &publishers);
      }
    } catch (...) {
      g_stop.store(true);
      for (auto& thread : threads) thread.join();
      throw;
    }

    const uint64_t started = steady_ns();
    while (!g_stop.load()) {
      const bool extra_running = !cfg.extra_config_path.empty() && extra.running();
      const int camera_exit = view_capture_exit.load();
      if (camera_exit != kExitOk && !cfg.imu.enabled && !extra_running) {
        request_stop_with_exit_code(camera_exit);
        break;
      }
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
