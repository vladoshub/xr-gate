#pragma once

#include <cstdint>

namespace xr_tracking {

enum class HandTrackingStatus : uint32_t {
  NoHands = 0,
  Tracking = 1,
  Lost = 2,
  Degraded = 3,
};

enum HandTrackingFlags : uint32_t {
  HAND_FLAG_LEFT_VALID = 1u << 0,
  HAND_FLAG_RIGHT_VALID = 1u << 1,
  HAND_FLAG_JOINTS_VALID = 1u << 2,
  HAND_FLAG_GESTURES_VALID = 1u << 3,
};

enum HandPoseFlags : uint32_t {
  HAND_POSE_VALID = 1u << 0,
  HAND_LINEAR_VELOCITY_VALID = 1u << 1,
  HAND_ANGULAR_VELOCITY_VALID = 1u << 2,
  HAND_JOINTS_VALID = 1u << 3,
  HAND_PINCH_VALID = 1u << 4,
  HAND_GRAB_VALID = 1u << 5,
};

constexpr uint32_t HAND_JOINT_COUNT_V2 = 21;
constexpr uint32_t HAND_TRACKING_FORMAT_VERSION_V2 = 2;

#pragma pack(push, 1)

struct HandJointF32V2 {
  uint32_t joint_id = 0;
  uint32_t flags = 0;
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
  float qw = 1.0f;
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float radius_m = 0.0f;
  float confidence = 0.0f;
};

struct HandSideF32V2 {
  uint32_t handedness = 0; // 1=left, 2=right
  uint32_t status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  uint32_t flags = 0;
  float confidence = 0.0f;

  float controller_px = 0.0f;
  float controller_py = 0.0f;
  float controller_pz = 0.0f;
  float controller_qw = 1.0f;
  float controller_qx = 0.0f;
  float controller_qy = 0.0f;
  float controller_qz = 0.0f;

  float palm_px = 0.0f;
  float palm_py = 0.0f;
  float palm_pz = 0.0f;
  float palm_qw = 1.0f;
  float palm_qx = 0.0f;
  float palm_qy = 0.0f;
  float palm_qz = 0.0f;

  float wrist_px = 0.0f;
  float wrist_py = 0.0f;
  float wrist_pz = 0.0f;
  float wrist_qw = 1.0f;
  float wrist_qx = 0.0f;
  float wrist_qy = 0.0f;
  float wrist_qz = 0.0f;

  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  float wx = 0.0f;
  float wy = 0.0f;
  float wz = 0.0f;

  float pinch_strength = 0.0f;
  float grab_strength = 0.0f;
  uint32_t pinch_active = 0;
  uint32_t grab_active = 0;

  uint32_t joint_count = HAND_JOINT_COUNT_V2;
  uint32_t reserved0 = 0;

  // 21-joint hand tracking order:
  // 0 wrist, 1..4 thumb, 5..8 index, 9..12 middle,
  // 13..16 ring, 17..20 little.
  HandJointF32V2 joints[HAND_JOINT_COUNT_V2];
};

struct HandTrackingFrameF32V2 {
  uint32_t version = HAND_TRACKING_FORMAT_VERSION_V2;
  uint32_t size_bytes = sizeof(HandTrackingFrameF32V2);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t tracking_status = static_cast<uint32_t>(HandTrackingStatus::NoHands);
  uint32_t flags = 0;
  float confidence = 0.0f;
  uint32_t hand_count = 0;

  HandSideF32V2 left;
  HandSideF32V2 right;
};

struct TrackingRingHeaderV1 {
  char magic[8] = {'H', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = 4096;
  uint32_t slot_count = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_header_size = 128;
  uint32_t payload_size = sizeof(HandTrackingFrameF32V2);
  uint32_t reserved0 = 0;
  uint64_t latest_sequence = 0;
};

struct TrackingSlotHeaderV1 {
  uint64_t seq_begin = 0; // odd while writer is active, even when committed
  uint64_t seq_end = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint32_t payload_size = sizeof(HandTrackingFrameF32V2);
  uint32_t flags = 0;
  uint8_t reserved[88] = {};
};

#pragma pack(pop)

static_assert(sizeof(TrackingSlotHeaderV1) == 128, "HAND_TRACKING slot header must be 128 bytes");
static_assert(sizeof(HandJointF32V2) == 44, "HAND_TRACKING_21_JOINT_F32_V2 joint must be 44 bytes");
static_assert(sizeof(HandSideF32V2) == 1072, "HAND_TRACKING_21_JOINT_F32_V2 side must be 1072 bytes");
static_assert(sizeof(HandTrackingFrameF32V2) == 2200, "HAND_TRACKING_21_JOINT_F32_V2 frame must be 2200 bytes");

}  // namespace xr_tracking
