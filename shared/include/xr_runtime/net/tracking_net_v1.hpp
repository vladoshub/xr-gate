#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <xr_tracking/types/tracking_types.hpp>

namespace xr_runtime::tracking_net_v1 {

#pragma pack(push, 1)

struct PacketHeader {
  char magic[4];          // "XTRK"
  uint16_t version;       // 1, little-endian on wire
  uint16_t header_size;   // sizeof(PacketHeader), little-endian on wire
  uint16_t packet_type;   // PacketType, little-endian on wire
  uint16_t flags;         // reserved for now, little-endian on wire
  uint32_t payload_size;  // little-endian on wire

  uint64_t transport_sequence;
  uint64_t stream_sequence;

  uint64_t send_timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t reset_counter;

  uint32_t reserved0;
  uint32_t reserved1;
};

struct HandSideSummaryF64V1 {
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
};

struct HandSummaryF64V1 {
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

  HandSideSummaryF64V1 left;
  HandSideSummaryF64V1 right;
};


struct HandSideSummaryF32V2 {
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
};

struct HandSummaryF32V2 {
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

  HandSideSummaryF32V2 left;
  HandSideSummaryF32V2 right;
};

struct HandJointsF32V2 {
  uint32_t version;
  uint32_t size_bytes;

  uint64_t sequence;
  uint64_t timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t reset_counter;

  uint32_t handedness;
  uint32_t status;
  uint32_t flags;
  float confidence;
  uint32_t joint_count;
  uint32_t reserved0;

