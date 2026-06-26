#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include <nlohmann/json.hpp>

#include <capture_client/sync/latest_stereo_reader.hpp>
#include <capture_client/transports/shm_transport.hpp>
#include <capture_client/transports/tcp_transport.hpp>
#include <capture_client/transports/capture_service_tcp_transport.hpp>

#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>
#include <mercury_hand_tracking/mercury_runtime_loader.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double ns_to_ms(int64_t ns) { return double(ns) / 1e6; }

std::string iso8601_now_utc() {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return os.str();
}

std::string zero_pad_u64(uint64_t v, int width = 12) {
  std::ostringstream os;
  os << std::setw(width) << std::setfill('0') << v;
  return os.str();
}

std::filesystem::path expand_user_path(std::string path) {
  if (path.empty()) return {};
  if (path == "~" || path.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
      if (path.size() == 1) return std::filesystem::path(home);
      return std::filesystem::path(home) / path.substr(2);
    }
  }
  return std::filesystem::path(path);
}

std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

void set_process_env(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path default_mercury_runtime_library_path() {
  std::string env = env_or_empty("XR_MERCURY_RUNTIME_LIB");
  if (!env.empty()) return expand_user_path(env);

#if defined(_WIN32)
  return expand_user_path("~/src/xr_tracking/third_party/monado/build-xr-mercury/src/xrt/tracking/hand/mercury/xr_mercury_runtime.dll");
#elif defined(__APPLE__)
  return expand_user_path("~/src/xr_tracking/third_party/monado/build-xr-mercury/src/xrt/tracking/hand/mercury/libxr_mercury_runtime.dylib");
#else
  return expand_user_path("~/src/xr_tracking/third_party/monado/build-xr-mercury/src/xrt/tracking/hand/mercury/libxr_mercury_runtime.so");
#endif
}


struct HandTrackingResult {
  uint64_t frame_sequence = 0;
  int64_t frame_timestamp_ns = 0;
  uint32_t status = 0;      // 0=no_hands placeholder, 1=tracking, 2=lost
  uint32_t hands_count = 0; // placeholder
  float confidence = 0.0f;
  double processing_ms = 0.0;
};

// Placeholder processor. This intentionally does no ML yet.
// It validates the runtime loop, timing, transport, and future integration point.
class HandTrackingProcessor {
 public:
  HandTrackingResult process(const capture_client::StereoPair& pair) {
    const auto t0 = std::chrono::steady_clock::now();

    HandTrackingResult r;
    r.frame_sequence = pair.sequence();
    r.frame_timestamp_ns = pair.timestamp_ns;
    r.status = 0;
    r.hands_count = 0;
    r.confidence = 0.0f;

    const auto t1 = std::chrono::steady_clock::now();
    r.processing_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return r;
  }
};

bool save_pgm(const std::filesystem::path& path,
              const capture_client::ImageFrame& frame) {
  if (frame.width == 0 || frame.height == 0) return false;
  if (frame.gray8.empty()) return false;

  const size_t expected_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
  if (frame.gray8.size() < expected_size) return false;

  std::ofstream out(path, std::ios::binary);
  if (!out) return false;

  out << "P5\n" << frame.width << " " << frame.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(frame.gray8.data()),
            static_cast<std::streamsize>(expected_size));
  return bool(out);
}


struct DatasetDumpConfig {
  bool enabled = false;
  std::filesystem::path root_dir = "hand_tracking_dataset";
  std::filesystem::path calibration_path;
  uint64_t every_n = 1;
  uint64_t max_frames = 0; // 0 = unlimited
};

class StereoDatasetDumper {
 public:
  StereoDatasetDumper(DatasetDumpConfig cfg,
                      std::string transport_type,
                      std::string cam0_stream,
                      std::string cam1_stream,
                      bool latest_only,
                      double max_stereo_delta_ms)
      : cfg_(std::move(cfg)),
        transport_type_(std::move(transport_type)),
        cam0_stream_(std::move(cam0_stream)),
        cam1_stream_(std::move(cam1_stream)),
        latest_only_(latest_only),
        max_stereo_delta_ms_(max_stereo_delta_ms) {}

