#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

namespace xr_tracking {

inline int64_t body_tracker_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* BODY_TRACKER_SET_FORMAT_NAME = "BODY_TRACKER_SET_F32_V1";
constexpr uint32_t BODY_TRACKER_SET_FORMAT_VERSION = 1;
constexpr uint32_t BODY_TRACKER_MAX_TRACKERS = 32;
constexpr uint32_t BODY_TRACKER_UDP_MAGIC = 0x4254524Bu; // 'BTRK' marker
constexpr uint32_t BODY_TRACKER_UDP_VERSION = 1;

enum BodyTrackerSpace : uint32_t {
  BODY_TRACKER_SPACE_UNKNOWN = 0,
  BODY_TRACKER_SPACE_TRACKING_WORLD = 1,
  BODY_TRACKER_SPACE_HMD = 2,
  BODY_TRACKER_SPACE_RUNTIME_HMD = 3,
  BODY_TRACKER_SPACE_STEAMVR_TRACKING = 4,
  BODY_TRACKER_SPACE_OPENXR_LOCAL = 5,
};

enum BodyTrackerSource : uint32_t {
  BODY_TRACKER_SOURCE_UNKNOWN = 0,
  BODY_TRACKER_SOURCE_STEAMVR = 1,
  BODY_TRACKER_SOURCE_OPENXR = 2,
  BODY_TRACKER_SOURCE_SLIMEVR = 3,
  BODY_TRACKER_SOURCE_VIVE_TRACKER = 4,
  BODY_TRACKER_SOURCE_TUNDRA_TRACKER = 5,
  BODY_TRACKER_SOURCE_APRILTAG = 6,
  BODY_TRACKER_SOURCE_IMU = 7,
  BODY_TRACKER_SOURCE_FRAME = 8,
};

enum BodyTrackerRole : uint32_t {
  BODY_TRACKER_ROLE_UNKNOWN = 0,
  BODY_TRACKER_ROLE_GENERIC = 1,
  BODY_TRACKER_ROLE_WAIST = 2,
  BODY_TRACKER_ROLE_CHEST = 3,
  BODY_TRACKER_ROLE_LEFT_FOOT = 4,
  BODY_TRACKER_ROLE_RIGHT_FOOT = 5,
  BODY_TRACKER_ROLE_LEFT_KNEE = 6,
  BODY_TRACKER_ROLE_RIGHT_KNEE = 7,
  BODY_TRACKER_ROLE_LEFT_ELBOW = 8,
  BODY_TRACKER_ROLE_RIGHT_ELBOW = 9,
  BODY_TRACKER_ROLE_LEFT_SHOULDER = 10,
  BODY_TRACKER_ROLE_RIGHT_SHOULDER = 11,
  BODY_TRACKER_ROLE_HEAD = 12,
  BODY_TRACKER_ROLE_CAMERA = 13,
  BODY_TRACKER_ROLE_LEFT_WRIST = 14,
  BODY_TRACKER_ROLE_RIGHT_WRIST = 15,
};

enum BodyTrackerStatus : uint32_t {
  BODY_TRACKER_STATUS_UNAVAILABLE = 0,
  BODY_TRACKER_STATUS_CONNECTED = 1,
  BODY_TRACKER_STATUS_TRACKING = 2,
  BODY_TRACKER_STATUS_LOST = 3,
  BODY_TRACKER_STATUS_STALE = 4,
};

enum BodyTrackerFrameFlags : uint32_t {
  BODY_TRACKER_FRAME_HAS_TRACKERS = 1u << 0,
};

enum BodyTrackerFlags : uint32_t {
  BODY_TRACKER_FLAG_POSE_VALID = 1u << 0,
  BODY_TRACKER_FLAG_POSITION_VALID = 1u << 1,
  BODY_TRACKER_FLAG_ORIENTATION_VALID = 1u << 2,
  BODY_TRACKER_FLAG_LINEAR_VELOCITY_VALID = 1u << 3,
  BODY_TRACKER_FLAG_ANGULAR_VELOCITY_VALID = 1u << 4,
  BODY_TRACKER_FLAG_ROLE_VALID = 1u << 5,
  BODY_TRACKER_FLAG_CONNECTED = 1u << 6,
};

#pragma pack(push, 1)

struct BodyTrackerPoseF32V1 {
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
  float qw = 1.0f;
  float qx = 0.0f;
  float qy = 0.0f;
  float qz = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  float wx = 0.0f;
  float wy = 0.0f;
  float wz = 0.0f;
};

struct BodyTrackerF32V1 {
  uint32_t tracker_index = 0;
  uint32_t role = BODY_TRACKER_ROLE_UNKNOWN;
  uint32_t status = BODY_TRACKER_STATUS_UNAVAILABLE;
  uint32_t flags = 0;

  uint64_t stable_id_hash = 0;
  uint64_t device_serial_hash = 0;

  float confidence = 0.0f;
  float battery = -1.0f;
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;

  BodyTrackerPoseF32V1 pose;

