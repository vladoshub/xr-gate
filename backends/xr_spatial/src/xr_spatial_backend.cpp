#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <vector>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <capture_client/sync/latest_stereo_reader.hpp>
#include <capture_client/transports/shm_transport.hpp>
#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_tracking/publishers/hmd_pose_shm_publisher.hpp>
#include <xr_spatial/publishers/runtime_spatial_summary_shm_publisher.hpp>
#include <xr_spatial/publishers/runtime_spatial_proxy_mesh_shm_publisher.hpp>
#include <xr_spatial_backend/calibration.hpp>
#include <xr_spatial_backend/pointcloud_map.hpp>
#include <xr_spatial_backend/pointcloud_filter.hpp>
#include <xr_spatial_backend/spatial_proxy_mesh_builder.hpp>
#include <xr_spatial_backend/stereo_depth_processor.hpp>

namespace fs = std::filesystem;

namespace {
std::atomic_bool g_stop{false};

void handle_signal(int) { g_stop = true; }

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}


xr_runtime::StreamInfo wait_for_stream_info(const std::string& registry_path,
                                            const std::string& stream_id,
                                            const std::string& label,
                                            double timeout_sec,
                                            int retry_interval_ms) {
  const auto start = std::chrono::steady_clock::now();
  auto next_log = start;
  retry_interval_ms = std::max(50, retry_interval_ms);
  while (!g_stop) {
    try {
      auto info = xr_runtime::stream_info_from_registry(registry_path, stream_id);
      std::cout << "[xr_spatial_backend] attached " << label
                << " stream: " << stream_id
                << " frame=" << info.frame_id
                << " shm=" << info.shm_name << "\n";
      return info;
    } catch (const std::exception& e) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed = std::chrono::duration<double>(now - start).count();
      if (timeout_sec > 0.0 && elapsed >= timeout_sec) {
        throw std::runtime_error("timeout waiting for " + label + " stream '" + stream_id +
                                 "' in registry " + registry_path + ": " + e.what());
      }
      if (now >= next_log) {
        std::cout << "[xr_spatial_backend] waiting for " << label
                  << " stream '" << stream_id << "' in " << registry_path
                  << ": " << e.what() << "\\n";
        next_log = now + std::chrono::seconds(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_interval_ms));
    }
  }
  throw std::runtime_error("stopped while waiting for " + label + " stream '" + stream_id + "'");
}

struct SelectedStereoPair {
  uint64_t sequence = 0;
  capture_client::StereoPair pair;
  int64_t age_to_target_ns = 0;
};

std::optional<capture_client::StereoPair> read_stereo_pair_sequence(
    capture_client::ICaptureTransport& capture,
    uint64_t seq,
    int64_t max_stereo_delta_ns) {
  capture_client::ImageFrame c0, c1;
  if (!capture.cam0().read_image_sequence(seq, c0)) return std::nullopt;
  if (!capture.cam1().read_image_sequence(seq, c1)) return std::nullopt;

  const int64_t delta = c0.timestamp_ns - c1.timestamp_ns;
  if (std::llabs(delta) > max_stereo_delta_ns) return std::nullopt;

  capture_client::StereoPair pair;
  pair.timestamp_ns = std::max(c0.timestamp_ns, c1.timestamp_ns);
  pair.cam0 = std::move(c0);
  pair.cam1 = std::move(c1);
  return pair;
}

std::optional<SelectedStereoPair> read_latest_stereo_near_timestamp(
    capture_client::ICaptureTransport& capture,
    int64_t target_timestamp_ns,
    uint64_t max_scan_back,
    int64_t max_stereo_delta_ns) {
  const uint64_t latest0 = capture.cam0().latest_sequence();
  const uint64_t latest1 = capture.cam1().latest_sequence();
  const uint64_t latest = std::min(latest0, latest1);
  if (latest == 0) return std::nullopt;

  const uint64_t scan_back = std::max<uint64_t>(1, max_scan_back);
  const uint64_t min_seq = latest > scan_back ? latest - scan_back + 1 : 1;

  std::optional<SelectedStereoPair> best;
  int64_t best_age_ns = LLONG_MAX;
  for (uint64_t seq = latest; seq >= min_seq; --seq) {
    auto pair = read_stereo_pair_sequence(capture, seq, max_stereo_delta_ns);
    if (pair) {
      const int64_t age_ns = std::llabs(pair->timestamp_ns - target_timestamp_ns);
      if (age_ns < best_age_ns) {
        best_age_ns = age_ns;
        SelectedStereoPair selected;
        selected.sequence = seq;
        selected.age_to_target_ns = age_ns;
        selected.pair = std::move(*pair);
        best = std::move(selected);
      }
    }
    if (seq == min_seq) break;
  }
  return best;
}

std::optional<SelectedStereoPair> read_latest_stereo_pair(
    capture_client::ICaptureTransport& capture,
    int64_t max_stereo_delta_ns) {
  const uint64_t latest0 = capture.cam0().latest_sequence();
  const uint64_t latest1 = capture.cam1().latest_sequence();
  const uint64_t latest = std::min(latest0, latest1);
  if (latest == 0) return std::nullopt;

  auto pair = read_stereo_pair_sequence(capture, latest, max_stereo_delta_ns);
  if (!pair) return std::nullopt;

  SelectedStereoPair selected;
  selected.sequence = latest;
  selected.age_to_target_ns = 0;
  selected.pair = std::move(*pair);
  return selected;
}

xr_runtime::HmdPoseF64V1 identity_pose_sample(uint64_t timestamp_ns) {
  xr_runtime::HmdPoseF64V1 pose{};
  pose.timestamp_ns = timestamp_ns;
  pose.qw = 1.0;
  pose.qx = 0.0;
  pose.qy = 0.0;
  pose.qz = 0.0;
  pose.px = 0.0;
  pose.py = 0.0;
  pose.pz = 0.0;
  pose.flags = 0;
  return pose;
}


void save_cloud_ply(const fs::path& path, const std::vector<xr_spatial_backend::Vec3f>& points) {
  if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
  std::ofstream os(path);
  if (!os) throw std::runtime_error("failed to write debug PLY: " + path.string());
  os << "ply\nformat ascii 1.0\n";
  os << "element vertex " << points.size() << "\n";
  os << "property float x\nproperty float y\nproperty float z\n";
  os << "end_header\n";
  for (const auto& p : points) {
    os << p.x << ' ' << p.y << ' ' << p.z << '\n';
  }
}

std::vector<xr_spatial_backend::Vec3f> transform_points(
    const std::vector<xr_spatial_backend::Vec3f>& src,
    const xr_spatial_backend::Transform3d& T) {
  std::vector<xr_spatial_backend::Vec3f> out;
  out.reserve(src.size());
  for (const auto& p : src) out.push_back(xr_spatial_backend::transform_point(T, p));
  return out;
}

struct QualityGateConfig {
  bool enabled = true;
  size_t min_raw_points = 1000;
  size_t min_filtered_points = 500;
  float far_depth_m = 0.0f; // <=0 disables far fraction check.
  float max_far_point_fraction = 1.0f;
  double max_pose_delta_m = 0.0;   // <=0 disables translation step gate.
  double max_pose_delta_deg = 0.0; // <=0 disables rotation step gate.
  double reinit_delta_m = 1.0;    // Large jump that likely means pose-source reinit; <=0 disables.
  double reinit_delta_deg = 45.0; // Large rotation jump that likely means pose-source reinit; <=0 disables.
  bool write_pose_health_csv = true;
  std::string pose_health_csv_name = "pose_health.csv";
  bool write_csv = true;
  std::string csv_name = "quality_gate.csv";
};

struct AppConfig {
  std::string mode = "runtime"; // runtime|scan

  std::string capture_registry = "/tmp/capture_service_streams.json";
  std::string camera0_stream = "camera0";
  std::string camera1_stream = "camera1";
  std::string imu_stream = "imu0";

  std::string pose_input = "shm"; // shm|none
  std::string pose_registry = "/tmp/tracking_streams.json";
  std::string pose_stream = "hmd_pose";
  double max_pose_age_ms = 120.0;
  double pose_wait_timeout_sec = 0.0; // <=0 waits forever when pose_input=shm.
  int pose_retry_interval_ms = 500;
  double pose_reattach_on_stale_ms = 500.0; // <=0 disables SHM reattach-on-stale.
  bool drop_stale_pose = true;
  uint64_t stereo_pose_sync_scan_back = 16;

  std::string calibration_path;
  std::string calib_profile_name = "unknown";
  std::string pose_frame_id = "tracking_pose";
  std::string map_frame_id = "tracking_world";
  double fisheye_balance = 0.0;
  bool zero_disparity = true;

