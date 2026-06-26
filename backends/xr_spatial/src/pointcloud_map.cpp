#include <xr_spatial_backend/pointcloud_map.hpp>

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <xr_spatial/publishers/runtime_spatial_summary_shm_publisher.hpp>

namespace xr_spatial_backend {
namespace {

struct VoxelKey {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;

  bool operator==(const VoxelKey& o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};

struct VoxelKeyHash {
  size_t operator()(const VoxelKey& k) const noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int32_t v) {
      uint32_t u = static_cast<uint32_t>(v);
      for (int i = 0; i < 4; ++i) {
        h ^= (u >> (i * 8)) & 0xffu;
        h *= 1099511628211ull;
      }
    };
    mix(k.x);
    mix(k.y);
    mix(k.z);
    return static_cast<size_t>(h);
  }
};

struct VoxelAccum {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  uint32_t n = 0;
};

}  // namespace

PointCloudSpatialMap::PointCloudSpatialMap(size_t max_accumulated_points, float max_abs_map_coord_m)
    : max_accumulated_points_(max_accumulated_points), max_abs_map_coord_m_(max_abs_map_coord_m) {
  points_tracking_.reserve(std::min<size_t>(max_accumulated_points_, 500000));
}

void PointCloudSpatialMap::reset() {
  points_tracking_.clear();
  integrated_frames_ = 0;
  dropped_frames_ = 0;
  last_frame_points_ = 0;
}

void PointCloudSpatialMap::integrate(const PointCloudFrame& cloud_camera, const Transform3d& T_tracking_camera) {
  if (cloud_camera.points.empty()) {
    ++dropped_frames_;
    last_frame_points_ = 0;
    return;
  }

  last_frame_points_ = static_cast<uint32_t>(cloud_camera.points.size());
  for (const Vec3f& p : cloud_camera.points) {
    if (points_tracking_.size() >= max_accumulated_points_) break;
    const Vec3f q = transform_point(T_tracking_camera, p);
    if (!is_finite(q) || !within_abs_limit(q, max_abs_map_coord_m_)) continue;
    points_tracking_.push_back(q);
  }
  ++integrated_frames_;
}

void PointCloudSpatialMap::drop_frame() {
  ++dropped_frames_;
  last_frame_points_ = 0;
}

SpatialMapStats PointCloudSpatialMap::query_from_hmd(const Vec3f& hmd_position_tracking) const {
  SpatialMapStats s;
  s.integrated_frames = integrated_frames_;
  s.dropped_frames = dropped_frames_;
  s.accumulated_points = static_cast<uint32_t>(std::min<size_t>(points_tracking_.size(), UINT32_MAX));
  s.last_frame_points = last_frame_points_;

  double best = std::numeric_limits<double>::infinity();
  Vec3f best_p{};
  // Cheap sparse query over capped accumulated points.
  const size_t step = points_tracking_.size() > 250000 ? (points_tracking_.size() / 250000 + 1) : 1;
  for (size_t i = 0; i < points_tracking_.size(); i += step) {
    const double d = distance(points_tracking_[i], hmd_position_tracking);
    if (d < best) {
      best = d;
      best_p = points_tracking_[i];
    }
  }
  if (std::isfinite(best)) {
    s.nearest_obstacle_distance_m = static_cast<float>(best);
    s.nearest_obstacle_point = best_p;
  }
  return s;
}

xr_spatial::RuntimeSpatialSummaryF32V1 PointCloudSpatialMap::make_summary(uint64_t sequence,
                                                               int64_t source_timestamp_ns,
                                                               uint64_t pose_timestamp_ns,
                                                               double pose_age_ms,
                                                               bool stale_pose,
                                                               bool scan_recording,
                                                               const PointCloudFrame& last_cloud,
                                                               const Vec3f& hmd_position_tracking) const {
  xr_spatial::RuntimeSpatialSummaryF32V1 out;
  out.sequence = sequence;
  out.timestamp_ns = static_cast<uint64_t>(xr_spatial::spatial_summary_now_ns());
  out.source_timestamp_ns = static_cast<uint64_t>(source_timestamp_ns > 0 ? source_timestamp_ns : 0);
  out.pose_timestamp_ns = pose_timestamp_ns;
  out.mapper_kind = xr_spatial::RUNTIME_SPATIAL_MAPPER_POINTCLOUD_FALLBACK;
  out.depth_kind = xr_spatial::RUNTIME_SPATIAL_DEPTH_STEREO_SGBM;
  out.status_flags = xr_spatial::RUNTIME_SPATIAL_FLAG_ACTIVE |
                     xr_spatial::RUNTIME_SPATIAL_FLAG_POINTCLOUD_FALLBACK;
  if (!last_cloud.points.empty()) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_FLAG_DEPTH_VALID;
  if (pose_timestamp_ns != 0) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_FLAG_POSE_VALID;
  if (integrated_frames_ > 0) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_FLAG_MAP_UPDATED;
  if (scan_recording) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_FLAG_SCAN_RECORDING;
  if (stale_pose) out.status_flags |= xr_spatial::RUNTIME_SPATIAL_FLAG_STALE_POSE;

  const SpatialMapStats stats = query_from_hmd(hmd_position_tracking);
  out.hmd_clearance_m = stats.nearest_obstacle_distance_m;
  out.nearest_obstacle_distance_m = stats.nearest_obstacle_distance_m;
  out.nearest_obstacle_px = stats.nearest_obstacle_point.x;
  out.nearest_obstacle_py = stats.nearest_obstacle_point.y;
  out.nearest_obstacle_pz = stats.nearest_obstacle_point.z;
  out.integrated_frames = stats.integrated_frames;
  out.dropped_frames = stats.dropped_frames;
  out.accumulated_points = stats.accumulated_points;
  out.last_frame_points = stats.last_frame_points;
  out.map_confidence = integrated_frames_ > 0 ? 0.25f : 0.0f; // fallback cloud, not ESDF.
  out.pose_age_ms = static_cast<float>(pose_age_ms);
  out.min_depth_m = last_cloud.min_depth_m;
  out.max_depth_m = last_cloud.max_depth_m;
  out.mean_depth_m = last_cloud.mean_depth_m;
  return out;
}

