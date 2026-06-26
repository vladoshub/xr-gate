#pragma once

#include <cstdint>

namespace xr_spatial {

constexpr const char* RUNTIME_SPATIAL_SUMMARY_FORMAT_NAME = "RUNTIME_SPATIAL_SUMMARY_F32_V1";
constexpr uint32_t RUNTIME_SPATIAL_SUMMARY_FORMAT_VERSION = 1;

// Runtime-facing compact spatial map summary.
// This contract is intentionally small and fast to consume. Heavy data such as
// meshes/TSDF/ESDF layers should be published/saved separately as slow streams
// or scan artifacts.
#pragma pack(push, 1)
struct RuntimeSpatialSummaryF32V1 {
  uint32_t version = RUNTIME_SPATIAL_SUMMARY_FORMAT_VERSION;
  uint32_t size_bytes = sizeof(RuntimeSpatialSummaryF32V1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;           // backend publish timestamp
  uint64_t source_timestamp_ns = 0;    // stereo/depth frame timestamp
  uint64_t pose_timestamp_ns = 0;      // pose sample used for this integration/query

  uint32_t status_flags = 0;
  uint32_t mapper_kind = 0;            // RuntimeSpatialMapperKind
  uint32_t depth_kind = 0;             // RuntimeSpatialDepthKind
  uint32_t reserved0 = 0;

  float hmd_clearance_m = -1.0f;
  float left_hand_clearance_m = -1.0f;
  float right_hand_clearance_m = -1.0f;
  float nearest_obstacle_distance_m = -1.0f;

  float nearest_obstacle_px = 0.0f;
  float nearest_obstacle_py = 0.0f;
  float nearest_obstacle_pz = 0.0f;
  float nearest_obstacle_nx = 0.0f;
  float nearest_obstacle_ny = 0.0f;
  float nearest_obstacle_nz = 0.0f;

  uint32_t integrated_frames = 0;
  uint32_t dropped_frames = 0;
  uint32_t accumulated_points = 0;
  uint32_t last_frame_points = 0;

  uint32_t tsdf_voxel_count = 0;
  uint32_t esdf_voxel_count = 0;
  float map_confidence = 0.0f;
  float pose_age_ms = 0.0f;

  float min_depth_m = 0.0f;
  float max_depth_m = 0.0f;
  float mean_depth_m = 0.0f;
  float reserved1 = 0.0f;
};
#pragma pack(pop)

static_assert(sizeof(RuntimeSpatialSummaryF32V1) == 144,
              "RuntimeSpatialSummaryF32V1 ABI size must remain 144 bytes");

constexpr uint32_t RUNTIME_SPATIAL_SUMMARY_PAYLOAD_SIZE = sizeof(RuntimeSpatialSummaryF32V1);

enum RuntimeSpatialMapperKind : uint32_t {
  RUNTIME_SPATIAL_MAPPER_NONE = 0,
  RUNTIME_SPATIAL_MAPPER_POINTCLOUD_FALLBACK = 1,
  RUNTIME_SPATIAL_MAPPER_VOXBLOX = 2,
};

enum RuntimeSpatialDepthKind : uint32_t {
  RUNTIME_SPATIAL_DEPTH_NONE = 0,
  RUNTIME_SPATIAL_DEPTH_STEREO_SGBM = 1,
};

enum RuntimeSpatialSummaryFlags : uint32_t {
  RUNTIME_SPATIAL_FLAG_ACTIVE = 1u << 0,
  RUNTIME_SPATIAL_FLAG_DEPTH_VALID = 1u << 1,
  RUNTIME_SPATIAL_FLAG_POSE_VALID = 1u << 2,
  RUNTIME_SPATIAL_FLAG_MAP_UPDATED = 1u << 3,
  RUNTIME_SPATIAL_FLAG_ESDF_VALID = 1u << 4,
  RUNTIME_SPATIAL_FLAG_POINTCLOUD_FALLBACK = 1u << 5,
  RUNTIME_SPATIAL_FLAG_SCAN_RECORDING = 1u << 6,
  RUNTIME_SPATIAL_FLAG_SCAN_FINALIZED = 1u << 7,
  RUNTIME_SPATIAL_FLAG_STALE_POSE = 1u << 8,
};

}  // namespace xr_spatial
