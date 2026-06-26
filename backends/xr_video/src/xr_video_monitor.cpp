#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include <xr_video/contracts/stereo_video_contract.hpp>
#include <xr_video/shm/stereo_video_shm_reader.hpp>
#include <xr_video/net/stereo_video_tcp.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }

struct Stats {
  uint64_t frames = 0;
  uint64_t last_seq = 0;
  uint64_t sequence_gaps = 0;
  uint64_t dropped_estimate = 0;
  int64_t first_source_ts_ns = 0;
  int64_t last_source_ts_ns = 0;
  std::vector<double> frame_dt_ms;
  std::vector<double> age_ms;
  std::vector<double> stereo_delta_ms;

  void add(const xr_video::StereoVideoFrame& f) {
    ++frames;
    if (last_seq != 0 && f.header.sequence > last_seq + 1) {
      ++sequence_gaps;
      dropped_estimate += f.header.sequence - last_seq - 1;
    }
    last_seq = f.header.sequence;
    if (first_source_ts_ns == 0) first_source_ts_ns = static_cast<int64_t>(f.header.source_timestamp_ns);
    if (last_source_ts_ns != 0) {
      frame_dt_ms.push_back(xr_video::ns_to_ms(static_cast<int64_t>(f.header.source_timestamp_ns) - last_source_ts_ns));
    }
    last_source_ts_ns = static_cast<int64_t>(f.header.source_timestamp_ns);
    age_ms.push_back(xr_video::ns_to_ms(static_cast<int64_t>(xr_video::monotonic_now_ns() - f.header.publish_timestamp_ns)));
    stereo_delta_ms.push_back(xr_video::ns_to_ms(f.header.left_timestamp_ns - f.header.right_timestamp_ns));
  }

