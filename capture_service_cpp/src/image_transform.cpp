#include "capture_service_cpp/image_transform.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace xr_capture_cpp {
namespace {

std::string normalize(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

cv::Mat rotate_image(const cv::Mat& in, const std::string& configured_mode) {
  const std::string mode = normalize(configured_mode);
  cv::Mat out;
  if (mode == "cw90" || mode == "90" || mode == "clockwise") cv::rotate(in, out, cv::ROTATE_90_CLOCKWISE);
  else if (mode == "ccw90" || mode == "270" || mode == "counterclockwise") cv::rotate(in, out, cv::ROTATE_90_COUNTERCLOCKWISE);
  else if (mode == "rotate180" || mode == "180") cv::rotate(in, out, cv::ROTATE_180);
  else if (mode == "none" || mode == "0" || mode.empty()) out = in.clone();
  else throw std::runtime_error("unsupported image rotation: " + configured_mode);
  return out;
}

cv::Mat flip_image(const cv::Mat& in, const std::string& configured_mode) {
  const std::string mode = normalize(configured_mode);
  cv::Mat out;
  if (mode == "x" || mode == "horizontal" || mode == "h") cv::flip(in, out, 1);
  else if (mode == "y" || mode == "vertical" || mode == "v") cv::flip(in, out, 0);
  else if (mode == "xy" || mode == "yx" || mode == "both") cv::flip(in, out, -1);
  else if (mode == "none" || mode.empty()) out = in.clone();
  else throw std::runtime_error("unsupported image flip: " + configured_mode);
  return out;
}

}  // namespace

cv::Mat to_gray8(const cv::Mat& input) {
  if (input.empty()) return {};
  cv::Mat gray;
  if (input.type() == CV_8UC1) return input.clone();
  if (input.channels() == 3) cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
  else if (input.channels() == 4) cv::cvtColor(input, gray, cv::COLOR_BGRA2GRAY);
  else {
    input.convertTo(gray, CV_8UC1);
  }
  return gray;
}

cv::Mat transform_gray_image(const cv::Mat& input, const std::string& rotate_mode, const std::string& flip_mode) {
  const std::string rotation = normalize(rotate_mode);
  const std::string flip = normalize(flip_mode);
  // Each returned image must own its pixels: XREAL reuses the decoded eye
  // buffer before the other eye arrives. Skip identity passes, not that copy.
  if (flip == "none" || flip.empty()) return rotate_image(input, rotation);
  if (rotation == "none" || rotation == "0" || rotation.empty()) {
    return flip_image(input, flip);
  }
  // Flipping both axes is another half-turn. In the XREAL right-eye profile,
  // ccw90 + xy is exactly cw90, so one rotation replaces two image passes.
  if (flip == "xy" || flip == "yx" || flip == "both") {
    if (rotation == "ccw90" || rotation == "270" || rotation == "counterclockwise") {
      return rotate_image(input, "cw90");
    }
    if (rotation == "cw90" || rotation == "90" || rotation == "clockwise") {
      return rotate_image(input, "ccw90");
    }
    if (rotation == "rotate180" || rotation == "180") return input.clone();
  }
  return flip_image(rotate_image(input, rotate_mode), flip_mode);
}

}  // namespace xr_capture_cpp
