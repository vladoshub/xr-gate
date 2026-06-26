#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <xr_spatial/contracts/runtime_spatial_summary_contract.hpp>
#include <xr_spatial_backend/pose_math.hpp>
#include <xr_spatial_backend/types.hpp>

namespace xr_spatial_backend {

struct SpatialMapStats {
  uint32_t integrated_frames = 0;
  uint32_t dropped_frames = 0;
  uint32_t accumulated_points = 0;
  uint32_t last_frame_points = 0;
  float nearest_obstacle_distance_m = -1.0f;
  Vec3f nearest_obstacle_point{};
};

struct VoxelizedPointCloudStats {
  uint32_t input_points = 0;
  uint32_t output_voxels = 0;
  uint32_t rejected_low_observations = 0;
};

class PointCloudSpatialMap {
 public:
  explicit PointCloudSpatialMap(size_t max_accumulated_points, float max_abs_map_coord_m = 100.0f);

  void reset();
  void integrate(const PointCloudFrame& cloud_camera, const Transform3d& T_tracking_camera);
  void drop_frame();
  SpatialMapStats query_from_hmd(const Vec3f& hmd_position_tracking) const;
  xr_spatial::RuntimeSpatialSummaryF32V1 make_summary(uint64_t sequence,
                                          int64_t source_timestamp_ns,
                                          uint64_t pose_timestamp_ns,
                                          double pose_age_ms,
                                          bool stale_pose,
                                          bool scan_recording,
                                          const PointCloudFrame& last_cloud,
                                          const Vec3f& hmd_position_tracking) const;

  void save_pointcloud_ply(const std::filesystem::path& path) const;
  VoxelizedPointCloudStats save_voxel_pointcloud_ply(const std::filesystem::path& path,
                                                      float voxel_size_m,
                                                      uint32_t min_observations,
                                                      size_t max_output_points) const;
  const std::vector<Vec3f>& points_tracking() const { return points_tracking_; }
  uint32_t integrated_frames() const { return integrated_frames_; }
  uint32_t dropped_frames() const { return dropped_frames_; }

 private:
  size_t max_accumulated_points_ = 1000000;
  float max_abs_map_coord_m_ = 100.0f;
  std::vector<Vec3f> points_tracking_;
  uint32_t integrated_frames_ = 0;
  uint32_t dropped_frames_ = 0;
  uint32_t last_frame_points_ = 0;
};

}  // namespace xr_spatial_backend
