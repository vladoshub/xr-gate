#include <xr_spatial_backend/stereo_depth_processor.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace xr_spatial_backend {

namespace {

cv::Mat image_to_mat(const capture_client::ImageFrame& f) {
  if (f.width == 0 || f.height == 0 || f.gray8.size() != size_t(f.width) * size_t(f.height)) {
    throw std::runtime_error("bad GRAY8 image payload");
  }
  cv::Mat m(static_cast<int>(f.height), static_cast<int>(f.width), CV_8UC1,
            const_cast<uint8_t*>(f.gray8.data()));
  return m.clone();
}

}  // namespace

StereoDepthProcessor::StereoDepthProcessor(StereoCalibration calibration, StereoDepthConfig config)
    : calibration_(std::move(calibration)), config_(std::move(config)) {
  config_.num_disparities = std::max(16, ((config_.num_disparities + 15) / 16) * 16);
  if (config_.block_size % 2 == 0) ++config_.block_size;
  config_.block_size = std::max(3, config_.block_size);

  config_.roi_x_min = std::clamp(config_.roi_x_min, 0.0f, 1.0f);
  config_.roi_x_max = std::clamp(config_.roi_x_max, 0.0f, 1.0f);
  config_.roi_y_min = std::clamp(config_.roi_y_min, 0.0f, 1.0f);
  config_.roi_y_max = std::clamp(config_.roi_y_max, 0.0f, 1.0f);
  if (config_.roi_x_max <= config_.roi_x_min) {
    config_.roi_x_min = 0.0f;
    config_.roi_x_max = 1.0f;
  }
  if (config_.roi_y_max <= config_.roi_y_min) {
    config_.roi_y_min = 0.0f;
    config_.roi_y_max = 1.0f;
  }

  const int channels = 1;
  const int p1 = 8 * channels * config_.block_size * config_.block_size;
  const int p2 = 32 * channels * config_.block_size * config_.block_size;
  sgbm_ = cv::StereoSGBM::create(config_.min_disparity,
                                  config_.num_disparities,
                                  config_.block_size,
                                  p1,
                                  p2,
                                  config_.disp12_max_diff,
                                  config_.pre_filter_cap,
                                  config_.uniqueness_ratio,
                                  config_.speckle_window_size,
                                  config_.speckle_range,
                                  cv::StereoSGBM::MODE_SGBM_3WAY);
}

void StereoDepthProcessor::ensure_maps(uint32_t width, uint32_t height) {
  if (maps_ready_) return;
  if (int(width) != calibration_.cam0.width || int(height) != calibration_.cam0.height) {
    throw std::runtime_error("input image size does not match calibration resolution");
  }
  const cv::Size image_size(static_cast<int>(width), static_cast<int>(height));
  cv::fisheye::initUndistortRectifyMap(calibration_.K0, calibration_.D0, calibration_.R1, calibration_.P1,
                                       image_size, CV_16SC2, map0x_, map0y_);
  cv::fisheye::initUndistortRectifyMap(calibration_.K1, calibration_.D1, calibration_.R2, calibration_.P2,
                                       image_size, CV_16SC2, map1x_, map1y_);
  maps_ready_ = true;
}

