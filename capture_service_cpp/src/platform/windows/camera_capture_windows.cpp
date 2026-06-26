#include "capture_service_cpp/platform/camera_capture.hpp"

#include <iostream>

namespace xr_capture_cpp {
namespace {

int camera_api_to_opencv(const RuntimeConfig& cfg) {
  if (cfg.camera_api == "dshow" || cfg.camera_api == "DSHOW") return cv::CAP_DSHOW;
  if (cfg.camera_api == "msmf" || cfg.camera_api == "MSMF") return cv::CAP_MSMF;
  return cv::CAP_ANY;
}

}  // namespace

bool CameraCapture::open(const RuntimeConfig& cfg) {
  const int api = camera_api_to_opencv(cfg);
  std::cerr << "[capture_service_cpp] opening camera index=" << cfg.camera_index << " api=" << cfg.camera_api << std::endl;
  return cap_.open(cfg.camera_index, api);
}

bool CameraCapture::read(cv::Mat& frame) {
  return cap_.read(frame);
}

}  // namespace xr_capture_cpp
