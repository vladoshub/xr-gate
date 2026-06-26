#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace xr_spatial {

constexpr const char* RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME = "RUNTIME_SPATIAL_PROXY_MESH_F32_V2";
constexpr uint32_t RUNTIME_SPATIAL_PROXY_MESH_FORMAT_VERSION = 2;

// Low-poly/voxel proxy surface for realtime overlays. This is intentionally not
// a full spatial map: it is a bounded, fixed-size surface proxy that can be
// transformed by xr_runtime_adapter and chunked over UDP by tracking_udp_bridge.
constexpr uint32_t RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES = 8192;
constexpr uint32_t RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES = 12000;

#pragma pack(push, 1)
struct RuntimeSpatialProxyVertexF32V1 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct RuntimeSpatialProxyTriangleU16V1 {
  uint16_t i0 = 0;
  uint16_t i1 = 0;
  uint16_t i2 = 0;
};

struct RuntimeSpatialProxyMeshF32V1 {
  uint32_t version = RUNTIME_SPATIAL_PROXY_MESH_FORMAT_VERSION;
  uint32_t size_bytes = sizeof(RuntimeSpatialProxyMeshF32V1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;        // publish timestamp
  uint64_t source_timestamp_ns = 0; // source map/depth timestamp
  uint64_t pose_timestamp_ns = 0;   // pose timestamp used by map update, if known

  uint32_t status_flags = 0;
  uint32_t vertex_count = 0;
  uint32_t triangle_count = 0;
  uint32_t max_vertices = RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES;
  uint32_t max_triangles = RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES;
  uint32_t mesh_kind = 2;           // 1=legacy voxel_proxy, 2=live_depth_grid
  uint32_t grid_width = 0;          // organized live depth grid width, 0 for legacy mesh
  uint32_t grid_height = 0;         // organized live depth grid height, 0 for legacy mesh
  uint32_t grid_decimation = 0;     // source image decimation step used to build the grid
  uint32_t valid_point_count = 0;   // valid finite points inside vertices[0..vertex_count)

  float bbox_min_x = 0.0f;
  float bbox_min_y = 0.0f;
  float bbox_min_z = 0.0f;
  float bbox_max_x = 0.0f;
  float bbox_max_y = 0.0f;
  float bbox_max_z = 0.0f;

  float voxel_size_m = 0.0f;
  float max_distance_m = 0.0f;
  float confidence = 0.0f;
  float reserved_f0 = 0.0f;

  RuntimeSpatialProxyVertexF32V1 vertices[RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES];
  RuntimeSpatialProxyTriangleU16V1 triangles[RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES];
};
#pragma pack(pop)

static_assert(sizeof(RuntimeSpatialProxyVertexF32V1) == 12, "vertex ABI size");
static_assert(sizeof(RuntimeSpatialProxyTriangleU16V1) == 6, "triangle ABI size");

constexpr uint32_t RUNTIME_SPATIAL_PROXY_MESH_PAYLOAD_SIZE = sizeof(RuntimeSpatialProxyMeshF32V1);

enum RuntimeSpatialProxyMeshFlags : uint32_t {
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_ACTIVE = 1u << 0,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_UPDATED = 1u << 1,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_VOXEL_PROXY = 1u << 2,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_QUANTIZED_FOR_UDP = 1u << 3,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_LIVE_DEPTH_GRID = 1u << 4,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_ORGANIZED_GRID = 1u << 5,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_CLEAR_FRAME = 1u << 6,
  RUNTIME_SPATIAL_PROXY_MESH_FLAG_HAS_TRIANGLES = 1u << 7,
};

#pragma pack(push, 1)
struct RuntimeSpatialProxyMeshUdpChunkHeaderV1 {
  char magic[4] = {'X','R','P','M'};
  // Wire header v2 keeps the same chunked payload layout but carries the
  // organized live-depth-grid metadata needed to treat this as one contract
  // over SHM and UDP.  The struct name stays V1 to avoid renaming all callsites.
  uint16_t version = 2;
  uint16_t header_size = sizeof(RuntimeSpatialProxyMeshUdpChunkHeaderV1);
  uint16_t chunk_index = 0;
  uint16_t chunk_count = 0;
  uint32_t payload_size = 0;
  uint32_t payload_offset_bytes = 0;
  uint32_t full_payload_size_bytes = 0;

