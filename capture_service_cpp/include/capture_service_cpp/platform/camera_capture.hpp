#pragma once

#include "capture_service_cpp/common.hpp"

#include <opencv2/opencv.hpp>

namespace xr_capture_cpp {

class CameraCapture {
 public:
  CameraCapture() = default;

  bool open(const CameraDeviceConfig& cfg, const std::string& label);
  bool read(cv::Mat& frame);
  void release();

 private:
  cv::VideoCapture cap_;
};

}  // namespace xr_capture_cpp
