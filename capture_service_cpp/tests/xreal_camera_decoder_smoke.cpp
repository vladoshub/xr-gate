#include "capture_service_cpp/vendor/xreal_camera_decoder.hpp"

#include <cstring>
#include <set>
#include <stdexcept>

using namespace xr_capture_cpp;
namespace {
void require(bool value) {
  if (!value) throw std::runtime_error("XREAL image decoder check failed");
}
void put_le(uint8_t* p, uint64_t value, size_t size) {
  for (size_t i = 0; i < size; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
}

int main() {
  init_xreal_camera_tables();
  cv::Mat frame(241, 640, CV_8UC2);
  cv::Mat eye(480, 640, CV_8UC1);
  for (size_t i = 0; i < 128; ++i) std::memset(frame.data + i * 2400, static_cast<int>(i), 2400);
  uint8_t* footer = frame.data + 307200;
  std::memset(footer, 0, 1280);
  put_le(footer, 1000000000000ULL, 8);
  put_le(footer + 18, 42, 2);
  footer[59] = 1;
  put_le(footer + 62, 1000009965000ULL, 8);
  XrealCameraMetadata meta;
  bool right = false;
  require(decode_xreal_eye(frame, eye, right, &meta));
  require(right && meta.eye == 1 && meta.pair_counter == 42);
  require(meta.t0_ns == 1000000000000ULL && meta.t62_ns == 1000009965000ULL);
  require(eye.data[0] == 0 && eye.data[2400] == 108);
  std::set<uint8_t> blocks;
  for (size_t i = 0; i < 128; ++i) {
    blocks.insert(eye.data[i * 2400]);
    for (size_t j = 0; j < 2400; ++j) require(eye.data[i * 2400 + j] == eye.data[i * 2400]);
  }
  require(blocks.size() == 128);
  cv::Mat invalid_eye;
  require(!decode_xreal_eye(frame, invalid_eye, right, &meta));
  require(!decode_xreal_eye(frame.rowRange(0, 240), eye, right, &meta));
  cv::Mat strided(482, 641, CV_8UC1);
  cv::Mat roi = strided.colRange(0, 640);
  require(!roi.isContinuous() && !decode_xreal_eye(roi, eye, right, &meta));
  footer[59] = 2;
  require(!decode_xreal_eye(frame, eye, right, &meta));
  footer[59] = 1;
  put_le(footer + 62, 0, 8);
  require(!decode_xreal_eye(frame, eye, right, &meta));
  return 0;
}