  void open() {
    if (!cfg_.enabled) return;
    if (cfg_.every_n == 0) cfg_.every_n = 1;

    std::filesystem::create_directories(cfg_.root_dir / "cam0");
    std::filesystem::create_directories(cfg_.root_dir / "cam1");

    timestamps_.open(cfg_.root_dir / "camera_timestamps.csv", std::ios::out | std::ios::trunc);
    if (!timestamps_) {
      throw std::runtime_error("failed to open dataset camera_timestamps.csv in " + cfg_.root_dir.string());
    }

    timestamps_ << "dump_index,pair_sequence,pair_timestamp_ns,"
                << "cam0_sequence,cam0_timestamp_ns,cam0_width,cam0_height,cam0_path,"
                << "cam1_sequence,cam1_timestamp_ns,cam1_width,cam1_height,cam1_path,"
                << "stereo_delta_ns\n";

    if (!cfg_.calibration_path.empty()) {
      copy_calibration_reference();
    }

    write_metadata(false);
  }

  bool should_dump(uint64_t processed_frames) const {
    if (!cfg_.enabled) return false;
    if (cfg_.max_frames != 0 && dumped_frames_ >= cfg_.max_frames) return false;
    return processed_frames == 1 || ((processed_frames - 1) % cfg_.every_n == 0);
  }

  void dump(const capture_client::StereoPair& pair) {
    if (!cfg_.enabled) return;

    const uint64_t dump_index = dumped_frames_ + 1;
    const std::string stem = zero_pad_u64(dump_index, 12);

    const std::filesystem::path cam0_rel = std::filesystem::path("cam0") / (stem + ".pgm");
    const std::filesystem::path cam1_rel = std::filesystem::path("cam1") / (stem + ".pgm");
    const std::filesystem::path cam0_abs = cfg_.root_dir / cam0_rel;
    const std::filesystem::path cam1_abs = cfg_.root_dir / cam1_rel;

    if (!save_pgm(cam0_abs, pair.cam0)) {
      throw std::runtime_error("failed to save " + cam0_abs.string());
    }
    if (!save_pgm(cam1_abs, pair.cam1)) {
      throw std::runtime_error("failed to save " + cam1_abs.string());
    }

    timestamps_ << dump_index << ','
                << pair.sequence() << ','
                << pair.timestamp_ns << ','
                << pair.cam0.sequence << ','
                << pair.cam0.timestamp_ns << ','
                << pair.cam0.width << ','
                << pair.cam0.height << ','
                << cam0_rel.generic_string() << ','
                << pair.cam1.sequence << ','
                << pair.cam1.timestamp_ns << ','
                << pair.cam1.width << ','
                << pair.cam1.height << ','
                << cam1_rel.generic_string() << ','
                << pair.timestamp_delta_ns() << '\n';

    if (dump_index == 1) {
      first_timestamp_ns_ = pair.timestamp_ns;
      cam0_width_ = pair.cam0.width;
      cam0_height_ = pair.cam0.height;
      cam1_width_ = pair.cam1.width;
      cam1_height_ = pair.cam1.height;
    }
    last_timestamp_ns_ = pair.timestamp_ns;
    dumped_frames_ = dump_index;
  }

  void close() {
    if (!cfg_.enabled) return;
    if (timestamps_) timestamps_.flush();
    write_metadata(true);
  }

  uint64_t dumped_frames() const { return dumped_frames_; }
  const std::filesystem::path& root_dir() const { return cfg_.root_dir; }

 private:
  void copy_calibration_reference() {
    try {
      const auto src = cfg_.calibration_path;
      if (!std::filesystem::exists(src)) {
        std::cerr << "[dataset_dump] calibration path does not exist: " << src << "\n";
        return;
      }

      const auto dst_root = cfg_.root_dir / "calibration";
      std::filesystem::create_directories(dst_root);
      const auto dst = dst_root / src.filename();

      if (std::filesystem::is_directory(src)) {
        std::filesystem::copy(src, dst,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);
      } else {
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
      }
      copied_calibration_path_ = std::filesystem::relative(dst, cfg_.root_dir).generic_string();
    } catch (const std::exception& e) {
      std::cerr << "[dataset_dump] warning: failed to copy calibration reference: " << e.what() << "\n";
    }
  }

