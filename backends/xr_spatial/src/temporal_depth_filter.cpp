#include <xr_spatial_backend/temporal_depth_filter.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace xr_spatial_backend {
namespace {
bool finite_transform(const Transform3d& t) {
  if (!std::isfinite(t.t.x) || !std::isfinite(t.t.y) || !std::isfinite(t.t.z)) return false;
  for (const auto& row : t.R.v) for (double v : row) if (!std::isfinite(v)) return false;
  return true;
}

void rebuild_cloud(StereoDepthResult& depth) {
  auto& cloud = depth.cloud;
  cloud.points.clear();
  float min_z = std::numeric_limits<float>::infinity(), max_z = 0.0f;
  double sum_z = 0.0;
  for (size_t i = 0; i < depth.live_grid_valid.size(); ++i) {
    if (!depth.live_grid_valid[i]) continue;
    const auto& p = depth.live_grid_points[i];
    cloud.points.push_back(p);
    min_z = std::min(min_z, p.z);
    max_z = std::max(max_z, p.z);
    sum_z += p.z;
  }
  const size_t n = cloud.points.size();
  cloud.min_depth_m = n ? min_z : 0.0f;
  cloud.max_depth_m = n ? max_z : 0.0f;
  cloud.mean_depth_m = n ? static_cast<float>(sum_z / n) : 0.0f;
  depth.live_grid_valid_points = static_cast<uint32_t>(n);
  depth.accepted_points = n;
  depth.accepted_pixel_fraction = depth.candidate_pixels
      ? static_cast<float>(n) / depth.candidate_pixels : 0.0f;
}
}  // namespace

TemporalDepthFilter::TemporalDepthFilter(TemporalDepthFilterConfig config,
                                         double fx, double fy, double cx, double cy)
    : config_(config), fx_(fx), fy_(fy), cx_(cx), cy_(cy) {
  if (config.history_frames < 1 || config.history_frames > 8 ||
      config.min_consistent_frames < 1 || config.min_consistent_frames > config.history_frames + 1 ||
      !std::isfinite(config.abs_tolerance_m) || config.abs_tolerance_m < 0 ||
      !std::isfinite(config.relative_tolerance) || config.relative_tolerance < 0 ||
      !std::isfinite(config.current_weight) || config.current_weight < 0 || config.current_weight > 1 ||
      config.max_age_ms <= 0) {
    throw std::invalid_argument("invalid temporal depth filter settings: history=1..8, "
        "min frames=1..history+1, tolerances>=0, current weight=0..1, max age>0");
  }
  if (!std::isfinite(fx) || !std::isfinite(fy) || fx <= 0 || fy <= 0 ||
      !std::isfinite(cx) || !std::isfinite(cy)) {
    throw std::invalid_argument("invalid rectified camera intrinsics for temporal depth filter");
  }
}

void TemporalDepthFilter::reset() {
  if (!history_.empty()) ++stats_.resets;
  history_.clear();
  width_ = height_ = decimation_ = 0;
}

