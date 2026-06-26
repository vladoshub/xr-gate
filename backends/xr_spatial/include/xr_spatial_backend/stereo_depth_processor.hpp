#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/calib3d.hpp>

#include <capture_client/contracts/messages.hpp>
#include <xr_spatial_backend/calibration.hpp>
#include <xr_spatial_backend/types.hpp>

namespace xr_spatial_backend {

struct StereoDepthConfig {
  int target_rate_hz = 10;
  int num_disparities = 96;  // must be divisible by 16
  int block_size = 5;
  int min_disparity = 0;
  int uniqueness_ratio = 10;
  int speckle_window_size = 100;
  int speckle_range = 2;
  int disp12_max_diff = 1;
  int pre_filter_cap = 31;
  int point_decimation = 2;
  float min_depth_m = 0.25f;
  float max_depth_m = 5.0f;
  // Reject pathological reprojected coordinates even when disparity/depth fields look finite.
  // This is a safety guard for bad disparity pixels and calibration/profile mistakes.
  float max_abs_camera_coord_m = 20.0f;
  // Normalized image ROI used before turning disparity into 3D points.
  // Keep this profile-owned: useful for ignoring noisy image borders and off-target clutter.
  float roi_x_min = 0.0f;
  float roi_x_max = 1.0f;
  float roi_y_min = 0.0f;
  float roi_y_max = 1.0f;
  bool save_first_debug_frame = false;
  std::string debug_dir;
  std::string depth_frame_id = "cam0_rect";
};

struct StereoDepthResult {
  PointCloudFrame cloud;
  cv::Mat left_rect;
  cv::Mat right_rect;
  cv::Mat disparity_float;

  // Organized live depth grid, preserving image-neighborhood topology after
  // decimation.  This is intentionally separate from cloud.points because the
  // pointcloud may be voxel-filtered or capped later, while live passthrough
  // rendering needs stable grid_width x grid_height layout.
  uint32_t live_grid_width = 0;
  uint32_t live_grid_height = 0;
  uint32_t live_grid_decimation = 0;
  uint32_t live_grid_valid_points = 0;
  std::vector<Vec3f> live_grid_points;
  std::vector<uint8_t> live_grid_valid;

  // Per-frame depth quality counters. They are intentionally stored with the
  // result so the backend can decide whether to integrate or reject a frame.
  size_t candidate_pixels = 0;
  size_t rejected_roi_pixels = 0;
  size_t rejected_disparity_pixels = 0;
  size_t rejected_nonfinite_points = 0;
  size_t rejected_depth_range_points = 0;
  size_t rejected_abs_coord_points = 0;
  size_t accepted_points = 0;
  float accepted_pixel_fraction = 0.0f;
};

class StereoDepthProcessor {
 public:
  StereoDepthProcessor(StereoCalibration calibration, StereoDepthConfig config);

  StereoDepthResult compute(const capture_client::ImageFrame& cam0,
                            const capture_client::ImageFrame& cam1);

  const StereoCalibration& calibration() const { return calibration_; }

 private:
  void ensure_maps(uint32_t width, uint32_t height);
  void maybe_save_debug(const StereoDepthResult& result);

  StereoCalibration calibration_;
  StereoDepthConfig config_;
  cv::Mat map0x_, map0y_, map1x_, map1y_;
  cv::Ptr<cv::StereoSGBM> sgbm_;
  bool maps_ready_ = false;
  bool debug_saved_ = false;
};

}  // namespace xr_spatial_backend
