#include "capture_service_cpp/camera_pipeline.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace xr_capture_cpp {

void camera_thread(const RuntimeConfig& cfg, std::unique_ptr<ICameraSource> source, StreamPublishers* publishers,
                   CameraViewSink* view_sink, std::atomic<int>& view_capture_exit) {
  const auto stop_capture = [&](int exit_code) {
    if (cfg.camera.stereo_enabled) request_stop_with_exit_code(exit_code);
    else view_capture_exit.store(exit_code);
  };
  try {
    const std::string source_name = source->name();
    source->open();
    std::cerr << "[capture_service_cpp] camera source started: " << source_name << std::endl;

    uint64_t last_data_ns = steady_ns();
    uint64_t last_view_ns = 0;
    uint64_t published_pairs = 0;
    bool stereo_stalled = false;
    // TCP waits for a sender and handles the stall timeout per connection.
    const auto stall_ns = cfg.camera.driver != "tcp" && cfg.camera.stall_exit_ms > 0
        ? static_cast<uint64_t>(cfg.camera.stall_exit_ms) * 1000000ULL : 0;
    while (!g_stop.load()) {
      StereoFrame frame;
      const SourceReadStatus status = source->read(frame, view_sink);
      if (view_sink && view_sink->finish_read()) last_view_ns = steady_ns();
      if (status == SourceReadStatus::EndOfStream) {
        std::cerr << "[capture_service_cpp][ERROR] camera source ended: " << source_name << std::endl;
        stop_capture(kExitDeviceLost);
        break;
      }
      const bool publish_pair = cfg.camera.stereo_enabled && frame.complete();
      if (publish_pair) last_data_ns = steady_ns();
      if (!publish_pair) {
        if (stall_ns) {
          const uint64_t now = steady_ns();
          const uint64_t last_usable_ns = std::max(last_data_ns, last_view_ns);
          if (now - last_usable_ns >= stall_ns) {
            std::cerr << "[capture_service_cpp][ERROR] no valid "
                      << (cfg.camera.stereo_enabled ? "stereo pairs" : "camera frames") << " for "
                      << cfg.camera.stall_exit_ms << " ms from " << source_name << std::endl;
            stop_capture(kExitDeviceLost);
            break;
          }
          if (cfg.camera.stereo_enabled && !stereo_stalled && now - last_data_ns >= stall_ns) {
            std::cerr << "[capture_service_cpp][WARN] stereo pairs stalled; additional camera views are active"
                      << std::endl;
            stereo_stalled = true;
          }
        }
        if (status == SourceReadStatus::Timeout) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (stereo_stalled) {
        std::cerr << "[capture_service_cpp] stereo pairs resumed" << std::endl;
        stereo_stalled = false;
      }
      const uint64_t timestamp_ns = frame.timestamp_ns ? frame.timestamp_ns : steady_ns();
      publishers->publish(cfg.camera.left_stream_id, frame.left.ptr<uint8_t>(), frame.left.total(),
                          timestamp_ns, static_cast<uint32_t>(frame.left.cols),
                          static_cast<uint32_t>(frame.left.rows), kFormatGray8, 0,
                          cfg.camera.left_frame_id);
      publishers->publish(cfg.camera.right_stream_id, frame.right.ptr<uint8_t>(), frame.right.total(),
                          timestamp_ns, static_cast<uint32_t>(frame.right.cols),
                          static_cast<uint32_t>(frame.right.rows), kFormatGray8, 0,
                          cfg.camera.right_frame_id);
      ++published_pairs;
      if (cfg.camera.driver == "tcp") {
        publishers->mark_tcp_capture_ready(kKindImage);
      }
      if (published_pairs % 1000 == 0) {
        std::cerr << "[capture_service_cpp] camera source=" << source_name
                  << " published_pairs=" << published_pairs << std::endl;
      }
    }
    source->close();
  } catch (const std::exception& e) {
    std::cerr << "[capture_service_cpp][ERROR] camera pipeline: " << e.what() << std::endl;
    try { source->close(); } catch (...) {}
    stop_capture(kExitRuntimeError);
  }
}

}  // namespace xr_capture_cpp
