#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <tbb/global_control.h>
#include <tbb/concurrent_queue.h>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <cereal/archives/json.hpp>

#include <Eigen/Dense>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <Eigen/Geometry>

#include <basalt/calibration/calibration.hpp>
#include <basalt/serialization/headers_serialization.h>
#include <basalt/io/dataset_io.h>
#include <basalt/imu/imu_types.h>
#include <basalt/optical_flow/optical_flow.h>
#include <basalt/utils/vio_config.h>
#include <basalt/vi_estimator/vio_estimator.h>

#include <capture_client/sync/synchronizer.hpp>
#include <capture_client/transports/shm_transport.hpp>
#include <capture_client/transports/tcp_transport.hpp>
#include <capture_client/transports/capture_service_tcp_transport.hpp>
#include <xr_tracking/publishers/hmd_pose_shm_publisher.hpp>

namespace {

std::atomic_bool g_stop{false};

void handle_signal(int) { g_stop = true; }

basalt::Calibration<double> load_calibration(const std::string& path) {
  basalt::Calibration<double> calib;
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    throw std::runtime_error("failed to open camera calibration: " + path);
  }
  cereal::JSONInputArchive ar(is);
  ar(calib);
  std::cout << "[capture_basalt_backend] loaded calibration with " << calib.intrinsics.size() << " cameras\n";
  return calib;
}

basalt::ImageData make_basalt_image(const capture_client::ImageFrame& frame, double image_scale) {
  if (frame.width == 0 || frame.height == 0) {
    throw std::runtime_error("empty image frame");
  }
  if (frame.gray8.size() != static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height)) {
    throw std::runtime_error("bad GRAY8 payload size");
  }

  basalt::ImageData out;
  out.exposure = 0.0;
  out.img.reset(new basalt::ManagedImage<uint16_t>(frame.width, frame.height));

  for (size_t y = 0; y < frame.height; ++y) {
    uint16_t* dst = out.img->RowPtr(y);
    const uint8_t* src = frame.gray8.data() + y * frame.width;
    for (size_t x = 0; x < frame.width; ++x) {
      const double v = std::clamp(double(src[x]) * image_scale, 0.0, 65535.0);
      dst[x] = static_cast<uint16_t>(v);
    }
  }
  return out;
}

struct ImageHealth {
  double mean = 0.0;
  double stddev = 0.0;
  double black_fraction = 1.0;
  double white_fraction = 0.0;

  int corners = 0;
  int grid_cells = 0;
  double laplacian_stddev = 0.0;
};

ImageHealth compute_image_health(const capture_client::ImageFrame& frame) {
  if (frame.width == 0 || frame.height == 0 || frame.gray8.empty()) {
    return {};
  }

  const size_t n = frame.gray8.size();
  double sum = 0.0;
  double sum2 = 0.0;
  size_t black = 0;
  size_t white = 0;

  for (uint8_t v : frame.gray8) {
    const double x = double(v);
    sum += x;
    sum2 += x * x;
    if (v <= 8) black++;
    if (v >= 248) white++;
  }

  ImageHealth h;
  h.mean = sum / double(n);
  const double var = std::max(0.0, sum2 / double(n) - h.mean * h.mean);
  h.stddev = std::sqrt(var);
  h.black_fraction = double(black) / double(n);
  h.white_fraction = double(white) / double(n);

  cv::Mat gray(
      static_cast<int>(frame.height),
      static_cast<int>(frame.width),
      CV_8UC1,
      const_cast<uint8_t*>(frame.gray8.data()));

  std::vector<cv::Point2f> corners;
  cv::goodFeaturesToTrack(
      gray,
      corners,
      800,
      0.01,
      7.0);

  h.corners = static_cast<int>(corners.size());

  constexpr int grid_cols = 4;
  constexpr int grid_rows = 4;
  bool occupied[grid_cols * grid_rows] = {};

  for (const auto& pt : corners) {
    const int gx = std::clamp(
        static_cast<int>(pt.x * grid_cols / std::max(1u, frame.width)),
        0,
        grid_cols - 1);
    const int gy = std::clamp(
        static_cast<int>(pt.y * grid_rows / std::max(1u, frame.height)),
        0,
        grid_rows - 1);
    occupied[gy * grid_cols + gx] = true;
  }

  int cells = 0;
  for (bool v : occupied) {
    if (v) {
      cells++;
    }
  }
  h.grid_cells = cells;

  cv::Mat lap;
  cv::Laplacian(gray, lap, CV_64F);
  cv::Scalar mean_lap;
  cv::Scalar std_lap;
  cv::meanStdDev(lap, mean_lap, std_lap);
  h.laplacian_stddev = std_lap[0];

  return h;
}

bool image_health_ok(
    const ImageHealth& h,
    double min_mean,
    double min_stddev,
    double max_black_fraction,
    double max_white_fraction,
    int min_corners,
    int min_grid_cells,
    double min_laplacian_stddev) {
  return h.mean >= min_mean &&
         h.stddev >= min_stddev &&
         h.black_fraction <= max_black_fraction &&
         h.white_fraction <= max_white_fraction &&
         h.corners >= min_corners &&
         h.grid_cells >= min_grid_cells &&
         h.laplacian_stddev >= min_laplacian_stddev;
}

std::string image_health_string(const ImageHealth& h) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2)
     << "mean=" << h.mean
     << " std=" << h.stddev
     << " black=" << h.black_fraction
     << " white=" << h.white_fraction
     << " corners=" << h.corners
     << " grid=" << h.grid_cells
     << " lap_std=" << h.laplacian_stddev;
  return ss.str();
}

