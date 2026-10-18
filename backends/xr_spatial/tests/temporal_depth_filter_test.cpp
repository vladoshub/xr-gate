#include <xr_spatial_backend/temporal_depth_filter.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace xr_spatial_backend;

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
void near(float actual, float expected, const char* message, float tolerance = 1e-5f) {
  check(std::abs(actual - expected) < tolerance, message);
}

StereoDepthResult grid(float z = 1.0f, uint32_t width = 7, uint32_t height = 5) {
  StereoDepthResult d;
  d.live_grid_width = width;
  d.live_grid_height = height;
  d.live_grid_decimation = 4;
  d.live_grid_points.resize(width * height);
  d.live_grid_valid.assign(width * height, 1);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    d.live_grid_points[y * width + x] = {(x * 4.0f - 12) / 20 * z, (y * 4.0f - 8) / 20 * z, z};
  }
  d.cloud.points = d.live_grid_points;
  d.cloud.sequence = 123;
  d.cloud.timestamp_ns = 999;
  d.cloud.frame_id = "camera";
  d.live_grid_valid_points = width * height;
  d.accepted_points = d.candidate_pixels = width * height;
  d.accepted_pixel_fraction = 1;
  return d;
}
TemporalDepthFilter filter(TemporalDepthFilterConfig cfg = {}) {
  return TemporalDepthFilter(cfg, 20, 20, 12, 8);
}
constexpr int64_t t = 1'000'000'000;
constexpr int64_t dt = 100'000'000;
constexpr size_t center = 2 * 7 + 3;

void confirmation_and_noise() {
  auto f = filter();
  auto d = grid();
  f.apply(d, {}, t);
  check(d.cloud.points.empty() && d.live_grid_valid_points == 0, "first frame bypassed confirmation");
  d = grid(1.02f);
  const auto stats = f.apply(d, {}, t + dt);
  check(stats.output_points == 35 && stats.smoothed_points == 35, "stable surface not confirmed");
  near(d.live_grid_points[center].z, 1.012f, "depth not smoothed");
  near(d.cloud.mean_depth_m, 1.012f, "cloud statistics not rebuilt");
  check(d.cloud.sequence == 123 && d.cloud.timestamp_ns == 999 && d.cloud.frame_id == "camera",
        "filter changed source identity or coordinates label");
  near(d.live_grid_points[0].x / d.live_grid_points[0].z, -.6f, "smoothing changed current pixel ray");
  d = grid(.4f);
  f.apply(d, {}, t + 2 * dt);
  check(d.cloud.points.empty(), "single-frame foreground outlier survived");
  d = grid(1.01f);
  f.apply(d, {}, t + 3 * dt);
  check(d.live_grid_valid_points == 35, "two-of-three confirmation failed after outlier");
  near(d.live_grid_points[center].z, 1.014f, "outlier contaminated smoothing");
}

void new_surface_and_holes() {
  auto f = filter();
  auto d = grid(); f.apply(d, {}, t);
  d = grid(); f.apply(d, {}, t + dt);
  d = grid(.4f); f.apply(d, {}, t + 2 * dt);
  check(d.cloud.points.empty(), "new surface appeared without confirmation");
  d = grid(.4f); f.apply(d, {}, t + 3 * dt);
  near(d.live_grid_points[center].z, .4f, "old background averaged into confirmed foreground");
  check(d.live_grid_valid_points == 35, "new foreground never confirmed");
  d = grid(.4f);
  d.live_grid_valid[center] = 0;
  d.live_grid_points[0].z = std::numeric_limits<float>::quiet_NaN();
  f.apply(d, {}, t + 4 * dt);
  check(!d.live_grid_valid[center] && !d.live_grid_valid[0] && d.cloud.points.size() == 33,
        "history filled a missing or invalid current pixel");
  near(d.accepted_pixel_fraction, 33.0f / 35, "accepted fraction differs from filtered grid");
}

void motion_compensation() {
  auto f = filter();
  auto d = grid(); f.apply(d, {}, t);
  Transform3d pose;
  pose.t.z = .15;
  d = grid(.85f); f.apply(d, pose, t + dt);
  check(d.live_grid_valid[center], "forward head motion rejected a static wall");
  near(d.live_grid_points[center].z, .85f, "forward motion changed wall geometry");
  f = filter();
  d = grid(); f.apply(d, {}, t);
  pose = {}; pose.t.x = .2;
  d = grid(); f.apply(d, pose, t + dt);
  check(d.live_grid_valid[center] && d.live_grid_valid_points >= 20, "lateral head motion lost overlapping surface");

  TemporalDepthFilterConfig cfg;
  cfg.abs_tolerance_m = .015f; cfg.relative_tolerance = 0;
  f = filter(cfg);
  d = grid(); f.apply(d, {}, t);
  const double angle = 20.0 * 3.141592653589793 / 180;
  pose = pose_to_transform(0, 0, 0, std::cos(angle / 2), 0, std::sin(angle / 2), 0);
  d = grid();
  for (auto& p : d.live_grid_points) {
    const float z = static_cast<float>(1 / (pose.R.v[2][0] * p.x + pose.R.v[2][1] * p.y + pose.R.v[2][2]));
    p = {p.x * z, p.y * z, z};
  }
  f.apply(d, pose, t + dt);
  check(d.live_grid_valid[center], "head rotation did not reproject a static wall");
  near(d.live_grid_points[center].z, static_cast<float>(1 / std::cos(angle)),
       "rotation smoothed different surfaces", .01f);

  f = filter(); d = grid(); f.apply(d, {}, t);
  pose = {}; pose.t.z = 2;
  d = grid(); f.apply(d, pose, t + dt);
  check(d.cloud.points.empty(), "history behind the camera confirmed unrelated depth");
}

