#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xr_spatial_backend {

struct Vec3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Quatd {
  double w = 1.0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Pose3d {
  Vec3d p;
  Quatd q;
};

struct PointCloudFrame {
  uint64_t sequence = 0;
  int64_t timestamp_ns = 0;
  std::string frame_id;
  std::vector<Vec3f> points;
  float min_depth_m = 0.0f;
  float max_depth_m = 0.0f;
  float mean_depth_m = 0.0f;
};

struct DepthStats {
  uint32_t valid_points = 0;
  float min_depth_m = 0.0f;
  float max_depth_m = 0.0f;
  float mean_depth_m = 0.0f;
};

}  // namespace xr_spatial_backend