  static double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
  }
  static double minv(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return *std::min_element(v.begin(), v.end());
  }
  static double maxv(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return *std::max_element(v.begin(), v.end());
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string input = "tcp";  // tcp or shm
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45700;
  std::string video_registry = "/tmp/xr_video_streams.json";
  std::string video_stream = "stereo_video";
  double duration_s = 10.0;
  int print_every = 30;
  double poll_timeout_s = 1.0;

  CLI::App app{"XR stereo video stream monitor"};
  app.add_option("--input", input, "Input: tcp or shm");
  app.add_option("--video-input", input, "Alias for --input: tcp or shm");
  app.add_option("--tcp-host", tcp_host, "XR video TCP host");
  app.add_option("--tcp-port", tcp_port, "XR video TCP port");
  app.add_option("--video-registry", video_registry, "XR video registry path for shm input");
  app.add_option("--video-stream", video_stream, "XR video stream id for shm input");
  app.add_option("--duration", duration_s, "Run duration in seconds. 0 means until Ctrl+C");
  app.add_option("--print-every", print_every, "Print every N received frames");
  app.add_option("--poll-timeout", poll_timeout_s, "SHM poll timeout per frame");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    std::cout << "== xr_video_monitor ==\n";
    std::cout << "input: " << input << "\n";

    std::unique_ptr<xr_video::StereoVideoTcpClient> tcp_client;
    std::unique_ptr<xr_video::StereoVideoShmReader> shm_reader;
    uint64_t last_shm_seq = 0;

    if (input == "tcp") {
      std::cout << "tcp: " << tcp_host << ":" << tcp_port << "\n";
      tcp_client = std::make_unique<xr_video::StereoVideoTcpClient>(tcp_host, tcp_port);
    } else if (input == "shm") {
      std::cout << "registry: " << video_registry << " stream: " << video_stream << "\n";
      auto info = xr_video::stereo_video_stream_from_registry(video_registry, video_stream);
      shm_reader = std::make_unique<xr_video::StereoVideoShmReader>(std::move(info));
    } else {
      throw std::runtime_error("unknown --input: " + input + "; expected tcp or shm");
    }

    Stats stats;
    const auto start = std::chrono::steady_clock::now();

    while (!g_stop) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(now - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;

      std::optional<xr_video::StereoVideoFrame> frame;
      if (tcp_client) {
        frame = tcp_client->read_next();
      } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(poll_timeout_s);
        while (std::chrono::steady_clock::now() < deadline && !g_stop) {
          auto latest = shm_reader->latest();
          if (latest && latest->header.sequence > last_shm_seq) {
            last_shm_seq = latest->header.sequence;
            frame = std::move(latest);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!frame) {
          std::cerr << "[xr_video_monitor] timeout waiting for SHM frame\n";
          continue;
        }
      }

      stats.add(*frame);

      if (print_every > 0 && stats.frames % static_cast<uint64_t>(print_every) == 0) {
        const double runtime_rate = static_cast<double>(stats.frames) / std::max(1e-9, elapsed_s);
        const double camera_duration_s =
            stats.first_source_ts_ns && stats.last_source_ts_ns > stats.first_source_ts_ns
                ? static_cast<double>(stats.last_source_ts_ns - stats.first_source_ts_ns) / 1e9
                : 0.0;
        const double camera_rate = camera_duration_s > 0.0
                                       ? static_cast<double>(stats.frames - 1) / camera_duration_s
                                       : 0.0;
        std::cout << "[xr_video_monitor] frame=" << stats.frames
                  << " seq=" << frame->header.sequence
                  << " runtime_rate=" << runtime_rate << "Hz"
                  << " camera_rate=" << camera_rate << "Hz"
                  << " age_ms=" << (stats.age_ms.empty() ? 0.0 : stats.age_ms.back())
                  << " stereo_delta_ms=" << (stats.stereo_delta_ms.empty() ? 0.0 : stats.stereo_delta_ms.back())
                  << " gaps=" << stats.sequence_gaps
                  << " dropped_est=" << stats.dropped_estimate
                  << " size=" << frame->header.width << "x" << frame->header.height
                  << " left_bytes=" << frame->left.size()
                  << " right_bytes=" << frame->right.size()
                  << "\n";
      }
    }

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double camera_duration_s =
        stats.first_source_ts_ns && stats.last_source_ts_ns > stats.first_source_ts_ns
            ? static_cast<double>(stats.last_source_ts_ns - stats.first_source_ts_ns) / 1e9
            : 0.0;

    std::cout << "=== xr_video_monitor summary ===\n";
    std::cout << "input: " << input << "\n";
    std::cout << "frames: " << stats.frames << "\n";
    std::cout << "runtime_s: " << elapsed_s << "\n";
    std::cout << "runtime_rate_hz: " << (stats.frames / std::max(1e-9, elapsed_s)) << "\n";
    std::cout << "camera_duration_s: " << camera_duration_s << "\n";
    std::cout << "camera_rate_hz: "
              << (camera_duration_s > 0.0 ? static_cast<double>(stats.frames - 1) / camera_duration_s : 0.0)
              << "\n";
    std::cout << "sequence_gaps: " << stats.sequence_gaps << "\n";
    std::cout << "dropped_estimate: " << stats.dropped_estimate << "\n";
    std::cout << "frame_dt_ms_mean: " << Stats::mean(stats.frame_dt_ms) << "\n";
    std::cout << "frame_dt_ms_min: " << Stats::minv(stats.frame_dt_ms) << "\n";
    std::cout << "frame_dt_ms_max: " << Stats::maxv(stats.frame_dt_ms) << "\n";
    std::cout << "age_ms_mean: " << Stats::mean(stats.age_ms) << "\n";
    std::cout << "age_ms_min: " << Stats::minv(stats.age_ms) << "\n";
    std::cout << "age_ms_max: " << Stats::maxv(stats.age_ms) << "\n";
    std::cout << "stereo_delta_ms_mean: " << Stats::mean(stats.stereo_delta_ms) << "\n";
    std::cout << "stereo_delta_ms_min: " << Stats::minv(stats.stereo_delta_ms) << "\n";
    std::cout << "stereo_delta_ms_max: " << Stats::maxv(stats.stereo_delta_ms) << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
