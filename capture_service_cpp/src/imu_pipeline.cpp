#include "capture_service_cpp/imu_pipeline.hpp"

#include <array>
#include <iostream>
#include <thread>

namespace xr_capture_cpp {

void imu_thread(const RuntimeConfig& cfg, std::unique_ptr<IImuSource> source, StreamPublishers* publishers) {
  try {
    const std::string source_name = source->name();
    source->open();
    std::cerr << "[capture_service_cpp] IMU source started: " << source_name << std::endl;

    uint64_t last_data_ns = steady_ns();
    uint64_t published_samples = 0;
    while (!g_stop.load()) {
      ImuReadResult result;
      const SourceReadStatus status = source->read(result);
      if (status == SourceReadStatus::EndOfStream) {
        std::cerr << "[capture_service_cpp][ERROR] IMU source ended: " << source_name << std::endl;
        request_stop_with_exit_code(kExitDeviceLost);
        break;
      }
      if (status == SourceReadStatus::Data) last_data_ns = steady_ns();
      if (status == SourceReadStatus::Timeout) {
        if (cfg.imu.stall_exit_ms > 0 &&
            steady_ns() - last_data_ns >= static_cast<uint64_t>(cfg.imu.stall_exit_ms) * 1000000ULL) {
          std::cerr << "[capture_service_cpp][ERROR] no IMU data for " << cfg.imu.stall_exit_ms
                    << " ms from " << source_name << std::endl;
          request_stop_with_exit_code(kExitDeviceLost);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      if (cfg.imu.raw_enabled && !result.raw_packet.empty()) {
        publishers->publish(cfg.imu.raw_stream_id, result.raw_packet.data(), result.raw_packet.size(),
                            result.receive_timestamp_ns ? result.receive_timestamp_ns : steady_ns(),
                            0, 0, kFormatBytes, 0, cfg.imu.raw_frame_id);
      }
      if (!result.has_sample) continue;

      std::array<float, 6> payload{};
      for (size_t i = 0; i < 3; ++i) {
        payload[i] = result.sample.gyro_rad_s[i];
        payload[i + 3] = result.sample.accel_m_s2[i];
      }
      publishers->publish(cfg.imu.stream_id, reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload),
                          result.sample.timestamp_ns ? result.sample.timestamp_ns : steady_ns(),
                          0, 0, kFormatImuF32Le, 0, cfg.imu.frame_id);
      ++published_samples;
      if (published_samples % 1000 == 0) {
        std::cerr << "[capture_service_cpp] IMU source=" << source_name
                  << " published_samples=" << published_samples << std::endl;
      }
    }
    source->close();
  } catch (const std::exception& e) {
    std::cerr << "[capture_service_cpp][ERROR] IMU pipeline: " << e.what() << std::endl;
    request_stop_with_exit_code(kExitRuntimeError);
    try { source->close(); } catch (...) {}
  }
}

}  // namespace xr_capture_cpp