StereoDepthResult StereoDepthProcessor::compute(const capture_client::ImageFrame& cam0,
                                                const capture_client::ImageFrame& cam1) {
  ensure_maps(cam0.width, cam0.height);

  const cv::Mat left = image_to_mat(cam0);
  const cv::Mat right = image_to_mat(cam1);

  StereoDepthResult out;
  cv::remap(left, out.left_rect, map0x_, map0y_, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
  cv::remap(right, out.right_rect, map1x_, map1y_, cv::INTER_LINEAR, cv::BORDER_CONSTANT);

  cv::Mat disp16;
  sgbm_->compute(out.left_rect, out.right_rect, disp16);
  disp16.convertTo(out.disparity_float, CV_32F, 1.0 / 16.0);

  cv::Mat xyz;
  cv::reprojectImageTo3D(out.disparity_float, xyz, calibration_.Q, true);

  out.cloud.sequence = cam0.sequence;
  out.cloud.timestamp_ns = std::max(cam0.timestamp_ns, cam1.timestamp_ns);
  out.cloud.frame_id = config_.depth_frame_id;
  out.cloud.points.reserve(size_t(cam0.width / std::max(1, config_.point_decimation)) *
                           size_t(cam0.height / std::max(1, config_.point_decimation)) / 2);

  const int step = std::max(1, config_.point_decimation);
  out.live_grid_decimation = static_cast<uint32_t>(step);
  out.live_grid_width = static_cast<uint32_t>((xyz.cols + step - 1) / step);
  out.live_grid_height = static_cast<uint32_t>((xyz.rows + step - 1) / step);
  out.live_grid_points.assign(static_cast<size_t>(out.live_grid_width) * out.live_grid_height, {});
  out.live_grid_valid.assign(out.live_grid_points.size(), 0);

  const int roi_x0 = std::clamp(static_cast<int>(std::floor(config_.roi_x_min * xyz.cols)), 0, xyz.cols);
  const int roi_x1 = std::clamp(static_cast<int>(std::ceil(config_.roi_x_max * xyz.cols)), 0, xyz.cols);
  const int roi_y0 = std::clamp(static_cast<int>(std::floor(config_.roi_y_min * xyz.rows)), 0, xyz.rows);
  const int roi_y1 = std::clamp(static_cast<int>(std::ceil(config_.roi_y_max * xyz.rows)), 0, xyz.rows);

  double sum_z = 0.0;
  float min_z = std::numeric_limits<float>::infinity();
  float max_z = 0.0f;

  for (int y = 0, gy = 0; y < xyz.rows; y += step, ++gy) {
    const cv::Vec3f* row = xyz.ptr<cv::Vec3f>(y);
    const float* drow = out.disparity_float.ptr<float>(y);
    for (int x = 0, gx = 0; x < xyz.cols; x += step, ++gx) {
      const size_t grid_idx = static_cast<size_t>(gy) * out.live_grid_width + static_cast<size_t>(gx);
      ++out.candidate_pixels;
      if (x < roi_x0 || x >= roi_x1 || y < roi_y0 || y >= roi_y1) {
        ++out.rejected_roi_pixels;
        continue;
      }

      const float disp = drow[x];
      const cv::Vec3f p = row[x];
      if (!std::isfinite(disp) || disp <= float(config_.min_disparity)) {
        ++out.rejected_disparity_pixels;
        continue;
      }
      if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        ++out.rejected_nonfinite_points;
        continue;
      }
      const float z = p[2];
      if (z < config_.min_depth_m || z > config_.max_depth_m) {
        ++out.rejected_depth_range_points;
        continue;
      }
      if (config_.max_abs_camera_coord_m > 0.0f &&
          (std::abs(p[0]) > config_.max_abs_camera_coord_m ||
           std::abs(p[1]) > config_.max_abs_camera_coord_m ||
           std::abs(p[2]) > config_.max_abs_camera_coord_m)) {
        ++out.rejected_abs_coord_points;
        continue;
      }
      const Vec3f point{p[0], p[1], p[2]};
      out.cloud.points.push_back(point);
      if (grid_idx < out.live_grid_points.size()) {
        out.live_grid_points[grid_idx] = point;
        out.live_grid_valid[grid_idx] = 1;
        ++out.live_grid_valid_points;
      }
      min_z = std::min(min_z, z);
      max_z = std::max(max_z, z);
      sum_z += z;
    }
  }

  out.accepted_points = out.cloud.points.size();
  if (out.candidate_pixels > 0) {
    out.accepted_pixel_fraction = static_cast<float>(
        static_cast<double>(out.accepted_points) / static_cast<double>(out.candidate_pixels));
  }

  if (!out.cloud.points.empty()) {
    out.cloud.min_depth_m = min_z;
    out.cloud.max_depth_m = max_z;
    out.cloud.mean_depth_m = static_cast<float>(sum_z / double(out.cloud.points.size()));
  }

  maybe_save_debug(out);
  return out;
}

void StereoDepthProcessor::maybe_save_debug(const StereoDepthResult& result) {
  if (!config_.save_first_debug_frame || debug_saved_ || config_.debug_dir.empty()) return;
  namespace fs = std::filesystem;
  fs::create_directories(config_.debug_dir);
  cv::imwrite((fs::path(config_.debug_dir) / "left_rect.png").string(), result.left_rect);
  cv::imwrite((fs::path(config_.debug_dir) / "right_rect.png").string(), result.right_rect);

  cv::Mat disp_vis;
  const double max_disp = std::max(1, config_.num_disparities);
  result.disparity_float.convertTo(disp_vis, CV_8U, 255.0 / max_disp);
  cv::imwrite((fs::path(config_.debug_dir) / "disparity_preview.png").string(), disp_vis);
  debug_saved_ = true;
}

}  // namespace xr_spatial_backend
