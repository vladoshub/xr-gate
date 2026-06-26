#pragma once

#include "capture_service_cpp/common.hpp"

#include <opencv2/opencv.hpp>

#include <string>

namespace xr_capture_cpp {

struct XrealEyeTransformConfig {
  std::string left_rotate;
  std::string right_rotate;
  std::string left_flip;
  std::string right_flip;
};

XrealEyeTransformConfig resolve_xreal_eye_transforms(const RuntimeConfig& cfg);
cv::Mat transform_xreal_gray_eye(const cv::Mat& in, const std::string& rotate_mode, const std::string& flip_mode);

}  // namespace xr_capture_cpp
