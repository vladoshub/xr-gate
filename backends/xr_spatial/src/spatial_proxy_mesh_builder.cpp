#include <xr_spatial_backend/spatial_proxy_mesh_builder.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <chrono>

namespace xr_spatial_backend {
namespace {

struct VoxelKey {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  bool operator==(const VoxelKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct VoxelKeyHash {
  size_t operator()(const VoxelKey& k) const noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int32_t v) {
      uint32_t u = static_cast<uint32_t>(v);
      for (int i = 0; i < 4; ++i) { h ^= (u >> (i * 8)) & 0xffu; h *= 1099511628211ull; }
    };
    mix(k.x); mix(k.y); mix(k.z);
    return static_cast<size_t>(h);
  }
};

struct Accum {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  uint32_t n = 0;
};

bool finite_point(const Vec3f& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

float dist2(const Vec3f& p) { return p.x * p.x + p.y * p.y + p.z * p.z; }

void include_bbox(xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh, const xr_spatial::RuntimeSpatialProxyVertexF32V1& v) {
  if (mesh.vertex_count == 0) {
    mesh.bbox_min_x = mesh.bbox_max_x = v.x;
    mesh.bbox_min_y = mesh.bbox_max_y = v.y;
    mesh.bbox_min_z = mesh.bbox_max_z = v.z;
  } else {
    mesh.bbox_min_x = std::min(mesh.bbox_min_x, v.x); mesh.bbox_max_x = std::max(mesh.bbox_max_x, v.x);
    mesh.bbox_min_y = std::min(mesh.bbox_min_y, v.y); mesh.bbox_max_y = std::max(mesh.bbox_max_y, v.y);
    mesh.bbox_min_z = std::min(mesh.bbox_min_z, v.z); mesh.bbox_max_z = std::max(mesh.bbox_max_z, v.z);
  }
}

bool add_cube(xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh, const Vec3f& c, float half) {
  if (mesh.vertex_count + 8 > xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES) return false;
  if (mesh.triangle_count + 12 > xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES) return false;
  if (mesh.vertex_count + 8 > mesh.max_vertices || mesh.triangle_count + 12 > mesh.max_triangles) return false;

  const uint16_t base = static_cast<uint16_t>(mesh.vertex_count);
  const float x0 = c.x - half, x1 = c.x + half;
  const float y0 = c.y - half, y1 = c.y + half;
  const float z0 = c.z - half, z1 = c.z + half;
  const xr_spatial::RuntimeSpatialProxyVertexF32V1 verts[8] = {
      {x0,y0,z0}, {x1,y0,z0}, {x1,y1,z0}, {x0,y1,z0},
      {x0,y0,z1}, {x1,y0,z1}, {x1,y1,z1}, {x0,y1,z1}};
  for (const auto& v : verts) {
    include_bbox(mesh, v);
    mesh.vertices[mesh.vertex_count++] = v;
  }
  const uint16_t tris[12][3] = {
      {0,1,2},{0,2,3}, {4,6,5},{4,7,6}, {0,4,5},{0,5,1},
      {1,5,6},{1,6,2}, {2,6,7},{2,7,3}, {3,7,4},{3,4,0}};
  for (const auto& t : tris) {
    mesh.triangles[mesh.triangle_count++] = {
        static_cast<uint16_t>(base + t[0]),
        static_cast<uint16_t>(base + t[1]),
        static_cast<uint16_t>(base + t[2])};
  }
  return true;
}

}  // namespace

xr_spatial::RuntimeSpatialProxyMeshF32V1 build_spatial_proxy_mesh_from_points(
    const std::vector<Vec3f>& points_map,
    uint64_t sequence,
    uint64_t source_timestamp_ns,
    const SpatialProxyMeshBuildConfig& cfg) {
  xr_spatial::RuntimeSpatialProxyMeshF32V1 mesh{};
  mesh.sequence = sequence;
  mesh.timestamp_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  mesh.source_timestamp_ns = source_timestamp_ns;
  mesh.status_flags = xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ACTIVE |
                      xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_VOXEL_PROXY;
  mesh.max_vertices = std::min<uint32_t>(cfg.max_vertices, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  mesh.max_triangles = std::min<uint32_t>(cfg.max_triangles, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
  mesh.voxel_size_m = cfg.voxel_size_m;
  mesh.max_distance_m = cfg.max_distance_m;

  if (!cfg.enabled || cfg.voxel_size_m <= 0.0f || points_map.empty()) return mesh;
  const float max_d2 = cfg.max_distance_m > 0.0f ? cfg.max_distance_m * cfg.max_distance_m : std::numeric_limits<float>::infinity();

  std::unordered_map<VoxelKey, Accum, VoxelKeyHash> voxels;
  voxels.reserve(std::min<size_t>(points_map.size(), 50000));
  const float inv = 1.0f / cfg.voxel_size_m;
  for (const Vec3f& p : points_map) {
    if (!finite_point(p)) continue;
    if (dist2(p) > max_d2) continue;
    VoxelKey k{static_cast<int32_t>(std::floor(p.x * inv)),
               static_cast<int32_t>(std::floor(p.y * inv)),
               static_cast<int32_t>(std::floor(p.z * inv))};
    auto& a = voxels[k];
    a.x += p.x; a.y += p.y; a.z += p.z; ++a.n;
  }

  std::vector<std::pair<float, Vec3f>> centers;
  centers.reserve(voxels.size());
  for (const auto& it : voxels) {
    const Accum& a = it.second;
    if (a.n < cfg.min_points_per_voxel) continue;
    Vec3f c{static_cast<float>(a.x / a.n), static_cast<float>(a.y / a.n), static_cast<float>(a.z / a.n)};
    centers.push_back({dist2(c), c});
  }
  std::sort(centers.begin(), centers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  const float half = std::max(0.005f, cfg.voxel_size_m * 0.48f);
  for (const auto& item : centers) {
    if (!add_cube(mesh, item.second, half)) break;
  }
  if (mesh.vertex_count > 0) {
    mesh.status_flags |= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_UPDATED;
    if (mesh.triangle_count > 0) mesh.status_flags |= xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_HAS_TRIANGLES;
    mesh.confidence = std::min(1.0f, static_cast<float>(mesh.triangle_count) / 512.0f);
  }
  return mesh;
}

void save_spatial_proxy_mesh_ply(const std::filesystem::path& path,
                                 const xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::ofstream os(path);
  if (!os) throw std::runtime_error("failed to write spatial proxy mesh PLY: " + path.string());
  os << "ply\nformat ascii 1.0\n";
  os << "element vertex " << mesh.vertex_count << "\n";
  os << "property float x\nproperty float y\nproperty float z\n";
  os << "element face " << mesh.triangle_count << "\n";
  os << "property list uchar int vertex_indices\n";
  os << "end_header\n";
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const auto& v = mesh.vertices[i];
    os << v.x << ' ' << v.y << ' ' << v.z << '\n';
  }
  for (uint32_t i = 0; i < mesh.triangle_count; ++i) {
    const auto& t = mesh.triangles[i];
    os << "3 " << t.i0 << ' ' << t.i1 << ' ' << t.i2 << '\n';
  }
}

}  // namespace xr_spatial_backend
