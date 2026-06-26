#include <xr_spatial_backend/pointcloud_filter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace xr_spatial_backend {
namespace {

struct VoxelKey {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;

  bool operator==(const VoxelKey& other) const {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct VoxelKeyHash {
  size_t operator()(const VoxelKey& k) const {
    // Splitmix-like integer mixing on packed voxel indices. This is good enough for
    // per-frame filtering and avoids pulling in a heavier hash dependency.
    uint64_t h = 0x9e3779b97f4a7c15ull;
    auto mix = [&h](int32_t v) {
      uint64_t x = static_cast<uint32_t>(v);
      x += 0x9e3779b97f4a7c15ull;
      x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
      x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
      h ^= x ^ (h >> 31);
    };
    mix(k.x);
    mix(k.y);
    mix(k.z);
    return static_cast<size_t>(h);
  }
};

VoxelKey key_for_point(const Vec3f& p, float voxel_size_m) {
  const float inv = 1.0f / voxel_size_m;
  return VoxelKey{
      static_cast<int32_t>(std::floor(p.x * inv)),
      static_cast<int32_t>(std::floor(p.y * inv)),
      static_cast<int32_t>(std::floor(p.z * inv)),
  };
}

void recompute_depth_stats(PointCloudFrame* cloud) {
  if (!cloud || cloud->points.empty()) {
    if (cloud) {
      cloud->min_depth_m = 0.0f;
      cloud->max_depth_m = 0.0f;
      cloud->mean_depth_m = 0.0f;
    }
    return;
  }

  double sum = 0.0;
  float min_z = cloud->points.front().z;
  float max_z = cloud->points.front().z;
  for (const auto& p : cloud->points) {
    min_z = std::min(min_z, p.z);
    max_z = std::max(max_z, p.z);
    sum += p.z;
  }
  cloud->min_depth_m = min_z;
  cloud->max_depth_m = max_z;
  cloud->mean_depth_m = static_cast<float>(sum / static_cast<double>(cloud->points.size()));
}

}  // namespace

PointCloudFrame filter_pointcloud(const PointCloudFrame& input,
                                  const PointCloudFilterConfig& config,
                                  PointCloudFilterStats* stats) {
  PointCloudFilterStats local;
  local.input_points = input.points.size();

  PointCloudFrame output = input;
  output.points.clear();

  if (input.points.empty()) {
    if (stats) *stats = local;
    return output;
  }

  if (config.voxel_size_m > 0.0f) {
    std::unordered_set<VoxelKey, VoxelKeyHash> seen;
    seen.reserve(input.points.size());
    output.points.reserve(input.points.size());
    for (const auto& p : input.points) {
      const VoxelKey k = key_for_point(p, config.voxel_size_m);
      if (seen.insert(k).second) {
        output.points.push_back(p);
      } else {
        ++local.voxel_dropped_points;
      }
    }
  } else {
    output.points = input.points;
  }

  if (config.max_points > 0 && output.points.size() > config.max_points) {
    local.cap_dropped_points = output.points.size() - config.max_points;
    output.points.resize(config.max_points);
  }

  recompute_depth_stats(&output);
  local.output_points = output.points.size();
  if (stats) *stats = local;
  return output;
}

}  // namespace xr_spatial_backend
