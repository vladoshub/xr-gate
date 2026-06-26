#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace xr_runtime {

struct Vec3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Quatd {
  double w = 1.0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Posed {
  Vec3d p;
  Quatd q;
};

#pragma pack(push, 1)

struct RingHeaderV1 {
  char magic[8];
  uint32_t version;
  uint32_t header_size;
  uint32_t slot_count;
  uint32_t slot_stride;
  uint32_t slot_header_size;
  uint32_t payload_size;
  uint32_t reserved0;
  uint64_t latest_sequence;
};

struct RingSlotHeaderV1 {
  uint64_t seq_begin;
  uint64_t seq_end;
  uint64_t timestamp_ns;
  uint64_t source_timestamp_ns;
  uint32_t payload_size;
  uint32_t flags;
  uint8_t reserved[88];
};

struct HmdPoseF64V1 {
  uint32_t version;
  uint32_t size_bytes;

  uint64_t sequence;
  uint64_t timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t reset_counter;

  double px;
  double py;
  double pz;
  double qw;
  double qx;
  double qy;
  double qz;

  double vx;
  double vy;
  double vz;
  double wx;
  double wy;
  double wz;

  uint32_t tracking_status;
  uint32_t flags;
  float confidence;
  uint32_t reserved0;
};

struct HandJointF64V1 {
  uint32_t joint_id;
  uint32_t flags;
  double px;
  double py;
  double pz;
  double qw;
  double qx;
  double qy;
  double qz;
  float radius_m;
  float confidence;
  uint32_t reserved0;
};

constexpr uint32_t HAND_JOINT_COUNT_V1 = 26;

struct HandSideF64V1 {
  uint32_t handedness; // 1=left, 2=right
  uint32_t status;
  uint32_t flags;
  float confidence;

  double palm_px;
  double palm_py;
  double palm_pz;
  double palm_qw;
  double palm_qx;
  double palm_qy;
  double palm_qz;

  double wrist_px;
  double wrist_py;
  double wrist_pz;
  double wrist_qw;
  double wrist_qx;
  double wrist_qy;
  double wrist_qz;

  double vx;
  double vy;
  double vz;
  double wx;
  double wy;
  double wz;

  float pinch_strength;
  float grab_strength;
  uint32_t pinch_active;
  uint32_t grab_active;

  uint32_t joint_count;
  uint32_t reserved0;

  HandJointF64V1 joints[HAND_JOINT_COUNT_V1];
};

struct HandTrackingFrameF64V1 {
  uint32_t version;
  uint32_t size_bytes;

  uint64_t sequence;
  uint64_t timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t reset_counter;

  uint32_t tracking_status;
  uint32_t flags;
  float confidence;
  uint32_t hand_count;

  HandSideF64V1 left;
  HandSideF64V1 right;
};


constexpr uint32_t HAND_JOINT_COUNT_V2 = 21;

struct HandJointF32V2 {
  uint32_t joint_id;
  uint32_t flags;
  float px;
  float py;
  float pz;
  float qw;
  float qx;
  float qy;
  float qz;
  float radius_m;
  float confidence;
};

struct HandSideF32V2 {
  uint32_t handedness; // 1=left, 2=right
  uint32_t status;
  uint32_t flags;
  float confidence;

  float controller_px;
  float controller_py;
  float controller_pz;
  float controller_qw;
  float controller_qx;
  float controller_qy;
  float controller_qz;

  float palm_px;
  float palm_py;
  float palm_pz;
  float palm_qw;
  float palm_qx;
  float palm_qy;
  float palm_qz;

  float wrist_px;
  float wrist_py;
  float wrist_pz;
  float wrist_qw;
  float wrist_qx;
  float wrist_qy;
  float wrist_qz;

  float vx;
  float vy;
  float vz;
  float wx;
  float wy;
  float wz;

  float pinch_strength;
  float grab_strength;
  uint32_t pinch_active;
  uint32_t grab_active;

  uint32_t joint_count;
  uint32_t reserved0;

  HandJointF32V2 joints[HAND_JOINT_COUNT_V2];
};

struct HandTrackingFrameF32V2 {
  uint32_t version;
  uint32_t size_bytes;

  uint64_t sequence;
  uint64_t timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t reset_counter;

  uint32_t tracking_status;
  uint32_t flags;
  float confidence;
  uint32_t hand_count;

  HandSideF32V2 left;
  HandSideF32V2 right;
};

#pragma pack(pop)

static_assert(sizeof(RingSlotHeaderV1) == 128, "ring slot header must be 128 bytes");
static_assert(sizeof(HmdPoseF64V1) == 160, "HMD pose payload must be 160 bytes");
static_assert(sizeof(HandJointF32V2) == 44, "HAND_TRACKING_21_JOINT_F32_V2 joint must be 44 bytes");
static_assert(sizeof(HandSideF32V2) == 1072, "HAND_TRACKING_21_JOINT_F32_V2 side must be 1072 bytes");
static_assert(sizeof(HandTrackingFrameF32V2) == 2200, "HAND_TRACKING_21_JOINT_F32_V2 frame must be 2200 bytes");

constexpr uint32_t HMD_FLAG_POSE_VALID = 1u << 0;
constexpr uint32_t HMD_FLAG_LINEAR_VELOCITY_VALID = 1u << 1;
constexpr uint32_t HMD_FLAG_ANGULAR_VELOCITY_VALID = 1u << 2;

constexpr uint32_t HAND_TRACKING_FORMAT_VERSION_V1 = 1;
constexpr uint32_t HAND_TRACKING_FORMAT_VERSION_V2 = 2;

constexpr uint32_t HAND_FLAG_LEFT_VALID = 1u << 0;
constexpr uint32_t HAND_FLAG_RIGHT_VALID = 1u << 1;
constexpr uint32_t HAND_FLAG_JOINTS_VALID = 1u << 2;
constexpr uint32_t HAND_FLAG_GESTURES_VALID = 1u << 3;

constexpr uint32_t HAND_POSE_VALID = 1u << 0;
constexpr uint32_t HAND_LINEAR_VELOCITY_VALID = 1u << 1;
constexpr uint32_t HAND_ANGULAR_VELOCITY_VALID = 1u << 2;
constexpr uint32_t HAND_JOINTS_VALID = 1u << 3;
constexpr uint32_t HAND_PINCH_VALID = 1u << 4;
constexpr uint32_t HAND_GRAB_VALID = 1u << 5;

inline uint32_t load_u32_le(const void* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint64_t load_u64_le(const void* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline std::string hmd_status_name(uint32_t s) {
  switch (s) {
    case 0: return "invalid";
    case 1: return "initializing";
    case 2: return "tracking";
    case 3: return "degraded";
    case 4: return "lost";
    default: return "unknown";
  }
}

inline std::string hand_status_name(uint32_t s) {
  switch (s) {
    case 0: return "no_hands";
    case 1: return "tracking";
    case 2: return "lost";
    case 3: return "degraded";
    default: return "unknown";
  }
}

}  // namespace xr_runtime