  HandJointF32V2 joints[HAND_JOINT_COUNT_V2];
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 64, "tracking_net_v1 PacketHeader must be 64 bytes");
static_assert(sizeof(HandSideSummaryF64V1) == 192, "HandSideSummaryF64V1 must be 192 bytes");
static_assert(sizeof(HandSummaryF64V1) == 440, "HandSummaryF64V1 must be 440 bytes");
static_assert(sizeof(HandSideSummaryF32V2) == 148, "HandSideSummaryF32V2 must be 148 bytes");
static_assert(sizeof(HandSummaryF32V2) == 352, "HandSummaryF32V2 must be 352 bytes");
static_assert(sizeof(HandJointsF32V2) == 988, "HandJointsF32V2 must be 988 bytes");
static_assert(sizeof(HandTrackingFrameF32V2) == 2200, "HandTrackingFrameF32V2 must be 2200 bytes");
static_assert(sizeof(float) == 4, "tracking_net_v1 requires IEEE-754 binary32 float layout");
static_assert(sizeof(double) == 8, "tracking_net_v1 requires IEEE-754 binary64 double layout");

// Wire format note:
//   tracking_net_v1 is an explicitly little-endian packed binary protocol.
//   That is safe for x86_64 and normal ARM64 little-endian hosts, and still
//   decodes correctly on big-endian hosts because all network I/O goes through
//   the encode/decode helpers below instead of raw host-endian struct copies.

enum class PacketType : uint16_t {
  HmdPoseF64 = 1,
  HandSummaryF64 = 2,
  HandSummaryF32V2 = 3,
  HandJointsF32V2 = 4,
  HandFullF32V2 = 5, // SHM-sized diagnostic packet; normally too large for MTU-safe UDP.
  Heartbeat = 255,
};

constexpr uint16_t VERSION = 1;
constexpr char MAGIC[4] = {'X', 'T', 'R', 'K'};

template <typename PacketT>
using PacketBytes = std::array<uint8_t, sizeof(PacketT)>;

inline void ensure_room(const uint8_t* begin, const uint8_t* p, const uint8_t* end, size_t n) {
  (void)begin;
  if (p > end || static_cast<size_t>(end - p) < n) {
    throw std::runtime_error("tracking_net_v1 buffer overflow/underflow");
  }
}

inline void write_u8(uint8_t*& p, const uint8_t* end, uint8_t v) {
  ensure_room(nullptr, p, end, 1);
  *p++ = v;
}

inline uint8_t read_u8(const uint8_t*& p, const uint8_t* end) {
  ensure_room(nullptr, p, end, 1);
  return *p++;
}

inline void write_bytes(uint8_t*& p, const uint8_t* end, const void* src, size_t n) {
  ensure_room(nullptr, p, end, n);
  std::memcpy(p, src, n);
  p += n;
}

inline void read_bytes(const uint8_t*& p, const uint8_t* end, void* dst, size_t n) {
  ensure_room(nullptr, p, end, n);
  std::memcpy(dst, p, n);
  p += n;
}

inline void write_u16_le(uint8_t*& p, const uint8_t* end, uint16_t v) {
  ensure_room(nullptr, p, end, 2);
  p[0] = static_cast<uint8_t>(v & 0xffu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  p += 2;
}

inline uint16_t read_u16_le(const uint8_t*& p, const uint8_t* end) {
  ensure_room(nullptr, p, end, 2);
  uint16_t v = static_cast<uint16_t>(p[0]) |
               static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
  p += 2;
  return v;
}

inline void write_u32_le(uint8_t*& p, const uint8_t* end, uint32_t v) {
  ensure_room(nullptr, p, end, 4);
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
  p += 4;
}

inline uint32_t read_u32_le(const uint8_t*& p, const uint8_t* end) {
  ensure_room(nullptr, p, end, 4);
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  p += 4;
  return v;
}

inline void write_u64_le(uint8_t*& p, const uint8_t* end, uint64_t v) {
  ensure_room(nullptr, p, end, 8);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
  p += 8;
}

inline uint64_t read_u64_le(const uint8_t*& p, const uint8_t* end) {
  ensure_room(nullptr, p, end, 8);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  p += 8;
  return v;
}

inline void write_f32_le(uint8_t*& p, const uint8_t* end, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  write_u32_le(p, end, bits);
}

inline float read_f32_le(const uint8_t*& p, const uint8_t* end) {
  const uint32_t bits = read_u32_le(p, end);
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

inline void write_f64_le(uint8_t*& p, const uint8_t* end, double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  write_u64_le(p, end, bits);
}

inline double read_f64_le(const uint8_t*& p, const uint8_t* end) {
  const uint64_t bits = read_u64_le(p, end);
  double v = 0.0;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

inline PacketHeader make_header(PacketType type,
                                uint64_t transport_sequence,
                                uint64_t stream_sequence,
                                uint64_t send_timestamp_ns,
                                uint64_t source_timestamp_ns,
                                uint64_t reset_counter,
                                uint32_t payload_size) {
  PacketHeader h {};
  std::memcpy(h.magic, MAGIC, sizeof(MAGIC));
  h.version = VERSION;
  h.header_size = static_cast<uint16_t>(sizeof(PacketHeader));
  h.packet_type = static_cast<uint16_t>(type);
  h.flags = 0;
  h.payload_size = payload_size;
  h.transport_sequence = transport_sequence;
  h.stream_sequence = stream_sequence;
  h.send_timestamp_ns = send_timestamp_ns;
  h.source_timestamp_ns = source_timestamp_ns;
  h.reset_counter = reset_counter;
  return h;
}

inline bool magic_ok(const PacketHeader& h) {
  return std::memcmp(h.magic, MAGIC, sizeof(MAGIC)) == 0;
}

inline void encode_header(uint8_t*& p, const uint8_t* end, const PacketHeader& h) {
  write_bytes(p, end, h.magic, sizeof(h.magic));
  write_u16_le(p, end, h.version);
  write_u16_le(p, end, h.header_size);
  write_u16_le(p, end, h.packet_type);
  write_u16_le(p, end, h.flags);
  write_u32_le(p, end, h.payload_size);
  write_u64_le(p, end, h.transport_sequence);
  write_u64_le(p, end, h.stream_sequence);
  write_u64_le(p, end, h.send_timestamp_ns);
  write_u64_le(p, end, h.source_timestamp_ns);
  write_u64_le(p, end, h.reset_counter);
  write_u32_le(p, end, h.reserved0);
  write_u32_le(p, end, h.reserved1);
}

inline PacketHeader decode_header(const uint8_t* data, size_t size) {
  if (size < sizeof(PacketHeader)) throw std::runtime_error("short packet header");
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  PacketHeader h {};
  read_bytes(p, end, h.magic, sizeof(h.magic));
  h.version = read_u16_le(p, end);
  h.header_size = read_u16_le(p, end);
  h.packet_type = read_u16_le(p, end);
  h.flags = read_u16_le(p, end);
  h.payload_size = read_u32_le(p, end);
  h.transport_sequence = read_u64_le(p, end);
  h.stream_sequence = read_u64_le(p, end);
  h.send_timestamp_ns = read_u64_le(p, end);
  h.source_timestamp_ns = read_u64_le(p, end);
  h.reset_counter = read_u64_le(p, end);
  h.reserved0 = read_u32_le(p, end);
  h.reserved1 = read_u32_le(p, end);
  return h;
}

inline void encode_hmd_pose_payload(uint8_t*& p, const uint8_t* end, const HmdPoseF64V1& v) {
  write_u32_le(p, end, v.version);
  write_u32_le(p, end, v.size_bytes);
  write_u64_le(p, end, v.sequence);
  write_u64_le(p, end, v.timestamp_ns);
  write_u64_le(p, end, v.source_timestamp_ns);
  write_u64_le(p, end, v.reset_counter);
  write_f64_le(p, end, v.px); write_f64_le(p, end, v.py); write_f64_le(p, end, v.pz);
  write_f64_le(p, end, v.qw); write_f64_le(p, end, v.qx); write_f64_le(p, end, v.qy); write_f64_le(p, end, v.qz);
  write_f64_le(p, end, v.vx); write_f64_le(p, end, v.vy); write_f64_le(p, end, v.vz);
  write_f64_le(p, end, v.wx); write_f64_le(p, end, v.wy); write_f64_le(p, end, v.wz);
  write_u32_le(p, end, v.tracking_status);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.confidence);
  write_u32_le(p, end, v.reserved0);
}

inline HmdPoseF64V1 decode_hmd_pose_payload(const uint8_t* data, size_t size) {
  if (size < sizeof(HmdPoseF64V1)) throw std::runtime_error("short HMD payload");
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  HmdPoseF64V1 v {};
  v.version = read_u32_le(p, end);
  v.size_bytes = read_u32_le(p, end);
  v.sequence = read_u64_le(p, end);
  v.timestamp_ns = read_u64_le(p, end);
  v.source_timestamp_ns = read_u64_le(p, end);
  v.reset_counter = read_u64_le(p, end);
  v.px = read_f64_le(p, end); v.py = read_f64_le(p, end); v.pz = read_f64_le(p, end);
  v.qw = read_f64_le(p, end); v.qx = read_f64_le(p, end); v.qy = read_f64_le(p, end); v.qz = read_f64_le(p, end);
  v.vx = read_f64_le(p, end); v.vy = read_f64_le(p, end); v.vz = read_f64_le(p, end);
  v.wx = read_f64_le(p, end); v.wy = read_f64_le(p, end); v.wz = read_f64_le(p, end);
  v.tracking_status = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.confidence = read_f32_le(p, end);
  v.reserved0 = read_u32_le(p, end);
  return v;
}

inline void encode_hand_side_summary(uint8_t*& p, const uint8_t* end, const HandSideSummaryF64V1& v) {
  write_u32_le(p, end, v.handedness);
  write_u32_le(p, end, v.status);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.confidence);
  write_f64_le(p, end, v.palm_px); write_f64_le(p, end, v.palm_py); write_f64_le(p, end, v.palm_pz);
  write_f64_le(p, end, v.palm_qw); write_f64_le(p, end, v.palm_qx); write_f64_le(p, end, v.palm_qy); write_f64_le(p, end, v.palm_qz);
  write_f64_le(p, end, v.wrist_px); write_f64_le(p, end, v.wrist_py); write_f64_le(p, end, v.wrist_pz);
  write_f64_le(p, end, v.wrist_qw); write_f64_le(p, end, v.wrist_qx); write_f64_le(p, end, v.wrist_qy); write_f64_le(p, end, v.wrist_qz);
  write_f64_le(p, end, v.vx); write_f64_le(p, end, v.vy); write_f64_le(p, end, v.vz);
  write_f64_le(p, end, v.wx); write_f64_le(p, end, v.wy); write_f64_le(p, end, v.wz);
  write_f32_le(p, end, v.pinch_strength);
  write_f32_le(p, end, v.grab_strength);
  write_u32_le(p, end, v.pinch_active);
  write_u32_le(p, end, v.grab_active);
}

inline HandSideSummaryF64V1 decode_hand_side_summary(const uint8_t*& p, const uint8_t* end) {
  HandSideSummaryF64V1 v {};
  v.handedness = read_u32_le(p, end);
  v.status = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.confidence = read_f32_le(p, end);
  v.palm_px = read_f64_le(p, end); v.palm_py = read_f64_le(p, end); v.palm_pz = read_f64_le(p, end);
  v.palm_qw = read_f64_le(p, end); v.palm_qx = read_f64_le(p, end); v.palm_qy = read_f64_le(p, end); v.palm_qz = read_f64_le(p, end);
  v.wrist_px = read_f64_le(p, end); v.wrist_py = read_f64_le(p, end); v.wrist_pz = read_f64_le(p, end);
  v.wrist_qw = read_f64_le(p, end); v.wrist_qx = read_f64_le(p, end); v.wrist_qy = read_f64_le(p, end); v.wrist_qz = read_f64_le(p, end);
  v.vx = read_f64_le(p, end); v.vy = read_f64_le(p, end); v.vz = read_f64_le(p, end);
  v.wx = read_f64_le(p, end); v.wy = read_f64_le(p, end); v.wz = read_f64_le(p, end);
  v.pinch_strength = read_f32_le(p, end);
  v.grab_strength = read_f32_le(p, end);
  v.pinch_active = read_u32_le(p, end);
  v.grab_active = read_u32_le(p, end);
  return v;
}

inline void encode_hand_summary_payload(uint8_t*& p, const uint8_t* end, const HandSummaryF64V1& v) {
  write_u32_le(p, end, v.version);
  write_u32_le(p, end, v.size_bytes);
  write_u64_le(p, end, v.sequence);
  write_u64_le(p, end, v.timestamp_ns);
  write_u64_le(p, end, v.source_timestamp_ns);
  write_u64_le(p, end, v.reset_counter);
  write_u32_le(p, end, v.tracking_status);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.confidence);
  write_u32_le(p, end, v.hand_count);
  encode_hand_side_summary(p, end, v.left);
  encode_hand_side_summary(p, end, v.right);
}

inline HandSummaryF64V1 decode_hand_summary_payload(const uint8_t* data, size_t size) {
  if (size < sizeof(HandSummaryF64V1)) throw std::runtime_error("short hand summary payload");
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  HandSummaryF64V1 v {};
  v.version = read_u32_le(p, end);
  v.size_bytes = read_u32_le(p, end);
  v.sequence = read_u64_le(p, end);
  v.timestamp_ns = read_u64_le(p, end);
  v.source_timestamp_ns = read_u64_le(p, end);
  v.reset_counter = read_u64_le(p, end);
  v.tracking_status = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.confidence = read_f32_le(p, end);
  v.hand_count = read_u32_le(p, end);
  v.left = decode_hand_side_summary(p, end);
  v.right = decode_hand_side_summary(p, end);
  return v;
}

inline HandJointF32V2 decode_hand_joint_f32_v2(const uint8_t*& p, const uint8_t* end) {
  HandJointF32V2 v {};
  v.joint_id = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.px = read_f32_le(p, end); v.py = read_f32_le(p, end); v.pz = read_f32_le(p, end);
  v.qw = read_f32_le(p, end); v.qx = read_f32_le(p, end); v.qy = read_f32_le(p, end); v.qz = read_f32_le(p, end);
  v.radius_m = read_f32_le(p, end);
  v.confidence = read_f32_le(p, end);
  return v;
}

inline HandSideSummaryF32V2 decode_hand_side_summary_f32_v2(const uint8_t*& p, const uint8_t* end) {
  HandSideSummaryF32V2 v {};
  v.handedness = read_u32_le(p, end);
  v.status = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.confidence = read_f32_le(p, end);
  v.controller_px = read_f32_le(p, end); v.controller_py = read_f32_le(p, end); v.controller_pz = read_f32_le(p, end);
  v.controller_qw = read_f32_le(p, end); v.controller_qx = read_f32_le(p, end); v.controller_qy = read_f32_le(p, end); v.controller_qz = read_f32_le(p, end);
  v.palm_px = read_f32_le(p, end); v.palm_py = read_f32_le(p, end); v.palm_pz = read_f32_le(p, end);
  v.palm_qw = read_f32_le(p, end); v.palm_qx = read_f32_le(p, end); v.palm_qy = read_f32_le(p, end); v.palm_qz = read_f32_le(p, end);
  v.wrist_px = read_f32_le(p, end); v.wrist_py = read_f32_le(p, end); v.wrist_pz = read_f32_le(p, end);
  v.wrist_qw = read_f32_le(p, end); v.wrist_qx = read_f32_le(p, end); v.wrist_qy = read_f32_le(p, end); v.wrist_qz = read_f32_le(p, end);
  v.vx = read_f32_le(p, end); v.vy = read_f32_le(p, end); v.vz = read_f32_le(p, end);
  v.wx = read_f32_le(p, end); v.wy = read_f32_le(p, end); v.wz = read_f32_le(p, end);
  v.pinch_strength = read_f32_le(p, end);
  v.grab_strength = read_f32_le(p, end);
  v.pinch_active = read_u32_le(p, end);
  v.grab_active = read_u32_le(p, end);
  v.joint_count = read_u32_le(p, end);
  v.reserved0 = read_u32_le(p, end);
  return v;
}

inline HandSummaryF32V2 decode_hand_summary_f32_v2_payload(const uint8_t* data, size_t size) {
  if (size < sizeof(HandSummaryF32V2)) throw std::runtime_error("short HAND_SUMMARY_F32_V2 payload");
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  HandSummaryF32V2 v {};
  v.version = read_u32_le(p, end);
  v.size_bytes = read_u32_le(p, end);
  v.sequence = read_u64_le(p, end);
  v.timestamp_ns = read_u64_le(p, end);
  v.source_timestamp_ns = read_u64_le(p, end);
  v.reset_counter = read_u64_le(p, end);
  v.tracking_status = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.confidence = read_f32_le(p, end);
  v.hand_count = read_u32_le(p, end);
  v.left = decode_hand_side_summary_f32_v2(p, end);
  v.right = decode_hand_side_summary_f32_v2(p, end);
  return v;
}

inline HandJointsF32V2 decode_hand_joints_f32_v2_payload(const uint8_t* data, size_t size) {
  if (size < sizeof(HandJointsF32V2)) throw std::runtime_error("short HAND_JOINTS_F32_V2 payload");
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  HandJointsF32V2 v {};
  v.version = read_u32_le(p, end);
  v.size_bytes = read_u32_le(p, end);
  v.sequence = read_u64_le(p, end);
  v.timestamp_ns = read_u64_le(p, end);
  v.source_timestamp_ns = read_u64_le(p, end);
  v.reset_counter = read_u64_le(p, end);
  v.handedness = read_u32_le(p, end);
  v.status = read_u32_le(p, end);
  v.flags = read_u32_le(p, end);
  v.confidence = read_f32_le(p, end);
  v.joint_count = read_u32_le(p, end);
  v.reserved0 = read_u32_le(p, end);
  for (uint32_t i = 0; i < HAND_JOINT_COUNT_V2; ++i) {
    v.joints[i] = decode_hand_joint_f32_v2(p, end);
  }
  return v;
}

inline HandTrackingFrameF32V2 decode_hand_full_f32_v2_payload(const uint8_t* data, size_t size) {
  if (size < sizeof(HandTrackingFrameF32V2)) throw std::runtime_error("short HAND_FULL_F32_V2 payload");
  HandTrackingFrameF32V2 v {};
  std::memcpy(&v, data, sizeof(v));
  return v;
}


struct HmdPosePacket {
  PacketHeader header;
  HmdPoseF64V1 payload;
};

struct HandSummaryPacket {
  PacketHeader header;
  HandSummaryF64V1 payload;
};

struct HandSummaryV2Packet {
  PacketHeader header;
  HandSummaryF32V2 payload;
};

struct HandJointsV2Packet {
  PacketHeader header;
  HandJointsF32V2 payload;
};

struct HandFullV2Packet {
  PacketHeader header;
  HandTrackingFrameF32V2 payload;
};

static_assert(sizeof(HmdPosePacket) == sizeof(PacketHeader) + sizeof(HmdPoseF64V1),
              "unexpected HmdPosePacket size");
static_assert(sizeof(HandSummaryPacket) == sizeof(PacketHeader) + sizeof(HandSummaryF64V1),
              "unexpected HandSummaryPacket size");
static_assert(sizeof(HandSummaryV2Packet) == sizeof(PacketHeader) + sizeof(HandSummaryF32V2),
              "unexpected HandSummaryV2Packet size");
static_assert(sizeof(HandJointsV2Packet) == sizeof(PacketHeader) + sizeof(HandJointsF32V2),
              "unexpected HandJointsV2Packet size");
static_assert(sizeof(HandFullV2Packet) == sizeof(PacketHeader) + sizeof(HandTrackingFrameF32V2),
              "unexpected HandFullV2Packet size");

constexpr size_t UDP_SAFE_DATAGRAM_BYTES = 1200;
constexpr size_t HAND_SUMMARY_V2_PACKET_BYTES = sizeof(HandSummaryV2Packet);
constexpr size_t HAND_JOINTS_V2_PACKET_BYTES = sizeof(HandJointsV2Packet);
constexpr size_t HAND_FULL_V2_PACKET_BYTES = sizeof(HandFullV2Packet);
static_assert(HAND_SUMMARY_V2_PACKET_BYTES <= UDP_SAFE_DATAGRAM_BYTES,
              "HAND_TRACKING_21_JOINT_F32_V2 summary packet should fit the safe UDP budget");
static_assert(HAND_JOINTS_V2_PACKET_BYTES <= UDP_SAFE_DATAGRAM_BYTES,
              "HAND_TRACKING_21_JOINT_F32_V2 per-hand joints packet should fit the safe UDP budget");

inline PacketBytes<HmdPosePacket> encode_hmd_pose_packet(const HmdPosePacket& packet) {
  PacketBytes<HmdPosePacket> out {};
  uint8_t* p = out.data();
  const uint8_t* end = out.data() + out.size();
  encode_header(p, end, packet.header);
  encode_hmd_pose_payload(p, end, packet.payload);
  return out;
}

inline PacketBytes<HandSummaryPacket> encode_hand_summary_packet(const HandSummaryPacket& packet) {
  PacketBytes<HandSummaryPacket> out {};
  uint8_t* p = out.data();
  const uint8_t* end = out.data() + out.size();
  encode_header(p, end, packet.header);
  encode_hand_summary_payload(p, end, packet.payload);
  return out;
}

inline void encode_hand_joint_f32_v2(uint8_t*& p, const uint8_t* end, const HandJointF32V2& v) {
  write_u32_le(p, end, v.joint_id);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.px); write_f32_le(p, end, v.py); write_f32_le(p, end, v.pz);
  write_f32_le(p, end, v.qw); write_f32_le(p, end, v.qx); write_f32_le(p, end, v.qy); write_f32_le(p, end, v.qz);
  write_f32_le(p, end, v.radius_m);
  write_f32_le(p, end, v.confidence);
}

