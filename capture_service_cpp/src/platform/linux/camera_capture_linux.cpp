#include "capture_service_cpp/platform/camera_capture.hpp"

#include <iostream>

namespace xr_capture_cpp {
namespace {

int camera_api_to_opencv(const RuntimeConfig& cfg) {
  if (cfg.camera_api == "any") return cv::CAP_ANY;
  return cv::CAP_V4L2;
}

}  // namespace

bool CameraCapture::open(const RuntimeConfig& cfg) {
  const int api = camera_api_to_opencv(cfg);
  std::cerr << "[capture_service_cpp] opening camera device=" << cfg.video_device << " api=" << cfg.camera_api << std::endl;
  if (!cap_.open(cfg.video_device, api)) return false;

  // XREAL Air 2 Ultra exposes a vendor-packed raw byte stream.  Do not let
  // OpenCV/V4L2 convert it to RGB/YUYV frames: the XREAL chunk decoder below
  // expects the original opaque byte layout.  These calls were part of the
  // pre-split camera path and must stay in the Linux platform opener.
  cap_.set(cv::CAP_PROP_FORMAT, -1);
  cap_.set(cv::CAP_PROP_CONVERT_RGB, false);
  cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);

  return cap_.isOpened();
}

bool CameraCapture::read(cv::Mat& frame) {
  return cap_.read(frame);
}

}  // namespace xr_capture_cpp
