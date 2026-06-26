#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <xr_tracking/types/tracking_types.hpp>

namespace xr_runtime {

// Runtime-facing HMD pose stream. This is the stable contract consumed by
// runtime-specific adapters such as Monado/OpenXR and SteamVR/OpenVR.
//
// Coordinate contract:
//   - position/orientation are already expressed in the runtime frame
//     selected by xr_runtime_adapter, usually "runtime_local".
//   - origin/recenter and prediction have already been applied by
//     xr_runtime_adapter when the corresponding flags are set.
//   - quaternion order is wxyz.
//   - units: metres, metres/second, radians/second.
//
// This payload is intentionally platform-neutral. Linux SHM, Windows shared
// memory, UDP, named pipes, or any future transport should carry this same
// structure/semantics instead of inventing runtime-specific pose formats.
#pragma pack(push, 1)

struct RuntimeHmdPoseF64V1 {
  uint32_t version = 1;
  uint32_t size_bytes = sizeof(RuntimeHmdPoseF64V1);

  uint64_t sequence = 0;              // Runtime output stream sequence.
  uint64_t timestamp_ns = 0;          // Runtime pose timestamp, usually target timestamp.
  uint64_t source_timestamp_ns = 0;   // Original HMD/backend source timestamp.
  uint64_t target_timestamp_ns = 0;   // Runtime target/prediction timestamp.
  uint64_t source_sequence = 0;       // Source HMD stream sequence.
  uint64_t reset_counter = 0;         // Backend/runtime reset counter.

  double px = 0.0;
  double py = 0.0;
  double pz = 0.0;
  double qw = 1.0;
  double qx = 0.0;
  double qy = 0.0;
  double qz = 0.0;

  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
  double wx = 0.0;
  double wy = 0.0;
  double wz = 0.0;

  uint32_t tracking_status = 0;       // Same status numbering as HmdPoseF64V1.
  uint32_t flags = 0;                 // RuntimeHmdPoseFlags.
  float confidence = 0.0f;
  uint32_t source_tracking_status = 0;
  uint32_t source_flags = 0;
  uint32_t reserved0 = 0;
};

#pragma pack(pop)

static_assert(sizeof(RuntimeHmdPoseF64V1) == 184,
              "Runtime HMD pose payload must stay ABI-stable at 184 bytes");

constexpr const char* RUNTIME_HMD_POSE_FORMAT_NAME = "RUNTIME_HMD_POSE_V1";
constexpr uint32_t RUNTIME_HMD_POSE_FORMAT_VERSION = 1;
constexpr uint32_t RUNTIME_HMD_POSE_PAYLOAD_SIZE = sizeof(RuntimeHmdPoseF64V1);

constexpr uint32_t RUNTIME_HMD_FLAG_POSE_VALID = 1u << 0;
constexpr uint32_t RUNTIME_HMD_FLAG_LINEAR_VELOCITY_VALID = 1u << 1;
constexpr uint32_t RUNTIME_HMD_FLAG_ANGULAR_VELOCITY_VALID = 1u << 2;
constexpr uint32_t RUNTIME_HMD_FLAG_PREDICTED = 1u << 3;
constexpr uint32_t RUNTIME_HMD_FLAG_ORIGIN_APPLIED = 1u << 4;
constexpr uint32_t RUNTIME_HMD_FLAG_RUNTIME_FRAME = 1u << 5;
constexpr uint32_t RUNTIME_HMD_FLAG_STALE_AS_LOST = 1u << 6;

inline RuntimeHmdPoseF64V1 runtime_hmd_pose_from_hmd_pose(
    const HmdPoseF64V1& hmd,
    uint64_t runtime_sequence,
    int64_t read_timestamp_ns,
    int64_t target_timestamp_ns,
    bool hmd_valid,
    bool predicted,
    bool origin_applied,
    bool stale_as_lost) {
  RuntimeHmdPoseF64V1 out;
  out.version = 1;
  out.size_bytes = sizeof(RuntimeHmdPoseF64V1);
  out.sequence = runtime_sequence;
  out.timestamp_ns = static_cast<uint64_t>(target_timestamp_ns > 0
                                               ? target_timestamp_ns
                                               : (hmd.timestamp_ns != 0
                                                      ? static_cast<int64_t>(hmd.timestamp_ns)
                                                      : read_timestamp_ns));
  out.source_timestamp_ns = hmd.source_timestamp_ns != 0 ? hmd.source_timestamp_ns : hmd.timestamp_ns;
  out.target_timestamp_ns = target_timestamp_ns > 0 ? static_cast<uint64_t>(target_timestamp_ns)
                                                    : out.timestamp_ns;
  out.source_sequence = hmd.sequence;
  out.reset_counter = hmd.reset_counter;

  out.px = hmd.px;
  out.py = hmd.py;
  out.pz = hmd.pz;
  out.qw = hmd.qw;
  out.qx = hmd.qx;
  out.qy = hmd.qy;
  out.qz = hmd.qz;
  out.vx = hmd.vx;
  out.vy = hmd.vy;
  out.vz = hmd.vz;
  out.wx = hmd.wx;
  out.wy = hmd.wy;
  out.wz = hmd.wz;

  out.tracking_status = hmd.tracking_status;
  out.confidence = hmd.confidence;
  out.source_tracking_status = hmd.tracking_status;
  out.source_flags = hmd.flags;

  uint32_t flags = 0;
  if (hmd_valid && (hmd.flags & HMD_FLAG_POSE_VALID) != 0u) {
    flags |= RUNTIME_HMD_FLAG_POSE_VALID;
  }
  if ((hmd.flags & HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
    flags |= RUNTIME_HMD_FLAG_LINEAR_VELOCITY_VALID;
  }
  if ((hmd.flags & HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) {
    flags |= RUNTIME_HMD_FLAG_ANGULAR_VELOCITY_VALID;
  }
  if (predicted) flags |= RUNTIME_HMD_FLAG_PREDICTED;
  if (origin_applied) flags |= RUNTIME_HMD_FLAG_ORIGIN_APPLIED;
  flags |= RUNTIME_HMD_FLAG_RUNTIME_FRAME;
  if (stale_as_lost) flags |= RUNTIME_HMD_FLAG_STALE_AS_LOST;
  out.flags = flags;

  return out;
}

inline std::string runtime_hmd_flags_summary(uint32_t flags) {
  std::string out;
  auto add = [&](const char* s) {
    if (!out.empty()) out += ",";
    out += s;
  };
  if ((flags & RUNTIME_HMD_FLAG_POSE_VALID) != 0u) add("pose");
  if ((flags & RUNTIME_HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) add("linvel");
  if ((flags & RUNTIME_HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) add("angvel");
  if ((flags & RUNTIME_HMD_FLAG_PREDICTED) != 0u) add("predicted");
  if ((flags & RUNTIME_HMD_FLAG_ORIGIN_APPLIED) != 0u) add("origin");
  if ((flags & RUNTIME_HMD_FLAG_RUNTIME_FRAME) != 0u) add("runtime_frame");
  if ((flags & RUNTIME_HMD_FLAG_STALE_AS_LOST) != 0u) add("stale_as_lost");
  return out.empty() ? "none" : out;
}

}  // namespace xr_runtime
