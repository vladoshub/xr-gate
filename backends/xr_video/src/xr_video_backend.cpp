#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <capture_client/transports/capture_service_tcp_transport.hpp>
#include <capture_client/sync/latest_stereo_reader.hpp>
#include <capture_client/transports/shm_transport.hpp>
#include <capture_client/transports/tcp_transport.hpp>
#include <xr_video/contracts/stereo_video_contract.hpp>
#include <xr_video/recording/stereo_video_recorder.hpp>
#include <xr_video/control/record_control_tcp.hpp>
#include <xr_video/shm/stereo_video_shm_publisher.hpp>
#include <xr_video/net/stereo_video_tcp.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }

struct Stats {
  uint64_t frames = 0;
  uint64_t last_capture_seq = 0;
  uint64_t sequence_gaps = 0;
  uint64_t dropped_estimate = 0;
  int64_t first_source_ts_ns = 0;
  int64_t last_source_ts_ns = 0;
};


int normalize_rotation_degrees(int degrees) {
  int d = degrees % 360;
  if (d < 0) d += 360;
  if (d != 0 && d != 90 && d != 180 && d != 270) {
    throw std::runtime_error("--rotate-degrees must be one of: 0, 90, 180, 270");
  }
  return d;
}

struct RotatedGray8Image {
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
};

RotatedGray8Image rotate_gray8_image_clockwise(const std::vector<uint8_t>& src,
                                               uint32_t width,
                                               uint32_t height,
                                               int degrees) {
  degrees = normalize_rotation_degrees(degrees);
  if (src.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
    throw std::runtime_error("GRAY8 rotation input size does not match width/height");
  }

  RotatedGray8Image out;
  if (degrees == 0) {
    out.pixels = src;
    out.width = width;
    out.height = height;
    return out;
  }

  if (degrees == 180) {
    out.width = width;
    out.height = height;
    out.pixels.resize(src.size());
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t nx = width - 1 - x;
        const uint32_t ny = height - 1 - y;
        out.pixels[static_cast<size_t>(ny) * width + nx] = src[static_cast<size_t>(y) * width + x];
      }
    }
    return out;
  }

  out.width = height;
  out.height = width;
  out.pixels.resize(src.size());

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t nx = 0;
      uint32_t ny = 0;
      if (degrees == 90) {          // clockwise: left-rotated input becomes upright
        nx = height - 1 - y;
        ny = x;
      } else {                      // 270 clockwise / 90 counter-clockwise
        nx = y;
        ny = width - 1 - x;
      }
      out.pixels[static_cast<size_t>(ny) * out.width + nx] = src[static_cast<size_t>(y) * width + x];
    }
  }
  return out;
}

void rotate_stereo_video_frame_in_place(xr_video::StereoVideoFrame& frame, int degrees) {
  degrees = normalize_rotation_degrees(degrees);
  if (degrees == 0) return;
  if (frame.header.pixel_format != static_cast<uint32_t>(xr_video::StereoVideoPixelFormat::Gray8)) {
    throw std::runtime_error("--rotate-degrees currently supports only GRAY8 stereo video");
  }

  auto left = rotate_gray8_image_clockwise(frame.left, frame.header.width, frame.header.height, degrees);
  auto right = rotate_gray8_image_clockwise(frame.right, frame.header.width, frame.header.height, degrees);
  if (left.width != right.width || left.height != right.height) {
    throw std::runtime_error("left/right rotated dimensions differ");
  }

  frame.left = std::move(left.pixels);
  frame.right = std::move(right.pixels);
  frame.header.width = left.width;
  frame.header.height = left.height;
  frame.header.stride_left = left.width;
  frame.header.stride_right = right.width;
  frame.header.left_size_bytes = static_cast<uint32_t>(frame.left.size());
  frame.header.right_size_bytes = static_cast<uint32_t>(frame.right.size());
}