  void write_metadata(bool final) const {
    nlohmann::json j;
    j["format"] = "xr_hand_tracking_stereo_dataset_v1";
    j["created_by"] = "capture_hand_tracking_backend";
    j["created_at_utc"] = created_at_utc_;
    j["finalized"] = final;
    j["transport"] = transport_type_;
    j["cam0_stream"] = cam0_stream_;
    j["cam1_stream"] = cam1_stream_;
    j["latest_only"] = latest_only_;
    j["max_stereo_delta_ms"] = max_stereo_delta_ms_;
    j["dump_every_n"] = cfg_.every_n;
    j["dump_max_frames"] = cfg_.max_frames;
    j["dumped_frames"] = dumped_frames_;
    j["first_timestamp_ns"] = first_timestamp_ns_;
    j["last_timestamp_ns"] = last_timestamp_ns_;
    j["cam0"] = {{"width", cam0_width_}, {"height", cam0_height_}, {"format", "GRAY8"}, {"dir", "cam0"}};
    j["cam1"] = {{"width", cam1_width_}, {"height", cam1_height_}, {"format", "GRAY8"}, {"dir", "cam1"}};
    j["timestamps_csv"] = "camera_timestamps.csv";
    if (!cfg_.calibration_path.empty()) {
      j["calibration_source"] = cfg_.calibration_path.string();
      j["calibration_copy"] = copied_calibration_path_;
    }
    j["notes"] = "Stereo dump for future hand-tracking/Mercury feasibility tests; no hand ML output is stored here.";

    std::ofstream out(cfg_.root_dir / "metadata.json", std::ios::out | std::ios::trunc);
    out << j.dump(2) << "\n";
  }

  DatasetDumpConfig cfg_;
  std::string transport_type_;
  std::string cam0_stream_;
  std::string cam1_stream_;
  bool latest_only_ = true;
  double max_stereo_delta_ms_ = 0.0;
  std::ofstream timestamps_;
  std::string created_at_utc_ = iso8601_now_utc();
  std::string copied_calibration_path_;
  uint64_t dumped_frames_ = 0;
  int64_t first_timestamp_ns_ = 0;
  int64_t last_timestamp_ns_ = 0;
  uint32_t cam0_width_ = 0;
  uint32_t cam0_height_ = 0;
  uint32_t cam1_width_ = 0;
  uint32_t cam1_height_ = 0;
};

struct Stats {
  uint64_t frames = 0;
  uint64_t sequence_gaps = 0;
  uint64_t total_gap = 0;
  uint64_t last_seq = 0;

  int64_t first_ts_ns = 0;
  int64_t last_ts_ns = 0;

  std::vector<double> frame_dt_ms;
  std::vector<double> stereo_delta_ms;
  std::vector<double> frame_age_ms;
  std::vector<double> processing_ms;

  void add(const capture_client::StereoPair& pair, const HandTrackingResult& result) {
    ++frames;

    if (last_seq != 0 && pair.sequence() > last_seq + 1) {
      ++sequence_gaps;
      total_gap += pair.sequence() - last_seq - 1;
    }
    last_seq = pair.sequence();

    if (first_ts_ns == 0) first_ts_ns = pair.timestamp_ns;
    if (last_ts_ns != 0) {
      frame_dt_ms.push_back(ns_to_ms(pair.timestamp_ns - last_ts_ns));
    }
    last_ts_ns = pair.timestamp_ns;

    stereo_delta_ms.push_back(ns_to_ms(pair.timestamp_delta_ns()));
    frame_age_ms.push_back(ns_to_ms(now_ns() - pair.timestamp_ns));
    processing_ms.push_back(result.processing_ms);
  }

  static double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
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
  std::string transport_type = "shm";
  std::string registry_path = "/tmp/capture_service_streams.json";
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45660;

  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string imu_stream = "imu0"; // Required by current transport config, not consumed here.

  double duration_s = 30.0;
  int print_every = 30;
  double max_stereo_delta_ms = 1.0;
  bool sequential = false;
  uint64_t max_scan_back = 8;

  int save_samples = 0;
  std::string sample_dir = "hand_tracking_samples";

  bool dump_dataset = false;
  std::string dump_dir = "hand_tracking_dataset";
  std::string dump_calib;
  uint64_t dump_every_n = 1;
  uint64_t dump_max_frames = 0;

  bool publish_hand_shm = true;
  bool no_hand_shm = false;
  std::string hand_registry_path = "/tmp/tracking_streams.json";
  std::string hand_stream = "hand_tracking";
  std::string hand_shm_name = "track_hand_tracking";
  std::string hand_frame = "tracking_world";
  uint32_t hand_slots = 1024;
  int hand_format_version = 1;
  uint64_t reset_counter = 0;


