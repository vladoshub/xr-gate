#include "capture_service_cpp/vendor/xreal_camera_transform.hpp"

#include "capture_service_cpp/image_transform.hpp"

namespace xr_capture_cpp {

XrealEyeTransformConfig resolve_xreal_eye_transforms(const RuntimeConfig& cfg) {
  return XrealEyeTransformConfig{
      cfg.camera.left_transform.rotate,
      cfg.camera.right_transform.rotate,
      cfg.camera.left_transform.flip,
      cfg.camera.right_transform.flip};
}

cv::Mat transform_xreal_gray_eye(const cv::Mat& in, const std::string& rotate_mode, const std::string& flip_mode) {
  return transform_gray_image(in, rotate_mode, flip_mode);
}

}  // namespace xr_capture_cpp