inline void encode_hand_side_summary_f32_v2(uint8_t*& p, const uint8_t* end, const HandSideSummaryF32V2& v) {
  write_u32_le(p, end, v.handedness);
  write_u32_le(p, end, v.status);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.confidence);
  write_f32_le(p, end, v.controller_px); write_f32_le(p, end, v.controller_py); write_f32_le(p, end, v.controller_pz);
  write_f32_le(p, end, v.controller_qw); write_f32_le(p, end, v.controller_qx); write_f32_le(p, end, v.controller_qy); write_f32_le(p, end, v.controller_qz);
  write_f32_le(p, end, v.palm_px); write_f32_le(p, end, v.palm_py); write_f32_le(p, end, v.palm_pz);
  write_f32_le(p, end, v.palm_qw); write_f32_le(p, end, v.palm_qx); write_f32_le(p, end, v.palm_qy); write_f32_le(p, end, v.palm_qz);
  write_f32_le(p, end, v.wrist_px); write_f32_le(p, end, v.wrist_py); write_f32_le(p, end, v.wrist_pz);
  write_f32_le(p, end, v.wrist_qw); write_f32_le(p, end, v.wrist_qx); write_f32_le(p, end, v.wrist_qy); write_f32_le(p, end, v.wrist_qz);
  write_f32_le(p, end, v.vx); write_f32_le(p, end, v.vy); write_f32_le(p, end, v.vz);
  write_f32_le(p, end, v.wx); write_f32_le(p, end, v.wy); write_f32_le(p, end, v.wz);
  write_f32_le(p, end, v.pinch_strength);
  write_f32_le(p, end, v.grab_strength);
  write_u32_le(p, end, v.pinch_active);
  write_u32_le(p, end, v.grab_active);
  write_u32_le(p, end, v.joint_count);
  write_u32_le(p, end, v.reserved0);
}

