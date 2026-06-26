#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>
#include <xr_spatial_backend/types.hpp>

namespace xr_spatial_backend {

struct SpatialProxyMeshBuildConfig {
  bool enabled = false;
  double rate_hz = 2.0;
  float voxel_size_m = 0.08f;
  float max_distance_m = 2.5f;
  uint32_t max_vertices = xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES;
  uint32_t max_triangles = xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES;
  uint32_t min_points_per_voxel = 1;

  // Live-depth-grid surface stitching.  These are used only when mesh_kind is
  // live_depth_grid; legacy voxel proxy generation still uses cube triangles.
  bool live_grid_triangles_enabled = true;
  float live_grid_max_edge_m = 0.10f;
  float live_grid_max_depth_jump_m = 0.10f;
};

xr_spatial::RuntimeSpatialProxyMeshF32V1 build_spatial_proxy_mesh_from_points(
    const std::vector<Vec3f>& points_map,
    uint64_t sequence,
    uint64_t source_timestamp_ns,
    const SpatialProxyMeshBuildConfig& cfg);

void save_spatial_proxy_mesh_ply(const std::filesystem::path& path,
                                 const xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh);

}  // namespace xr_spatial_backend