void distinct_frames_and_resets() {
  auto f = filter(); auto d = grid(); f.apply(d, {}, t);
  d = grid(); auto stats = f.apply(d, {}, t);
  check(d.cloud.points.empty() && stats.duplicate_frames == 1, "duplicate frame counted as temporal evidence");
  d = grid(); f.apply(d, {}, t + dt);
  check(d.live_grid_valid_points == 35, "duplicate corrupted raw history");
  f.reset(); d = grid(); f.apply(d, {}, t + 2 * dt);
  check(d.cloud.points.empty(), "explicit tracking reset retained history");
  d = grid(); f.apply(d, {}, t + 10 * dt);
  check(d.cloud.points.empty(), "expired frames still confirmed depth");
  d = grid(); f.apply(d, {}, t + 9 * dt);
  check(d.cloud.points.empty(), "backward clock retained future history");
  d = grid(1, 6, 5); f.apply(d, {}, t + 11 * dt);
  check(d.cloud.points.empty(), "grid resize retained incompatible history");
  check(f.stats().resets >= 4, "history reset diagnostics missing");

  TemporalDepthFilterConfig cfg; cfg.min_consistent_frames = 3;
  f = filter(cfg);
  d = grid(); f.apply(d, {}, t);
  d = grid(); f.apply(d, {}, t + dt);
  check(d.cloud.points.empty(), "multiple neighbors counted as multiple frames");
  d = grid(); f.apply(d, {}, t + 2 * dt);
  check(d.live_grid_valid_points == 35, "three-frame confirmation did not warm up");
  cfg.min_consistent_frames = 1; cfg.current_weight = 1;
  f = filter(cfg); d = grid(); f.apply(d, {}, t);
  check(d.live_grid_valid_points == 35, "single-frame option rejected first frame");
}

void occlusion_and_projection_bounds() {
  TemporalDepthFilter f({}, 20, 20, 4, 0);
  auto d = grid(1, 3, 1);
  d.live_grid_valid = {1, 1, 0};
  d.live_grid_points = {{0, 0, .4f}, {0, 0, 1}, {0, 0, 0}};
  f.apply(d, {}, t);
  d.live_grid_valid = {0, 1, 0};
  d.live_grid_points = {{0, 0, 0}, {0, 0, 1}, {0, 0, 0}};
  f.apply(d, {}, t + dt);
  check(d.cloud.points.empty(), "hidden background won historical z-buffer collision");
  // Exactly -0.5 rounds to -1; it must not become an unsigned array index.
  f.reset(); d = grid(1, 3, 1);
  d.live_grid_valid = {1, 0, 0};
  d.live_grid_points[0] = {-.3f, 0, 1};
  f.apply(d, {}, t);
  d = grid(1, 3, 1); f.apply(d, {}, t + dt);
}

void disable_and_validation() {
  TemporalDepthFilterConfig cfg; cfg.enabled = false;
  auto f = filter(cfg); auto d = grid(.4f);
  f.apply(d, {}, t);
  check(d.cloud.points.size() == 35 && d.live_grid_valid_points == 35 && f.stats().frames == 0,
        "disabled filter changed frame");
  near(d.live_grid_points[center].z, .4f, "disabled filter smoothed depth");
  auto invalid = [](TemporalDepthFilterConfig c) {
    try { auto unused = filter(c); (void)unused; return false; }
    catch (const std::invalid_argument&) { return true; }
  };
  cfg = {}; cfg.history_frames = 9; check(invalid(cfg), "unbounded history accepted");
  cfg = {}; cfg.min_consistent_frames = 4; check(invalid(cfg), "impossible confirmation accepted");
  cfg = {}; cfg.abs_tolerance_m = -1; check(invalid(cfg), "negative tolerance accepted");
  cfg = {}; cfg.relative_tolerance = std::numeric_limits<float>::quiet_NaN(); check(invalid(cfg), "NaN accepted");
  cfg = {}; cfg.current_weight = 1.1f; check(invalid(cfg), "invalid smoothing weight accepted");
  cfg = {}; cfg.max_age_ms = 0; check(invalid(cfg), "unbounded age accepted");
  f = filter(); d = grid(); f.apply(d, {}, t);
  Transform3d bad; bad.t.x = std::numeric_limits<double>::infinity();
  bool threw = false;
  try { d = grid(); f.apply(d, bad, t + dt); } catch (const std::invalid_argument&) { threw = true; }
  check(threw, "non-finite motion accepted");
  d = grid(); f.apply(d, {}, t + 2 * dt);
  check(d.cloud.points.empty(), "bad motion did not clear history");
}
}  // namespace

int main() {
  try {
    confirmation_and_noise();
    new_surface_and_holes();
    motion_compensation();
    distinct_frames_and_resets();
    occlusion_and_projection_bounds();
    disable_and_validation();
    std::cout << "temporal depth: noise, motion, occlusion, resets and disable checks passed\n";
  } catch (const std::exception& e) {
    std::cerr << "temporal depth regression: " << e.what() << '\n';
    return 1;
  }
}
