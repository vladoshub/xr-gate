#include "capture_service_cpp/camera_pipeline.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>

namespace xr_capture_cpp {

void camera_thread(const RuntimeConfig& cfg, std::unique_ptr<ICameraSource> source, StreamPublishers* publishers) {
  try {
    const std::string source_name = source->name();
    source->open();
    std::cerr << "[capture_service_cpp] camera source started: " << source_name << std::endl;

    uint64_t last_data_ns = steady_ns();
    uint64_t published_pairs = 0;
    while (!g_stop.load()) {
      StereoFrame frame;
      const SourceReadStatus status = source->read(frame);
      if (status == SourceReadStatus::EndOfStream) {
        std::cerr << "[capture_service_cpp][ERROR] camera source ended: " << source_name << std::endl;
        request_stop_with_exit_code(kExitDeviceLost);
        break;
      }
      if (status == SourceReadStatus::Data) last_data_ns = steady_ns();
      if (status == SourceReadStatus::Timeout) {
        if (cfg.camera.stall_exit_ms > 0 &&
            steady_ns() - last_data_ns >= static_cast<uint64_t>(cfg.camera.stall_exit_ms) * 1000000ULL) {
          std::cerr << "[capture_service_cpp][ERROR] no camera data for " << cfg.camera.stall_exit_ms
                    << " ms from " << source_name << std::endl;
          request_stop_with_exit_code(kExitDeviceLost);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (!frame.complete()) continue;

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
      if (published_pairs % 1000 == 0) {
        std::cerr << "[capture_service_cpp] camera source=" << source_name
                  << " published_pairs=" << published_pairs << std::endl;
      }
    }
    source->close();
  } catch (const std::exception& e) {
    std::cerr << "[capture_service_cpp][ERROR] camera pipeline: " << e.what() << std::endl;
    request_stop_with_exit_code(kExitRuntimeError);
    try { source->close(); } catch (...) {}
  }
}

}  // namespace xr_capture_cpp
