#pragma once

#include <opencv2/opencv.hpp>

namespace xr_capture_cpp {

constexpr int kXrealEyeWidth = 640;
constexpr int kXrealEyeHeight = 480;

void init_xreal_camera_tables();
bool decode_xreal_eye(const cv::Mat& frame, cv::Mat& eye, bool& is_right);

}  // namespace xr_capture_cpp