inline void encode_hand_summary_f32_v2_payload(uint8_t*& p, const uint8_t* end, const HandSummaryF32V2& v) {
  write_u32_le(p, end, v.version);
  write_u32_le(p, end, v.size_bytes);
  write_u64_le(p, end, v.sequence);
  write_u64_le(p, end, v.timestamp_ns);
  write_u64_le(p, end, v.source_timestamp_ns);
  write_u64_le(p, end, v.reset_counter);
  write_u32_le(p, end, v.tracking_status);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.confidence);
  write_u32_le(p, end, v.hand_count);
  encode_hand_side_summary_f32_v2(p, end, v.left);
  encode_hand_side_summary_f32_v2(p, end, v.right);
}

inline void encode_hand_joints_f32_v2_payload(uint8_t*& p, const uint8_t* end, const HandJointsF32V2& v) {
  write_u32_le(p, end, v.version);
  write_u32_le(p, end, v.size_bytes);
  write_u64_le(p, end, v.sequence);
  write_u64_le(p, end, v.timestamp_ns);
  write_u64_le(p, end, v.source_timestamp_ns);
  write_u64_le(p, end, v.reset_counter);
  write_u32_le(p, end, v.handedness);
  write_u32_le(p, end, v.status);
  write_u32_le(p, end, v.flags);
  write_f32_le(p, end, v.confidence);
  write_u32_le(p, end, v.joint_count);
  write_u32_le(p, end, v.reserved0);
  for (uint32_t i = 0; i < HAND_JOINT_COUNT_V2; ++i) {
    encode_hand_joint_f32_v2(p, end, v.joints[i]);
  }
}

