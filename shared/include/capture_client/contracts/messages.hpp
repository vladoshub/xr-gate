#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace capture_client {

enum class StreamKind : uint32_t { IMAGE = 1, IMU = 2, BYTES = 3 };

enum class FormatCode : uint32_t {
  UNKNOWN = 0,
  GRAY8 = 1,
  YUY2 = 2,
  RGB8 = 3,
  BGR8 = 4,
  IMU_F32_LE = 101,
  BYTES = 255,
};

struct StreamInfo {
  std::string stream_id;
  std::string shm_name;
  std::string kind;
  std::string frame_id;
  int width = 0;
  int height = 0;
  int format_code = 0;
  std::string format_name;
  size_t payload_size = 0;
  size_t slot_count = 0;
  size_t slot_header_size = 128;
  size_t header_size = 4096;
  size_t slot_stride = 0;
};

struct RawMessage {
  std::string stream_id;
  uint64_t sequence = 0;
  int64_t timestamp_ns = 0;
  uint64_t monotonic_ns = 0;
  uint32_t payload_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_code = 0;
  uint32_t flags = 0;
  std::string frame_id;
  std::vector<uint8_t> payload;
};

struct ImageFrame {
  uint64_t sequence = 0;
  int64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> gray8;
};

struct ImuSample {
  uint64_t sequence = 0;
  int64_t timestamp_ns = 0;
  double gyro_rad_s[3] = {0.0, 0.0, 0.0};
  double accel_m_s2[3] = {0.0, 0.0, 0.0};
};

}  // namespace capture_client