  uint64_t mesh_sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;

  uint32_t status_flags = 0;
  uint32_t vertex_count = 0;
  uint32_t triangle_count = 0;
  uint32_t mesh_kind = 0;

  float bbox_min_x = 0.0f;
  float bbox_min_y = 0.0f;
  float bbox_min_z = 0.0f;
  float bbox_max_x = 0.0f;
  float bbox_max_y = 0.0f;
  float bbox_max_z = 0.0f;
  float voxel_size_m = 0.0f;
  float confidence = 0.0f;

  uint64_t pose_timestamp_ns = 0;
  uint32_t grid_width = 0;
  uint32_t grid_height = 0;
  uint32_t grid_decimation = 0;
  uint32_t valid_point_count = 0;
  uint32_t max_vertices = RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES;
  uint32_t max_triangles = RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES;
  float max_distance_m = 0.0f;
  uint32_t reserved1 = 0;
};
#pragma pack(pop)

static_assert(sizeof(RuntimeSpatialProxyMeshUdpChunkHeaderV1) == 136,
              "RuntimeSpatialProxyMeshUdpChunkHeaderV1 ABI size must remain 136 bytes");

inline bool proxy_mesh_udp_magic_ok(const RuntimeSpatialProxyMeshUdpChunkHeaderV1& h) {
  return h.magic[0] == 'X' && h.magic[1] == 'R' && h.magic[2] == 'P' && h.magic[3] == 'M';
}

inline std::vector<uint8_t> proxy_mesh_payload_bytes(const RuntimeSpatialProxyMeshF32V1& mesh) {
  const uint32_t vc = std::min<uint32_t>(mesh.vertex_count, RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  const uint32_t tc = std::min<uint32_t>(mesh.triangle_count, RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
  const size_t vertex_bytes = static_cast<size_t>(vc) * sizeof(RuntimeSpatialProxyVertexF32V1);
  const size_t tri_bytes = static_cast<size_t>(tc) * sizeof(RuntimeSpatialProxyTriangleU16V1);
  std::vector<uint8_t> out(vertex_bytes + tri_bytes);
  if (vertex_bytes) std::memcpy(out.data(), mesh.vertices, vertex_bytes);
  if (tri_bytes) std::memcpy(out.data() + vertex_bytes, mesh.triangles, tri_bytes);
  return out;
}

inline RuntimeSpatialProxyMeshF32V1 proxy_mesh_from_payload(const RuntimeSpatialProxyMeshUdpChunkHeaderV1& h,
                                                            const std::vector<uint8_t>& payload) {
  RuntimeSpatialProxyMeshF32V1 mesh{};
  mesh.sequence = h.mesh_sequence;
  mesh.timestamp_ns = h.timestamp_ns;
  mesh.source_timestamp_ns = h.source_timestamp_ns;
  mesh.status_flags = h.status_flags;
  mesh.vertex_count = std::min<uint32_t>(h.vertex_count, RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  mesh.triangle_count = std::min<uint32_t>(h.triangle_count, RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
  mesh.mesh_kind = h.mesh_kind;
  mesh.grid_width = h.grid_width;
  mesh.grid_height = h.grid_height;
  mesh.grid_decimation = h.grid_decimation;
  mesh.valid_point_count = h.valid_point_count;
  mesh.max_vertices = std::min<uint32_t>(h.max_vertices, RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  mesh.max_triangles = std::min<uint32_t>(h.max_triangles, RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
  mesh.pose_timestamp_ns = h.pose_timestamp_ns;
  mesh.bbox_min_x = h.bbox_min_x; mesh.bbox_min_y = h.bbox_min_y; mesh.bbox_min_z = h.bbox_min_z;
  mesh.bbox_max_x = h.bbox_max_x; mesh.bbox_max_y = h.bbox_max_y; mesh.bbox_max_z = h.bbox_max_z;
  mesh.voxel_size_m = h.voxel_size_m;
  mesh.max_distance_m = h.max_distance_m;
  mesh.confidence = h.confidence;
  const size_t vertex_bytes = static_cast<size_t>(mesh.vertex_count) * sizeof(RuntimeSpatialProxyVertexF32V1);
  const size_t tri_bytes = static_cast<size_t>(mesh.triangle_count) * sizeof(RuntimeSpatialProxyTriangleU16V1);
  if (payload.size() < vertex_bytes + tri_bytes) {
    throw std::runtime_error("short spatial proxy mesh UDP payload reassembly");
  }
  if (vertex_bytes) std::memcpy(mesh.vertices, payload.data(), vertex_bytes);
  if (tri_bytes) std::memcpy(mesh.triangles, payload.data() + vertex_bytes, tri_bytes);
  return mesh;
}

inline std::vector<std::vector<uint8_t>> encode_proxy_mesh_udp_chunks(
    const RuntimeSpatialProxyMeshF32V1& mesh,
    size_t mtu_bytes) {
  const size_t header_size = sizeof(RuntimeSpatialProxyMeshUdpChunkHeaderV1);
  if (mtu_bytes <= header_size + 64) {
    throw std::runtime_error("spatial proxy mesh UDP MTU too small");
  }
  const std::vector<uint8_t> payload = proxy_mesh_payload_bytes(mesh);
  const size_t max_payload = mtu_bytes - header_size;
  const uint16_t chunk_count = static_cast<uint16_t>(std::max<size_t>(1, (payload.size() + max_payload - 1) / max_payload));
  std::vector<std::vector<uint8_t>> chunks;
  chunks.reserve(chunk_count);
  for (uint16_t i = 0; i < chunk_count; ++i) {
    const size_t off = static_cast<size_t>(i) * max_payload;
    const size_t n = off < payload.size() ? std::min(max_payload, payload.size() - off) : 0;
    RuntimeSpatialProxyMeshUdpChunkHeaderV1 h{};
    h.chunk_index = i;
    h.chunk_count = chunk_count;
    h.payload_size = static_cast<uint32_t>(n);
    h.payload_offset_bytes = static_cast<uint32_t>(off);
    h.full_payload_size_bytes = static_cast<uint32_t>(payload.size());
    h.mesh_sequence = mesh.sequence;
    h.timestamp_ns = mesh.timestamp_ns;
    h.source_timestamp_ns = mesh.source_timestamp_ns;
    h.status_flags = mesh.status_flags;
    h.vertex_count = std::min<uint32_t>(mesh.vertex_count, RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
    h.triangle_count = std::min<uint32_t>(mesh.triangle_count, RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
    h.mesh_kind = mesh.mesh_kind;
    h.grid_width = mesh.grid_width;
    h.grid_height = mesh.grid_height;
    h.grid_decimation = mesh.grid_decimation;
    h.valid_point_count = mesh.valid_point_count;
    h.max_vertices = std::min<uint32_t>(mesh.max_vertices, RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
    h.max_triangles = std::min<uint32_t>(mesh.max_triangles, RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
    h.pose_timestamp_ns = mesh.pose_timestamp_ns;
    h.bbox_min_x = mesh.bbox_min_x; h.bbox_min_y = mesh.bbox_min_y; h.bbox_min_z = mesh.bbox_min_z;
    h.bbox_max_x = mesh.bbox_max_x; h.bbox_max_y = mesh.bbox_max_y; h.bbox_max_z = mesh.bbox_max_z;
    h.voxel_size_m = mesh.voxel_size_m;
    h.max_distance_m = mesh.max_distance_m;
    h.confidence = mesh.confidence;
    std::vector<uint8_t> packet(header_size + n);
    std::memcpy(packet.data(), &h, header_size);
    if (n) std::memcpy(packet.data() + header_size, payload.data() + off, n);
    chunks.push_back(std::move(packet));
  }
  return chunks;
}

}  // namespace xr_spatial
