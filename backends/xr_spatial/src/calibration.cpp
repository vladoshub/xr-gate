#include <xr_spatial_backend/calibration.hpp>

#include <cmath>
#include <fstream>
#include <stdexcept>

#include <opencv2/calib3d.hpp>
#include <nlohmann/json.hpp>

namespace xr_spatial_backend {
namespace {

const nlohmann::json& require_obj(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) throw std::runtime_error(std::string("missing calibration field: ") + key);
  return j.at(key);
}

CameraIntrinsics read_intrinsics(const nlohmann::json& intr_node, const nlohmann::json& res_node) {
  const auto& intr = intr_node.contains("intrinsics") ? intr_node.at("intrinsics") : intr_node;
  CameraIntrinsics c;
  c.width = res_node.at(0).get<int>();
  c.height = res_node.at(1).get<int>();
  c.fx = intr.at("fx").get<double>();
  c.fy = intr.at("fy").get<double>();
  c.cx = intr.at("cx").get<double>();
  c.cy = intr.at("cy").get<double>();
  c.k1 = intr.value("k1", 0.0);
  c.k2 = intr.value("k2", 0.0);
  c.k3 = intr.value("k3", 0.0);
  c.k4 = intr.value("k4", 0.0);
  return c;
}

Transform3d read_pose(const nlohmann::json& p) {
  Pose3d pose;
  pose.p = {p.at("px").get<double>(), p.at("py").get<double>(), p.at("pz").get<double>()};
  pose.q = {p.at("qw").get<double>(), p.at("qx").get<double>(), p.at("qy").get<double>(), p.at("qz").get<double>()};
  return pose_to_transform(pose);
}

cv::Mat K(const CameraIntrinsics& c) {
  return (cv::Mat_<double>(3,3) << c.fx, 0.0, c.cx, 0.0, c.fy, c.cy, 0.0, 0.0, 1.0);
}

cv::Mat D(const CameraIntrinsics& c) {
  return (cv::Mat_<double>(4,1) << c.k1, c.k2, c.k3, c.k4);
}

cv::Mat mat3_to_cv(const Mat3d& m) {
  cv::Mat out(3, 3, CV_64F);
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) out.at<double>(r,c) = m.v[r][c];
  return out;
}

cv::Mat vec3_to_cv(const Vec3d& v) {
  return (cv::Mat_<double>(3,1) << v.x, v.y, v.z);
}

Mat3d cv_to_mat3(const cv::Mat& m) {
  Mat3d out;
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) out.v[r][c] = m.at<double>(r,c);
  return out;
}

}  // namespace

StereoCalibration load_stereo_calibration_json(const std::string& path) {
  std::ifstream is(path);
  if (!is) throw std::runtime_error("failed to open stereo calibration JSON: " + path);
  nlohmann::json root;
  is >> root;
  const auto& v0 = require_obj(root, "value0");
  const auto& intr = require_obj(v0, "intrinsics");
  const auto& res = require_obj(v0, "resolution");
  const auto& t_imu_cam = require_obj(v0, "T_imu_cam");

  if (!intr.is_array() || intr.size() < 2 || !res.is_array() || res.size() < 2 ||
      !t_imu_cam.is_array() || t_imu_cam.size() < 2) {
    throw std::runtime_error("stereo calibration JSON must contain two cameras in value0.intrinsics/resolution/T_imu_cam");
  }

  StereoCalibration c;
  c.cam0 = read_intrinsics(intr.at(0), res.at(0));
  c.cam1 = read_intrinsics(intr.at(1), res.at(1));
  c.T_imu_cam0 = read_pose(t_imu_cam.at(0));
  c.T_imu_cam1 = read_pose(t_imu_cam.at(1));
  c.K0 = K(c.cam0);
  c.K1 = K(c.cam1);
  c.D0 = D(c.cam0);
  c.D1 = D(c.cam1);

  // This calibration schema stores T_imu_cam. OpenCV stereoRectify wants transform from cam0 to cam1:
  // X_cam1 = R_c1_c0 * X_cam0 + t_c1_c0.
  const Mat3d R_i_c0 = c.T_imu_cam0.R;
  const Mat3d R_i_c1 = c.T_imu_cam1.R;
  const Mat3d R_c1_i = transpose(R_i_c1);
  const Mat3d R_c1_c0 = multiply(R_c1_i, R_i_c0);
  const Vec3d t_c1_c0 = multiply(R_c1_i, c.T_imu_cam0.t - c.T_imu_cam1.t);
  c.R_c1_c0 = mat3_to_cv(R_c1_c0);
  c.t_c1_c0 = vec3_to_cv(t_c1_c0);
  c.baseline_m = std::sqrt(t_c1_c0.x*t_c1_c0.x + t_c1_c0.y*t_c1_c0.y + t_c1_c0.z*t_c1_c0.z);
  return c;
}

void compute_rectification(StereoCalibration& c, double fisheye_balance, bool zero_disparity) {
  const cv::Size image_size(c.cam0.width, c.cam0.height);
  const int flags = zero_disparity ? cv::CALIB_ZERO_DISPARITY : 0;
  cv::fisheye::stereoRectify(c.K0, c.D0, c.K1, c.D1, image_size,
                             c.R_c1_c0, c.t_c1_c0,
                             c.R1, c.R2, c.P1, c.P2, c.Q,
                             flags, image_size, fisheye_balance, 1.0);

  // OpenCV rectified 3D points are in cam0_rect. If p_rect = R1 * p_raw,
  // then p_raw = R1^T * p_rect, so T_imu_cam0_rect = T_imu_cam0_raw * T_raw_rect.
  c.T_cam0_raw_cam0_rect.R = transpose(cv_to_mat3(c.R1));
  c.T_cam0_raw_cam0_rect.t = {0.0, 0.0, 0.0};
}

}  // namespace xr_spatial_backend