inline PacketBytes<HandSummaryV2Packet> encode_hand_summary_v2_packet(const HandSummaryV2Packet& packet) {
  PacketBytes<HandSummaryV2Packet> out {};
  uint8_t* p = out.data();
  const uint8_t* end = out.data() + out.size();
  encode_header(p, end, packet.header);
  encode_hand_summary_f32_v2_payload(p, end, packet.payload);
  return out;
}

inline PacketBytes<HandJointsV2Packet> encode_hand_joints_v2_packet(const HandJointsV2Packet& packet) {
  PacketBytes<HandJointsV2Packet> out {};
  uint8_t* p = out.data();
  const uint8_t* end = out.data() + out.size();
  encode_header(p, end, packet.header);
  encode_hand_joints_f32_v2_payload(p, end, packet.payload);
  return out;
}

inline HandSideSummaryF64V1 make_hand_side_summary(const HandSideF64V1& src) {
  HandSideSummaryF64V1 out {};
  out.handedness = src.handedness;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;

  out.palm_px = src.palm_px; out.palm_py = src.palm_py; out.palm_pz = src.palm_pz;
  out.palm_qw = src.palm_qw; out.palm_qx = src.palm_qx; out.palm_qy = src.palm_qy; out.palm_qz = src.palm_qz;
  out.wrist_px = src.wrist_px; out.wrist_py = src.wrist_py; out.wrist_pz = src.wrist_pz;
  out.wrist_qw = src.wrist_qw; out.wrist_qx = src.wrist_qx; out.wrist_qy = src.wrist_qy; out.wrist_qz = src.wrist_qz;
  out.vx = src.vx; out.vy = src.vy; out.vz = src.vz;
  out.wx = src.wx; out.wy = src.wy; out.wz = src.wz;
  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;
  return out;
}

