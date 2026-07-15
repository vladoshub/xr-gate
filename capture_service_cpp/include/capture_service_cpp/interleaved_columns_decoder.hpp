#pragma once

#include <cstddef>
#include <cstdint>

namespace xr_capture_cpp {

// Splits an 8-bit stereo image whose bytes are arranged per row as:
//   left[0], right[0], left[1], right[1], ...
//
// This is a generic byte-layout decoder. It has no dependency on a camera SDK
// and does not perform capture, calibration, rectification, or device control.
void deinterleave_gray8_columns(const uint8_t* source,
                                size_t source_size,
                                size_t source_row_stride,
                                int eye_width,
                                int eye_height,
                                uint8_t* left,
                                size_t left_row_stride,
                                uint8_t* right,
                                size_t right_row_stride);

}  // namespace xr_capture_cpp
