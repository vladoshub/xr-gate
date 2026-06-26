#pragma once

#include "capture_service_cpp/common.hpp"

#include <opencv2/opencv.hpp>

namespace xr_capture_cpp {

class CameraCapture {
 public:
  CameraCapture() = default;

  bool open(const RuntimeConfig& cfg);
  bool read(cv::Mat& frame);

 private:
  cv::VideoCapture cap_;
};

}  // namespace xr_capture_cpp
