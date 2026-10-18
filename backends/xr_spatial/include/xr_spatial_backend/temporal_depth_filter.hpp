#pragma once

#include <deque>

#include <xr_spatial_backend/pose_math.hpp>
#include <xr_spatial_backend/stereo_depth_processor.hpp>

namespace xr_spatial_backend {

struct TemporalDepthFilterConfig {
  bool enabled = true;
  int history_frames = 2;
  int min_consistent_frames = 2; // Includes the current frame; votes are per frame, not per point.
  float abs_tolerance_m = 0.04f;
  float relative_tolerance = 0.03f;
  float current_weight = 0.6f;
  int max_age_ms = 500;
};

struct TemporalDepthFilterStats {
  uint64_t frames = 0;
  uint64_t input_points = 0;
  uint64_t output_points = 0;
  uint64_t rejected_points = 0;
  uint64_t smoothed_points = 0;
  uint64_t resets = 0;
  uint64_t duplicate_frames = 0;
};

// Reprojects raw historical depth into the current rectified camera. Only current
// valid pixels can survive: history confirms/smooths them, but never fills holes.
class TemporalDepthFilter {
 public:
  TemporalDepthFilter(TemporalDepthFilterConfig config, double fx, double fy,
                      double cx, double cy);
  TemporalDepthFilterStats apply(StereoDepthResult& depth,
                                  const Transform3d& T_tracking_camera,
                                  int64_t timestamp_ns);
  void reset();
  const TemporalDepthFilterStats& stats() const { return stats_; }

 private:
  struct Frame {
    int64_t timestamp_ns;
    Transform3d T_tracking_camera;
    std::vector<Vec3f> points;
    std::vector<uint8_t> valid;
  };
  TemporalDepthFilterConfig config_;
  double fx_, fy_, cx_, cy_;
  uint32_t width_ = 0, height_ = 0, decimation_ = 0;
  std::deque<Frame> history_;
  std::vector<std::vector<float>> projected_depths_;
  TemporalDepthFilterStats stats_;
};

}  // namespace xr_spatial_backend