inline HandSummaryF64V1 make_hand_summary(const HandTrackingFrameF64V1& src) {
  HandSummaryF64V1 out {};
  out.version = 1;
  out.size_bytes = sizeof(HandSummaryF64V1);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = make_hand_side_summary(src.left);
  out.right = make_hand_side_summary(src.right);
  return out;
}

inline HandSideSummaryF32V2 make_hand_side_summary_v2(const HandSideF32V2& src) {
  HandSideSummaryF32V2 out {};
  out.handedness = src.handedness;
  out.status = src.status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.controller_px = src.controller_px; out.controller_py = src.controller_py; out.controller_pz = src.controller_pz;
  out.controller_qw = src.controller_qw; out.controller_qx = src.controller_qx; out.controller_qy = src.controller_qy; out.controller_qz = src.controller_qz;
  out.palm_px = src.palm_px; out.palm_py = src.palm_py; out.palm_pz = src.palm_pz;
  out.palm_qw = src.palm_qw; out.palm_qx = src.palm_qx; out.palm_qy = src.palm_qy; out.palm_qz = src.palm_qz;
  out.wrist_px = src.wrist_px; out.wrist_py = src.wrist_py; out.wrist_pz = src.wrist_pz;
  out.wrist_qw = src.wrist_qw; out.wrist_qx = src.wrist_qx; out.wrist_qy = src.wrist_qy; out.wrist_qz = src.wrist_qz;
  out.vx = src.vx; out.vy = src.vy; out.vz = src.vz;
  out.wx = src.wx; out.wy = src.wy; out.wz = src.wz;
  out.pinch_strength = src.pinch_strength;
  out.grab_strength = src.grab_strength;
  out.pinch_active = src.pinch_active;
  out.grab_active = src.grab_active;
  out.joint_count = src.joint_count;
  out.reserved0 = src.reserved0;
  return out;
}

