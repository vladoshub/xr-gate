#include "capture_service_cpp/vendor/xreal_camera_transform.hpp"

#include <cstdlib>

namespace xr_capture_cpp {
namespace {

std::string first_env_or_config(const char* primary_env_name, const char* legacy_env_name, const std::string& configured, const std::string& fallback) {
  const char* primary_value = std::getenv(primary_env_name);
  if (primary_value && *primary_value) return std::string(primary_value);
  const char* legacy_value = std::getenv(legacy_env_name);
  if (legacy_value && *legacy_value) return std::string(legacy_value);
  if (!configured.empty()) return configured;
  return fallback;
}

cv::Mat rotate_gray(const cv::Mat& in, const std::string& mode) {
  cv::Mat out;
  if (mode == "cw90" || mode == "90" || mode == "clockwise") cv::rotate(in, out, cv::ROTATE_90_CLOCKWISE);
  else if (mode == "ccw90" || mode == "270" || mode == "counterclockwise") cv::rotate(in, out, cv::ROTATE_90_COUNTERCLOCKWISE);
  else if (mode == "rotate180" || mode == "180") cv::rotate(in, out, cv::ROTATE_180);
  else out = in.clone();
  return out;
}

cv::Mat flip_gray(const cv::Mat& in, const std::string& mode) {
  cv::Mat out;
  if (mode == "x" || mode == "horizontal" || mode == "h") cv::flip(in, out, 1);
  else if (mode == "y" || mode == "vertical" || mode == "v") cv::flip(in, out, 0);
  else if (mode == "xy" || mode == "yx" || mode == "both" || mode == "180-after" || mode == "rotate180-after") cv::flip(in, out, -1);
  else out = in.clone();
  return out;
}

}  // namespace

XrealEyeTransformConfig resolve_xreal_eye_transforms(const RuntimeConfig& cfg) {
  XrealEyeTransformConfig out;
  out.left_rotate = first_env_or_config("XR_CAPTURE_CPP_LEFT_ROTATE", "CPP_CAPTURE_LEFT_ROTATE", cfg.left_rotate, "ccw90");
  out.right_rotate = first_env_or_config("XR_CAPTURE_CPP_RIGHT_ROTATE", "CPP_CAPTURE_RIGHT_ROTATE", cfg.right_rotate, "ccw90");
  out.left_flip = first_env_or_config("XR_CAPTURE_CPP_LEFT_FLIP", "CPP_CAPTURE_LEFT_FLIP", cfg.left_flip, "none");
  // Right-eye direct raw output is portrait after ccw90, but on XREAL Air 2
  // Ultra it is still upside down relative to camera0. Apply a post-rotation
  // 180-degree output flip by default. Override with XR_CAPTURE_CPP_RIGHT_FLIP
  // or CPP_CAPTURE_RIGHT_FLIP if another camera convention is observed.
  out.right_flip = first_env_or_config("XR_CAPTURE_CPP_RIGHT_FLIP", "CPP_CAPTURE_RIGHT_FLIP", cfg.right_flip, "xy");
  return out;
}

cv::Mat transform_xreal_gray_eye(const cv::Mat& in, const std::string& rotate_mode, const std::string& flip_mode) {
  return flip_gray(rotate_gray(in, rotate_mode), flip_mode);
}

}  // namespace xr_capture_cpp