TemporalDepthFilterStats TemporalDepthFilter::apply(StereoDepthResult& depth,
                                                    const Transform3d& T_tracking_camera,
                                                    int64_t timestamp_ns) {
  TemporalDepthFilterStats frame_stats;
  if (!config_.enabled) return frame_stats;
  const size_t size = static_cast<size_t>(depth.live_grid_width) * depth.live_grid_height;
  if (!depth.live_grid_width || !depth.live_grid_height || !depth.live_grid_decimation ||
      depth.live_grid_points.size() != size || depth.live_grid_valid.size() != size) {
    throw std::invalid_argument("invalid organized grid for temporal depth filter");
  }
  if (!finite_transform(T_tracking_camera) || timestamp_ns <= 0) {
    reset();
    throw std::invalid_argument("invalid camera pose/timestamp for temporal depth filter");
  }
  if (width_ != depth.live_grid_width || height_ != depth.live_grid_height ||
      decimation_ != depth.live_grid_decimation ||
      (!history_.empty() && timestamp_ns < history_.back().timestamp_ns)) reset();
  width_ = depth.live_grid_width;
  height_ = depth.live_grid_height;
  decimation_ = depth.live_grid_decimation;
  const int64_t max_age_ns = int64_t(config_.max_age_ms) * 1'000'000;
  while (!history_.empty() && timestamp_ns - history_.front().timestamp_ns > max_age_ns) {
    history_.pop_front();
    if (history_.empty()) ++stats_.resets;
  }
  frame_stats.frames = 1;
  for (size_t i = 0; i < size; ++i) {
    if (depth.live_grid_valid[i] && is_finite(depth.live_grid_points[i]) && depth.live_grid_points[i].z > 0)
      ++frame_stats.input_points;
    else depth.live_grid_valid[i] = 0;
  }
  const bool duplicate = !history_.empty() && timestamp_ns == history_.back().timestamp_ns;
  if (duplicate) {
    // A duplicate must neither supply another vote nor get integrated twice.
    std::fill(depth.live_grid_valid.begin(), depth.live_grid_valid.end(), 0);
    ++frame_stats.duplicate_frames;
  } else {
    projected_depths_.resize(history_.size());
    const auto T_camera_tracking = inverse(T_tracking_camera);
    for (size_t h = 0; h < history_.size(); ++h) {
      auto& projected = projected_depths_[h];
      projected.assign(size, 0.0f);
      const auto& previous = history_[h];
      const auto T_camera_previous = compose(T_camera_tracking, previous.T_tracking_camera);
      for (size_t i = 0; i < size; ++i) {
        if (!previous.valid[i]) continue;
        const auto p = transform_point(T_camera_previous, previous.points[i]);
        if (!is_finite(p) || p.z <= 0) continue;
        const double x = (fx_ * p.x / p.z + cx_) / decimation_;
        const double y = (fy_ * p.y / p.z + cy_) / decimation_;
        if (!std::isfinite(x) || !std::isfinite(y) || x <= -0.5 || y <= -0.5 ||
            x >= double(width_) - 0.5 || y >= double(height_) - 0.5) continue;
        const size_t index = static_cast<size_t>(std::llround(y)) * width_ + std::llround(x);
        // Keep the visible surface when historical points land in the same cell.
        if (projected[index] == 0 || p.z < projected[index]) projected[index] = p.z;
      }
    }

    // Preserve RAW history even during warm-up or when quality gates reject the
    // filtered output; otherwise no point could ever receive its second vote.
    Frame current{timestamp_ns, T_tracking_camera, depth.live_grid_points, depth.live_grid_valid};
    for (uint32_t y = 0; y < height_; ++y) {
      for (uint32_t x = 0; x < width_; ++x) {
        const size_t i = static_cast<size_t>(y) * width_ + x;
        if (!depth.live_grid_valid[i]) continue;
        auto& p = depth.live_grid_points[i];
        const float tolerance = std::max(config_.abs_tolerance_m, config_.relative_tolerance * p.z);
        int supports = 0;
        double sum_z = 0.0;
        for (const auto& projected : projected_depths_) {
          float best_delta = std::numeric_limits<float>::infinity(), best_z = 0;
          // Decimated samples move between cells during camera motion. A one-cell
          // search avoids discarding the same surface due to raster quantization.
          for (int yy = std::max(0, int(y) - 1); yy <= std::min(int(height_) - 1, int(y) + 1); ++yy) {
            for (int xx = std::max(0, int(x) - 1); xx <= std::min(int(width_) - 1, int(x) + 1); ++xx) {
              const float z = projected[static_cast<size_t>(yy) * width_ + xx];
              const float delta = std::abs(z - p.z);
              if (z > 0 && delta <= tolerance && delta < best_delta) {
                best_delta = delta;
                best_z = z;
              }
            }
          }
          if (best_z > 0) { ++supports; sum_z += best_z; }
        }
        if (supports + 1 < config_.min_consistent_frames) {
          depth.live_grid_valid[i] = 0;
        } else if (supports && config_.current_weight < 1.0f) {
          const float z = static_cast<float>(config_.current_weight * p.z +
              (1.0 - config_.current_weight) * sum_z / supports);
          const float scale = z / p.z;
          p.x *= scale;
          p.y *= scale;
          p.z = z;
          ++frame_stats.smoothed_points;
        }
      }
    }
    history_.push_back(std::move(current));
    while (history_.size() > static_cast<size_t>(config_.history_frames)) history_.pop_front();
  }
  rebuild_cloud(depth);
  frame_stats.output_points = depth.cloud.points.size();
  frame_stats.rejected_points = frame_stats.input_points - frame_stats.output_points;
  stats_.frames += frame_stats.frames;
  stats_.input_points += frame_stats.input_points;
  stats_.output_points += frame_stats.output_points;
  stats_.rejected_points += frame_stats.rejected_points;
  stats_.smoothed_points += frame_stats.smoothed_points;
  stats_.duplicate_frames += frame_stats.duplicate_frames;
  return frame_stats;
}

}  // namespace xr_spatial_backend