inline HandSummaryF32V2 make_hand_summary_v2(const HandTrackingFrameF32V2& src) {
  HandSummaryF32V2 out {};
  out.version = 2;
  out.size_bytes = sizeof(HandSummaryF32V2);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.tracking_status = src.tracking_status;
  out.flags = src.flags;
  out.confidence = src.confidence;
  out.hand_count = src.hand_count;
  out.left = make_hand_side_summary_v2(src.left);
  out.right = make_hand_side_summary_v2(src.right);
  return out;
}

inline HandJointsF32V2 make_hand_joints_v2(const HandTrackingFrameF32V2& src, bool right_hand) {
  const HandSideF32V2& hand = right_hand ? src.right : src.left;
  HandJointsF32V2 out {};
  out.version = 2;
  out.size_bytes = sizeof(HandJointsF32V2);
  out.sequence = src.sequence;
  out.timestamp_ns = src.timestamp_ns;
  out.source_timestamp_ns = src.source_timestamp_ns;
  out.reset_counter = src.reset_counter;
  out.handedness = hand.handedness;
  out.status = hand.status;
  out.flags = hand.flags;
  out.confidence = hand.confidence;
  out.joint_count = hand.joint_count;
  for (uint32_t i = 0; i < HAND_JOINT_COUNT_V2; ++i) {
    out.joints[i] = hand.joints[i];
  }
  return out;
}