  char tracker_id[64] = {};
};

struct BodyTrackerSetFrameF32V1 {
  uint32_t version = BODY_TRACKER_SET_FORMAT_VERSION;
  uint32_t size_bytes = sizeof(BodyTrackerSetFrameF32V1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t reset_counter = 0;

  uint32_t space = BODY_TRACKER_SPACE_UNKNOWN;
  uint32_t source = BODY_TRACKER_SOURCE_UNKNOWN;
  uint32_t flags = 0;
  uint32_t tracker_count = 0;

  BodyTrackerF32V1 trackers[BODY_TRACKER_MAX_TRACKERS];
};

struct BodyTrackerSetUdpHeaderV1 {
  uint32_t magic = BODY_TRACKER_UDP_MAGIC;
  uint16_t version = BODY_TRACKER_UDP_VERSION;
  uint16_t header_size = sizeof(BodyTrackerSetUdpHeaderV1);
  uint32_t payload_size = sizeof(BodyTrackerSetFrameF32V1);
  uint32_t reserved0 = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
};

struct BodyTrackerSetRingHeaderV1 {
  char magic[8] = {'B', 'T', 'R', 'K', 'R', 'G', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = 4096;
  uint32_t slot_count = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_header_size = 128;
  uint32_t payload_size = sizeof(BodyTrackerSetFrameF32V1);
  uint32_t reserved0 = 0;
  uint64_t latest_sequence = 0;
};

struct BodyTrackerSetRingSlotHeaderV1 {
  uint64_t seq_begin = 0;
  uint64_t seq_end = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint32_t payload_size = sizeof(BodyTrackerSetFrameF32V1);
  uint32_t flags = 0;
  uint8_t reserved[88] = {};
};

#pragma pack(pop)

static_assert(sizeof(BodyTrackerSetUdpHeaderV1) == 32, "body tracker UDP header must be 32 bytes");
static_assert(sizeof(BodyTrackerSetRingSlotHeaderV1) == 128, "body tracker ring slot header must be 128 bytes");

struct BodyTrackerKey {
  enum class Kind { None, StableHash, SerialHash, Text, Role, Index, Slot };

  Kind kind = Kind::None;
  uint64_t value = 0;
  char text[sizeof(BodyTrackerF32V1::tracker_id)] = {};

  bool operator==(const BodyTrackerKey& other) const {
    return kind == other.kind && value == other.value &&
           (kind != Kind::Text || std::strncmp(text, other.text, sizeof(text)) == 0);
  }
};

// Filters may use the legacy role/index fallbacks. Device registration requires
// a persistent identity and leaves slot_index unset.
inline BodyTrackerKey body_tracker_key(const BodyTrackerF32V1& tracker,
                                       std::optional<uint32_t> slot_index = std::nullopt) {
  using Kind = BodyTrackerKey::Kind;
  if (tracker.stable_id_hash != 0) return {Kind::StableHash, tracker.stable_id_hash};
  if (tracker.tracker_id[0] != '\0') {
    BodyTrackerKey key{Kind::Text};
    std::memcpy(key.text, tracker.tracker_id, sizeof(key.text));
    return key;
  }
  if (tracker.device_serial_hash != 0) return {Kind::SerialHash, tracker.device_serial_hash};
  if (!slot_index) return {};
  if ((tracker.flags & BODY_TRACKER_FLAG_ROLE_VALID) != 0u && tracker.role != BODY_TRACKER_ROLE_UNKNOWN) {
    return {Kind::Role, tracker.role};
  }
  if (tracker.tracker_index != 0) return {Kind::Index, tracker.tracker_index};
  return {Kind::Slot, *slot_index};
}

inline std::string normalize_body_tracker_posix_shm_name(std::string name) {
  if (name.empty()) throw std::runtime_error("empty SHM name");
  if (name[0] != '/') name = "/" + name;
  return name;
}

inline BodyTrackerSetFrameF32V1 make_empty_body_tracker_set_frame(
    uint64_t sequence,
    int64_t timestamp_ns,
    uint32_t space = BODY_TRACKER_SPACE_UNKNOWN,
    uint32_t source = BODY_TRACKER_SOURCE_UNKNOWN) {
  BodyTrackerSetFrameF32V1 f;
  f.sequence = sequence;
  f.timestamp_ns = static_cast<uint64_t>(timestamp_ns > 0 ? timestamp_ns : body_tracker_now_ns());
  f.source_timestamp_ns = f.timestamp_ns;
  f.space = space;
  f.source = source;
  f.flags = 0;
  f.tracker_count = 0;
  return f;
}

inline BodyTrackerSetFrameF32V1 decode_body_tracker_set_udp_packet(const void* data, size_t size) {
  if (size != sizeof(BodyTrackerSetUdpHeaderV1) + sizeof(BodyTrackerSetFrameF32V1)) {
    throw std::runtime_error("invalid body_tracker_set UDP packet size");
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  BodyTrackerSetUdpHeaderV1 header{};
  std::memcpy(&header, bytes, sizeof(header));
  if (header.magic != BODY_TRACKER_UDP_MAGIC) {
    throw std::runtime_error("invalid body_tracker_set UDP magic");
  }
  if (header.version != BODY_TRACKER_UDP_VERSION) {
    throw std::runtime_error("unsupported body_tracker_set UDP version");
  }
  if (header.header_size != sizeof(BodyTrackerSetUdpHeaderV1) ||
      header.payload_size != sizeof(BodyTrackerSetFrameF32V1)) {
    throw std::runtime_error("invalid body_tracker_set UDP header sizes");
  }
  BodyTrackerSetFrameF32V1 frame{};
  std::memcpy(&frame, bytes + sizeof(header), sizeof(frame));
  if (frame.version != BODY_TRACKER_SET_FORMAT_VERSION ||
      frame.size_bytes != sizeof(BodyTrackerSetFrameF32V1)) {
    throw std::runtime_error("invalid body_tracker_set frame header");
  }
  if (frame.tracker_count > BODY_TRACKER_MAX_TRACKERS) {
    throw std::runtime_error("body_tracker_set tracker_count exceeds max");
  }
  return frame;
}

}  // namespace xr_tracking