  xr_spatial_backend::StereoDepthConfig depth;
  size_t max_accumulated_points = 1000000;
  float max_abs_map_coord_m = 100.0f;
  std::string tracking_origin = "first_pose"; // first_pose|absolute

  std::string mapper_backend = "live_depth_grid"; // live_depth_grid|pointcloud_fallback
  xr_spatial_backend::PointCloudFilterConfig point_filter;
  QualityGateConfig quality_gate;
  xr_spatial_backend::SpatialProxyMeshBuildConfig proxy_mesh;
  bool publish_spatial_proxy_mesh_shm = false;
  std::string spatial_proxy_mesh_stream = "spatial_proxy_mesh";
  std::string spatial_proxy_mesh_shm_name = "spatial_proxy_mesh";
  uint32_t spatial_proxy_mesh_slots = 8;

  bool publish_runtime_spatial_shm = false;
  std::string runtime_registry = "/tmp/runtime_tracking_streams.json";
  std::string spatial_stream = "runtime_spatial_summary";
  std::string spatial_shm_name = "runtime_spatial_summary";

  double scan_duration_sec = 30.0;
  std::string scan_output_dir = "/tmp/xr_spatial_scan";
  bool reset_map_on_scan_start = true;
  bool exit_after_scan = false;
  bool save_pointcloud_ply = true;
  bool save_voxel_pointcloud_ply = true;
  float scan_voxel_size_m = 0.02f;
  uint32_t scan_min_observations = 2;
  size_t scan_max_voxel_points = 200000;
  bool save_trajectory_csv = true;
  bool save_metadata_json = true;
  bool save_debug_clouds = true;
  std::string map_frame = "tracking"; // tracking|camera

  int print_every = 30;
};

void save_metadata(const fs::path& path,
                   const AppConfig& cfg,
                   const xr_spatial_backend::StereoCalibration& calib,
                   const xr_spatial_backend::PointCloudSpatialMap& map,
                   int64_t started_ns,
                   int64_t ended_ns) {
  if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
  nlohmann::json j;
  j["mode"] = cfg.mode;
  j["duration_sec"] = double(ended_ns - started_ns) * 1e-9;
  j["capture_registry"] = cfg.capture_registry;
  j["camera0_stream"] = cfg.camera0_stream;
  j["camera1_stream"] = cfg.camera1_stream;
  j["pose_input"] = cfg.pose_input;
  j["pose_registry"] = cfg.pose_registry;
  j["pose_stream"] = cfg.pose_stream;
  j["pose_reattach_on_stale_ms"] = cfg.pose_reattach_on_stale_ms;
  j["calibration_path"] = cfg.calibration_path;
  j["calib_profile_name"] = cfg.calib_profile_name;
  j["camera_resolution"] = {calib.cam0.width, calib.cam0.height};
  j["baseline_m"] = calib.baseline_m;
  j["depth_algorithm"] = "opencv_sgbm";
  j["depth_num_disparities"] = cfg.depth.num_disparities;
  j["depth_block_size"] = cfg.depth.block_size;
  j["depth_rate_hz"] = cfg.depth.target_rate_hz;
  j["depth_min_m"] = cfg.depth.min_depth_m;
  j["depth_max_m"] = cfg.depth.max_depth_m;
  j["point_decimation"] = cfg.depth.point_decimation;
  j["max_abs_camera_coord_m"] = cfg.depth.max_abs_camera_coord_m;
  j["max_abs_map_coord_m"] = cfg.max_abs_map_coord_m;
  j["tracking_origin"] = cfg.tracking_origin;
  j["point_filter_voxel_size_m"] = cfg.point_filter.voxel_size_m;
  j["point_filter_max_points_per_frame"] = cfg.point_filter.max_points;
  j["depth_roi_x_min"] = cfg.depth.roi_x_min;
  j["depth_roi_x_max"] = cfg.depth.roi_x_max;
  j["depth_roi_y_min"] = cfg.depth.roi_y_min;
  j["depth_roi_y_max"] = cfg.depth.roi_y_max;
  j["quality_gate_enabled"] = cfg.quality_gate.enabled;
  j["quality_min_raw_points"] = cfg.quality_gate.min_raw_points;
  j["quality_min_filtered_points"] = cfg.quality_gate.min_filtered_points;
  j["quality_far_depth_m"] = cfg.quality_gate.far_depth_m;
  j["quality_max_far_point_fraction"] = cfg.quality_gate.max_far_point_fraction;
  j["quality_max_pose_delta_m"] = cfg.quality_gate.max_pose_delta_m;
  j["quality_max_pose_delta_deg"] = cfg.quality_gate.max_pose_delta_deg;
  j["quality_reinit_delta_m"] = cfg.quality_gate.reinit_delta_m;
  j["quality_reinit_delta_deg"] = cfg.quality_gate.reinit_delta_deg;
  j["quality_pose_health_csv_name"] = cfg.quality_gate.pose_health_csv_name;
  j["quality_csv_name"] = cfg.quality_gate.csv_name;
  j["mapper"] = cfg.mapper_backend;
  j["spatial_proxy_mesh_enabled"] = cfg.proxy_mesh.enabled;
  j["spatial_proxy_mesh_rate_hz"] = cfg.proxy_mesh.rate_hz;
  j["spatial_proxy_mesh_voxel_size_m"] = cfg.proxy_mesh.voxel_size_m;
  j["spatial_proxy_mesh_max_distance_m"] = cfg.proxy_mesh.max_distance_m;
  j["spatial_proxy_mesh_max_vertices"] = cfg.proxy_mesh.max_vertices;
  j["spatial_proxy_mesh_max_triangles"] = cfg.proxy_mesh.max_triangles;
  j["spatial_proxy_mesh_live_grid_triangles_enabled"] = cfg.proxy_mesh.live_grid_triangles_enabled;
  j["spatial_proxy_mesh_live_grid_max_edge_m"] = cfg.proxy_mesh.live_grid_max_edge_m;
  j["spatial_proxy_mesh_live_grid_max_depth_jump_m"] = cfg.proxy_mesh.live_grid_max_depth_jump_m;
  j["scanner_backend_only"] = true;
  j["scan_save_voxel_pointcloud_ply"] = cfg.save_voxel_pointcloud_ply;
  j["scan_voxel_size_m"] = cfg.scan_voxel_size_m;
  j["scan_min_observations"] = cfg.scan_min_observations;
  j["scan_max_voxel_points"] = cfg.scan_max_voxel_points;
  j["integrated_frames"] = map.integrated_frames();
  j["dropped_frames"] = map.dropped_frames();
  j["accumulated_points"] = map.points_tracking().size();
  j["map_frame"] = cfg.map_frame_id;
  j["depth_frame"] = cfg.depth.depth_frame_id;
  j["pose_frame"] = cfg.pose_frame_id;
  j["map_frame_mode"] = cfg.map_frame;
  std::ofstream os(path);
  if (!os) throw std::runtime_error("failed to write metadata: " + path.string());
  os << j.dump(2) << "\n";
}


