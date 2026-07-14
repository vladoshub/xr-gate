#include "capture_service_cpp/interleaved_columns_decoder.hpp"

#include <limits>
#include <stdexcept>

namespace xr_capture_cpp {
namespace {

size_t checked_row_bytes(int eye_width) {
  if (eye_width <= 0) throw std::invalid_argument("interleaved eye width must be positive");
  const size_t width = static_cast<size_t>(eye_width);
  if (width > std::numeric_limits<size_t>::max() / 2U) {
    throw std::invalid_argument("interleaved eye width is too large");
  }
  return width * 2U;
}

size_t required_size(size_t row_stride, size_t row_bytes, int height) {
  if (height <= 0) throw std::invalid_argument("interleaved eye height must be positive");
  const size_t rows_before_last = static_cast<size_t>(height - 1);
  if (rows_before_last > 0 && row_stride > (std::numeric_limits<size_t>::max() - row_bytes) / rows_before_last) {
    throw std::invalid_argument("interleaved image size overflows size_t");
  }
  return rows_before_last * row_stride + row_bytes;
}

}  // namespace

void deinterleave_gray8_columns(const uint8_t* source,
                                size_t source_size,
                                size_t source_row_stride,
                                int eye_width,
                                int eye_height,
                                uint8_t* left,
                                size_t left_row_stride,
                                uint8_t* right,
                                size_t right_row_stride) {
  if (source == nullptr || left == nullptr || right == nullptr) {
    throw std::invalid_argument("interleaved decoder received a null buffer");
  }

  const size_t width = static_cast<size_t>(eye_width);
  const size_t source_row_bytes = checked_row_bytes(eye_width);
  if (source_row_stride < source_row_bytes) {
    throw std::invalid_argument("interleaved source row stride is smaller than two eye rows");
  }
  if (left_row_stride < width || right_row_stride < width) {
    throw std::invalid_argument("interleaved output row stride is smaller than one eye row");
  }
  if (source_size < required_size(source_row_stride, source_row_bytes, eye_height)) {
    throw std::invalid_argument("interleaved source buffer is smaller than the configured stereo frame");
  }

  for (int y = 0; y < eye_height; ++y) {
    const uint8_t* source_row = source + static_cast<size_t>(y) * source_row_stride;
    uint8_t* left_row = left + static_cast<size_t>(y) * left_row_stride;
    uint8_t* right_row = right + static_cast<size_t>(y) * right_row_stride;
    for (size_t x = 0; x < width; ++x) {
      left_row[x] = source_row[x * 2U];
      right_row[x] = source_row[x * 2U + 1U];
    }
  }
}

}  // namespace xr_capture_cpp
