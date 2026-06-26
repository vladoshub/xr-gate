#pragma once

#include <string>

#include <opencv2/core.hpp>

#include <xr_spatial_backend/pose_math.hpp>

namespace xr_spatial_backend {

struct CameraIntrinsics {
  int width = 0;
  int height = 0;
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  double k1 = 0.0;
  double k2 = 0.0;
  double k3 = 0.0;
  double k4 = 0.0;
};

struct StereoCalibration {
  CameraIntrinsics cam0;
  CameraIntrinsics cam1;
  Transform3d T_imu_cam0;
  Transform3d T_imu_cam1;
  Transform3d T_cam0_raw_cam0_rect;
  cv::Mat K0, D0, K1, D1;
  cv::Mat R_c1_c0, t_c1_c0;
  cv::Mat R1, R2, P1, P2, Q;
  double baseline_m = 0.0;
};

StereoCalibration load_stereo_calibration_json(const std::string& path);

// Compatibility alias for older callers. The JSON layout is currently the
// Basalt-style stereo/IMU calibration schema, but xr_spatial itself does
// not depend on Basalt.
inline StereoCalibration load_basalt_stereo_calibration(const std::string& path) {
  return load_stereo_calibration_json(path);
}
void compute_rectification(StereoCalibration& calib, double fisheye_balance, bool zero_disparity);

}  // namespace xr_spatial_backend