struct ImuHealth {
  size_t samples = 0;
  double gyro_norm_mean = 0.0;
  double gyro_norm_stddev = 0.0;
  double gyro_norm_max = 0.0;
  double accel_norm_mean = 0.0;
  double accel_norm_stddev = 0.0;
  double accel_norm_max = 0.0;
  double accel_magnitude_error = std::numeric_limits<double>::infinity();
};

template <typename ImuSamples>
ImuHealth compute_imu_health(const ImuSamples& samples, double expected_gravity_magnitude) {
  ImuHealth h;
  h.samples = samples.size();
  if (h.samples == 0) {
    return h;
  }

  double gyro_sum = 0.0;
  double gyro_sum2 = 0.0;
  double accel_sum = 0.0;
  double accel_sum2 = 0.0;

  for (const auto& s : samples) {
    const double gx = s.gyro_rad_s[0];
    const double gy = s.gyro_rad_s[1];
    const double gz = s.gyro_rad_s[2];
    const double ax = s.accel_m_s2[0];
    const double ay = s.accel_m_s2[1];
    const double az = s.accel_m_s2[2];

    const double gyro_norm = std::sqrt(gx * gx + gy * gy + gz * gz);
    const double accel_norm = std::sqrt(ax * ax + ay * ay + az * az);

    gyro_sum += gyro_norm;
    gyro_sum2 += gyro_norm * gyro_norm;
    accel_sum += accel_norm;
    accel_sum2 += accel_norm * accel_norm;
    h.gyro_norm_max = std::max(h.gyro_norm_max, gyro_norm);
    h.accel_norm_max = std::max(h.accel_norm_max, accel_norm);
  }

  const double n = static_cast<double>(h.samples);
  h.gyro_norm_mean = gyro_sum / n;
  h.accel_norm_mean = accel_sum / n;
  h.gyro_norm_stddev = std::sqrt(std::max(0.0, gyro_sum2 / n - h.gyro_norm_mean * h.gyro_norm_mean));
  h.accel_norm_stddev = std::sqrt(std::max(0.0, accel_sum2 / n - h.accel_norm_mean * h.accel_norm_mean));
  h.accel_magnitude_error = std::abs(h.accel_norm_mean - expected_gravity_magnitude);
  return h;
}

bool imu_health_ok(
    const ImuHealth& h,
    int min_samples,
    double max_gyro_norm,
    double max_gyro_stddev,
    double max_accel_magnitude_error,
    double max_accel_stddev) {
  return h.samples >= static_cast<size_t>(std::max(0, min_samples)) &&
         h.gyro_norm_mean <= max_gyro_norm &&
         h.gyro_norm_stddev <= max_gyro_stddev &&
         h.accel_magnitude_error <= max_accel_magnitude_error &&
         h.accel_norm_stddev <= max_accel_stddev;
}

std::string imu_health_string(const ImuHealth& h) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(4)
     << "samples=" << h.samples
     << " gyro_mean=" << h.gyro_norm_mean
     << " gyro_std=" << h.gyro_norm_stddev
     << " gyro_max=" << h.gyro_norm_max
     << " accel_mean=" << h.accel_norm_mean
     << " accel_std=" << h.accel_norm_stddev
     << " accel_err=" << h.accel_magnitude_error;
  return ss.str();
}

void write_trajectory_header(std::ofstream& os) {
  os << "#timestamp [ns],p_RS_R_x [m],p_RS_R_y [m],p_RS_R_z [m],"
        "q_RS_w [],q_RS_x [],q_RS_y [],q_RS_z []\n";
}

void write_pose(std::ofstream& os, const basalt::PoseVelBiasState<double>::Ptr& data) {
  const Sophus::SE3d& pose = data->T_w_i;
  os << std::scientific << std::setprecision(18)
     << data->t_ns << ","
     << pose.translation().x() << ","
     << pose.translation().y() << ","
     << pose.translation().z() << ","
     << pose.unit_quaternion().w() << ","
     << pose.unit_quaternion().x() << ","
     << pose.unit_quaternion().y() << ","
     << pose.unit_quaternion().z() << "\n";
}


struct BackendControlFile {
  double gravity_magnitude = 9.80665;
  uint64_t reset_counter = 1;
};

