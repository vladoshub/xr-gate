#pragma once

#include <cstddef>

#include <xr_spatial_backend/types.hpp>

namespace xr_spatial_backend {

struct PointCloudFilterConfig {
  // 0 disables voxel downsample.
  float voxel_size_m = 0.0f;
  // 0 disables cap. Applied after voxel downsample.
  size_t max_points = 0;
};

struct PointCloudFilterStats {
  size_t input_points = 0;
  size_t output_points = 0;
  size_t voxel_dropped_points = 0;
  size_t cap_dropped_points = 0;
};

PointCloudFrame filter_pointcloud(const PointCloudFrame& input,
                                  const PointCloudFilterConfig& config,
                                  PointCloudFilterStats* stats = nullptr);

}  // namespace xr_spatial_backend
