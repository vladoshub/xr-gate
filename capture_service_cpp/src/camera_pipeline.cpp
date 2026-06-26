#include "capture_service_cpp/camera_pipeline.hpp"

#include "capture_service_cpp/platform/camera_capture.hpp"
#include "capture_service_cpp/vendor/xreal_camera_decoder.hpp"
#include "capture_service_cpp/vendor/xreal_camera_transform.hpp"

#include <iostream>
#include <stdexcept>
#include <thread>

namespace xr_capture_cpp {

void camera_thread(const RuntimeConfig& cfg, StreamPublishers* publishers) {
  CameraCapture cap;
  if (!cap.open(cfg)) throw std::runtime_error("failed to open camera");

  cv::Mat frame;
  cv::Mat raw_eye(cv::Size(kXrealEyeWidth, kXrealEyeHeight), CV_8UC1);
  cv::Mat latest_left;
  cv::Mat latest_right;
  const XrealEyeTransformConfig transforms = resolve_xreal_eye_transforms(cfg);
  bool have_left = false;
  bool have_right = false;
  uint64_t decoded = 0;
  uint64_t decode_fail = 0;
  uint64_t published_pairs = 0;
  uint64_t last_log = steady_ns();

  while (!g_stop.load()) {
    if (!cap.read(frame) || frame.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    bool is_right = false;
    if (!decode_xreal_eye(frame, raw_eye, is_right)) {
      ++decode_fail;
      continue;
    }
    ++decoded;
    if (is_right) {
      latest_right = transform_xreal_gray_eye(raw_eye, transforms.right_rotate, transforms.right_flip);
      have_right = true;
    } else {
      latest_left = transform_xreal_gray_eye(raw_eye, transforms.left_rotate, transforms.left_flip);
      have_left = true;
    }
    if (have_left && have_right) {
      const uint64_t ts = steady_ns();
      publishers->publish("camera0", latest_left.ptr<uint8_t>(), latest_left.total(), ts, latest_left.cols, latest_left.rows, kFormatGray8, 0, "camera0");
      publishers->publish("camera1", latest_right.ptr<uint8_t>(), latest_right.total(), ts, latest_right.cols, latest_right.rows, kFormatGray8, 0, "camera1");
      ++published_pairs;
      have_left = false;
      have_right = false;
    }
    const uint64_t now = steady_ns();
    if (now - last_log > 2000000000ULL) {
      std::cerr << "[capture_service_cpp] camera decoded=" << decoded << " decode_fail=" << decode_fail
                << " published_pairs=" << published_pairs
                << " left_rotate=" << transforms.left_rotate << " left_flip=" << transforms.left_flip
                << " right_rotate=" << transforms.right_rotate << " right_flip=" << transforms.right_flip << std::endl;
      last_log = now;
    }
  }
}

}  // namespace xr_capture_cpp