std::optional<std::string> read_text_file(const std::string& path) {
  std::ifstream is(path);
  if (!is) return std::nullopt;
  std::ostringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

std::optional<double> parse_json_double_field(const std::string& text, const std::string& key) {
  const std::regex re("\\\"" + key + "\\\"\\s*:\\s*([-+]?((\\d+\\.?\\d*)|(\\.\\d+))([eE][-+]?\\d+)?)");
  std::smatch m;
  if (!std::regex_search(text, m, re)) return std::nullopt;
  try {
    return std::stod(m[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<uint64_t> parse_json_u64_field(const std::string& text, const std::string& key) {
  const std::regex re("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
  std::smatch m;
  if (!std::regex_search(text, m, re)) return std::nullopt;
  try {
    return static_cast<uint64_t>(std::stoull(m[1].str()));
  } catch (...) {
    return std::nullopt;
  }
}

void write_backend_control_file(const std::string& path, const BackendControlFile& control) {
  const std::filesystem::path p(path);
  if (!p.parent_path().empty()) {
    std::filesystem::create_directories(p.parent_path());
  }

  std::ofstream os(path);
  if (!os) {
    throw std::runtime_error("failed to write backend control file: " + path);
  }

  os << "{\n";
  os << "  \"gravity_magnitude\": " << std::setprecision(10) << std::defaultfloat << control.gravity_magnitude << ",\n";
  os << "  \"reset_counter\": " << control.reset_counter << "\n";
  os << "}\n";
}

BackendControlFile read_or_create_backend_control_file(
    const std::string& path,
    const BackendControlFile& defaults,
    bool create_missing) {
  const auto text = read_text_file(path);
  if (!text) {
    if (!create_missing) return defaults;
    write_backend_control_file(path, defaults);
    std::cout << "[capture_basalt_backend] created control file: " << path << "\n";
    return defaults;
  }

  BackendControlFile out = defaults;

  const auto gravity = parse_json_double_field(*text, "gravity_magnitude");
  if (gravity) {
    out.gravity_magnitude = *gravity;
  }

  const auto reset = parse_json_u64_field(*text, "reset_counter");
  if (reset) {
    out.reset_counter = *reset;
  }

  if (!std::isfinite(out.gravity_magnitude) || out.gravity_magnitude <= 0.0) {
    throw std::runtime_error("invalid control file gravity_magnitude in " + path +
                             "; expected positive finite number");
  }

  return out;
}

[[maybe_unused]] std::optional<BackendControlFile> try_read_backend_control_file(
    const std::string& path,
    const BackendControlFile& defaults) {
  try {
    return read_or_create_backend_control_file(path, defaults, false);
  } catch (const std::exception& e) {
    std::cerr << "[capture_basalt_backend] WARN: failed to read control file: "
              << e.what() << "\n";
    return std::nullopt;
  }
}


}  // namespace

int main(int argc, char** argv) {
  std::string transport_type = "shm";
  std::string registry_path = "/tmp/capture_service_streams.json";
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45555;
  std::string cam_calib_path;
  std::string config_path;
  std::string out_dir = ".";
  std::string trajectory_path;
  bool save_trajectory = false;
  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string imu_stream = "imu0";
  double duration_s = 60.0;
  double image_scale = 256.0;

  bool startup_visual_gate = false;
  int startup_visual_good_frames = 20;
  int startup_visual_print_every = 30;
  double startup_min_mean = 14.0;
  double startup_min_stddev = 6.0;
  double startup_max_black_fraction = 0.85;
  double startup_max_white_fraction = 0.25;
  int startup_min_corners = 80;
  int startup_min_grid_cells = 8;
  double startup_min_laplacian_stddev = 8.0;

  bool startup_imu_gate = false;
  int startup_imu_good_frames = 30;
  int startup_imu_min_samples = 10;
  double startup_imu_max_gyro_norm = 0.08;
  double startup_imu_max_gyro_stddev = 0.04;
  double startup_imu_max_accel_magnitude_error = 0.75;
  double startup_imu_max_accel_stddev = 0.35;

  double gravity_z = -9.80665;
  bool use_imu = true;
  bool no_imu = false;
  bool use_double = false;
  int num_threads = 0;
  int print_every = 30;
  bool print_queue = false;
  bool enforce_realtime = false;
  bool no_enforce_realtime = false;
  bool publish_pose_shm = true;
  bool no_pose_shm = false;
  std::string pose_registry_path = "/tmp/tracking_streams.json";
  std::string pose_stream = "hmd_pose";
  std::string pose_shm_name = "track_hmd_pose";
  std::string pose_frame = "tracking_world";
  uint32_t pose_slots = 1024;
  uint64_t reset_counter = 1;
  CLI::App app{"capture_service native Basalt backend"};
  app.add_option("--transport", transport_type, "Capture transport: shm, tcp, or capture_tcp");
  app.add_option("--registry", registry_path, "capture_service SHM registry path; used when --transport shm");
  app.add_option("--tcp-host", tcp_host, "capture_net_v1 host; used when --transport tcp; capture_service TCP host when --transport capture_tcp");
  app.add_option("--tcp-port", tcp_port, "capture_net_v1 port; used when --transport tcp; capture_service TCP port when --transport capture_tcp");
  app.add_option("--cam-calib", cam_calib_path, "Basalt camera calibration JSON")->required();
  app.add_option("--config-path", config_path, "Basalt VIO config JSON")->required();
  app.add_option("--out-dir", out_dir, "Output directory");
  app.add_option("--trajectory", trajectory_path, "Output trajectory CSV path. Providing a path implies --save-trajectory.");
  app.add_flag("--save-trajectory", save_trajectory, "Save trajectory CSV for debugging. Disabled by default to avoid disk writes.");
  app.add_option("--duration", duration_s, "Run duration in seconds");
  app.add_option("--image-scale", image_scale, "GRAY8 -> uint16 scale. Default 256 matches Basalt dataset image path for GRAY8 sources.");

  app.add_flag("--startup-visual-gate", startup_visual_gate,
               "Delay feeding Basalt until stereo images are bright/contrasty enough for stable VIO initialization");
  app.add_option("--startup-visual-good-frames", startup_visual_good_frames,
                 "Number of consecutive good stereo frames required before starting VIO input");
  app.add_option("--startup-min-mean", startup_min_mean,
                 "Startup visual gate: minimum GRAY8 mean per camera");
  app.add_option("--startup-min-stddev", startup_min_stddev,
                 "Startup visual gate: minimum GRAY8 stddev per camera");
  app.add_option("--startup-max-black-fraction", startup_max_black_fraction,
                 "Startup visual gate: maximum fraction of very dark pixels per camera");
  app.add_option("--startup-max-white-fraction", startup_max_white_fraction,
                 "Startup visual gate: maximum fraction of saturated pixels per camera");
  app.add_option("--startup-min-corners", startup_min_corners,
                 "Startup visual gate: minimum Shi-Tomasi corners per camera");
  app.add_option("--startup-min-grid-cells", startup_min_grid_cells,
                 "Startup visual gate: minimum occupied 4x4 grid cells per camera");
  app.add_option("--startup-min-laplacian-stddev", startup_min_laplacian_stddev,
                 "Startup visual gate: minimum Laplacian stddev/sharpness per camera");
  app.add_option("--startup-visual-print-every", startup_visual_print_every,
                 "Print startup visual gate status every N skipped frames");

  app.add_flag("--startup-imu-gate", startup_imu_gate,
               "Delay feeding Basalt until IMU looks stationary/resting for stable VIO initialization");
  app.add_option("--startup-imu-good-frames", startup_imu_good_frames,
                 "Startup IMU gate: number of consecutive good IMU windows required before starting VIO input");
  app.add_option("--startup-imu-min-samples", startup_imu_min_samples,
                 "Startup IMU gate: minimum IMU samples in each camera-synchronized window");
  app.add_option("--startup-imu-max-gyro-norm", startup_imu_max_gyro_norm,
                 "Startup IMU gate: maximum mean gyro norm in rad/s; lower requires more rest");
  app.add_option("--startup-imu-max-gyro-stddev", startup_imu_max_gyro_stddev,
                 "Startup IMU gate: maximum gyro norm stddev in rad/s");
  app.add_option("--startup-imu-max-accel-magnitude-error", startup_imu_max_accel_magnitude_error,
                 "Startup IMU gate: maximum |mean(|accel|)-gravity| in m/s^2");
  app.add_option("--startup-imu-max-accel-stddev", startup_imu_max_accel_stddev,
                 "Startup IMU gate: maximum accel norm stddev in m/s^2");

  double gravity_magnitude = std::abs(gravity_z);
  std::string control_file_path = "/tmp/xr_backend_control.json";
  int control_poll_ms = 500;
  bool no_control_file = false;
  auto* gravity_z_opt = app.add_option("--gravity-z", gravity_z,
      "World gravity vector Z component passed to Basalt. Deprecated for normal use; prefer --gravity-magnitude.");
  auto* gravity_magnitude_opt = app.add_option("--gravity-magnitude", gravity_magnitude,
      "World gravity magnitude in m/s^2. Applied as gravity vector (0,0,-abs(g)).");
  app.add_option("--cam0-stream", cam0_stream, "Stream id for cam0");
  app.add_option("--cam1-stream", cam1_stream, "Stream id for cam1");
  app.add_option("--imu-stream", imu_stream, "Stream id for IMU");
  app.add_option("--num-threads", num_threads, "TBB max allowed parallelism");
  app.add_option("--print-every", print_every, "Print every N camera frames");
  app.add_flag("--print-queue", print_queue, "Print internal Basalt queue sizes");
  app.add_flag("--no-imu", no_imu, "Disable IMU");
  app.add_flag("--use-double", use_double, "Use double precision VIO estimator");
  app.add_flag("--enforce-realtime", enforce_realtime, "Force vio_enforce_realtime=true. Disabled by default for validation/regression mode.");
  app.add_flag("--no-enforce-realtime", no_enforce_realtime, "Keep vio_enforce_realtime disabled. This is the default; flag kept for compatibility.");
  app.add_flag("--no-pose-shm", no_pose_shm, "Disable hmd_pose SHM publisher. Enabled by default.");
  app.add_option("--pose-registry", pose_registry_path, "Tracking output registry path");
  app.add_option("--pose-stream", pose_stream, "HMD pose stream id");
  app.add_option("--pose-shm-name", pose_shm_name, "POSIX SHM name for HMD pose stream");
  app.add_option("--pose-frame", pose_frame, "Pose coordinate frame metadata. Current default is Basalt world/local frame.");
  app.add_option("--pose-slots", pose_slots, "HMD pose SHM ring slot count");
  app.add_option("--reset-counter", reset_counter, "Pose reset/recenter counter written into HMD pose payload");
  app.add_option("--control-file", control_file_path,
                 "Backend runtime control JSON. Created with defaults if missing.");
  app.add_option("--control-poll-ms", control_poll_ms,
                 "Poll interval for backend runtime control JSON; <=0 disables live reload.");
  app.add_flag("--no-control-file", no_control_file,
               "Disable backend runtime control JSON creation/reload.");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  if (gravity_magnitude_opt->count() > 0) {
    gravity_z = -std::abs(gravity_magnitude);
  } else if (gravity_z_opt->count() == 0) {
    gravity_z = -std::abs(gravity_magnitude);
  } else {
    gravity_magnitude = std::abs(gravity_z);
  }

  if (!std::isfinite(gravity_magnitude) || gravity_magnitude <= 0.0) {
    throw std::runtime_error("--gravity-magnitude must be a positive finite number");
  }
  if (!std::isfinite(gravity_z)) {
    throw std::runtime_error("--gravity-z must be finite");
  }

  BackendControlFile initial_control;
  initial_control.gravity_magnitude = std::abs(gravity_magnitude);
  initial_control.reset_counter = reset_counter;

  if (!no_control_file) {
    initial_control = read_or_create_backend_control_file(control_file_path, initial_control, true);
    gravity_magnitude = initial_control.gravity_magnitude;
    gravity_z = -std::abs(gravity_magnitude);
    reset_counter = initial_control.reset_counter;
  }

  if (no_imu) use_imu = false;
  if (startup_imu_gate && !use_imu) {
    throw std::runtime_error("--startup-imu-gate requires IMU input; remove --no-imu or disable the IMU gate");
  }
  if (startup_imu_good_frames <= 0) {
    throw std::runtime_error("--startup-imu-good-frames must be positive");
  }
  if (startup_imu_min_samples < 0) {
    throw std::runtime_error("--startup-imu-min-samples must be non-negative");
  }
  if (!std::isfinite(startup_imu_max_gyro_norm) || startup_imu_max_gyro_norm < 0.0 ||
      !std::isfinite(startup_imu_max_gyro_stddev) || startup_imu_max_gyro_stddev < 0.0 ||
      !std::isfinite(startup_imu_max_accel_magnitude_error) || startup_imu_max_accel_magnitude_error < 0.0 ||
      !std::isfinite(startup_imu_max_accel_stddev) || startup_imu_max_accel_stddev < 0.0) {
    throw std::runtime_error("startup IMU gate thresholds must be finite non-negative numbers");
  }
  if (no_enforce_realtime) enforce_realtime = false;
  if (no_pose_shm) publish_pose_shm = false;
  if (!trajectory_path.empty()) save_trajectory = true;
  if (save_trajectory && trajectory_path.empty()) trajectory_path = out_dir + "/trajectory.csv";

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::filesystem::create_directories(out_dir);

  try {
    bool backend_should_restart = false;
    do {
    backend_should_restart = false;
    BackendControlFile active_control;
    active_control.gravity_magnitude = std::abs(gravity_magnitude);
    active_control.reset_counter = reset_counter;
    if (!no_control_file) {
      active_control = read_or_create_backend_control_file(control_file_path, active_control, true);
      gravity_magnitude = active_control.gravity_magnitude;
      gravity_z = -std::abs(gravity_magnitude);
      reset_counter = active_control.reset_counter;
    }

    std::cout << "== capture_basalt_backend native ==\n";
    std::cout << "transport: " << transport_type << "\n";
    if (transport_type == "shm") {
      std::cout << "registry: " << registry_path << "\n";
    } else if (transport_type == "tcp") {
      std::cout << "tcp: " << tcp_host << ":" << tcp_port << "\n";
    } else if (transport_type == "capture_tcp") {
      std::cout << "capture_tcp: " << tcp_host << ":" << tcp_port << "\n";
    }
    std::cout << "cam_calib: " << cam_calib_path << "\n";
    std::cout << "config: " << config_path << "\n";
    std::cout << "duration_s: " << duration_s << "\n";
    if (startup_visual_gate) {
      std::cout << "startup_visual_gate: enabled"
                << " good_frames=" << startup_visual_good_frames
                << " min_mean=" << startup_min_mean
                << " min_stddev=" << startup_min_stddev
                << " max_black_fraction=" << startup_max_black_fraction
                << " max_white_fraction=" << startup_max_white_fraction
                << " min_corners=" << startup_min_corners
                << " min_grid_cells=" << startup_min_grid_cells
                << " min_laplacian_stddev=" << startup_min_laplacian_stddev
                << "\n";
    } else {
      std::cout << "startup_visual_gate: disabled\n";
    }

    if (startup_imu_gate) {
      std::cout << "startup_imu_gate: enabled"
                << " good_frames=" << startup_imu_good_frames
                << " min_samples=" << startup_imu_min_samples
                << " max_gyro_norm=" << startup_imu_max_gyro_norm
                << " max_gyro_stddev=" << startup_imu_max_gyro_stddev
                << " max_accel_magnitude_error=" << startup_imu_max_accel_magnitude_error
                << " max_accel_stddev=" << startup_imu_max_accel_stddev
                << " expected_gravity=" << std::abs(gravity_magnitude)
                << "\n";
    } else {
      std::cout << "startup_imu_gate: disabled\n";
    }
    std::cout << "gravity_magnitude: " << gravity_magnitude << "\n";
    std::cout << "gravity_vector: [0, 0, " << gravity_z << "]\n";
    std::cout << "reset_counter: " << reset_counter << "\n";
    std::cout << "control_file: " << (no_control_file ? std::string("disabled") : control_file_path) << "\n";
    if (!no_control_file) {
      std::cout << "control_poll_ms: " << control_poll_ms << "\n";
    }
    std::cout << "trajectory: " << (save_trajectory ? trajectory_path : std::string("disabled")) << "\n";
    std::cout << "pose_shm: " << (publish_pose_shm ? std::string("enabled") : std::string("disabled")) << "\n";
    if (publish_pose_shm) {
      std::cout << "pose_registry: " << pose_registry_path << "\n";
      std::cout << "pose_stream: " << pose_stream << "\n";
      std::cout << "pose_frame: " << pose_frame << "\n";
    }

    std::unique_ptr<tbb::global_control> tbb_global_control;
    if (num_threads > 0) {
      tbb_global_control = std::make_unique<tbb::global_control>(
          tbb::global_control::max_allowed_parallelism, num_threads);
    }

    basalt::VioConfig vio_config;
    vio_config.load(config_path);
    if (enforce_realtime) {
      vio_config.vio_enforce_realtime = true;
    }

    basalt::Calibration<double> calib = load_calibration(cam_calib_path);

    std::unique_ptr<capture_client::ICaptureTransport> capture_transport;
    if (transport_type == "shm") {
      capture_transport = std::make_unique<capture_client::ShmCaptureTransport>(
          registry_path, cam0_stream, cam1_stream, imu_stream);
    } else if (transport_type == "tcp") {
      capture_client::TcpCaptureTransportConfig tcp_cfg;
      tcp_cfg.host = tcp_host;
      tcp_cfg.port = tcp_port;
      tcp_cfg.cam0_stream = cam0_stream;
      tcp_cfg.cam1_stream = cam1_stream;
      tcp_cfg.imu_stream = imu_stream;
      capture_transport = std::make_unique<capture_client::TcpCaptureTransport>(std::move(tcp_cfg));
    } else if (transport_type == "capture_tcp") {
      capture_client::CaptureServiceTcpTransportConfig capture_tcp_cfg;
      capture_tcp_cfg.host = tcp_host;
      capture_tcp_cfg.port = tcp_port;
      capture_tcp_cfg.cam0_stream = cam0_stream;
      capture_tcp_cfg.cam1_stream = cam1_stream;
      capture_tcp_cfg.imu_stream = imu_stream;
      capture_tcp_cfg.subscribe_imu = true;
      capture_transport = std::make_unique<capture_client::CaptureServiceTcpTransport>(std::move(capture_tcp_cfg));
    } else {
      throw std::runtime_error("unknown --transport: " + transport_type + "; expected shm, tcp, or capture_tcp");
    }
    std::cout << "[capture_basalt_backend] capture transport ready: " << capture_transport->type() << "\n";
    capture_client::StereoImuSynchronizer sync(*capture_transport);

    std::unique_ptr<xr_runtime::HmdPoseShmPublisher> pose_pub;
    if (publish_pose_shm) {
      xr_runtime::HmdPoseShmPublisherConfig pose_cfg;
      pose_cfg.registry_path = pose_registry_path;
      pose_cfg.stream_id = pose_stream;
      pose_cfg.shm_name = pose_shm_name;
      pose_cfg.frame_id = pose_frame;
      pose_cfg.slot_count = pose_slots;
      pose_cfg.created_by = "capture_basalt_backend";
      pose_pub = std::make_unique<xr_runtime::HmdPoseShmPublisher>(pose_cfg);
      std::cout << "[capture_basalt_backend] publishing pose SHM stream "
                << pose_stream << " -> " << pose_shm_name << "\n";
    }

    auto opt_flow_ptr = basalt::OpticalFlowFactory::getOpticalFlow(vio_config, calib);
    auto vio = basalt::VioEstimatorFactory::getVioEstimator(
        vio_config, calib, Eigen::Vector3d(0.0, 0.0, gravity_z), use_imu, use_double);

    tbb::concurrent_bounded_queue<basalt::PoseVelBiasState<double>::Ptr> out_state_queue;
    out_state_queue.set_capacity(10000);
    opt_flow_ptr->output_queue = &vio->vision_data_queue;
    vio->out_state_queue = &out_state_queue;
    vio->initialize(Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero());

    std::atomic_bool input_finished{false};
    std::atomic_bool control_reset_requested{false};
    std::atomic<double> requested_gravity_magnitude;
    requested_gravity_magnitude.store(gravity_magnitude);
    std::atomic<uint64_t> requested_reset_counter;
    requested_reset_counter.store(reset_counter);
    std::atomic<size_t> frame_count{0};
    std::atomic<size_t> imu_count_total{0};
    std::atomic<int64_t> first_frame_ns{0};
    std::atomic<int64_t> last_frame_ns{0};

    std::ofstream traj;
    if (save_trajectory) {
      traj.open(trajectory_path);
      if (!traj) throw std::runtime_error("failed to open trajectory output: " + trajectory_path);
      write_trajectory_header(traj);
    }

    std::thread control_thread;
    if (!no_control_file && control_poll_ms > 0) {
      control_thread = std::thread([&]() {
        const uint64_t active_reset_counter = reset_counter;
        const BackendControlFile defaults = active_control;

        while (!g_stop && !input_finished && !control_reset_requested.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(control_poll_ms));

          const auto latest = try_read_backend_control_file(control_file_path, defaults);
          if (!latest) continue;

          if (latest->reset_counter != active_reset_counter) {
            std::cout << "[capture_basalt_backend] control reset detected: "
                      << "old_reset_counter=" << active_reset_counter
                      << " new_reset_counter=" << latest->reset_counter
                      << " new_gravity_magnitude=" << latest->gravity_magnitude
                      << "\n";

            requested_gravity_magnitude.store(latest->gravity_magnitude);
            requested_reset_counter.store(latest->reset_counter);
            control_reset_requested.store(true);
            break;
          }
        }
      });
    }

    std::thread state_thread([&]() {
      size_t pose_count = 0;
      Eigen::Vector3d prev_p = Eigen::Vector3d::Zero();
      Eigen::Quaterniond prev_q = Eigen::Quaterniond::Identity();
      int64_t prev_t_ns = 0;

      while (!g_stop) {
        basalt::PoseVelBiasState<double>::Ptr data;
        out_state_queue.pop(data);
        if (!data) break;

        const Sophus::SE3d& pose = data->T_w_i;
        const Eigen::Vector3d p = pose.translation();
        Eigen::Quaterniond q = pose.unit_quaternion();
        q.normalize();

        xr_runtime::HmdPoseF64V1 hp {};
        hp.version = 1;
        hp.size_bytes = sizeof(xr_runtime::HmdPoseF64V1);
        hp.sequence = pose_count + 1;
        hp.timestamp_ns = static_cast<uint64_t>(data->t_ns);
        hp.source_timestamp_ns = static_cast<uint64_t>(data->t_ns);
        hp.reset_counter = reset_counter;

        hp.px = p.x();
        hp.py = p.y();
        hp.pz = p.z();

        hp.qw = q.w();
        hp.qx = q.x();
        hp.qy = q.y();
        hp.qz = q.z();

        hp.tracking_status = 2u;
        hp.flags = xr_runtime::HMD_FLAG_POSE_VALID;
        hp.confidence = 1.0f;

        if (prev_t_ns > 0 && data->t_ns > prev_t_ns) {
          const double dt = double(data->t_ns - prev_t_ns) * 1e-9;
          if (dt > 0.0) {
            const Eigen::Vector3d v = (p - prev_p) / dt;
            hp.vx = v.x();
            hp.vy = v.y();
            hp.vz = v.z();
            hp.flags |= xr_runtime::HMD_FLAG_LINEAR_VELOCITY_VALID;

            Eigen::Quaterniond dq = q * prev_q.conjugate();
            dq.normalize();
            if (dq.w() < 0.0) {
              dq.coeffs() *= -1.0;
            }
            Eigen::AngleAxisd aa(dq);
            if (std::isfinite(aa.angle())) {
              const Eigen::Vector3d w = aa.axis() * (aa.angle() / dt);
              if (w.allFinite()) {
                hp.wx = w.x();
                hp.wy = w.y();
                hp.wz = w.z();
                hp.flags |= xr_runtime::HMD_FLAG_ANGULAR_VELOCITY_VALID;
              }
            }
          }
        }

        if (pose_pub) {
          pose_pub->publish(hp);
        }

        if (save_trajectory) {
          write_pose(traj, data);
        }

        prev_p = p;
        prev_q = q;
        prev_t_ns = data->t_ns;
        pose_count++;
      }

      if (save_trajectory) traj.flush();
      std::cout << "[capture_basalt_backend] state thread finished, poses=" << pose_count << "\n";
    });

    std::thread input_thread([&]() {
      const auto wall_start = std::chrono::steady_clock::now();
      bool startup_visual_ready = !startup_visual_gate;
      int startup_visual_consecutive_good = 0;
      size_t startup_visual_seen = 0;
      bool startup_imu_ready = !startup_imu_gate;
      int startup_imu_consecutive_good = 0;
      size_t startup_imu_seen = 0;

      while (!g_stop && !control_reset_requested.load()) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        if (duration_s > 0.0 && elapsed >= duration_s) break;

        auto packet = sync.read_next(1.0);
        if (!packet) {
          std::cerr << "[capture_basalt_backend] WARN: sync timeout\n";
          continue;
        }

        if (packet->imu_samples.empty()) {
          std::cerr << "[capture_basalt_backend] WARN: empty IMU window at frame timestamp "
                    << packet->pair.timestamp_ns << "\n";
        }

        if (!startup_visual_ready) {
          const ImageHealth h0 = compute_image_health(packet->pair.cam0);
          const ImageHealth h1 = compute_image_health(packet->pair.cam1);

          const bool ok0 = image_health_ok(
              h0, startup_min_mean, startup_min_stddev,
              startup_max_black_fraction, startup_max_white_fraction,
              startup_min_corners, startup_min_grid_cells, startup_min_laplacian_stddev);
          const bool ok1 = image_health_ok(
              h1, startup_min_mean, startup_min_stddev,
              startup_max_black_fraction, startup_max_white_fraction,
              startup_min_corners, startup_min_grid_cells, startup_min_laplacian_stddev);

          const bool ok = ok0 && ok1;
          startup_visual_seen++;

          if (ok) {
            startup_visual_consecutive_good++;
          } else {
            startup_visual_consecutive_good = 0;
          }

          if (startup_visual_seen == 1 ||
              startup_visual_consecutive_good >= startup_visual_good_frames ||
              (startup_visual_print_every > 0 &&
               startup_visual_seen % static_cast<size_t>(startup_visual_print_every) == 0)) {
            std::cout << "[capture_basalt_backend] startup visual gate: "
                      << "seen=" << startup_visual_seen
                      << " good=" << startup_visual_consecutive_good << "/" << startup_visual_good_frames
                      << " cam0{" << image_health_string(h0) << "} ok0=" << ok0
                      << " cam1{" << image_health_string(h1) << "} ok1=" << ok1
                      << "\n";
          }

          if (startup_visual_consecutive_good >= startup_visual_good_frames) {
            startup_visual_ready = true;
            std::cout << "[capture_basalt_backend] startup visual gate passed at frame timestamp "
                      << packet->pair.timestamp_ns << "\n";
          }
        }

        if (!startup_imu_ready) {
          const ImuHealth imu_h = compute_imu_health(packet->imu_samples, std::abs(gravity_magnitude));
          const bool imu_ok = imu_health_ok(
              imu_h,
              startup_imu_min_samples,
              startup_imu_max_gyro_norm,
              startup_imu_max_gyro_stddev,
              startup_imu_max_accel_magnitude_error,
              startup_imu_max_accel_stddev);

          startup_imu_seen++;
          if (imu_ok) {
            startup_imu_consecutive_good++;
          } else {
            startup_imu_consecutive_good = 0;
          }

          if (startup_imu_seen == 1 ||
              startup_imu_consecutive_good >= startup_imu_good_frames ||
              (startup_visual_print_every > 0 &&
               startup_imu_seen % static_cast<size_t>(startup_visual_print_every) == 0)) {
            std::cout << "[capture_basalt_backend] startup IMU gate: "
                      << "seen=" << startup_imu_seen
                      << " good=" << startup_imu_consecutive_good << "/" << startup_imu_good_frames
                      << " imu{" << imu_health_string(imu_h) << "} ok=" << imu_ok
                      << "\n";
          }

          if (startup_imu_consecutive_good >= startup_imu_good_frames) {
            startup_imu_ready = true;
            std::cout << "[capture_basalt_backend] startup IMU gate passed at frame timestamp "
                      << packet->pair.timestamp_ns << "\n";
          }
        }

        if (!startup_visual_ready || !startup_imu_ready) {
          continue;
        }

        for (const auto& s : packet->imu_samples) {
          basalt::ImuData<double>::Ptr imu(new basalt::ImuData<double>);
          imu->t_ns = s.timestamp_ns;
          imu->gyro = Eigen::Vector3d(s.gyro_rad_s[0], s.gyro_rad_s[1], s.gyro_rad_s[2]);
          imu->accel = Eigen::Vector3d(s.accel_m_s2[0], s.accel_m_s2[1], s.accel_m_s2[2]);
          vio->imu_data_queue.push(imu);
          imu_count_total++;
        }

        basalt::OpticalFlowInput::Ptr input(new basalt::OpticalFlowInput);
        input->t_ns = packet->pair.timestamp_ns;
        input->img_data.emplace_back(make_basalt_image(packet->pair.cam0, image_scale));
        input->img_data.emplace_back(make_basalt_image(packet->pair.cam1, image_scale));
        opt_flow_ptr->input_queue.push(input);

        const size_t n = ++frame_count;
        if (n == 1) first_frame_ns = packet->pair.timestamp_ns;
        last_frame_ns = packet->pair.timestamp_ns;

        if (print_every > 0 && n % static_cast<size_t>(print_every) == 0) {
          const double stream_s = double(last_frame_ns - first_frame_ns) * 1e-9;
          const double rate = stream_s > 0.0 ? double(n - 1) / stream_s : 0.0;
          std::cout << "[capture_basalt_backend] frame=" << n
                    << " stream_rate=" << rate << "Hz"
                    << " imu_count=" << packet->imu_samples.size()
                    << " latest_imu_delta_ms=" << packet->latest_imu_delta_ms()
                    << "\n";
        }
      }

      opt_flow_ptr->input_queue.push(nullptr);
      vio->imu_data_queue.push(nullptr);
      input_finished = true;
      std::cout << "[capture_basalt_backend] input thread finished\n";
    });

    std::thread queue_thread;
    if (print_queue) {
      queue_thread = std::thread([&]() {
        while (!g_stop && !input_finished) {
          std::cout << "[capture_basalt_backend] queues: opt_in=" << opt_flow_ptr->input_queue.size()
                    << " opt_out=" << (opt_flow_ptr->output_queue ? opt_flow_ptr->output_queue->size() : 0)
                    << " state_out=" << out_state_queue.size()
                    << " imu_in=" << vio->imu_data_queue.size()
                    << "\n";
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      });
    }

    const auto run_start = std::chrono::steady_clock::now();
    input_thread.join();
    if (control_thread.joinable()) control_thread.join();
    vio->maybe_join();
    vio->drain_input_queues();
    out_state_queue.push(nullptr);
    state_thread.join();
    if (!control_reset_requested.load()) g_stop = true;
    if (queue_thread.joinable()) queue_thread.join();
    const auto run_end = std::chrono::steady_clock::now();

    const size_t frames = frame_count.load();
    const size_t imu_count = imu_count_total.load();
    const int64_t start_ns = first_frame_ns.load();
    const int64_t end_ns = last_frame_ns.load();
    const double cam_duration_s = (frames > 1) ? double(end_ns - start_ns) * 1e-9 : 0.0;
    const double cam_rate_hz = (frames > 1 && cam_duration_s > 0.0) ? double(frames - 1) / cam_duration_s : 0.0;
    const double imu_per_frame = frames > 0 ? double(imu_count) / double(frames) : 0.0;
    const double runtime_s = std::chrono::duration<double>(run_end - run_start).count();

    std::cout << "=== capture_basalt_backend summary ===\n";
    std::cout << "runtime_s: " << runtime_s << "\n";
    std::cout << "camera_frames: " << frames << "\n";
    std::cout << "imu_samples: " << imu_count << "\n";
    std::cout << "camera_duration_s: " << cam_duration_s << "\n";
    std::cout << "camera_rate_hz: " << cam_rate_hz << "\n";
    std::cout << "imu_per_frame: " << imu_per_frame << "\n";
    std::cout << "trajectory: " << (save_trajectory ? trajectory_path : std::string("disabled")) << "\n";
    std::cout << "pose_shm: " << (publish_pose_shm ? std::string("enabled") : std::string("disabled")) << "\n";
    if (publish_pose_shm) {
      std::cout << "pose_registry: " << pose_registry_path << "\n";
      std::cout << "pose_stream: " << pose_stream << "\n";
      std::cout << "pose_frame: " << pose_frame << "\n";
    }

    const bool control_reset_requested_final = control_reset_requested.load();
    std::cout << "control_reset_requested: " << (control_reset_requested_final ? "true" : "false") << "\n";
    if (control_reset_requested_final && !g_stop) {
      gravity_magnitude = requested_gravity_magnitude.load();
      gravity_z = -std::abs(gravity_magnitude);
      reset_counter = requested_reset_counter.load();
      backend_should_restart = true;
    }

    if (backend_should_restart && !g_stop) {
      std::cout << "[capture_basalt_backend] restarting internal VIO estimator after control reset; "
                << "gravity_magnitude=" << gravity_magnitude
                << " reset_counter=" << reset_counter << "\n";
      continue;
    }

    return 0;
    } while (!g_stop);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
