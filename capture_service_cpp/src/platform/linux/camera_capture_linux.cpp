#include "capture_service_cpp/platform/camera_capture.hpp"

#include <iostream>

namespace xr_capture_cpp {
namespace {

int camera_api_to_opencv(const std::string& api_name) {
  if (api_name == "any" || api_name == "auto") return cv::CAP_ANY;
  if (api_name == "gstreamer") return cv::CAP_GSTREAMER;
  return cv::CAP_V4L2;
}

void apply_properties(cv::VideoCapture& cap, const CameraDeviceConfig& cfg) {
  if (cfg.raw_format) cap.set(cv::CAP_PROP_FORMAT, -1);
  cap.set(cv::CAP_PROP_CONVERT_RGB, cfg.convert_rgb ? 1.0 : 0.0);
  if (cfg.buffer_size > 0) cap.set(cv::CAP_PROP_BUFFERSIZE, cfg.buffer_size);
  if (cfg.width > 0) cap.set(cv::CAP_PROP_FRAME_WIDTH, cfg.width);
  if (cfg.height > 0) cap.set(cv::CAP_PROP_FRAME_HEIGHT, cfg.height);
  if (cfg.fps > 0) cap.set(cv::CAP_PROP_FPS, cfg.fps);
}

}  // namespace

bool CameraCapture::open(const CameraDeviceConfig& cfg, const std::string& label) {
  const int api = camera_api_to_opencv(cfg.api);
  bool ok = false;
  if (!cfg.device_path.empty()) {
    std::cerr << "[capture_service_cpp] opening " << label << " device=" << cfg.device_path
              << " api=" << cfg.api << std::endl;
    ok = cap_.open(cfg.device_path, api);
  } else {
    std::cerr << "[capture_service_cpp] opening " << label << " index=" << cfg.index
              << " api=" << cfg.api << std::endl;
    ok = cap_.open(cfg.index, api);
  }
  if (!ok) return false;
  apply_properties(cap_, cfg);
  return cap_.isOpened();
}

bool CameraCapture::read(cv::Mat& frame) {
  return cap_.read(frame);
}

void CameraCapture::release() {
  cap_.release();
}

}  // namespace xr_capture_cpp