void PointCloudSpatialMap::save_pointcloud_ply(const std::filesystem::path& path) const {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::ofstream os(path);
  if (!os) throw std::runtime_error("failed to write pointcloud PLY: " + path.string());
  os << "ply\nformat ascii 1.0\n";
  os << "element vertex " << points_tracking_.size() << "\n";
  os << "property float x\nproperty float y\nproperty float z\n";
  os << "end_header\n";
  for (const Vec3f& p : points_tracking_) {
    os << p.x << ' ' << p.y << ' ' << p.z << '\n';
  }
}

VoxelizedPointCloudStats PointCloudSpatialMap::save_voxel_pointcloud_ply(
    const std::filesystem::path& path,
    float voxel_size_m,
    uint32_t min_observations,
    size_t max_output_points) const {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());

  VoxelizedPointCloudStats stats;
  stats.input_points = static_cast<uint32_t>(std::min<size_t>(points_tracking_.size(), UINT32_MAX));

  if (voxel_size_m <= 0.0f) {
    throw std::runtime_error("voxel_size_m must be > 0 for voxel pointcloud PLY");
  }
  min_observations = std::max<uint32_t>(1, min_observations);

  std::unordered_map<VoxelKey, VoxelAccum, VoxelKeyHash> voxels;
  voxels.reserve(std::min<size_t>(points_tracking_.size(), 1000000));
  const float inv = 1.0f / voxel_size_m;
  for (const Vec3f& p : points_tracking_) {
    if (!is_finite(p) || !within_abs_limit(p, max_abs_map_coord_m_)) continue;
    VoxelKey k{static_cast<int32_t>(std::floor(p.x * inv)),
               static_cast<int32_t>(std::floor(p.y * inv)),
               static_cast<int32_t>(std::floor(p.z * inv))};
    auto& a = voxels[k];
    a.x += p.x;
    a.y += p.y;
    a.z += p.z;
    ++a.n;
  }

  struct Center {
    Vec3f p;
    uint32_t n = 0;
  };
  std::vector<Center> centers;
  centers.reserve(voxels.size());
  for (const auto& it : voxels) {
    const VoxelAccum& a = it.second;
    if (a.n < min_observations) {
      ++stats.rejected_low_observations;
      continue;
    }
    centers.push_back({Vec3f{static_cast<float>(a.x / a.n),
                             static_cast<float>(a.y / a.n),
                             static_cast<float>(a.z / a.n)},
                       a.n});
  }

  std::sort(centers.begin(), centers.end(), [](const Center& a, const Center& b) {
    if (a.n != b.n) return a.n > b.n;
    const float da = a.p.x * a.p.x + a.p.y * a.p.y + a.p.z * a.p.z;
    const float db = b.p.x * b.p.x + b.p.y * b.p.y + b.p.z * b.p.z;
    return da < db;
  });
  if (max_output_points > 0 && centers.size() > max_output_points) {
    centers.resize(max_output_points);
  }
  stats.output_voxels = static_cast<uint32_t>(std::min<size_t>(centers.size(), UINT32_MAX));

  std::ofstream os(path);
  if (!os) throw std::runtime_error("failed to write voxel pointcloud PLY: " + path.string());
  os << "ply\nformat ascii 1.0\n";
  os << "comment generated_by xr_spatial_backend primitive_scanner\n";
  os << "comment voxel_size_m " << voxel_size_m << "\n";
  os << "comment min_observations " << min_observations << "\n";
  os << "element vertex " << centers.size() << "\n";
  os << "property float x\nproperty float y\nproperty float z\n";
  os << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  os << "property uint observations\n";
  os << "end_header\n";
  for (const Center& c : centers) {
    const uint32_t strength = std::min<uint32_t>(255, 32u + c.n * 16u);
    const uint32_t red = 32;
    const uint32_t green = strength;
    const uint32_t blue = std::min<uint32_t>(255, 96u + c.n * 8u);
    os << c.p.x << ' ' << c.p.y << ' ' << c.p.z << ' '
       << red << ' ' << green << ' ' << blue << ' '
       << c.n << '\n';
  }
  return stats;
}

}  // namespace xr_spatial_backend
