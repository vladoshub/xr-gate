#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace xr_video {

inline uint64_t monotonic_now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline double ns_to_ms(int64_t ns) { return static_cast<double>(ns) / 1e6; }

enum class StereoVideoPixelFormat : uint32_t {
  Unknown = 0,
  Gray8 = 1,
  Rgb8 = 2,
  Bgr8 = 3,
};

enum class StereoVideoLayout : uint32_t {
  Unknown = 0,
  SeparateLeftRight = 1,
};

enum StereoVideoFlags : uint32_t {
  STEREO_VIDEO_FLAG_VALID = 1u << 0,
};

constexpr uint32_t XR_STEREO_VIDEO_FORMAT_VERSION_V1 = 1;
constexpr uint32_t XR_STEREO_VIDEO_FORMAT_CODE_V1 = 301;
constexpr const char* XR_STEREO_VIDEO_FORMAT_NAME_V1 = "XR_STEREO_VIDEO_V1";

#pragma pack(push, 1)

struct StereoVideoFrameHeaderV1 {
  uint32_t version = XR_STEREO_VIDEO_FORMAT_VERSION_V1;
  uint32_t size_bytes = sizeof(StereoVideoFrameHeaderV1);

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;         // publish/runtime timestamp
  uint64_t source_timestamp_ns = 0;  // synchronized stereo source timestamp
  uint64_t publish_timestamp_ns = 0;

  int64_t left_timestamp_ns = 0;
  int64_t right_timestamp_ns = 0;

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_left = 0;
  uint32_t stride_right = 0;

  uint32_t pixel_format = static_cast<uint32_t>(StereoVideoPixelFormat::Gray8);
  uint32_t layout = static_cast<uint32_t>(StereoVideoLayout::SeparateLeftRight);

  uint32_t left_size_bytes = 0;
  uint32_t right_size_bytes = 0;

  uint32_t flags = STEREO_VIDEO_FLAG_VALID;
  uint32_t reserved0 = 0;
};

struct StereoVideoRingHeaderV1 {
  char magic[8] = {'X', 'S', 'V', 'S', 'H', 'M', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = 4096;
  uint32_t slot_count = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_header_size = 128;
  uint32_t payload_size = 0;
  uint32_t reserved0 = 0;
  uint64_t latest_sequence = 0;
};

struct StereoVideoSlotHeaderV1 {
  uint64_t seq_begin = 0;  // odd while writer is active, even after commit
  uint64_t seq_end = 0;
  uint64_t timestamp_ns = 0;
  uint64_t source_timestamp_ns = 0;
  uint32_t payload_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_code = XR_STEREO_VIDEO_FORMAT_CODE_V1;
  uint32_t flags = 0;
  uint32_t reserved0 = 0;
  uint8_t reserved[72] = {};
};

struct StereoVideoNetFrameHeaderV1 {
  char magic[8] = {'X', 'S', 'V', 'N', 'E', 'T', '1', '\0'};
  uint32_t version = 1;
  uint32_t header_size = sizeof(StereoVideoNetFrameHeaderV1);
  uint32_t frame_header_size = sizeof(StereoVideoFrameHeaderV1);
  uint32_t left_size_bytes = 0;
  uint32_t right_size_bytes = 0;
  uint32_t reserved0 = 0;
  uint64_t sequence = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t publish_timestamp_ns = 0;
};

#pragma pack(pop)

static_assert(sizeof(StereoVideoFrameHeaderV1) == 96, "XR_STEREO_VIDEO_V1 header ABI changed");
static_assert(sizeof(StereoVideoSlotHeaderV1) == 128, "XR_STEREO_VIDEO slot header must be 128 bytes");

struct StereoVideoFrame {
  StereoVideoFrameHeaderV1 header;
  std::vector<uint8_t> left;
  std::vector<uint8_t> right;
};

inline uint32_t bytes_per_pixel(StereoVideoPixelFormat fmt) {
  switch (fmt) {
    case StereoVideoPixelFormat::Gray8:
      return 1;
    case StereoVideoPixelFormat::Rgb8:
    case StereoVideoPixelFormat::Bgr8:
      return 3;
    default:
      return 0;
  }
}

inline size_t stereo_video_payload_size_for(uint32_t width,
                                            uint32_t height,
                                            StereoVideoPixelFormat fmt) {
  const uint32_t bpp = bytes_per_pixel(fmt);
  if (width == 0 || height == 0 || bpp == 0) {
    throw std::runtime_error("invalid stereo video dimensions/pixel format");
  }
  return sizeof(StereoVideoFrameHeaderV1) +
         static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bpp) * 2u;
}

inline void validate_frame(const StereoVideoFrame& f, size_t max_payload_size = 0) {
  if (f.header.version != XR_STEREO_VIDEO_FORMAT_VERSION_V1) {
    throw std::runtime_error("bad XR stereo video frame version");
  }
  if (f.header.width == 0 || f.header.height == 0) {
    throw std::runtime_error("bad XR stereo video frame dimensions");
  }
  if (f.header.pixel_format != static_cast<uint32_t>(StereoVideoPixelFormat::Gray8) &&
      f.header.pixel_format != static_cast<uint32_t>(StereoVideoPixelFormat::Rgb8) &&
      f.header.pixel_format != static_cast<uint32_t>(StereoVideoPixelFormat::Bgr8)) {
    throw std::runtime_error("unsupported XR stereo video pixel format");
  }
  if (f.header.layout != static_cast<uint32_t>(StereoVideoLayout::SeparateLeftRight)) {
    throw std::runtime_error("unsupported XR stereo video layout");
  }
  if (f.header.left_size_bytes != f.left.size() || f.header.right_size_bytes != f.right.size()) {
    throw std::runtime_error("XR stereo video frame size metadata does not match payload");
  }
  const size_t payload_size = sizeof(StereoVideoFrameHeaderV1) + f.left.size() + f.right.size();
  if (max_payload_size != 0 && payload_size > max_payload_size) {
    throw std::runtime_error("XR stereo video payload exceeds configured slot payload size");
  }
}

inline void write_frame_payload(uint8_t* dst, const StereoVideoFrame& f) {
  std::memcpy(dst, &f.header, sizeof(StereoVideoFrameHeaderV1));
  std::memcpy(dst + sizeof(StereoVideoFrameHeaderV1), f.left.data(), f.left.size());
  std::memcpy(dst + sizeof(StereoVideoFrameHeaderV1) + f.left.size(), f.right.data(), f.right.size());
}

inline StereoVideoFrame read_frame_payload(const uint8_t* payload, size_t payload_size) {
  if (payload_size < sizeof(StereoVideoFrameHeaderV1)) {
    throw std::runtime_error("XR stereo video payload too small");
  }
  StereoVideoFrame out;
  std::memcpy(&out.header, payload, sizeof(StereoVideoFrameHeaderV1));
  if (out.header.size_bytes != sizeof(StereoVideoFrameHeaderV1)) {
    throw std::runtime_error("XR stereo video frame header size mismatch");
  }
  const size_t expected = sizeof(StereoVideoFrameHeaderV1) +
                          static_cast<size_t>(out.header.left_size_bytes) +
                          static_cast<size_t>(out.header.right_size_bytes);
  if (expected > payload_size) {
    throw std::runtime_error("XR stereo video payload truncated");
  }
  const uint8_t* left = payload + sizeof(StereoVideoFrameHeaderV1);
  const uint8_t* right = left + out.header.left_size_bytes;
  out.left.assign(left, left + out.header.left_size_bytes);
  out.right.assign(right, right + out.header.right_size_bytes);
  validate_frame(out);
  return out;
}

}  // namespace xr_video