  std::string hand_tracker = "placeholder";
  std::string mercury_runtime_lib;
  std::string mercury_models = "~/xr_hand_models/mercury";
  std::string mercury_calib = "~/src/xr_tracking/calibration_dataset/final/xreal_air2ultra/ZBBM5DZFMP/unified_480_ccw90/mercury_calib_unified_480_ccw90.json";
  bool mercury_swap_cameras = false;
  bool mercury_detector_fusion = false;
  bool mercury_fusion_stereo_pairs = false;
  double mercury_min_detection_confidence = 0.05;
  double mercury_fusion_min_conf = 0.05;
  double mercury_fusion_min_center_dist_px = 70.0;
  bool mercury_boundary_circle = false;
  double mercury_boundary0_center_x = 0.5;
  double mercury_boundary0_center_y = 0.5;
  double mercury_boundary0_radius = 0.55;
  double mercury_boundary1_center_x = 0.5;
  double mercury_boundary1_center_y = 0.5;
  double mercury_boundary1_radius = 0.55;
  int mercury_orientation0 = 0;
  int mercury_orientation1 = 0;

  CLI::App app{"Hand tracking backend scaffold using capture_client LatestStereoReader"};
  app.add_option("--transport", transport_type, "Capture transport: shm, tcp, or capture_tcp");
  app.add_option("--registry", registry_path, "capture_service SHM registry path; used when --transport shm");
  app.add_option("--tcp-host", tcp_host, "capture_net_v1 host; used when --transport tcp; capture_service TCP host when --transport capture_tcp");
  app.add_option("--tcp-port", tcp_port, "capture_net_v1 port; used when --transport tcp; capture_service TCP port when --transport capture_tcp");

  app.add_option("--cam0-stream", cam0_stream, "Stream id for cam0");
  app.add_option("--cam1-stream", cam1_stream, "Stream id for cam1");
  app.add_option("--imu-stream", imu_stream, "Stream id for IMU. Required by current transport but not read by this backend.");

  app.add_option("--duration", duration_s, "Run duration in seconds. 0 means until Ctrl+C");
  app.add_option("--print-every", print_every, "Print every N stereo frames");
  app.add_option("--max-stereo-delta-ms", max_stereo_delta_ms, "Max accepted cam0/cam1 timestamp delta");
  app.add_flag("--sequential", sequential, "Read stereo frames sequentially instead of latest-only");
  app.add_option("--max-scan-back", max_scan_back, "Latest-only search depth for valid stereo pair");

  app.add_option("--save-samples", save_samples, "Save first N stereo pairs as PGM for debugging");
  app.add_option("--sample-dir", sample_dir, "Directory for saved sample PGM frames");

  app.add_flag("--dump-dataset", dump_dataset, "Enable HT0 stereo dataset dump for future hand-tracking/Mercury tests");
  app.add_option("--dump-dir", dump_dir, "Dataset output directory used with --dump-dataset");
  app.add_option("--dump-calib", dump_calib, "Calibration file or directory to copy into the dataset");
  app.add_option("--dump-every-n", dump_every_n, "Dump every Nth processed stereo pair; 1 dumps all pairs");
  app.add_option("--dump-max-frames", dump_max_frames, "Maximum dumped stereo pairs; 0 means unlimited");