std::string recorder_status_line(const xr_video::StereoVideoRecorder& recorder) {
  const auto st = recorder.status();
  std::ostringstream os;
  os << "recording=" << (st.active ? 1 : 0)
     << " frames_written=" << st.frames_written
     << " input_frames_seen=" << st.input_frames_seen
     << " frames_skipped=" << st.frames_skipped
     << " dir=" << st.record_dir
     << " mic_device=" << st.mic_device;
  return os.str();
}

xr_video::StereoVideoFrame make_video_frame(const capture_client::StereoPair& pair,
                                            uint64_t output_sequence,
                                            int rotate_degrees) {
  if (pair.cam0.width == 0 || pair.cam0.height == 0) {
    throw std::runtime_error("cam0 frame has invalid dimensions");
  }
  if (pair.cam1.width != pair.cam0.width || pair.cam1.height != pair.cam0.height) {
    throw std::runtime_error("cam0/cam1 dimensions differ");
  }
  if (pair.cam0.gray8.size() != static_cast<size_t>(pair.cam0.width) * pair.cam0.height ||
      pair.cam1.gray8.size() != static_cast<size_t>(pair.cam1.width) * pair.cam1.height) {
    throw std::runtime_error("cam0/cam1 payload sizes do not match GRAY8 dimensions");
  }

  xr_video::StereoVideoFrame out;
  out.left = pair.cam0.gray8;
  out.right = pair.cam1.gray8;

  out.header.version = xr_video::XR_STEREO_VIDEO_FORMAT_VERSION_V1;
  out.header.size_bytes = sizeof(xr_video::StereoVideoFrameHeaderV1);
  out.header.sequence = output_sequence;
  out.header.timestamp_ns = xr_video::monotonic_now_ns();
  out.header.source_timestamp_ns = static_cast<uint64_t>(pair.timestamp_ns);
  out.header.publish_timestamp_ns = out.header.timestamp_ns;
  out.header.left_timestamp_ns = pair.cam0.timestamp_ns;
  out.header.right_timestamp_ns = pair.cam1.timestamp_ns;
  out.header.width = pair.cam0.width;
  out.header.height = pair.cam0.height;
  out.header.stride_left = pair.cam0.width;
  out.header.stride_right = pair.cam1.width;
  out.header.pixel_format = static_cast<uint32_t>(xr_video::StereoVideoPixelFormat::Gray8);
  out.header.layout = static_cast<uint32_t>(xr_video::StereoVideoLayout::SeparateLeftRight);
  out.header.left_size_bytes = static_cast<uint32_t>(out.left.size());
  out.header.right_size_bytes = static_cast<uint32_t>(out.right.size());
  out.header.flags = xr_video::STEREO_VIDEO_FLAG_VALID;

  rotate_stereo_video_frame_in_place(out, rotate_degrees);
  xr_video::validate_frame(out);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_transport = "capture_tcp";  // shm, tcp, capture_tcp
  std::string registry_path = "/tmp/capture_service_streams.json";
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45660;
  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string imu_stream = "imu0";

  std::string output = "tcp";  // shm, tcp, both, none
  std::string video_registry = "/tmp/xr_video_streams.json";
  std::string video_stream = "stereo_video";
  std::string video_shm_name = "/xr_stereo_video_v1";
  std::string video_frame_id = "camera_stereo";
  uint32_t video_slots = 8;
  std::string video_tcp_bind = "0.0.0.0";
  int video_tcp_port = 45700;

  double duration_s = 0.0;
  int print_every = 30;
  double max_stereo_delta_ms = 1.0;
  bool sequential = false;
  uint64_t max_scan_back = 8;
  int rotate_degrees = 0;

  std::string record_dir;
  uint32_t record_every_n = 1;
  uint64_t record_max_frames = 0;
  bool record_sbs = false;
  std::string mic_device;
  std::string record_control_bind = "127.0.0.1";
  int record_control_tcp_port = 0;

  CLI::App app{"XR stereo video backend: capture_service stereo -> XR_STEREO_VIDEO_V1 SHM/TCP"};
  app.add_option("--input-transport", input_transport, "Input transport: shm, tcp, or capture_tcp");
  app.add_option("--transport", input_transport, "Alias for --input-transport: shm, tcp, or capture_tcp");
  app.add_option("--registry", registry_path, "capture_service SHM registry path; used with --input-transport shm");
  app.add_option("--tcp-host", tcp_host, "capture input TCP host");
  app.add_option("--tcp-port", tcp_port, "capture input TCP port");
  app.add_option("--cam0-stream", cam0_stream, "Stream id for cam0/left runtime video image");
  app.add_option("--cam1-stream", cam1_stream, "Stream id for cam1/right runtime video image");
  app.add_option("--imu-stream", imu_stream, "IMU stream id; required only by legacy shm/tcp capture transports");
  app.add_option("--output", output, "Output mode: none, shm, tcp, or both");
  app.add_option("--video-registry", video_registry, "XR video SHM registry path");
  app.add_option("--video-stream", video_stream, "XR video stream id");
  app.add_option("--video-shm-name", video_shm_name, "XR video POSIX SHM name");
  app.add_option("--video-frame", video_frame_id, "XR video frame id metadata");
  app.add_option("--video-slots", video_slots, "XR video SHM ring slots");
  app.add_option("--video-tcp-bind", video_tcp_bind, "XR video TCP output bind host");
  app.add_option("--video-tcp-port", video_tcp_port, "XR video TCP output port");
  app.add_option("--duration", duration_s, "Run duration in seconds. 0 means until Ctrl+C");
  app.add_option("--print-every", print_every, "Print every N stereo frames");
  app.add_option("--max-stereo-delta-ms", max_stereo_delta_ms, "Max accepted cam0/cam1 timestamp delta");
  app.add_flag("--sequential", sequential, "Read stereo frames sequentially instead of latest-only");
  app.add_option("--max-scan-back", max_scan_back, "Latest-only search depth for valid stereo pair");
  app.add_option("--rotate-degrees", rotate_degrees, "Rotate each GRAY8 camera image clockwise: 0, 90, 180, or 270. Use 90 if the image is rotated 90 degrees left.");
  app.add_option("--video-rotate-degrees", rotate_degrees, "Alias for --rotate-degrees");
  app.add_option("--record-dir", record_dir, "If set, record stereo frames as PGM sequences into this directory");
  app.add_option("--record-every-n", record_every_n, "Record every Nth input stereo frame; 1 records all frames");
  app.add_option("--record-max-frames", record_max_frames, "Maximum recorded stereo frames; 0 means unlimited");
  app.add_flag("--record-sbs", record_sbs, "Also write side-by-side PGM frames into record_dir/sbs");
  app.add_option("--mic-device", mic_device, "System microphone device/path to store in recording metadata for future audio capture");
  app.add_option("--record-control-bind", record_control_bind, "TCP bind host for recording control commands");
  app.add_option("--record-control-tcp-port", record_control_tcp_port, "TCP port for recording control. 0 disables. Commands: record start [dir], record stop, record status, quit");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  rotate_degrees = normalize_rotation_degrees(rotate_degrees);

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    std::cout << "== xr_video_backend ==\n";
    std::cout << "input_transport: " << input_transport << "\n";
    std::cout << "input_tcp: " << tcp_host << ":" << tcp_port << "\n";
    std::cout << "output: " << output << "\n";
    std::cout << "rotate_degrees: " << rotate_degrees << "\n";

    std::unique_ptr<capture_client::ICaptureTransport> transport;
    if (input_transport == "capture_tcp") {
      capture_client::CaptureServiceTcpTransportConfig cfg;
      cfg.host = tcp_host;
      cfg.port = tcp_port;
      cfg.cam0_stream = cam0_stream;
      cfg.cam1_stream = cam1_stream;
      cfg.imu_stream = imu_stream;
      cfg.subscribe_imu = false;
      transport = std::make_unique<capture_client::CaptureServiceTcpTransport>(std::move(cfg));
    } else if (input_transport == "tcp") {
      capture_client::TcpCaptureTransportConfig cfg;
      cfg.host = tcp_host;
      cfg.port = tcp_port;
      cfg.cam0_stream = cam0_stream;
      cfg.cam1_stream = cam1_stream;
      cfg.imu_stream = imu_stream;
      transport = std::make_unique<capture_client::TcpCaptureTransport>(std::move(cfg));
    } else if (input_transport == "shm") {
      transport = std::make_unique<capture_client::ShmCaptureTransport>(
          registry_path, cam0_stream, cam1_stream, imu_stream);
    } else {
      throw std::runtime_error("unknown --input-transport: " + input_transport);
    }

    capture_client::LatestStereoReader reader(
        *transport,
        static_cast<int64_t>(max_stereo_delta_ms * 1e6),
        !sequential,
        max_scan_back);

    std::unique_ptr<xr_video::StereoVideoTcpServer> tcp_server;
    if (output == "tcp" || output == "both") {
      tcp_server = std::make_unique<xr_video::StereoVideoTcpServer>(video_tcp_bind, video_tcp_port);
      std::cout << "video_tcp: " << video_tcp_bind << ":" << video_tcp_port << "\n";
    }

    std::unique_ptr<xr_video::StereoVideoShmPublisher> shm_pub;

    xr_video::StereoVideoRecorder recorder;
    xr_video::StereoVideoRecorderConfig initial_record_cfg;
    initial_record_cfg.record_dir = record_dir;
    initial_record_cfg.mic_device = mic_device;
    initial_record_cfg.every_n = record_every_n;
    initial_record_cfg.max_frames = record_max_frames;
    initial_record_cfg.write_sbs = record_sbs;
    if (!record_dir.empty()) {
      recorder.start(initial_record_cfg);
      std::cout << "recording: " << recorder_status_line(recorder) << "\n";
    }

    std::unique_ptr<xr_video::RecordControlTcpServer> record_control;
    if (record_control_tcp_port > 0) {
      xr_video::RecordControlCallbacks cb;
      cb.start = [&](const std::string& dir) {
        xr_video::StereoVideoRecorderConfig cfg = initial_record_cfg;
        if (!dir.empty()) cfg.record_dir = dir;
        recorder.start(cfg);
        return std::string("OK started ") + recorder_status_line(recorder);
      };
      cb.stop = [&]() {
        recorder.stop();
        return std::string("OK stopped ") + recorder_status_line(recorder);
      };
      cb.status = [&]() {
        return std::string("OK ") + recorder_status_line(recorder);
      };
      cb.quit = [&]() {
        g_stop = true;
        return std::string("OK quitting ") + recorder_status_line(recorder);
      };
      record_control = std::make_unique<xr_video::RecordControlTcpServer>(
          record_control_bind, record_control_tcp_port, std::move(cb));
      std::cout << "record_control_tcp: " << record_control_bind << ":" << record_control_tcp_port << "\n";
    }

    Stats stats;
    const auto start = std::chrono::steady_clock::now();

    while (!g_stop) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(now - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;

      auto pair = reader.read_next(1.0);
      if (!pair) {
        std::cerr << "[xr_video_backend] timeout waiting for stereo pair\n";
        continue;
      }

      ++stats.frames;
      if (stats.last_capture_seq != 0 && pair->sequence() > stats.last_capture_seq + 1) {
        ++stats.sequence_gaps;
        stats.dropped_estimate += pair->sequence() - stats.last_capture_seq - 1;
      }
      stats.last_capture_seq = pair->sequence();
      if (stats.first_source_ts_ns == 0) stats.first_source_ts_ns = pair->timestamp_ns;
      stats.last_source_ts_ns = pair->timestamp_ns;

      auto frame = make_video_frame(*pair, stats.frames, rotate_degrees);
      const uint32_t output_width = frame.header.width;
      const uint32_t output_height = frame.header.height;

      recorder.record(frame);

      if ((output == "shm" || output == "both") && !shm_pub) {
        xr_video::StereoVideoShmPublisherConfig cfg;
        cfg.registry_path = video_registry;
        cfg.stream_id = video_stream;
        cfg.shm_name = video_shm_name;
        cfg.frame_id = video_frame_id;
        cfg.slot_count = video_slots;
        cfg.width = frame.header.width;
        cfg.height = frame.header.height;
        cfg.pixel_format = xr_video::StereoVideoPixelFormat::Gray8;
        shm_pub = std::make_unique<xr_video::StereoVideoShmPublisher>(std::move(cfg));
        std::cout << "video_shm: registry=" << video_registry
                  << " stream=" << video_stream
                  << " shm=" << video_shm_name
                  << " size=" << frame.header.width << "x" << frame.header.height << "\n";
      }

      if (shm_pub) shm_pub->publish(frame);
      if (tcp_server) tcp_server->publish(std::move(frame));

      if (print_every > 0 && stats.frames % static_cast<uint64_t>(print_every) == 0) {
        const double runtime_rate = static_cast<double>(stats.frames) / std::max(1e-9, elapsed_s);
        const double camera_duration_s =
            stats.first_source_ts_ns && stats.last_source_ts_ns > stats.first_source_ts_ns
                ? static_cast<double>(stats.last_source_ts_ns - stats.first_source_ts_ns) / 1e9
                : 0.0;
        const double camera_rate = camera_duration_s > 0.0
                                       ? static_cast<double>(stats.frames - 1) / camera_duration_s
                                       : 0.0;
        std::cout << "[xr_video_backend] frame=" << stats.frames
                  << " capture_seq=" << pair->sequence()
                  << " runtime_rate=" << runtime_rate << "Hz"
                  << " camera_rate=" << camera_rate << "Hz"
                  << " stereo_delta_ms=" << xr_video::ns_to_ms(pair->timestamp_delta_ns())
                  << " gaps=" << stats.sequence_gaps
                  << " dropped_est=" << stats.dropped_estimate
                  << " size=" << output_width << "x" << output_height
                  << " input_size=" << pair->cam0.width << "x" << pair->cam0.height
                  << " rotate_degrees=" << rotate_degrees
                  << " tcp_clients=" << (tcp_server ? tcp_server->client_count() : 0)
                  << " record=" << (recorder.active() ? 1 : 0)
                  << " record_frames=" << recorder.status().frames_written
                  << "\n";
      }
    }

    const auto final_record_status_before_stop = recorder.status();
    recorder.stop();

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double camera_duration_s =
        stats.first_source_ts_ns && stats.last_source_ts_ns > stats.first_source_ts_ns
            ? static_cast<double>(stats.last_source_ts_ns - stats.first_source_ts_ns) / 1e9
            : 0.0;

    std::cout << "=== xr_video_backend summary ===\n";
    std::cout << "input_transport: " << transport->type() << "\n";
    std::cout << "output: " << output << "\n";
    std::cout << "rotate_degrees: " << rotate_degrees << "\n";
    std::cout << "frames: " << stats.frames << "\n";
    std::cout << "runtime_s: " << elapsed_s << "\n";
    std::cout << "runtime_rate_hz: " << (stats.frames / std::max(1e-9, elapsed_s)) << "\n";
    std::cout << "camera_duration_s: " << camera_duration_s << "\n";
    std::cout << "camera_rate_hz: "
              << (camera_duration_s > 0.0 ? static_cast<double>(stats.frames - 1) / camera_duration_s : 0.0)
              << "\n";
    std::cout << "sequence_gaps: " << stats.sequence_gaps << "\n";
    std::cout << "dropped_estimate: " << stats.dropped_estimate << "\n";
    if (tcp_server) std::cout << "tcp_clients: " << tcp_server->client_count() << "\n";
    std::cout << "record_active_at_stop: " << (final_record_status_before_stop.active ? 1 : 0) << "\n";
    std::cout << "record_dir: " << final_record_status_before_stop.record_dir << "\n";
    std::cout << "record_frames_written: " << final_record_status_before_stop.frames_written << "\n";
    std::cout << "record_frames_skipped: " << final_record_status_before_stop.frames_skipped << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