double pose_translation_delta_m(const xr_runtime::HmdPoseF64V1& a,
                                const xr_runtime::HmdPoseF64V1& b) {
  const double dx = a.px - b.px;
  const double dy = a.py - b.py;
  const double dz = a.pz - b.pz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double pose_rotation_delta_deg(const xr_runtime::HmdPoseF64V1& a,
                               const xr_runtime::HmdPoseF64V1& b) {
  double dot = a.qw * b.qw + a.qx * b.qx + a.qy * b.qy + a.qz * b.qz;
  dot = std::abs(dot);
  if (dot > 1.0) dot = 1.0;
  constexpr double kRadToDeg = 57.29577951308232;
  return 2.0 * std::acos(dot) * kRadToDeg;
}

size_t count_far_points(const xr_spatial_backend::PointCloudFrame& cloud, float far_depth_m) {
  if (far_depth_m <= 0.0f) return 0;
  size_t n = 0;
  for (const auto& p : cloud.points) {
    if (p.z > far_depth_m) ++n;
  }
  return n;
}

struct PoseReaderState {
  std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>> reader;
  uint64_t reattach_attempts = 0;
  uint64_t reattach_successes = 0;
  int64_t next_reattach_ns = 0;
  std::string last_error;
};

bool attach_pose_reader_once(PoseReaderState& state,
                             const AppConfig& cfg,
                             const std::string& reason,
                             bool wait_at_startup) {
  ++state.reattach_attempts;
  try {
    xr_runtime::StreamInfo info;
    if (wait_at_startup) {
      info = wait_for_stream_info(cfg.pose_registry,
                                  cfg.pose_stream,
                                  "pose",
                                  cfg.pose_wait_timeout_sec,
                                  cfg.pose_retry_interval_ms);
    } else {
      info = xr_runtime::stream_info_from_registry(cfg.pose_registry, cfg.pose_stream);
    }
    state.reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>>(
        info, xr_runtime::HMD_POSE_FORMAT_NAME);
    ++state.reattach_successes;
    state.last_error.clear();
    std::cout << "[xr_spatial_backend] pose stream attached reason=" << reason
              << " stream=" << cfg.pose_stream
              << " frame=" << info.frame_id
              << " shm=" << info.shm_name
              << " attempts=" << state.reattach_attempts
              << " successes=" << state.reattach_successes << "\n";
    return true;
  } catch (const std::exception& e) {
    state.last_error = e.what();
    state.next_reattach_ns = now_ns() +
        static_cast<int64_t>(std::max(50, cfg.pose_retry_interval_ms)) * 1000000LL;
    std::cerr << "[xr_spatial_backend] WARN: failed to attach pose stream reason="
              << reason << " registry=" << cfg.pose_registry
              << " stream=" << cfg.pose_stream
              << " error=" << state.last_error << "\n";
    return false;
  }
}

bool maybe_reattach_pose_reader(PoseReaderState& state,
                                const AppConfig& cfg,
                                const std::string& reason) {
  const int64_t now = now_ns();
  if (state.next_reattach_ns > 0 && now < state.next_reattach_ns) return false;
  state.next_reattach_ns = now +
      static_cast<int64_t>(std::max(50, cfg.pose_retry_interval_ms)) * 1000000LL;
  return attach_pose_reader_once(state, cfg, reason, false);
}


constexpr uint32_t kSpatialMeshKindLiveDepthGrid = 2;

float point_distance_m(const xr_spatial::RuntimeSpatialProxyVertexF32V1& a,
                       const xr_spatial::RuntimeSpatialProxyVertexF32V1& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool live_grid_vertex_finite(const xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh, uint32_t i) {
  if (i >= mesh.vertex_count || i >= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES) return false;
  const auto& v = mesh.vertices[i];
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool live_grid_edge_ok(const xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh,
                       const xr_spatial_backend::StereoDepthResult& depth,
                       uint32_t a,
                       uint32_t b,
                       float max_edge_m,
                       float max_depth_jump_m) {
  if (!live_grid_vertex_finite(mesh, a) || !live_grid_vertex_finite(mesh, b)) return false;
  if (max_edge_m > 0.0f && point_distance_m(mesh.vertices[a], mesh.vertices[b]) > max_edge_m) return false;
  if (max_depth_jump_m > 0.0f &&
      a < depth.live_grid_points.size() && b < depth.live_grid_points.size()) {
    const float za = depth.live_grid_points[a].z;
    const float zb = depth.live_grid_points[b].z;
    if (std::isfinite(za) && std::isfinite(zb) && std::abs(za - zb) > max_depth_jump_m) return false;
  }
  return true;
}

bool live_grid_triangle_ok(const xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh,
                           const xr_spatial_backend::StereoDepthResult& depth,
                           uint32_t a,
                           uint32_t b,
                           uint32_t c,
                           float max_edge_m,
                           float max_depth_jump_m) {
  return live_grid_edge_ok(mesh, depth, a, b, max_edge_m, max_depth_jump_m) &&
         live_grid_edge_ok(mesh, depth, b, c, max_edge_m, max_depth_jump_m) &&
         live_grid_edge_ok(mesh, depth, c, a, max_edge_m, max_depth_jump_m);
}

void add_live_grid_triangle(xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh,
                            uint32_t a,
                            uint32_t b,
                            uint32_t c) {
  if (mesh.triangle_count >= mesh.max_triangles ||
      mesh.triangle_count >= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES) return;
  mesh.triangles[mesh.triangle_count++] = {
      static_cast<uint16_t>(a), static_cast<uint16_t>(b), static_cast<uint16_t>(c)};
}

xr_spatial::RuntimeSpatialProxyMeshF32V1 make_clear_live_depth_grid_payload(
    uint64_t sequence,
    uint64_t source_timestamp_ns,
    uint64_t pose_timestamp_ns) {
  xr_spatial::RuntimeSpatialProxyMeshF32V1 out{};
  out.sequence = sequence;
  out.source_timestamp_ns = source_timestamp_ns;
  out.pose_timestamp_ns = pose_timestamp_ns;
  out.mesh_kind = kSpatialMeshKindLiveDepthGrid;
  out.vertex_count = 0;
  out.triangle_count = 0;
  out.valid_point_count = 0;
  out.status_flags = xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_UPDATED |
                     xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_LIVE_DEPTH_GRID |
                     xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ORGANIZED_GRID |
                     xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_CLEAR_FRAME;
  out.confidence = 0.0f;
  return out;
}

xr_spatial::RuntimeSpatialProxyMeshF32V1 make_live_depth_grid_payload(
    const xr_spatial_backend::StereoDepthResult& depth,
    const xr_spatial_backend::Transform3d& T_map_cloud,
    uint64_t sequence,
    uint64_t source_timestamp_ns,
    uint64_t pose_timestamp_ns,
    const xr_spatial_backend::SpatialProxyMeshBuildConfig& cfg) {
  xr_spatial::RuntimeSpatialProxyMeshF32V1 out{};
  out.sequence = sequence;
  out.source_timestamp_ns = source_timestamp_ns;
  out.pose_timestamp_ns = pose_timestamp_ns;
  out.mesh_kind = kSpatialMeshKindLiveDepthGrid;
  out.grid_width = depth.live_grid_width;
  out.grid_height = depth.live_grid_height;
  out.grid_decimation = depth.live_grid_decimation;
  out.max_vertices = std::min<uint32_t>(cfg.max_vertices, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  out.max_triangles = std::min<uint32_t>(cfg.max_triangles, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
  out.triangle_count = 0;
  out.voxel_size_m = 0.0f;
  out.max_distance_m = depth.cloud.max_depth_m;

  const uint32_t total = static_cast<uint32_t>(std::min<size_t>(
      depth.live_grid_points.size(), out.max_vertices));
  out.vertex_count = total;

  const float nan = std::numeric_limits<float>::quiet_NaN();
  bool have_bbox = false;
  float minx = 0.0f, miny = 0.0f, minz = 0.0f, maxx = 0.0f, maxy = 0.0f, maxz = 0.0f;
  uint32_t valid = 0;
  for (uint32_t i = 0; i < total; ++i) {
    if (i >= depth.live_grid_valid.size() || !depth.live_grid_valid[i]) {
      out.vertices[i] = {nan, nan, nan};
      continue;
    }
    const auto q = xr_spatial_backend::transform_point(T_map_cloud, depth.live_grid_points[i]);
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
      out.vertices[i] = {nan, nan, nan};
      continue;
    }
    out.vertices[i] = {q.x, q.y, q.z};
    ++valid;
    if (!have_bbox) {
      minx = maxx = q.x; miny = maxy = q.y; minz = maxz = q.z; have_bbox = true;
    } else {
      minx = std::min(minx, q.x); maxx = std::max(maxx, q.x);
      miny = std::min(miny, q.y); maxy = std::max(maxy, q.y);
      minz = std::min(minz, q.z); maxz = std::max(maxz, q.z);
    }
  }
  if (cfg.live_grid_triangles_enabled && out.max_triangles > 0 &&
      out.grid_width > 1 && out.grid_height > 1 &&
      static_cast<uint64_t>(out.grid_width) * static_cast<uint64_t>(out.grid_height) <= out.vertex_count) {
    auto idx = [&](uint32_t x, uint32_t y) -> uint32_t { return y * out.grid_width + x; };
    for (uint32_t y = 0; y + 1 < out.grid_height; ++y) {
      for (uint32_t x = 0; x + 1 < out.grid_width; ++x) {
        const uint32_t i00 = idx(x, y);
        const uint32_t i10 = idx(x + 1, y);
        const uint32_t i01 = idx(x, y + 1);
        const uint32_t i11 = idx(x + 1, y + 1);
        if (live_grid_triangle_ok(out, depth, i00, i10, i01,
                                  cfg.live_grid_max_edge_m, cfg.live_grid_max_depth_jump_m)) {
          add_live_grid_triangle(out, i00, i10, i01);
        }
        if (live_grid_triangle_ok(out, depth, i10, i11, i01,
                                  cfg.live_grid_max_edge_m, cfg.live_grid_max_depth_jump_m)) {
          add_live_grid_triangle(out, i10, i11, i01);
        }
        if (out.triangle_count >= out.max_triangles ||
            out.triangle_count >= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES) break;
      }
      if (out.triangle_count >= out.max_triangles ||
          out.triangle_count >= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES) break;
    }
  }

  out.valid_point_count = valid;
  if (have_bbox) {
    out.bbox_min_x = minx; out.bbox_min_y = miny; out.bbox_min_z = minz;
    out.bbox_max_x = maxx; out.bbox_max_y = maxy; out.bbox_max_z = maxz;
  }
  out.confidence = total > 0 ? static_cast<float>(static_cast<double>(valid) / static_cast<double>(total)) : 0.0f;
  out.status_flags = xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_UPDATED |
                     xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_LIVE_DEPTH_GRID |
                     xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ORGANIZED_GRID;
  if (valid > 0) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ACTIVE;
  else out.status_flags |= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_CLEAR_FRAME;
  if (out.triangle_count > 0) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_HAS_TRIANGLES;
  return out;
}


void write_pose_health_header(std::ofstream& os) {
  os << "frame_timestamp_ns,pose_timestamp_ns,pose_age_ms,pose_delta_m,pose_delta_deg,"
        "stale_pose,reinit_suspected,action\n";
}

void write_pose_health_row(std::ofstream& os,
                           int64_t frame_ts,
                           const xr_runtime::HmdPoseF64V1& pose,
                           double pose_age_ms,
                           double pose_delta_m,
                           double pose_delta_deg,
                           bool stale_pose,
                           bool reinit_suspected,
                           const std::string& action) {
  os << frame_ts << ','
     << pose.timestamp_ns << ','
     << pose_age_ms << ','
     << pose_delta_m << ','
     << pose_delta_deg << ','
     << (stale_pose ? 1 : 0) << ','
     << (reinit_suspected ? 1 : 0) << ','
     << action << '\n';
}

void write_quality_gate_header(std::ofstream& os) {
  os << "frame_timestamp_ns,pose_timestamp_ns,accepted,reject_reason,pose_age_ms,"
        "raw_points,filtered_points,candidate_pixels,accepted_pixel_fraction,"
        "rejected_roi,rejected_disparity,rejected_nonfinite,rejected_depth,rejected_abs_coord,"
        "far_depth_m,far_points,far_fraction,pose_delta_m,pose_delta_deg,"
        "min_depth_m,mean_depth_m,max_depth_m\n";
}

void write_quality_gate_row(std::ofstream& os,
                            int64_t frame_ts,
                            const xr_runtime::HmdPoseF64V1& pose,
                            bool accepted,
                            const std::string& reason,
                            double pose_age_ms,
                            size_t raw_points,
                            size_t filtered_points,
                            const xr_spatial_backend::StereoDepthResult& depth,
                            float far_depth_m,
                            size_t far_points,
                            double far_fraction,
                            double pose_delta_m,
                            double pose_delta_deg) {
  os << frame_ts << ','
     << pose.timestamp_ns << ','
     << (accepted ? 1 : 0) << ','
     << reason << ','
     << pose_age_ms << ','
     << raw_points << ','
     << filtered_points << ','
     << depth.candidate_pixels << ','
     << depth.accepted_pixel_fraction << ','
     << depth.rejected_roi_pixels << ','
     << depth.rejected_disparity_pixels << ','
     << depth.rejected_nonfinite_points << ','
     << depth.rejected_depth_range_points << ','
     << depth.rejected_abs_coord_points << ','
     << far_depth_m << ','
     << far_points << ','
     << far_fraction << ','
     << pose_delta_m << ','
     << pose_delta_deg << ','
     << depth.cloud.min_depth_m << ','
     << depth.cloud.mean_depth_m << ','
     << depth.cloud.max_depth_m << '\n';
}

void write_trajectory_header(std::ofstream& os) {
  os << "frame_timestamp_ns,pose_timestamp_ns,pose_age_ms,px,py,pz,qw,qx,qy,qz,cloud_points\n";
}

void write_trajectory_row(std::ofstream& os,
                          int64_t frame_ts,
                          const xr_runtime::HmdPoseF64V1& pose,
                          double age_ms,
                          size_t cloud_points) {
  os << frame_ts << ','
     << pose.timestamp_ns << ','
     << age_ms << ','
     << pose.px << ',' << pose.py << ',' << pose.pz << ','
     << pose.qw << ',' << pose.qx << ',' << pose.qy << ',' << pose.qz << ','
     << cloud_points << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  AppConfig cfg;

  CLI::App app{"XR spatial backend: stereo depth + spatial map scan/runtime publisher"};
  app.add_option("--mode", cfg.mode, "Mode: runtime or scan")->check(CLI::IsMember({"runtime", "scan"}));

  app.add_option("--capture-registry", cfg.capture_registry, "capture_service SHM registry path");
  app.add_option("--camera0-stream", cfg.camera0_stream, "Left/cam0 stream id");
  app.add_option("--camera1-stream", cfg.camera1_stream, "Right/cam1 stream id");
  app.add_option("--imu-stream", cfg.imu_stream, "IMU stream id required by current capture SHM transport");

  app.add_option("--pose-input", cfg.pose_input, "Pose input mode: shm or none")->check(CLI::IsMember({"shm", "none"}));
  app.add_option("--pose-registry", cfg.pose_registry, "Tracking pose SHM registry path when --pose-input=shm");
  app.add_option("--pose-stream", cfg.pose_stream, "Tracking pose stream id when --pose-input=shm");
  app.add_option("--pose-frame-id", cfg.pose_frame_id, "Pose stream frame id label stored in metadata");
  app.add_option("--pose-wait-timeout-sec", cfg.pose_wait_timeout_sec, "Wait this many seconds for pose registry/stream at startup; <=0 waits forever when --pose-input=shm");
  app.add_option("--pose-retry-interval-ms", cfg.pose_retry_interval_ms, "Retry interval while waiting for or reattaching pose registry/stream");
  app.add_option("--pose-reattach-on-stale-ms", cfg.pose_reattach_on_stale_ms, "Reattach pose SHM reader when latest pose sample is stale by this wall-clock age; <=0 disables");
  app.add_option("--max-pose-age-ms", cfg.max_pose_age_ms, "Maximum stereo-frame-to-pose timestamp delta before pose is stale");
  app.add_flag("--drop-stale-pose,!--no-drop-stale-pose", cfg.drop_stale_pose, "Drop frames whose pose timestamp is stale");
  app.add_option("--stereo-pose-sync-scan-back", cfg.stereo_pose_sync_scan_back, "Scan back this many latest stereo pairs and choose the one closest to the pose timestamp");

  app.add_option("--calib", cfg.calibration_path, "Stereo calibration JSON path")->required();
  app.add_option("--calib-profile-name", cfg.calib_profile_name, "Calibration/profile label stored in scan metadata");
  app.add_option("--depth-frame-id", cfg.depth.depth_frame_id, "Frame id label for generated depth/pointcloud samples");
  app.add_option("--map-frame-id", cfg.map_frame_id, "Map/world frame id label stored in metadata and runtime summary");
  app.add_option("--fisheye-balance", cfg.fisheye_balance, "OpenCV fisheye rectification balance");
  app.add_flag("--zero-disparity,!--no-zero-disparity", cfg.zero_disparity, "Use CALIB_ZERO_DISPARITY in stereoRectify");

  app.add_option("--depth-rate-hz", cfg.depth.target_rate_hz, "Target stereo depth processing rate");
  app.add_option("--num-disparities", cfg.depth.num_disparities, "SGBM num disparities, rounded to multiple of 16");
  app.add_option("--block-size", cfg.depth.block_size, "SGBM block size, odd >=3");
  app.add_option("--uniqueness-ratio", cfg.depth.uniqueness_ratio, "SGBM uniqueness ratio");
  app.add_option("--speckle-window-size", cfg.depth.speckle_window_size, "SGBM speckle window size");
  app.add_option("--speckle-range", cfg.depth.speckle_range, "SGBM speckle range");
  app.add_option("--point-decimation", cfg.depth.point_decimation, "Use every Nth pixel for point cloud");
  app.add_option("--min-depth-m", cfg.depth.min_depth_m, "Minimum accepted depth");
  app.add_option("--max-depth-m", cfg.depth.max_depth_m, "Maximum accepted depth");
  app.add_option("--max-abs-camera-coordinate-m", cfg.depth.max_abs_camera_coord_m, "Reject stereo points with any camera-frame coordinate above this absolute value; <=0 disables");
  app.add_option("--depth-roi-x-min", cfg.depth.roi_x_min, "Normalized left ROI bound for depth point extraction");
  app.add_option("--depth-roi-x-max", cfg.depth.roi_x_max, "Normalized right ROI bound for depth point extraction");
  app.add_option("--depth-roi-y-min", cfg.depth.roi_y_min, "Normalized top ROI bound for depth point extraction");
  app.add_option("--depth-roi-y-max", cfg.depth.roi_y_max, "Normalized bottom ROI bound for depth point extraction");
  app.add_option("--max-accumulated-points", cfg.max_accumulated_points, "Pointcloud fallback accumulation cap");
  app.add_option("--max-abs-map-coordinate-m", cfg.max_abs_map_coord_m, "Reject transformed map-frame points with any coordinate above this absolute value; <=0 disables");
  app.add_option("--map-frame", cfg.map_frame, "Fallback accumulation frame: tracking or camera")->check(CLI::IsMember({"tracking", "camera"}));
  app.add_option("--tracking-origin", cfg.tracking_origin, "For --map-frame tracking: first_pose keeps scan-local coordinates; absolute stores raw tracking coordinates")->check(CLI::IsMember({"first_pose", "absolute"}));
  app.add_option("--mapper-backend", cfg.mapper_backend, "Spatial mapper backend: live_depth_grid or pointcloud_fallback")->check(CLI::IsMember({"live_depth_grid", "pointcloud_fallback"}));
  app.add_option("--point-voxel-size-m", cfg.point_filter.voxel_size_m, "Per-frame voxel downsample size before accumulation; <=0 disables");
  app.add_option("--max-points-per-frame", cfg.point_filter.max_points, "Per-frame point cap after filtering; 0 disables");
  app.add_flag("--quality-gate-enabled,!--no-quality-gate-enabled", cfg.quality_gate.enabled, "Enable frame quality gate before map/TSDF integration");
  app.add_option("--quality-min-raw-points", cfg.quality_gate.min_raw_points, "Reject frame if raw depth cloud has fewer points; 0 disables");
  app.add_option("--quality-min-filtered-points", cfg.quality_gate.min_filtered_points, "Reject frame if filtered depth cloud has fewer points; 0 disables");
  app.add_option("--quality-far-depth-m", cfg.quality_gate.far_depth_m, "Depth threshold for far-point fraction check; <=0 disables");
  app.add_option("--quality-max-far-point-fraction", cfg.quality_gate.max_far_point_fraction, "Reject frame when fraction of points beyond --quality-far-depth-m is above this value");
  app.add_option("--quality-max-pose-delta-m", cfg.quality_gate.max_pose_delta_m, "Reject frame if sampled-pose translation step is above this value; <=0 disables");
  app.add_option("--quality-max-pose-delta-deg", cfg.quality_gate.max_pose_delta_deg, "Reject frame if sampled-pose rotation step is above this value; <=0 disables");
  app.add_option("--quality-reinit-delta-m", cfg.quality_gate.reinit_delta_m, "Reject before depth compute if pose jump likely indicates pose-source reinit; <=0 disables");
  app.add_option("--quality-reinit-delta-deg", cfg.quality_gate.reinit_delta_deg, "Reject before depth compute if rotation jump likely indicates pose-source reinit; <=0 disables");
  app.add_flag("--quality-write-pose-health-csv,!--no-quality-write-pose-health-csv", cfg.quality_gate.write_pose_health_csv, "Write scan pose_health.csv with pose age/jump diagnostics");
  app.add_option("--quality-pose-health-csv-name", cfg.quality_gate.pose_health_csv_name, "Pose health CSV filename inside scan output dir");
  app.add_flag("--quality-write-csv,!--no-quality-write-csv", cfg.quality_gate.write_csv, "Write scan quality_gate.csv with per-frame accept/reject diagnostics");
  app.add_option("--quality-csv-name", cfg.quality_gate.csv_name, "Quality gate CSV filename inside scan output dir");
  app.add_flag("--save-debug-frames", cfg.depth.save_first_debug_frame, "Save first rectified/disparity debug images");
  app.add_option("--debug-dir", cfg.depth.debug_dir, "Debug image output directory");

  app.add_flag("--publish-runtime-spatial-shm", cfg.publish_runtime_spatial_shm, "Publish runtime_spatial_summary SHM stream");
  app.add_option("--runtime-registry", cfg.runtime_registry, "Runtime registry path for spatial summary");
  app.add_option("--spatial-stream", cfg.spatial_stream, "Runtime spatial summary stream id");
  app.add_option("--spatial-shm-name", cfg.spatial_shm_name, "Runtime spatial summary SHM name");

  app.add_flag("--publish-spatial-proxy-mesh-shm", cfg.publish_spatial_proxy_mesh_shm, "Publish low-poly spatial proxy mesh SHM stream");
  app.add_option("--spatial-proxy-mesh-stream", cfg.spatial_proxy_mesh_stream, "Spatial proxy mesh stream id");
  app.add_option("--spatial-proxy-mesh-shm-name", cfg.spatial_proxy_mesh_shm_name, "Spatial proxy mesh POSIX SHM name");
  app.add_option("--spatial-proxy-mesh-slots", cfg.spatial_proxy_mesh_slots, "Spatial proxy mesh output ring slot count");
  app.add_flag("--spatial-proxy-mesh-enabled,!--no-spatial-proxy-mesh-enabled", cfg.proxy_mesh.enabled, "Enable low-poly/voxel proxy mesh generation");
  app.add_option("--spatial-proxy-mesh-rate-hz", cfg.proxy_mesh.rate_hz, "Proxy mesh publish rate Hz; <=0 publishes every integrated frame");
  app.add_option("--spatial-proxy-mesh-voxel-size-m", cfg.proxy_mesh.voxel_size_m, "Proxy mesh voxel/cube size in metres");
  app.add_option("--spatial-proxy-mesh-max-distance-m", cfg.proxy_mesh.max_distance_m, "Keep proxy mesh points within this map-frame distance from origin; <=0 disables");
  app.add_option("--spatial-proxy-mesh-max-vertices", cfg.proxy_mesh.max_vertices, "Proxy mesh max vertices");
  app.add_option("--spatial-proxy-mesh-max-triangles", cfg.proxy_mesh.max_triangles, "Proxy mesh max triangles");
  app.add_option("--spatial-proxy-mesh-min-points-per-voxel", cfg.proxy_mesh.min_points_per_voxel, "Proxy mesh min source points per emitted voxel cube");
  app.add_flag("--spatial-live-grid-triangles-enabled,!--no-spatial-live-grid-triangles-enabled",
               cfg.proxy_mesh.live_grid_triangles_enabled,
               "Generate optional triangle index buffer from organized live depth grid");
  app.add_option("--spatial-live-grid-max-edge-m", cfg.proxy_mesh.live_grid_max_edge_m,
                 "Max world-space edge length for live depth grid triangles; <=0 disables edge length gate");
  app.add_option("--spatial-live-grid-max-depth-jump-m", cfg.proxy_mesh.live_grid_max_depth_jump_m,
                 "Max camera-depth jump for live depth grid triangles; <=0 disables depth jump gate");

  app.add_option("--scan-duration-sec", cfg.scan_duration_sec, "Scan duration for --mode scan");
  app.add_option("--scan-output-dir", cfg.scan_output_dir, "Directory for scan artifacts");
  app.add_flag("--reset-map-on-scan-start,!--no-reset-map-on-scan-start", cfg.reset_map_on_scan_start, "Reset map before scan");
  app.add_flag("--exit-after-scan", cfg.exit_after_scan, "Exit after scan finalization");
  app.add_flag("--save-pointcloud-ply,!--no-save-pointcloud-ply", cfg.save_pointcloud_ply, "Save accumulated pointcloud PLY");
  app.add_flag("--save-voxel-pointcloud-ply,!--no-save-voxel-pointcloud-ply", cfg.save_voxel_pointcloud_ply, "Save backend-only voxelized scan pointcloud PLY");
  app.add_option("--scan-voxel-size-m", cfg.scan_voxel_size_m, "Voxel size for backend-only scanner PLY");
  app.add_option("--scan-min-observations", cfg.scan_min_observations, "Minimum observations per output voxel in scanner PLY");
  app.add_option("--scan-max-voxel-points", cfg.scan_max_voxel_points, "Maximum output voxel points in scanner PLY; 0 disables cap");
  app.add_flag("--save-trajectory-csv,!--no-save-trajectory-csv", cfg.save_trajectory_csv, "Save scan trajectory CSV");
  app.add_flag("--save-metadata-json,!--no-save-metadata-json", cfg.save_metadata_json, "Save scan metadata JSON");
  app.add_flag("--save-debug-clouds,!--no-save-debug-clouds", cfg.save_debug_clouds, "Save first camera-frame and tracking-frame debug PLY clouds");
  app.add_option("--print-every", cfg.print_every, "Print every N integrated frames");

  CLI11_PARSE(app, argc, argv);

  try {
    std::cout << "[xr_spatial_backend] mode=" << cfg.mode << "\n";
    std::cout << "[xr_spatial_backend] capture_registry=" << cfg.capture_registry << "\n";
    std::cout << "[xr_spatial_backend] pose_input=" << cfg.pose_input
              << " registry=" << cfg.pose_registry
              << " stream=" << cfg.pose_stream
              << " reattach_on_stale_ms=" << cfg.pose_reattach_on_stale_ms << "\n";
    std::cout << "[xr_spatial_backend] calib=" << cfg.calibration_path << "\n";

    const bool pose_enabled = cfg.pose_input == "shm";
    if (!pose_enabled && cfg.map_frame != "camera") {
      throw std::runtime_error("--pose-input=none requires --map-frame=camera; use pose_input=shm for tracking/world-frame output");
    }
    if (!pose_enabled && cfg.mode == "scan" && cfg.save_trajectory_csv) {
      std::cout << "[xr_spatial_backend] WARN: --pose-input=none disables scan trajectory CSV; no tracked pose is available\n";
      cfg.save_trajectory_csv = false;
    }
    if (!pose_enabled && cfg.quality_gate.write_pose_health_csv) {
      cfg.quality_gate.write_pose_health_csv = false;
    }

    auto calib = xr_spatial_backend::load_stereo_calibration_json(cfg.calibration_path);
    xr_spatial_backend::compute_rectification(calib, cfg.fisheye_balance, cfg.zero_disparity);
    std::cout << "[xr_spatial_backend] calibration loaded: resolution="
              << calib.cam0.width << "x" << calib.cam0.height
              << " baseline_m=" << calib.baseline_m << "\n";

    capture_client::ShmCaptureTransport capture(cfg.capture_registry,
                                                cfg.camera0_stream,
                                                cfg.camera1_stream,
                                                cfg.imu_stream);
    // When pose_input=shm, integrate stereo frames near the pose timestamp instead of
    // blindly taking the newest camera frame. Pose often lags camera capture by one or
    // two 30 Hz frames, so selecting the newest frame can make every frame look stale.
    // When pose_input=none, publish camera-frame live depth grid/mesh without requiring
    // any tracking backend.
    PoseReaderState pose_state;
    if (pose_enabled) {
      attach_pose_reader_once(pose_state, cfg, "startup", true);
    }

    fs::path scan_dir(cfg.scan_output_dir);
    if (cfg.mode == "scan" && cfg.depth.save_first_debug_frame && cfg.depth.debug_dir.empty()) {
      cfg.depth.debug_dir = (scan_dir / "debug").string();
    }

    xr_spatial_backend::StereoDepthProcessor depth_processor(calib, cfg.depth);
    xr_spatial_backend::PointCloudSpatialMap map(cfg.max_accumulated_points, cfg.max_abs_map_coord_m);

    std::unique_ptr<xr_spatial::RuntimeSpatialSummaryShmPublisher> spatial_pub;
    std::unique_ptr<xr_spatial::RuntimeSpatialProxyMeshShmPublisher> spatial_proxy_mesh_pub;
    if (cfg.publish_runtime_spatial_shm) {
      xr_spatial::RuntimeSpatialSummaryShmPublisherConfig pcfg;
      pcfg.registry_path = cfg.runtime_registry;
      pcfg.stream_id = cfg.spatial_stream;
      pcfg.shm_name = cfg.spatial_shm_name;
      pcfg.frame_id = cfg.map_frame_id;
      spatial_pub = std::make_unique<xr_spatial::RuntimeSpatialSummaryShmPublisher>(pcfg);
      std::cout << "[xr_spatial_backend] publishing spatial summary SHM stream "
                << cfg.spatial_stream << " -> " << cfg.spatial_shm_name << "\n";
    }

    if (cfg.publish_spatial_proxy_mesh_shm) {
      xr_spatial::RuntimeSpatialProxyMeshShmPublisherConfig pcfg;
      pcfg.registry_path = cfg.runtime_registry;
      pcfg.stream_id = cfg.spatial_proxy_mesh_stream;
      pcfg.shm_name = cfg.spatial_proxy_mesh_shm_name;
      pcfg.frame_id = cfg.map_frame_id;
      pcfg.slot_count = cfg.spatial_proxy_mesh_slots;
      pcfg.created_by = "xr_spatial_backend";
      spatial_proxy_mesh_pub = std::make_unique<xr_spatial::RuntimeSpatialProxyMeshShmPublisher>(pcfg);
      std::cout << "[xr_spatial_backend] publishing spatial proxy mesh SHM stream "
                << cfg.spatial_proxy_mesh_stream << " -> " << cfg.spatial_proxy_mesh_shm_name
                << " frame=" << cfg.map_frame_id
                << " voxel=" << cfg.proxy_mesh.voxel_size_m
                << " max_vertices=" << cfg.proxy_mesh.max_vertices
                << " max_triangles=" << cfg.proxy_mesh.max_triangles << "\n";
    }

    std::ofstream traj;
    std::ofstream quality_csv;
    std::ofstream pose_health_csv;
    if (cfg.mode == "scan") {
      if (cfg.reset_map_on_scan_start) {
        map.reset();
      }
      fs::create_directories(scan_dir);
      if (cfg.save_trajectory_csv) {
        traj.open(scan_dir / "trajectory.csv");
        if (!traj) throw std::runtime_error("failed to open trajectory.csv in " + scan_dir.string());
        write_trajectory_header(traj);
      }
      if (cfg.quality_gate.write_csv) {
        quality_csv.open(scan_dir / cfg.quality_gate.csv_name);
        if (!quality_csv) throw std::runtime_error("failed to open quality CSV in " + scan_dir.string());
        write_quality_gate_header(quality_csv);
      }
      if (cfg.quality_gate.write_pose_health_csv) {
        pose_health_csv.open(scan_dir / cfg.quality_gate.pose_health_csv_name);
        if (!pose_health_csv) throw std::runtime_error("failed to open pose health CSV in " + scan_dir.string());
        write_pose_health_header(pose_health_csv);
      }
      std::cout << "[xr_spatial_backend] scan output dir=" << scan_dir << " duration_sec="
                << cfg.scan_duration_sec << "\n";
      std::cout << "[xr_spatial_backend] scan mode is backend-only: runtime spatial SHM="
                << (cfg.publish_runtime_spatial_shm ? "enabled" : "disabled")
                << " proxy mesh SHM=" << (cfg.publish_spatial_proxy_mesh_shm ? "enabled" : "disabled")
                << " mapper_backend=" << cfg.mapper_backend
                << " map_frame=" << cfg.map_frame << "\n";
      if (!pose_enabled || cfg.map_frame == "camera") {
        std::cerr << "[xr_spatial_backend] WARN: scan is running without tracking-world pose; "
                  << "outputs will be camera-local/debug only and will smear if accumulated across head motion\n";
      }
    }

    const int64_t start_ns = now_ns();
    int64_t last_depth_ns = 0;
    uint64_t summary_seq = 0;
    uint64_t proxy_mesh_seq = 0;
    int64_t last_proxy_mesh_publish_ns = 0;
    uint32_t last_proxy_mesh_vertices = 0;
    uint32_t last_proxy_mesh_triangles = 0;
    uint32_t integrated_since_start = 0;
    bool saved_first_debug_cloud = false;
    std::optional<xr_spatial_backend::Transform3d> T_map_tracking_origin;
    std::optional<xr_runtime::HmdPoseF64V1> last_integrated_pose;
    // Pose-step gate reference. Updated for every sampled pose, even when the frame is
    // rejected by depth/quality gates. This keeps pose_delta_m/deg as true
    // frame-to-frame motion instead of accumulated motion since the last integrated frame.
    std::optional<xr_runtime::HmdPoseF64V1> last_quality_pose;

    while (!g_stop) {
      if (cfg.mode == "scan") {
        const double elapsed = double(now_ns() - start_ns) * 1e-9;
        if (elapsed >= cfg.scan_duration_sec) break;
      }

      std::optional<xr_runtime::HmdPoseF64V1> sampled_pose;
      if (pose_enabled) {
        if (!pose_state.reader) {
          maybe_reattach_pose_reader(pose_state, cfg, "reader_missing");
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }

        sampled_pose = pose_state.reader->latest();
        if (!sampled_pose || (sampled_pose->flags & xr_runtime::HMD_FLAG_POSE_VALID) == 0u) {
          maybe_reattach_pose_reader(pose_state, cfg, "no_valid_pose_sample");
          std::cerr << "[xr_spatial_backend] WARN: no valid pose sample; dropping stereo frame"
                    << " reattach_attempts=" << pose_state.reattach_attempts
                    << " successes=" << pose_state.reattach_successes << "\n";
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }

        if (cfg.pose_reattach_on_stale_ms > 0.0) {
          const double wall_age_ms = std::abs(
              double(now_ns() - static_cast<int64_t>(sampled_pose->timestamp_ns))) / 1e6;
          if (wall_age_ms > cfg.pose_reattach_on_stale_ms) {
            maybe_reattach_pose_reader(pose_state, cfg, "stale_pose_wall_age");
            sampled_pose = pose_state.reader ? pose_state.reader->latest() : std::nullopt;
            if (!sampled_pose || (sampled_pose->flags & xr_runtime::HMD_FLAG_POSE_VALID) == 0u) {
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
              continue;
            }
          }
        }
      }

      std::optional<SelectedStereoPair> selected_pair;
      if (pose_enabled) {
        selected_pair = read_latest_stereo_near_timestamp(
            capture,
            static_cast<int64_t>(sampled_pose->timestamp_ns),
            cfg.stereo_pose_sync_scan_back,
            1'000'000);
      } else {
        selected_pair = read_latest_stereo_pair(capture, 1'000'000);
      }
      if (!selected_pair) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      auto& pair = selected_pair->pair;
      const xr_runtime::HmdPoseF64V1 pose_for_log = sampled_pose.value_or(
          identity_pose_sample(static_cast<uint64_t>(pair.timestamp_ns)));

      const int64_t min_interval_ns = cfg.depth.target_rate_hz > 0
                                          ? static_cast<int64_t>(1e9 / double(cfg.depth.target_rate_hz))
                                          : 0;
      if (min_interval_ns > 0 && last_depth_ns > 0 &&
          pair.timestamp_ns - last_depth_ns < min_interval_ns) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      last_depth_ns = pair.timestamp_ns;

      const uint64_t pose_timestamp_ns = pose_enabled && sampled_pose
          ? sampled_pose->timestamp_ns
          : 0u;
      const double pose_age_ms = pose_enabled && sampled_pose
          ? std::abs(double(pair.timestamp_ns - static_cast<int64_t>(sampled_pose->timestamp_ns))) / 1e6
          : 0.0;
      const double health_pose_delta_m = pose_enabled && last_quality_pose ? pose_translation_delta_m(*last_quality_pose, *sampled_pose) : 0.0;
      const double health_pose_delta_deg = pose_enabled && last_quality_pose ? pose_rotation_delta_deg(*last_quality_pose, *sampled_pose) : 0.0;
      const bool stale_pose = pose_enabled && cfg.max_pose_age_ms > 0.0 && pose_age_ms > cfg.max_pose_age_ms;
      const bool pose_reinit_suspected = pose_enabled && last_quality_pose &&
          ((cfg.quality_gate.reinit_delta_m > 0.0 && health_pose_delta_m > cfg.quality_gate.reinit_delta_m) ||
           (cfg.quality_gate.reinit_delta_deg > 0.0 && health_pose_delta_deg > cfg.quality_gate.reinit_delta_deg));

      std::string pose_action = "continue";
      if (stale_pose && cfg.drop_stale_pose) {
        pose_action = "drop_stale_pose";
      } else if (pose_enabled && cfg.quality_gate.enabled && pose_reinit_suspected) {
        pose_action = "drop_pose_reinit_suspected";
      }
      if (pose_health_csv) {
        write_pose_health_row(pose_health_csv, pair.timestamp_ns, pose_for_log, pose_age_ms,
                              health_pose_delta_m, health_pose_delta_deg,
                              stale_pose, pose_reinit_suspected, pose_action);
      }

      if (stale_pose && cfg.drop_stale_pose) {
        map.drop_frame();
        std::cerr << "[xr_spatial_backend] WARN: stale pose age_ms=" << pose_age_ms
                  << " scan_back=" << cfg.stereo_pose_sync_scan_back
                  << "; dropping stereo frame\n";
        if (sampled_pose) last_quality_pose = *sampled_pose;
        continue;
      }
      if (pose_enabled && cfg.quality_gate.enabled && pose_reinit_suspected) {
        map.drop_frame();
        std::cerr << "[xr_spatial_backend] WARN: pose-source reinit suspected; dropping frame"
                  << " pose_delta_m=" << health_pose_delta_m
                  << " pose_delta_deg=" << health_pose_delta_deg << "\n";
        if (sampled_pose) last_quality_pose = *sampled_pose;
        continue;
      }

      auto depth = depth_processor.compute(pair.cam0, pair.cam1);
      const size_t raw_points = depth.cloud.points.size();
      xr_spatial_backend::PointCloudFilterStats filter_stats;
      depth.cloud = xr_spatial_backend::filter_pointcloud(depth.cloud, cfg.point_filter, &filter_stats);
      const size_t filtered_points = depth.cloud.points.size();
      const size_t far_points = count_far_points(depth.cloud, cfg.quality_gate.far_depth_m);
      const double far_fraction = filtered_points > 0
                                      ? static_cast<double>(far_points) / static_cast<double>(filtered_points)
                                      : 0.0;
      const double pose_delta_m = pose_enabled && last_quality_pose ? pose_translation_delta_m(*last_quality_pose, *sampled_pose) : 0.0;
      const double pose_delta_deg = pose_enabled && last_quality_pose ? pose_rotation_delta_deg(*last_quality_pose, *sampled_pose) : 0.0;

      bool quality_accept = true;
      std::string quality_reason = "accepted";
      if (cfg.quality_gate.enabled) {
        if (cfg.quality_gate.min_raw_points > 0 && raw_points < cfg.quality_gate.min_raw_points) {
          quality_accept = false;
          quality_reason = "raw_points_low";
        } else if (cfg.quality_gate.min_filtered_points > 0 &&
                   filtered_points < cfg.quality_gate.min_filtered_points) {
          quality_accept = false;
          quality_reason = "filtered_points_low";
        } else if (cfg.quality_gate.far_depth_m > 0.0f &&
                   far_fraction > static_cast<double>(cfg.quality_gate.max_far_point_fraction)) {
          quality_accept = false;
          quality_reason = "far_fraction_high";
        } else if (pose_enabled && last_quality_pose && cfg.quality_gate.max_pose_delta_m > 0.0 &&
                   pose_delta_m > cfg.quality_gate.max_pose_delta_m) {
          quality_accept = false;
          quality_reason = "pose_translation_jump";
        } else if (pose_enabled && last_quality_pose && cfg.quality_gate.max_pose_delta_deg > 0.0 &&
                   pose_delta_deg > cfg.quality_gate.max_pose_delta_deg) {
          quality_accept = false;
          quality_reason = "pose_rotation_jump";
        }
      }

      if (quality_csv) {
        write_quality_gate_row(quality_csv, pair.timestamp_ns, pose_for_log, quality_accept, quality_reason,
                               pose_age_ms, raw_points, filtered_points, depth,
                               cfg.quality_gate.far_depth_m, far_points, far_fraction,
                               pose_delta_m, pose_delta_deg);
      }
      if (!quality_accept) {
        map.drop_frame();
        std::cerr << "[xr_spatial_backend] quality gate rejected frame reason=" << quality_reason
                  << " raw_points=" << raw_points
                  << " filtered_points=" << filtered_points
                  << " far_fraction=" << far_fraction
                  << " pose_delta_m=" << pose_delta_m
                  << " pose_delta_deg=" << pose_delta_deg << "\n";
        if (cfg.mapper_backend == "live_depth_grid" && spatial_proxy_mesh_pub && cfg.proxy_mesh.enabled) {
          auto clear_grid = make_clear_live_depth_grid_payload(++proxy_mesh_seq,
              static_cast<uint64_t>(pair.timestamp_ns), pose_timestamp_ns);
          spatial_proxy_mesh_pub->publish(clear_grid);
        }
        if (sampled_pose) last_quality_pose = *sampled_pose;
        continue;
      }

      const xr_spatial_backend::Transform3d T_tracking_imu = pose_enabled
          ? xr_spatial_backend::pose_to_transform(
                sampled_pose->px, sampled_pose->py, sampled_pose->pz,
                sampled_pose->qw, sampled_pose->qx, sampled_pose->qy, sampled_pose->qz)
          : xr_spatial_backend::Transform3d{};
      const xr_spatial_backend::Transform3d T_tracking_cam0_raw = pose_enabled
          ? xr_spatial_backend::compose(T_tracking_imu, calib.T_imu_cam0)
          : xr_spatial_backend::Transform3d{};
      const xr_spatial_backend::Transform3d T_tracking_cam0_rect = pose_enabled
          ? xr_spatial_backend::compose(T_tracking_cam0_raw, calib.T_cam0_raw_cam0_rect)
          : xr_spatial_backend::Transform3d{};


      xr_spatial_backend::Transform3d T_map_cloud;
      xr_spatial_backend::Vec3f hmd_position{};
      if (cfg.map_frame == "camera") {
        T_map_cloud = xr_spatial_backend::Transform3d{};
        hmd_position = {0.0f, 0.0f, 0.0f};
      } else if (cfg.map_frame == "tracking") {
        if (!T_map_tracking_origin) {
          if (cfg.tracking_origin == "first_pose") {
            T_map_tracking_origin = xr_spatial_backend::inverse(T_tracking_imu);
            std::cout << "[xr_spatial_backend] initialized scan-local tracking origin from first valid pose"
                      << " p=(" << sampled_pose->px << "," << sampled_pose->py << "," << sampled_pose->pz << ")\n";
          } else {
            T_map_tracking_origin = xr_spatial_backend::Transform3d{};
            std::cout << "[xr_spatial_backend] using absolute tracking coordinates for map output\n";
          }
        }
        T_map_cloud = xr_spatial_backend::compose(*T_map_tracking_origin, T_tracking_cam0_rect);
        hmd_position = xr_spatial_backend::transform_point(
            *T_map_tracking_origin,
            xr_spatial_backend::Vec3f{static_cast<float>(sampled_pose->px),
                                     static_cast<float>(sampled_pose->py),
                                     static_cast<float>(sampled_pose->pz)});
      } else {
        throw std::runtime_error("invalid --map-frame: " + cfg.map_frame + "; expected tracking or camera");
      }

      if (cfg.mode == "scan" && cfg.save_debug_clouds && !saved_first_debug_cloud && !depth.cloud.points.empty()) {
        const fs::path debug_dir = scan_dir / "debug";
        save_cloud_ply(debug_dir / "first_cloud_cam0_rect.ply", depth.cloud.points);
        save_cloud_ply(debug_dir / "first_cloud_tracking_absolute.ply",
                       transform_points(depth.cloud.points, T_tracking_cam0_rect));
        // first_cloud_tracking.ply is kept as a compatibility alias for older commands.
        save_cloud_ply(debug_dir / "first_cloud_tracking.ply",
                       transform_points(depth.cloud.points, T_map_cloud));
        save_cloud_ply(debug_dir / "first_cloud_map.ply",
                       transform_points(depth.cloud.points, T_map_cloud));
        saved_first_debug_cloud = true;
        std::cout << "[xr_spatial_backend] saved first debug clouds: "
                  << (debug_dir / "first_cloud_cam0_rect.ply") << " and "
                  << (debug_dir / "first_cloud_tracking.ply") << "\n";
      }
      if (cfg.mapper_backend != "live_depth_grid") {
        map.integrate(depth.cloud, T_map_cloud);
      }
      ++integrated_since_start;
      if (sampled_pose) {
        last_integrated_pose = *sampled_pose;
        last_quality_pose = *sampled_pose;
      }

      if (traj) {
        write_trajectory_row(traj, pair.timestamp_ns, pose_for_log, pose_age_ms, depth.cloud.points.size());
      }

      auto summary = map.make_summary(++summary_seq,
                                      pair.timestamp_ns,
                                      pose_timestamp_ns,
                                      pose_age_ms,
                                      stale_pose,
                                      cfg.mode == "scan",
                                      depth.cloud,
                                      hmd_position);
      if (spatial_pub) spatial_pub->publish(summary);

      if (spatial_proxy_mesh_pub && cfg.proxy_mesh.enabled) {
        const int64_t publish_now_ns = now_ns();
        const int64_t proxy_period_ns = cfg.proxy_mesh.rate_hz > 0.0
            ? static_cast<int64_t>(1e9 / cfg.proxy_mesh.rate_hz)
            : 0;
        if (last_proxy_mesh_publish_ns == 0 || proxy_period_ns <= 0 ||
            publish_now_ns - last_proxy_mesh_publish_ns >= proxy_period_ns) {
          xr_spatial::RuntimeSpatialProxyMeshF32V1 proxy_mesh{};
          if (cfg.mapper_backend == "live_depth_grid") {
            proxy_mesh = make_live_depth_grid_payload(
                depth, T_map_cloud, ++proxy_mesh_seq,
                static_cast<uint64_t>(pair.timestamp_ns), pose_timestamp_ns, cfg.proxy_mesh);
          } else {
            proxy_mesh = xr_spatial_backend::build_spatial_proxy_mesh_from_points(
                map.points_tracking(), ++proxy_mesh_seq, static_cast<uint64_t>(pair.timestamp_ns), cfg.proxy_mesh);
            proxy_mesh.pose_timestamp_ns = pose_timestamp_ns;
          }
          spatial_proxy_mesh_pub->publish(proxy_mesh);
          last_proxy_mesh_vertices = proxy_mesh.vertex_count;
          last_proxy_mesh_triangles = proxy_mesh.triangle_count;
          last_proxy_mesh_publish_ns = publish_now_ns;
        }
      }

      if (cfg.print_every > 0 && integrated_since_start % static_cast<uint32_t>(cfg.print_every) == 0) {
        std::cout << "[xr_spatial_backend] integrated=" << integrated_since_start
                  << " cloud_points=" << depth.cloud.points.size()
                  << " grid=" << depth.live_grid_width << "x" << depth.live_grid_height
                  << " grid_valid=" << depth.live_grid_valid_points
                  << " proxy_vertices=" << last_proxy_mesh_vertices
                  << " proxy_triangles=" << last_proxy_mesh_triangles
                  << " filter_in=" << filter_stats.input_points
                  << " roi_reject=" << depth.rejected_roi_pixels
                  << " far_fraction=" << far_fraction
                  << " accumulated=" << map.points_tracking().size()
                  << " depth_mean_m=" << depth.cloud.mean_depth_m
                  << " pose_age_ms=" << pose_age_ms
                  << " hmd_clearance_m=" << summary.hmd_clearance_m
                  << " flags=0x" << std::hex << summary.status_flags << std::dec
                  << "\n";
      }
    }

    const int64_t end_ns = now_ns();
    if (traj) traj.flush();
    if (quality_csv) quality_csv.flush();

    if (cfg.mode == "scan") {
      std::cout << "[xr_spatial_backend] finalizing scan\n";
      if (cfg.save_pointcloud_ply) {
        map.save_pointcloud_ply(scan_dir / "pointcloud.ply");
      }
      if (cfg.save_voxel_pointcloud_ply) {
        const auto voxel_stats = map.save_voxel_pointcloud_ply(scan_dir / "voxel_pointcloud.ply",
                                                               cfg.scan_voxel_size_m,
                                                               cfg.scan_min_observations,
                                                               cfg.scan_max_voxel_points);
        std::cout << "[xr_spatial_backend] saved voxel_pointcloud.ply"
                  << " input_points=" << voxel_stats.input_points
                  << " output_voxels=" << voxel_stats.output_voxels
                  << " rejected_low_observations=" << voxel_stats.rejected_low_observations
                  << " voxel_size_m=" << cfg.scan_voxel_size_m
                  << " min_observations=" << cfg.scan_min_observations << "\n";
      }
      if (cfg.proxy_mesh.enabled) {
        auto proxy_mesh = xr_spatial_backend::build_spatial_proxy_mesh_from_points(
            map.points_tracking(), ++proxy_mesh_seq, static_cast<uint64_t>(end_ns), cfg.proxy_mesh);
        xr_spatial_backend::save_spatial_proxy_mesh_ply(scan_dir / "spatial_proxy_mesh.ply", proxy_mesh);
      }
      if (cfg.save_metadata_json) {
        save_metadata(scan_dir / "scan_metadata.json", cfg, calib, map, start_ns, end_ns);
      }
      std::cout << "[xr_spatial_backend] scan saved to " << scan_dir << "\n";
      if (cfg.exit_after_scan) return 0;
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[xr_spatial_backend][ERROR] " << e.what() << "\n";
    return 1;
  }
}