  app.add_flag("--no-hand-shm", no_hand_shm, "Disable hand_tracking SHM output");
  app.add_option("--hand-registry", hand_registry_path, "tracking registry path for hand_tracking output");
  app.add_option("--hand-stream", hand_stream, "tracking stream id for hand_tracking output");
  app.add_option("--hand-shm-name", hand_shm_name, "POSIX SHM name for hand_tracking output");
  app.add_option("--hand-frame", hand_frame, "frame id metadata for hand_tracking output");
  app.add_option("--hand-slots", hand_slots, "number of output ring slots for hand_tracking SHM");
  app.add_option("--hand-format-version", hand_format_version,
                 "hand_tracking SHM payload version: 1=HAND_TRACKING_V1/F64/26 joints, 2=HAND_TRACKING_21_JOINT_F32_V2/F32/21 joints");
  app.add_option("--reset-counter", reset_counter, "tracking reset counter for output frames");
  app.add_option("--hand-tracker", hand_tracker,
                 "Hand tracker backend: placeholder or mercury");
  app.add_option("--mercury-runtime-lib", mercury_runtime_lib,
                 "Path to xr_mercury_runtime shared library. Defaults to XR_MERCURY_RUNTIME_LIB or Monado build path");
  app.add_option("--mercury-models", mercury_models,
                 "Mercury model directory containing grayscale ONNX files");
  app.add_option("--mercury-calib", mercury_calib,
                 "Stereo calibration JSON in Basalt-compatible format used to create Mercury stereo calibration");
  app.add_flag("--mercury-swap-cameras", mercury_swap_cameras,
               "Swap cam0/cam1 before feeding Mercury");
  app.add_flag("--mercury-detector-fusion", mercury_detector_fusion,
               "Enable XR Mercury detector fusion mode");
  app.add_flag("--mercury-fusion-stereo-pairs", mercury_fusion_stereo_pairs,
               "Enable stereo-pair-consistent XR Mercury fusion selection");
  app.add_option("--mercury-min-detection-confidence", mercury_min_detection_confidence,
                 "MERCURY_MIN_DETECTION_CONFIDENCE value");
  app.add_option("--mercury-fusion-min-conf", mercury_fusion_min_conf,
                 "MERCURY_XR_FUSION_MIN_CONF value");
  app.add_option("--mercury-fusion-min-center-dist-px", mercury_fusion_min_center_dist_px,
                 "MERCURY_XR_FUSION_MIN_CENTER_DIST_PX value");
  app.add_flag("--mercury-boundary-circle", mercury_boundary_circle,
               "Set Mercury camera boundary type to HT_IMAGE_BOUNDARY_CIRCLE");
  app.add_option("--mercury-boundary0-center-x", mercury_boundary0_center_x, "Mercury view0 boundary center x [0..1]");
  app.add_option("--mercury-boundary0-center-y", mercury_boundary0_center_y, "Mercury view0 boundary center y [0..1]");
  app.add_option("--mercury-boundary0-radius", mercury_boundary0_radius, "Mercury view0 boundary radius");
  app.add_option("--mercury-boundary1-center-x", mercury_boundary1_center_x, "Mercury view1 boundary center x [0..1]");
  app.add_option("--mercury-boundary1-center-y", mercury_boundary1_center_y, "Mercury view1 boundary center y [0..1]");
  app.add_option("--mercury-boundary1-radius", mercury_boundary1_radius, "Mercury view1 boundary radius");
  app.add_option("--mercury-orientation0", mercury_orientation0, "Mercury camera orientation for cam0: 0,90,180,270");
  app.add_option("--mercury-orientation1", mercury_orientation1, "Mercury camera orientation for cam1: 0,90,180,270");