inline void validate_common_header(const PacketHeader& h, size_t received_size) {
  if (!magic_ok(h)) {
    throw std::runtime_error("bad magic");
  }
  if (h.version != VERSION) {
    throw std::runtime_error("unsupported version " + std::to_string(h.version));
  }
  if (h.header_size != sizeof(PacketHeader)) {
    throw std::runtime_error("bad header_size " + std::to_string(h.header_size));
  }
  if (received_size < sizeof(PacketHeader) + h.payload_size) {
    throw std::runtime_error("short UDP datagram");
  }
}

inline void validate_hmd_pose_packet_header(const PacketHeader& h, size_t received_size) {
  validate_common_header(h, received_size);
  if (h.packet_type != static_cast<uint16_t>(PacketType::HmdPoseF64)) {
    throw std::runtime_error("unexpected packet_type " + std::to_string(h.packet_type));
  }
  if (h.payload_size != sizeof(HmdPoseF64V1)) {
    throw std::runtime_error("bad HMD payload_size " + std::to_string(h.payload_size));
  }
}

inline void validate_hand_summary_packet_header(const PacketHeader& h, size_t received_size) {
  validate_common_header(h, received_size);
  if (h.packet_type != static_cast<uint16_t>(PacketType::HandSummaryF64)) {
    throw std::runtime_error("unexpected packet_type " + std::to_string(h.packet_type));
  }
  if (h.payload_size != sizeof(HandSummaryF64V1)) {
    throw std::runtime_error("bad hand summary payload_size " + std::to_string(h.payload_size));
  }
}

inline void validate_hand_summary_v2_packet_header(const PacketHeader& h, size_t received_size) {
  validate_common_header(h, received_size);
  if (h.packet_type != static_cast<uint16_t>(PacketType::HandSummaryF32V2)) {
    throw std::runtime_error("unexpected packet_type " + std::to_string(h.packet_type));
  }
  if (h.payload_size != sizeof(HandSummaryF32V2)) {
    throw std::runtime_error("bad HAND_SUMMARY_F32_V2 payload_size " + std::to_string(h.payload_size));
  }
}

inline void validate_hand_joints_v2_packet_header(const PacketHeader& h, size_t received_size) {
  validate_common_header(h, received_size);
  if (h.packet_type != static_cast<uint16_t>(PacketType::HandJointsF32V2)) {
    throw std::runtime_error("unexpected packet_type " + std::to_string(h.packet_type));
  }
  if (h.payload_size != sizeof(HandJointsF32V2)) {
    throw std::runtime_error("bad HAND_JOINTS_F32_V2 payload_size " + std::to_string(h.payload_size));
  }
}

inline void validate_hand_full_v2_packet_header(const PacketHeader& h, size_t received_size) {
  validate_common_header(h, received_size);
  if (h.packet_type != static_cast<uint16_t>(PacketType::HandFullF32V2)) {
    throw std::runtime_error("unexpected packet_type " + std::to_string(h.packet_type));
  }
  if (h.payload_size != sizeof(HandTrackingFrameF32V2)) {
    throw std::runtime_error("bad HAND_FULL_F32_V2 payload_size " + std::to_string(h.payload_size));
  }
}


inline const char* packet_type_name(uint16_t t) {
  switch (static_cast<PacketType>(t)) {
    case PacketType::HmdPoseF64: return "HMD_POSE_F64";
    case PacketType::HandSummaryF64: return "HAND_SUMMARY_F64";
    case PacketType::HandSummaryF32V2: return "HAND_SUMMARY_F32_V2";
    case PacketType::HandJointsF32V2: return "HAND_JOINTS_F32_V2";
    case PacketType::HandFullF32V2: return "HAND_FULL_F32_V2";
    case PacketType::Heartbeat: return "HEARTBEAT";
    default: return "UNKNOWN";
  }
}

}  // namespace xr_runtime::tracking_net_v1
