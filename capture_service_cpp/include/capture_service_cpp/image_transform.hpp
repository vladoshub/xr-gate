#pragma once

#include <opencv2/opencv.hpp>
#include <string>

namespace xr_capture_cpp {

cv::Mat to_gray8(const cv::Mat& input);
bool image_rotation_swaps_dimensions(const std::string& rotate_mode);
cv::Mat transform_image(const cv::Mat& input, const std::string& rotate_mode, const std::string& flip_mode);
cv::Mat transform_gray_image(const cv::Mat& input, const std::string& rotate_mode, const std::string& flip_mode);

}  // namespace xr_capture_cpp