  try {
    app.parse(argc, argv);
    publish_hand_shm = !no_hand_shm;
    if (hand_format_version != 1 && hand_format_version != 2) {
      std::cerr << "ERROR: --hand-format-version must be 1 or 2\n";
      return 2;
    }
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    std::cout << "== capture_hand_tracking_backend scaffold ==\n";
    std::cout << "transport: " << transport_type << "\n";
    if (transport_type == "shm") {
      std::cout << "registry: " << registry_path << "\n";
    } else if (transport_type == "tcp") {
      std::cout << "tcp: " << tcp_host << ":" << tcp_port << "\n";
    } else if (transport_type == "capture_tcp") {
      std::cout << "capture_tcp: " << tcp_host << ":" << tcp_port << "\n";
    }
    std::cout << "cam0_stream: " << cam0_stream << "\n";
    std::cout << "cam1_stream: " << cam1_stream << "\n";
    std::cout << "mode: " << (sequential ? "sequential" : "latest-only") << "\n";
    std::cout << "inference: " << hand_tracker << "\n";
    std::cout << "hand_shm: " << (publish_hand_shm ? "enabled" : "disabled") << "\n";
    if (publish_hand_shm) {
      std::cout << "hand_registry: " << hand_registry_path << "\n";
      std::cout << "hand_stream: " << hand_stream << "\n";
      std::cout << "hand_frame: " << hand_frame << "\n";
      std::cout << "hand_format_version: " << hand_format_version << "\n";
    }

    std::unique_ptr<capture_client::ICaptureTransport> transport;
    if (transport_type == "shm") {
      transport = std::make_unique<capture_client::ShmCaptureTransport>(
          registry_path, cam0_stream, cam1_stream, imu_stream);
    } else if (transport_type == "tcp") {
      capture_client::TcpCaptureTransportConfig cfg;
      cfg.host = tcp_host;
      cfg.port = tcp_port;
      cfg.cam0_stream = cam0_stream;
      cfg.cam1_stream = cam1_stream;
      cfg.imu_stream = imu_stream;
      transport = std::make_unique<capture_client::TcpCaptureTransport>(std::move(cfg));
    } else if (transport_type == "capture_tcp") {
      capture_client::CaptureServiceTcpTransportConfig capture_tcp_cfg;
      capture_tcp_cfg.host = tcp_host;
      capture_tcp_cfg.port = tcp_port;
      capture_tcp_cfg.cam0_stream = cam0_stream;
      capture_tcp_cfg.cam1_stream = cam1_stream;
      capture_tcp_cfg.imu_stream = imu_stream;
      capture_tcp_cfg.subscribe_imu = false;
      transport = std::make_unique<capture_client::CaptureServiceTcpTransport>(std::move(capture_tcp_cfg));
    } else {
      throw std::runtime_error("unknown --transport: " + transport_type + "; expected shm, tcp, or capture_tcp");
    }

    std::unique_ptr<xr_tracking::HandTrackingShmPublisher> hand_publisher_v1;
    std::unique_ptr<xr_tracking::HandTrackingShmPublisherV2> hand_publisher_v2;
    if (publish_hand_shm) {
      xr_tracking::HandTrackingShmPublisherConfig cfg;
      cfg.registry_path = hand_registry_path;
      cfg.stream_id = hand_stream;
      cfg.shm_name = hand_shm_name;
      cfg.frame_id = hand_frame;
      cfg.slot_count = hand_slots;
      if (hand_format_version == 1) {
        hand_publisher_v1 = std::make_unique<xr_tracking::HandTrackingShmPublisher>(std::move(cfg));
      } else {
        hand_publisher_v2 = std::make_unique<xr_tracking::HandTrackingShmPublisherV2>(std::move(cfg));
      }
      std::cout << "[capture_hand_tracking_backend] publishing hand_tracking SHM stream "
                << hand_stream << " -> " << hand_shm_name
                << " format=HAND_TRACKING_V" << hand_format_version << "\n";
    }

    capture_client::LatestStereoReader reader(
        *transport,
        static_cast<int64_t>(max_stereo_delta_ms * 1e6),
        !sequential,
        max_scan_back);

    HandTrackingProcessor processor;
    std::unique_ptr<xr_tracking::MercuryRuntimeProcessor> mercury_processor;
    if (hand_tracker == "mercury") {
      if (mercury_detector_fusion) set_process_env("MERCURY_XR_DETECTOR_FUSION", "1");
      if (mercury_fusion_stereo_pairs) set_process_env("MERCURY_XR_DETECTOR_FUSION_STEREO_PAIRS", "1");
      set_process_env("MERCURY_MIN_DETECTION_CONFIDENCE", std::to_string(mercury_min_detection_confidence));
      set_process_env("MERCURY_XR_FUSION_MIN_CONF", std::to_string(mercury_fusion_min_conf));
      set_process_env("MERCURY_XR_FUSION_MIN_CENTER_DIST_PX", std::to_string(mercury_fusion_min_center_dist_px));

      xr_tracking::MercuryRuntimeConfig mercury_cfg;
      mercury_cfg.library_path = mercury_runtime_lib.empty()
                                     ? default_mercury_runtime_library_path()
                                     : expand_user_path(mercury_runtime_lib);
      mercury_cfg.models_dir = expand_user_path(mercury_models);
      mercury_cfg.calib_json = expand_user_path(mercury_calib);
      mercury_cfg.swap_cameras = mercury_swap_cameras;
      mercury_cfg.boundary_circle = mercury_boundary_circle;
      mercury_cfg.boundary0_center_x = static_cast<float>(mercury_boundary0_center_x);
      mercury_cfg.boundary0_center_y = static_cast<float>(mercury_boundary0_center_y);
      mercury_cfg.boundary0_radius = static_cast<float>(mercury_boundary0_radius);
      mercury_cfg.boundary1_center_x = static_cast<float>(mercury_boundary1_center_x);
      mercury_cfg.boundary1_center_y = static_cast<float>(mercury_boundary1_center_y);
      mercury_cfg.boundary1_radius = static_cast<float>(mercury_boundary1_radius);
      mercury_cfg.orientation0 = mercury_orientation0;
      mercury_cfg.orientation1 = mercury_orientation1;

      std::cout << "mercury_runtime_lib: " << mercury_cfg.library_path << "\n"
                << "mercury_models: " << mercury_cfg.models_dir << "\n"
                << "mercury_calib: " << mercury_cfg.calib_json << "\n"
                << "mercury_detector_fusion: " << (mercury_detector_fusion ? "yes" : "no") << "\n"
                << "mercury_fusion_stereo_pairs: " << (mercury_fusion_stereo_pairs ? "yes" : "no") << "\n";
      mercury_processor = std::make_unique<xr_tracking::MercuryRuntimeProcessor>(std::move(mercury_cfg));
    }

    Stats stats;

    DatasetDumpConfig dump_cfg;
    dump_cfg.enabled = dump_dataset;
    dump_cfg.root_dir = dump_dir;
    dump_cfg.calibration_path = dump_calib;
    dump_cfg.every_n = std::max<uint64_t>(1, dump_every_n);
    dump_cfg.max_frames = dump_max_frames;
    StereoDatasetDumper dataset_dumper(std::move(dump_cfg),
                                       transport_type,
                                       cam0_stream,
                                       cam1_stream,
                                       !sequential,
                                       max_stereo_delta_ms);
    dataset_dumper.open();
    if (dump_dataset) {
      std::cout << "[capture_hand_tracking_backend] dumping stereo dataset to "
                << dataset_dumper.root_dir() << "\n";
    }

    if (save_samples > 0) {
      std::filesystem::create_directories(sample_dir);
      std::cout << "saving first " << save_samples << " samples to " << sample_dir << "\n";
    }

    const auto start = std::chrono::steady_clock::now();

    while (!g_stop) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(now - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;

      auto pair = reader.read_next(1.0);
      if (!pair) {
        std::cerr << "[capture_hand_tracking_backend] timeout waiting for stereo pair\n";
        continue;
      }

      auto result = processor.process(*pair);

      if (hand_publisher_v1) {
        auto frame = xr_tracking::make_no_hands_frame(pair->timestamp_ns, reset_counter);
        // Placeholder/no_hands output. The future ML processor will fill left/right state here.
        frame.source_timestamp_ns = static_cast<uint64_t>(pair->timestamp_ns);
        frame.timestamp_ns = static_cast<uint64_t>(now_ns());
        frame.tracking_status = result.status;
        frame.confidence = result.confidence;
        frame.hand_count = result.hands_count;
        hand_publisher_v1->publish(frame);
      } else if (hand_publisher_v2) {
        xr_tracking::HandTrackingFrameF32V2 frame;
        if (mercury_processor) {
          const auto mercury_t0 = std::chrono::steady_clock::now();
          frame = mercury_processor->process(*pair, reset_counter);
          const auto mercury_t1 = std::chrono::steady_clock::now();

          result.processing_ms =
              std::chrono::duration<double, std::milli>(mercury_t1 - mercury_t0).count();
          result.status = frame.tracking_status;
          result.confidence = frame.confidence;
          result.hands_count = frame.hand_count;
        } else {
          frame = xr_tracking::make_no_hands_frame_v2(pair->timestamp_ns, reset_counter);
          frame.source_timestamp_ns = static_cast<uint64_t>(pair->timestamp_ns);
          frame.timestamp_ns = static_cast<uint64_t>(now_ns());
          frame.tracking_status = result.status;
          frame.confidence = result.confidence;
          frame.hand_count = result.hands_count;
        }
        hand_publisher_v2->publish(frame);
      }

      stats.add(*pair, result);

      if (dataset_dumper.should_dump(stats.frames)) {
        dataset_dumper.dump(*pair);
      }

      if (save_samples > 0 && static_cast<int>(stats.frames) <= save_samples) {
        const auto base = std::filesystem::path(sample_dir) /
                          ("seq_" + std::to_string(pair->sequence()));
        save_pgm(base.string() + "_cam0.pgm", pair->cam0);
        save_pgm(base.string() + "_cam1.pgm", pair->cam1);
      }

      if (print_every > 0 && stats.frames % static_cast<uint64_t>(print_every) == 0) {
        const double runtime_rate = double(stats.frames) / std::max(1e-9, elapsed_s);
        const double camera_duration_s =
            stats.first_ts_ns && stats.last_ts_ns > stats.first_ts_ns
                ? double(stats.last_ts_ns - stats.first_ts_ns) / 1e9
                : 0.0;
        const double camera_rate =
            camera_duration_s > 0.0 ? double(stats.frames - 1) / camera_duration_s : 0.0;

        std::cout << "[capture_hand_tracking_backend] frame=" << stats.frames
                  << " seq=" << pair->sequence()
                  << " runtime_rate=" << runtime_rate << "Hz"
                  << " camera_rate=" << camera_rate << "Hz"
                  << " stereo_delta_ms=" << ns_to_ms(pair->timestamp_delta_ns())
                  << " age_ms=" << (stats.frame_age_ms.empty() ? 0.0 : stats.frame_age_ms.back())
                  << " proc_ms=" << result.processing_ms
                  << " hands=" << result.hands_count
                  << " status=" << result.status
                  << " gaps=" << stats.sequence_gaps
                  << " dropped_est=" << stats.total_gap
                  << " size=" << pair->cam0.width << "x" << pair->cam0.height
                  << "\n";
      }
    }

    dataset_dumper.close();

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double camera_duration_s =
        stats.first_ts_ns && stats.last_ts_ns > stats.first_ts_ns
            ? double(stats.last_ts_ns - stats.first_ts_ns) / 1e9
            : 0.0;

    std::cout << "=== capture_hand_tracking_backend summary ===\n";
    std::cout << "transport: " << transport->type() << "\n";
    std::cout << "frames: " << stats.frames << "\n";
    std::cout << "runtime_s: " << elapsed_s << "\n";
    std::cout << "runtime_rate_hz: " << (stats.frames / std::max(1e-9, elapsed_s)) << "\n";
    std::cout << "camera_duration_s: " << camera_duration_s << "\n";
    std::cout << "camera_rate_hz: "
              << (camera_duration_s > 0.0 ? double(stats.frames - 1) / camera_duration_s : 0.0)
              << "\n";
    std::cout << "sequence_gaps: " << stats.sequence_gaps << "\n";
    std::cout << "dropped_estimate: " << stats.total_gap << "\n";
    std::cout << "frame_dt_ms_mean: " << Stats::mean(stats.frame_dt_ms) << "\n";
    std::cout << "frame_dt_ms_min: " << Stats::minv(stats.frame_dt_ms) << "\n";
    std::cout << "frame_dt_ms_max: " << Stats::maxv(stats.frame_dt_ms) << "\n";
    std::cout << "stereo_delta_ms_mean: " << Stats::mean(stats.stereo_delta_ms) << "\n";
    std::cout << "stereo_delta_ms_min: " << Stats::minv(stats.stereo_delta_ms) << "\n";
    std::cout << "stereo_delta_ms_max: " << Stats::maxv(stats.stereo_delta_ms) << "\n";
    std::cout << "frame_age_ms_mean: " << Stats::mean(stats.frame_age_ms) << "\n";
    std::cout << "frame_age_ms_min: " << Stats::minv(stats.frame_age_ms) << "\n";
    std::cout << "frame_age_ms_max: " << Stats::maxv(stats.frame_age_ms) << "\n";
    std::cout << "processing_ms_mean: " << Stats::mean(stats.processing_ms) << "\n";
    std::cout << "processing_ms_min: " << Stats::minv(stats.processing_ms) << "\n";
    std::cout << "processing_ms_max: " << Stats::maxv(stats.processing_ms) << "\n";
    std::cout << "hands_status: "
              << (hand_tracker == "mercury" ? "mercury_runtime" : "placeholder_no_hands")
              << "\n";
    std::cout << "hand_shm: " << (publish_hand_shm ? "enabled" : "disabled") << "\n";
    if (publish_hand_shm) {
      std::cout << "hand_registry: " << hand_registry_path << "\n";
      std::cout << "hand_stream: " << hand_stream << "\n";
      std::cout << "hand_format_version: " << hand_format_version << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
