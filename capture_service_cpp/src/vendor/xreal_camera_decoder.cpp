#include "capture_service_cpp/vendor/xreal_camera_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xr_capture_cpp {
namespace {

constexpr int kCamHeaderLen = 2;
constexpr size_t kChunkSize = 2400;
constexpr size_t kChunkAmount = 128;
constexpr size_t kChunkSumLen = 128;
constexpr size_t kCamBufferSize = (kXrealEyeHeight + kCamHeaderLen) * kXrealEyeWidth;
constexpr size_t kCamImageDataSize = kXrealEyeHeight * kXrealEyeWidth;

const uint8_t kChunkMap[128] = {
    119, 54,  21,  0,   108, 22,  51,  63,  93, 99,  67, 7,   32,  112, 52,
    43,  14,  35,  75,  116, 64,  71,  44,  89, 18,  88, 26,  61,  70,  56,
    90,  79,  87,  120, 81,  101, 121, 17,  72, 31,  53, 124, 127, 113, 111,
    36,  48,  19,  37,  83,  126, 74,  109, 5,  84,  41, 76,  30,  110, 29,
    12,  115, 28,  102, 105, 62,  103, 20,  3,  68,  49, 77,  117, 125, 106,
    60,  69,  98,  9,   16,  78,  47,  40,  2,  118, 34, 13,  50,  46,  80,
    85,  66,  42,  123, 122, 96,  11,  25,  97, 39,  6,  86,  1,   8,   82,
    92,  59,  104, 24,  15,  73,  65,  38,  58, 10,  23, 33,  55,  57,  107,
    100, 94,  27,  95,  45,  91,  4,   114};

uint8_t g_reverse_chunk_map[128] = {};

}  // namespace

void init_xreal_camera_tables() {
  for (size_t i = 0; i < kChunkAmount; ++i) g_reverse_chunk_map[kChunkMap[i]] = static_cast<uint8_t>(i);
}

bool decode_xreal_eye(const cv::Mat& frame, cv::Mat& eye, bool& is_right) {
  if (frame.empty()) return false;
  if (static_cast<size_t>(frame.total() * frame.elemSize()) < kCamBufferSize) return false;
  const uint8_t* blocks = frame.ptr<uint8_t>();
  uint8_t* data = eye.ptr<uint8_t>();
  const uint8_t* header = blocks + kCamImageDataSize;
  is_right = (*(header + 0x3B)) != 0;

  size_t offset = 0;
  size_t sum = kChunkSumLen * 0xFF + 1;
  for (size_t i = 0; i < kChunkAmount; ++i) {
    size_t val = 0;
    for (size_t j = 0; j < kChunkSumLen; ++j) val += blocks[kChunkSize * i + j];
    if (val < sum) {
      offset = i;
      sum = val;
    }
  }
  offset = g_reverse_chunk_map[offset];
  for (size_t i = 0; i < kChunkAmount; ++i) {
    const size_t src_idx = kChunkSize * kChunkMap[(offset + i) % kChunkAmount];
    const size_t dst_idx = kChunkSize * i;
    std::memcpy(data + dst_idx, blocks + src_idx, kChunkSize);
  }
  return true;
}

}  // namespace xr_capture_cpp
