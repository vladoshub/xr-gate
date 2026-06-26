#include <openvr.h>

#ifndef _WIN32
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#endif

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_runtime/contracts/runtime_pose_stream.hpp>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using Mesh = xr_spatial::RuntimeSpatialProxyMeshF32V1;
using MeshReader = xr_runtime::TrackingRingReader<Mesh>;
using RuntimePose = xr_runtime::RuntimeHmdPoseF64V1;
using RuntimePoseReader = xr_runtime::TrackingRingReader<RuntimePose>;

uint64_t now_ns_u64() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string wall_time_string() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count() % 1000;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream os;
  os << std::put_time(&tm, "%F %T") << "." << std::setw(3) << std::setfill('0') << ms;
  return os.str();
}

class Logger {
 public:
  explicit Logger(std::string file) : file_(std::move(file)) {
    if (!file_.empty()) {
      std::filesystem::path p(file_);
      if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path());
      out_.open(file_, std::ios::app);
    }
  }

  template <typename... Args>
  void info(Args&&... args) { line("INFO", std::forward<Args>(args)...); }
  template <typename... Args>
  void warn(Args&&... args) { line("WARN", std::forward<Args>(args)...); }
  template <typename... Args>
  void error(Args&&... args) { line("ERROR", std::forward<Args>(args)...); }
  template <typename... Args>
  void debug(Args&&... args) { line("DEBUG", std::forward<Args>(args)...); }

 private:
  template <typename... Args>
  void line(const char* level, Args&&... args) {
    std::ostringstream msg;
    (msg << ... << args);
    const std::string s = "[" + wall_time_string() + "][xr_steamvr_spatial_overlay][" + level + "] " + msg.str();
    std::cerr << s << "\n";
    if (out_) {
      out_ << s << "\n";
      out_.flush();
    }
  }

  std::string file_;
  std::ofstream out_;
};

std::string env_string(const char* name, const std::string& def) {
  if (const char* v = std::getenv(name); v && *v) return std::string(v);
  return def;
}

int env_int(const char* name, int def) {
  if (const char* v = std::getenv(name); v && *v) {
    try { return std::stoi(v); } catch (...) {}
  }
  return def;
}

double env_double(const char* name, double def) {
  if (const char* v = std::getenv(name); v && *v) {
    try { return std::stod(v); } catch (...) {}
  }
  return def;
}

bool env_bool(const char* name, bool def) {
  if (const char* v = std::getenv(name); v && *v) {
    const std::string s(v);
    if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "off") return false;
  }
  return def;
}

struct Config {
  std::string registry = env_string("SPATIAL_OVERLAY_REGISTRY", "/tmp/runtime_tracking_streams.json");
  std::string stream = env_string("SPATIAL_OVERLAY_STREAM", "runtime_spatial_proxy_mesh");
  std::string log_file = env_string("SPATIAL_OVERLAY_LOG_FILE", "/tmp/xr_steamvr_spatial_overlay.log");
  std::string overlay_key = env_string("SPATIAL_OVERLAY_KEY", "xr_tracking.spatial_overlay");
  std::string overlay_name = env_string("SPATIAL_OVERLAY_NAME", "XR Spatial Overlay");

  // Use xr_runtime_adapter output as the camera pose for projection.
  // This keeps mesh projection in the same runtime_local frame as runtime_spatial_proxy_mesh.
  // openvr is kept as a fallback/debug source. identity is dry-run/debug only.
  std::string pose_source = env_string("SPATIAL_OVERLAY_POSE_SOURCE", "xr_runtime"); // xr_runtime|openvr|identity
  std::string pose_registry = env_string("SPATIAL_OVERLAY_POSE_REGISTRY", "/tmp/runtime_tracking_streams.json");
  std::string pose_stream = env_string("SPATIAL_OVERLAY_POSE_STREAM", "runtime_hmd_pose");
  bool clear_when_no_mesh = env_bool("SPATIAL_OVERLAY_CLEAR_WHEN_NO_MESH", true);
  bool clear_when_no_pose = env_bool("SPATIAL_OVERLAY_CLEAR_WHEN_NO_POSE", true);
  bool clear_on_stale_skip = env_bool("SPATIAL_OVERLAY_CLEAR_ON_STALE_SKIP", true);
  bool pose_verbose = env_bool("SPATIAL_OVERLAY_POSE_VERBOSE", false);

  int width = env_int("SPATIAL_OVERLAY_WIDTH", 1280);
  int height = env_int("SPATIAL_OVERLAY_HEIGHT", 720);
  double render_hz = env_double("SPATIAL_OVERLAY_RENDER_HZ", 60.0);

  // Stereo overlay mode renders a side-by-side texture and asks SteamVR to
  // sample the left/right halves per eye.  This is still an OpenVR overlay
  // quad, but it provides proper binocular disparity for the projected mesh.
  bool stereo_sbs = env_bool("SPATIAL_OVERLAY_STEREO_SBS", false);
  bool stereo_openvr_flag = env_bool("SPATIAL_OVERLAY_STEREO_OPENVR_FLAG", true);
  double stereo_ipd_m = env_double("SPATIAL_OVERLAY_STEREO_IPD_M", 0.064);
  bool stereo_draw_hud = env_bool("SPATIAL_OVERLAY_STEREO_DRAW_HUD", false);
  double log_every_sec = env_double("SPATIAL_OVERLAY_LOG_EVERY_SEC", 1.0);
  double attach_retry_ms = env_double("SPATIAL_OVERLAY_ATTACH_RETRY_MS", 500.0);

  // SetOverlayRaw can fail under high-rate raw IPC uploads on Linux.
  // OpenGL texture submit is the stable path for continuously updated overlays.
  std::string submit_mode = env_string("SPATIAL_OVERLAY_SUBMIT_MODE", "opengl"); // opengl|raw|auto
  // SteamVR/OpenGL texture origin can differ from our CPU canvas.  Keep this
  // in overlay config; it is a 2D texture upload fix, not a 3D mesh transform.
  bool texture_flip_y = env_bool("SPATIAL_OVERLAY_TEXTURE_FLIP_Y", true);
  bool texture_flip_x = env_bool("SPATIAL_OVERLAY_TEXTURE_FLIP_X", false);
  bool texture_rotate_180 = env_bool("SPATIAL_OVERLAY_TEXTURE_ROTATE_180", false);

  double overlay_width_m = env_double("SPATIAL_OVERLAY_QUAD_WIDTH_M", 2.2);
  double overlay_distance_m = env_double("SPATIAL_OVERLAY_QUAD_DISTANCE_M", 1.0);
  double overlay_y_offset_m = env_double("SPATIAL_OVERLAY_QUAD_Y_OFFSET_M", 0.0);
  double alpha = env_double("SPATIAL_OVERLAY_ALPHA", 0.75);
  double vertical_fov_deg = env_double("SPATIAL_OVERLAY_VERTICAL_FOV_DEG", 75.0);
  double min_depth_m = env_double("SPATIAL_OVERLAY_MIN_DEPTH_M", 0.05);
  double max_depth_m = env_double("SPATIAL_OVERLAY_MAX_DEPTH_M", 8.0);
  double max_mesh_age_ms = env_double("SPATIAL_OVERLAY_MAX_MESH_AGE_MS", 3000.0);

  // A spatial map is usually static.  Staleness means "not refreshed recently",
  // not "invalid geometry".  Keep drawing the last mesh by default and only
  // warn loudly, otherwise a finished scan makes the overlay disappear.
  bool draw_stale_mesh = env_bool("SPATIAL_OVERLAY_DRAW_STALE_MESH", true);
  bool visibility_test_pattern = env_bool("SPATIAL_OVERLAY_TEST_PATTERN", false);
  bool force_visible_border = env_bool("SPATIAL_OVERLAY_FORCE_VISIBLE_BORDER", false);

  bool dry_run = env_bool("SPATIAL_OVERLAY_DRY_RUN", false);
  // Passthrough rendering controls.  Mesh fill is the primary mode; wire/points
  // are optional diagnostics drawn over it.  The input contract is organized
  // vertices + optional triangle indices, so point mode must draw only projected
  // finite/valid vertices and mesh mode must draw only triangles.
  bool draw_mesh = env_bool("SPATIAL_OVERLAY_DRAW_MESH", true);
  bool draw_wire = env_bool("SPATIAL_OVERLAY_DRAW_WIRE", false);
  bool draw_points = env_bool("SPATIAL_OVERLAY_DRAW_POINTS", false);
  bool draw_bbox = env_bool("SPATIAL_OVERLAY_DRAW_BBOX", false);
  int point_radius_px = env_int("SPATIAL_OVERLAY_POINT_RADIUS_PX", 2);
  double mesh_alpha = env_double("SPATIAL_OVERLAY_MESH_ALPHA", 0.38);
  double wire_alpha = env_double("SPATIAL_OVERLAY_WIRE_ALPHA", 0.82);
  double point_alpha = env_double("SPATIAL_OVERLAY_POINT_ALPHA", 0.85);
  bool draw_distance_markers = env_bool("SPATIAL_OVERLAY_DRAW_DISTANCE_MARKERS", true);
  bool color_by_distance = env_bool("SPATIAL_OVERLAY_COLOR_BY_DISTANCE", true);
  double distance_marker_step_m = env_double("SPATIAL_OVERLAY_DISTANCE_MARKER_STEP_M", 0.5);
  double distance_marker_max_m = env_double("SPATIAL_OVERLAY_DISTANCE_MARKER_MAX_M", 3.0);
  bool verbose_frames = env_bool("SPATIAL_OVERLAY_VERBOSE_FRAMES", false);
};

void print_help() {
  std::cout << "xr_steamvr_spatial_overlay\n"
            << "  Reads runtime_spatial_proxy_mesh from SHM and displays a diagnostic SteamVR overlay.\n\n"
            << "Options mirror env vars:\n"
            << "  --registry PATH                    default SPATIAL_OVERLAY_REGISTRY\n"
            << "  --stream NAME                      default SPATIAL_OVERLAY_STREAM\n"
            << "  --log-file PATH                    default SPATIAL_OVERLAY_LOG_FILE\n"
            << "  --width N --height N               overlay raw texture size\n"
            << "  --render-hz HZ                     render/update rate\n"
            << "  --stereo-sbs / --no-stereo-sbs     render side-by-side left/right eye texture\n"
            << "  --stereo-ipd M                     stereo eye separation in meters\n"
            << "  --dry-run                          read/log SHM without OpenVR\n"
            << "  --verbose-frames                   log every frame stats\n"
            << "  --draw-mesh / --no-draw-mesh       filled triangle passthrough surface\n"
            << "  --draw-wire / --no-draw-wire       triangle wire overlay\n"
            << "  --draw-points / --no-draw-points   point overlay from valid projected vertices\n"
            << "  --point-radius N                   point overlay radius in pixels\n"
            << "  --draw-stale-mesh                  keep drawing stale/static mesh\n"
            << "  --no-draw-stale-mesh               hide mesh when older than max age\n"
            << "  --test-pattern                     draw visible diagnostic border/crosshair\n"
            << "  --no-visible-border                do not force diagnostic border over mesh\n"
            << "  --help\n";
}

bool parse_args(int argc, char** argv, Config& c) {
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
      return std::string(argv[++i]);
    };
    if (a == "--help" || a == "-h") { print_help(); return false; }
    if (a == "--registry") c.registry = need("--registry");
    else if (a == "--stream") c.stream = need("--stream");
    else if (a == "--log-file") c.log_file = need("--log-file");
    else if (a == "--pose-source") c.pose_source = need("--pose-source");
    else if (a == "--pose-registry") c.pose_registry = need("--pose-registry");
    else if (a == "--pose-stream") c.pose_stream = need("--pose-stream");
    else if (a == "--width") c.width = std::stoi(need("--width"));
    else if (a == "--height") c.height = std::stoi(need("--height"));
    else if (a == "--render-hz") c.render_hz = std::stod(need("--render-hz"));
    else if (a == "--stereo-sbs") c.stereo_sbs = true;
    else if (a == "--no-stereo-sbs") c.stereo_sbs = false;
    else if (a == "--stereo-ipd") c.stereo_ipd_m = std::stod(need("--stereo-ipd"));
    else if (a == "--dry-run") c.dry_run = true;
    else if (a == "--verbose-frames") c.verbose_frames = true;
    else if (a == "--draw-mesh") c.draw_mesh = true;
    else if (a == "--no-draw-mesh") c.draw_mesh = false;
    else if (a == "--draw-wire") c.draw_wire = true;
    else if (a == "--no-draw-wire") c.draw_wire = false;
    else if (a == "--draw-points") c.draw_points = true;
    else if (a == "--no-draw-points") c.draw_points = false;
    else if (a == "--point-radius") c.point_radius_px = std::stoi(need("--point-radius"));
    else if (a == "--draw-stale-mesh") c.draw_stale_mesh = true;
    else if (a == "--no-draw-stale-mesh") c.draw_stale_mesh = false;
    else if (a == "--test-pattern") c.visibility_test_pattern = true;
    else if (a == "--no-visible-border") c.force_visible_border = false;
    else throw std::runtime_error("unknown argument: " + a);
  }
  return true;
}

struct Vec3 { double x = 0, y = 0, z = 0; };
struct Pt2 { int x = 0, y = 0; double depth = 0; bool valid = false; };

struct Pose34 {
  double m[3][4]{};
};

Pose34 pose34_offset_local_x(const Pose34& pose, double local_x_m) {
  Pose34 out = pose;
  // device_to_absolute stores world-space basis vectors in the matrix columns.
  // Local +X is the HMD/right-eye direction in the same convention used by
  // world_to_hmd().  Shift only translation; orientation stays the same.
  out.m[0][3] += pose.m[0][0] * local_x_m;
  out.m[1][3] += pose.m[1][0] * local_x_m;
  out.m[2][3] += pose.m[2][0] * local_x_m;
  return out;
}

Vec3 world_to_hmd(const Pose34& device_to_abs, const Vec3& p_abs) {
  const double tx = device_to_abs.m[0][3];
  const double ty = device_to_abs.m[1][3];
  const double tz = device_to_abs.m[2][3];
  const double dx = p_abs.x - tx;
  const double dy = p_abs.y - ty;
  const double dz = p_abs.z - tz;
  // inverse rigid transform: R^T * (p_abs - t)
  return {
      device_to_abs.m[0][0] * dx + device_to_abs.m[1][0] * dy + device_to_abs.m[2][0] * dz,
      device_to_abs.m[0][1] * dx + device_to_abs.m[1][1] * dy + device_to_abs.m[2][1] * dz,
      device_to_abs.m[0][2] * dx + device_to_abs.m[1][2] * dy + device_to_abs.m[2][2] * dz,
  };
}


bool runtime_pose_is_valid(const RuntimePose& p) {
  if (p.sequence == 0) return false;
  if ((p.flags & xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID) == 0u) return false;
  return std::isfinite(p.px) && std::isfinite(p.py) && std::isfinite(p.pz) &&
         std::isfinite(p.qw) && std::isfinite(p.qx) && std::isfinite(p.qy) && std::isfinite(p.qz);
}

Pose34 pose34_from_runtime_pose(const RuntimePose& p) {
  Pose34 out{};
  double w = p.qw;
  double x = p.qx;
  double y = p.qy;
  double z = p.qz;
  const double n = std::sqrt(w * w + x * x + y * y + z * z);
  if (n > 1e-12 && std::isfinite(n)) {
    w /= n; x /= n; y /= n; z /= n;
  } else {
    w = 1.0; x = y = z = 0.0;
  }

  const double xx = x * x;
  const double yy = y * y;
  const double zz = z * z;
  const double xy = x * y;
  const double xz = x * z;
  const double yz = y * z;
  const double wx = w * x;
  const double wy = w * y;
  const double wz = w * z;

  // RuntimeHmdPoseF64V1 quaternion is HMD/device orientation in runtime_local.
  // Build an OpenVR-like device_to_absolute 3x4 matrix so world_to_hmd() can
  // reuse the same inverse projection path for both OpenVR and xr_runtime poses.
  out.m[0][0] = 1.0 - 2.0 * (yy + zz);
  out.m[0][1] = 2.0 * (xy - wz);
  out.m[0][2] = 2.0 * (xz + wy);
  out.m[1][0] = 2.0 * (xy + wz);
  out.m[1][1] = 1.0 - 2.0 * (xx + zz);
  out.m[1][2] = 2.0 * (yz - wx);
  out.m[2][0] = 2.0 * (xz - wy);
  out.m[2][1] = 2.0 * (yz + wx);
  out.m[2][2] = 1.0 - 2.0 * (xx + yy);
  out.m[0][3] = p.px;
  out.m[1][3] = p.py;
  out.m[2][3] = p.pz;
  return out;
}

double runtime_pose_yaw_deg(const RuntimePose& p) {
  const double siny_cosp = 2.0 * (p.qw * p.qy + p.qx * p.qz);
  const double cosy_cosp = 1.0 - 2.0 * (p.qy * p.qy + p.qz * p.qz);
  return std::atan2(siny_cosp, cosy_cosp) * 180.0 / 3.14159265358979323846;
}


class RgbaCanvas {
 public:
  RgbaCanvas(int w, int h)
      : w_(w),
        h_(h),
        pixels_(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0),
        z_(static_cast<size_t>(w) * static_cast<size_t>(h), std::numeric_limits<float>::infinity()) {}

  void clear() {
    std::fill(pixels_.begin(), pixels_.end(), 0);
    std::fill(z_.begin(), z_.end(), std::numeric_limits<float>::infinity());
  }
  uint8_t* data() { return pixels_.data(); }
  const uint8_t* data() const { return pixels_.data(); }
  uint32_t width() const { return static_cast<uint32_t>(w_); }
  uint32_t height() const { return static_cast<uint32_t>(h_); }

  void put_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= w_ || y < 0 || y >= h_) return;
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)) * 4u;
    pixels_[i + 0] = r;
    pixels_[i + 1] = g;
    pixels_[i + 2] = b;
    pixels_[i + 3] = std::max<uint8_t>(pixels_[i + 3], a);
  }

  void blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= w_ || y < 0 || y >= h_ || a == 0) return;
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)) * 4u;
    const float sa = static_cast<float>(a) / 255.0f;
    const float da = static_cast<float>(pixels_[i + 3]) / 255.0f;
    const float out_a = sa + da * (1.0f - sa);
    if (out_a <= 1e-6f) return;
    auto blend = [&](uint8_t src, uint8_t dst) -> uint8_t {
      const float out = (static_cast<float>(src) * sa + static_cast<float>(dst) * da * (1.0f - sa)) / out_a;
      return static_cast<uint8_t>(std::clamp(out, 0.0f, 255.0f));
    };
    pixels_[i + 0] = blend(r, pixels_[i + 0]);
    pixels_[i + 1] = blend(g, pixels_[i + 1]);
    pixels_[i + 2] = blend(b, pixels_[i + 2]);
    pixels_[i + 3] = static_cast<uint8_t>(std::clamp(out_a * 255.0f, 0.0f, 255.0f));
  }

  bool blend_pixel_depth(int x, int y, double depth, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= w_ || y < 0 || y >= h_ || !std::isfinite(depth) || a == 0) return false;
    const size_t zi = static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x);
    if (depth > static_cast<double>(z_[zi])) return false;
    z_[zi] = static_cast<float>(depth);
    blend_pixel(x, y, r, g, b, a);
    return true;
  }

  void line(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      put_pixel(x0, y0, r, g, b, a);
      // simple thickness 2 for visibility
      put_pixel(x0 + 1, y0, r, g, b, a / 2);
      put_pixel(x0, y0 + 1, r, g, b, a / 2);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  void cross(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    line(x - 2, y, x + 2, y, r, g, b, a);
    line(x, y - 2, x, y + 2, r, g, b, a);
  }

  int disc(int cx, int cy, int radius, double depth, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    radius = std::max(1, radius);
    int n = 0;
    const int rr = radius * radius;
    for (int y = cy - radius; y <= cy + radius; ++y) {
      for (int x = cx - radius; x <= cx + radius; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= rr && blend_pixel_depth(x, y, depth, r, g, b, a)) ++n;
      }
    }
    return n;
  }

  int filled_triangle(const Pt2& p0, const Pt2& p1, const Pt2& p2,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!p0.valid || !p1.valid || !p2.valid) return 0;
    const int min_x = std::max(0, std::min({p0.x, p1.x, p2.x}));
    const int max_x = std::min(w_ - 1, std::max({p0.x, p1.x, p2.x}));
    const int min_y = std::max(0, std::min({p0.y, p1.y, p2.y}));
    const int max_y = std::min(h_ - 1, std::max({p0.y, p1.y, p2.y}));
    if (min_x > max_x || min_y > max_y) return 0;

    const double x0 = static_cast<double>(p0.x), y0 = static_cast<double>(p0.y);
    const double x1 = static_cast<double>(p1.x), y1 = static_cast<double>(p1.y);
    const double x2 = static_cast<double>(p2.x), y2 = static_cast<double>(p2.y);
    const double area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (!std::isfinite(area) || std::abs(area) < 0.5) return 0;
    const double inv_area = 1.0 / area;
    int n = 0;
    for (int y = min_y; y <= max_y; ++y) {
      for (int x = min_x; x <= max_x; ++x) {
        const double px = static_cast<double>(x) + 0.5;
        const double py = static_cast<double>(y) + 0.5;
        const double w0 = ((x1 - px) * (y2 - py) - (y1 - py) * (x2 - px)) * inv_area;
        const double w1 = ((x2 - px) * (y0 - py) - (y2 - py) * (x0 - px)) * inv_area;
        const double w2 = 1.0 - w0 - w1;
        const double eps = -1e-5;
        if (w0 >= eps && w1 >= eps && w2 >= eps) {
          const double depth = w0 * p0.depth + w1 * p1.depth + w2 * p2.depth;
          if (blend_pixel_depth(x, y, depth, r, g, b, a)) ++n;
        }
      }
    }
    return n;
  }

 private:
  int w_ = 0;
  int h_ = 0;
  std::vector<uint8_t> pixels_;
  std::vector<float> z_;
};


void draw_tiny_glyph(RgbaCanvas& canvas, int x, int y, char ch,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale = 2) {
  const char* rows[7] = {"   ","   ","   ","   ","   ","   ","   "};
  switch (ch) {
    case '0': { static const char* p[7] = {"111","101","101","101","101","101","111"}; std::copy(p,p+7,rows); break; }
    case '1': { static const char* p[7] = {"010","110","010","010","010","010","111"}; std::copy(p,p+7,rows); break; }
    case '2': { static const char* p[7] = {"111","001","001","111","100","100","111"}; std::copy(p,p+7,rows); break; }
    case '3': { static const char* p[7] = {"111","001","001","111","001","001","111"}; std::copy(p,p+7,rows); break; }
    case '4': { static const char* p[7] = {"101","101","101","111","001","001","001"}; std::copy(p,p+7,rows); break; }
    case '5': { static const char* p[7] = {"111","100","100","111","001","001","111"}; std::copy(p,p+7,rows); break; }
    case '6': { static const char* p[7] = {"111","100","100","111","101","101","111"}; std::copy(p,p+7,rows); break; }
    case '7': { static const char* p[7] = {"111","001","001","010","010","010","010"}; std::copy(p,p+7,rows); break; }
    case '8': { static const char* p[7] = {"111","101","101","111","101","101","111"}; std::copy(p,p+7,rows); break; }
    case '9': { static const char* p[7] = {"111","101","101","111","001","001","111"}; std::copy(p,p+7,rows); break; }
    case '.': { static const char* p[7] = {"000","000","000","000","000","110","110"}; std::copy(p,p+7,rows); break; }
    case 'm': { static const char* p[7] = {"000","000","110","111","101","101","101"}; std::copy(p,p+7,rows); break; }
    case 'd': { static const char* p[7] = {"001","001","111","101","101","101","111"}; std::copy(p,p+7,rows); break; }
    case 'i': { static const char* p[7] = {"010","000","110","010","010","010","111"}; std::copy(p,p+7,rows); break; }
    case 's': { static const char* p[7] = {"111","100","100","111","001","001","111"}; std::copy(p,p+7,rows); break; }
    case 't': { static const char* p[7] = {"010","010","111","010","010","010","011"}; std::copy(p,p+7,rows); break; }
    case 'n': { static const char* p[7] = {"000","000","110","101","101","101","101"}; std::copy(p,p+7,rows); break; }
    case 'e': { static const char* p[7] = {"000","000","111","100","111","100","111"}; std::copy(p,p+7,rows); break; }
    case 'a': { static const char* p[7] = {"000","000","111","001","111","101","111"}; std::copy(p,p+7,rows); break; }
    case 'x': { static const char* p[7] = {"000","000","101","101","010","101","101"}; std::copy(p,p+7,rows); break; }
    case 'q': { static const char* p[7] = {"000","111","101","101","111","001","001"}; std::copy(p,p+7,rows); break; }
    case ':': { static const char* p[7] = {"000","110","110","000","110","110","000"}; std::copy(p,p+7,rows); break; }
    case ' ': default: break;
  }
  for (int yy = 0; yy < 7; ++yy) {
    for (int xx = 0; xx < 3; ++xx) {
      if (rows[yy][xx] == '1') {
        for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx) {
          canvas.put_pixel(x + xx * scale + sx, y + yy * scale + sy, r, g, b, a);
        }
      }
    }
  }
}

void draw_tiny_text(RgbaCanvas& canvas, int x, int y, const std::string& text,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale = 2) {
  int cx = x;
  for (const char ch : text) {
    draw_tiny_glyph(canvas, cx, y, ch, r, g, b, a, scale);
    cx += 4 * scale;
  }
}

void distance_color(double depth_m, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!std::isfinite(depth_m)) { r = 180; g = 180; b = 180; return; }
  if (depth_m < 0.50) { r = 255; g = 64; b = 64; return; }       // very near
  if (depth_m < 1.00) { r = 255; g = 220; b = 64; return; }      // near
  if (depth_m < 1.50) { r = 80; g = 255; b = 120; return; }      // mid
  if (depth_m < 2.50) { r = 80; g = 200; b = 255; return; }      // far
  r = 160; g = 120; b = 255;                                     // very far
}

std::string fixed_m(double v, int decimals = 1) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(decimals) << v << "m";
  return os.str();
}

void draw_visibility_pattern(RgbaCanvas& canvas, uint64_t frame_no, bool strong) {
  const int w = static_cast<int>(canvas.width());
  const int h = static_cast<int>(canvas.height());
  const uint8_t a = strong ? 255 : 170;
  const uint8_t g = strong ? 255 : 210;
  // Border.
  canvas.line(0, 0, w - 1, 0, 0, g, 255, a);
  canvas.line(w - 1, 0, w - 1, h - 1, 0, g, 255, a);
  canvas.line(w - 1, h - 1, 0, h - 1, 0, g, 255, a);
  canvas.line(0, h - 1, 0, 0, 0, g, 255, a);
  // Center crosshair.
  canvas.line(w / 2 - w / 10, h / 2, w / 2 + w / 10, h / 2, 255, 255, 0, a);
  canvas.line(w / 2, h / 2 - h / 10, w / 2, h / 2 + h / 10, 255, 255, 0, a);
  // Slow moving tick mark in the top edge so we know SetOverlayRaw is updating.
  const int tick = static_cast<int>((frame_no * 13u) % static_cast<uint64_t>(std::max(1, w - 80)));
  for (int y = 8; y < 24 && y < h; ++y) {
    for (int x = tick; x < tick + 64 && x < w; ++x) canvas.put_pixel(x, y, 255, 64, 64, 230);
  }
}

struct RenderStats {
  uint32_t seq = 0;
  uint32_t vertices = 0;
  uint32_t triangles = 0;
  int finite_vertices = 0;
  int projected_vertices = 0;
  int clipped_vertices = 0;
  int edges_drawn = 0;
  int triangles_drawn = 0;
  int mesh_pixels_drawn = 0;
  int points_drawn = 0;
  double mesh_age_ms = 0.0;
  double min_depth_m = std::numeric_limits<double>::infinity();
  double max_depth_m = 0.0;
  double sum_depth_m = 0.0;
  int depth_samples = 0;
};

void draw_distance_hud(RgbaCanvas& canvas, const Config& c, const RenderStats& st, const Mesh& mesh) {
  if (!c.draw_distance_markers) return;
  const int w = static_cast<int>(canvas.width());
  const int h = static_cast<int>(canvas.height());
  const double min_d = std::isfinite(st.min_depth_m) ? st.min_depth_m : 0.0;
  const double max_d = st.max_depth_m;
  const double mean_d = st.depth_samples > 0 ? st.sum_depth_m / static_cast<double>(st.depth_samples) : 0.0;
  draw_tiny_text(canvas, 12, 12, "dist " + fixed_m(min_d) + "-" + fixed_m(max_d), 255, 255, 255, 230, 2);
  draw_tiny_text(canvas, 12, 34, "mean " + fixed_m(mean_d) + " seq " + std::to_string(mesh.sequence), 210, 240, 255, 220, 2);
  const int y = std::max(20, h - 36);
  const int x0 = 16;
  const int bar_w = std::max(100, w - 32);
  canvas.line(x0, y, x0 + bar_w, y, 180, 180, 180, 150);
  const double max_m = std::max(0.5, c.distance_marker_max_m);
  const double step = std::max(0.25, c.distance_marker_step_m);
  for (double d = step; d <= max_m + 1e-6; d += step) {
    const int x = x0 + static_cast<int>((d / max_m) * static_cast<double>(bar_w));
    uint8_t r=255,g=255,b=255; distance_color(d, r, g, b);
    canvas.line(x, y - 8, x, y + 8, r, g, b, 220);
    draw_tiny_text(canvas, x - 10, y + 12, fixed_m(d), r, g, b, 220, 1);
  }
}

Pt2 project_point(const Config& c, int view_x, int view_y, int w, int h, const Pose34& hmd_pose, const xr_spatial::RuntimeSpatialProxyVertexF32V1& v) {
  const Vec3 world{v.x, v.y, v.z};
  if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) return {};
  const Vec3 hp = world_to_hmd(hmd_pose, world);
  const double depth = -hp.z; // OpenVR HMD forward is usually -Z.
  if (!std::isfinite(depth) || depth < c.min_depth_m || depth > c.max_depth_m) return {};
  const double kPi = 3.14159265358979323846;
  const double tan_y = std::tan((c.vertical_fov_deg * kPi / 180.0) * 0.5);
  const double aspect = static_cast<double>(w) / static_cast<double>(std::max(1, h));
  const double tan_x = tan_y * aspect;
  const double nx = hp.x / (depth * tan_x);
  const double ny = hp.y / (depth * tan_y);
  if (!std::isfinite(nx) || !std::isfinite(ny) || std::abs(nx) > 1.15 || std::abs(ny) > 1.15) return {0, 0, depth, false};
  const int px = view_x + static_cast<int>((nx * 0.5 + 0.5) * static_cast<double>(w));
  const int py = view_y + static_cast<int>((0.5 - ny * 0.5) * static_cast<double>(h));
  return {px, py, depth, true};
}

void draw_bbox(RgbaCanvas& canvas, const Config& c, int view_x, int view_y, int view_w, int view_h, const Pose34& pose, const Mesh& mesh, RenderStats& stats) {
  const float xs[2] = {mesh.bbox_min_x, mesh.bbox_max_x};
  const float ys[2] = {mesh.bbox_min_y, mesh.bbox_max_y};
  const float zs[2] = {mesh.bbox_min_z, mesh.bbox_max_z};
  xr_spatial::RuntimeSpatialProxyVertexF32V1 verts[8]{};
  int n = 0;
  for (int ix = 0; ix < 2; ++ix) for (int iy = 0; iy < 2; ++iy) for (int iz = 0; iz < 2; ++iz) {
    verts[n++] = {xs[ix], ys[iy], zs[iz]};
  }
  Pt2 p[8];
  for (int i = 0; i < 8; ++i) p[i] = project_point(c, view_x, view_y, view_w, view_h, pose, verts[i]);
  auto edge = [&](int a, int b) {
    if (p[a].valid && p[b].valid) canvas.line(p[a].x, p[a].y, p[b].x, p[b].y, 255, 220, 80, 180);
  };
  // generated vertex order: ix/iy/iz nested, edges differ by one bit in one coordinate
  for (int a = 0; a < 8; ++a) for (int b = a + 1; b < 8; ++b) {
    const int xora = a ^ b;
    if (xora == 1 || xora == 2 || xora == 4) edge(a, b);
  }
}

RenderStats render_mesh(RgbaCanvas& canvas, const Config& c, int view_x, int view_y, int view_w, int view_h, const Pose34& hmd_pose, const Mesh& mesh, bool draw_hud) {
  RenderStats st{};
  st.seq = static_cast<uint32_t>(mesh.sequence & 0xffffffffu);
  st.vertices = std::min<uint32_t>(mesh.vertex_count, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  st.triangles = std::min<uint32_t>(mesh.triangle_count, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);
  st.mesh_age_ms = mesh.timestamp_ns > 0 ? static_cast<double>(now_ns_u64() - mesh.timestamp_ns) / 1e6 : 0.0;

  std::vector<Pt2> projected(st.vertices);
  for (uint32_t i = 0; i < st.vertices; ++i) {
    const auto& v = mesh.vertices[i];
    if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) ++st.finite_vertices;
    projected[i] = project_point(c, view_x, view_y, view_w, view_h, hmd_pose, v);
    if (projected[i].valid) {
      ++st.projected_vertices;
      st.min_depth_m = std::min(st.min_depth_m, projected[i].depth);
      st.max_depth_m = std::max(st.max_depth_m, projected[i].depth);
      st.sum_depth_m += projected[i].depth;
      ++st.depth_samples;
    } else ++st.clipped_vertices;
  }

  const uint8_t mesh_a = static_cast<uint8_t>(std::clamp(c.mesh_alpha, 0.0, 1.0) * 255.0);
  const uint8_t wire_a = static_cast<uint8_t>(std::clamp(c.wire_alpha, 0.0, 1.0) * 255.0);
  const uint8_t point_a = static_cast<uint8_t>(std::clamp(c.point_alpha, 0.0, 1.0) * 255.0);

  if (c.draw_mesh) {
    for (uint32_t i = 0; i < st.triangles; ++i) {
      const auto& t = mesh.triangles[i];
      if (t.i0 >= st.vertices || t.i1 >= st.vertices || t.i2 >= st.vertices) continue;
      const Pt2& a = projected[t.i0];
      const Pt2& b = projected[t.i1];
      const Pt2& d = projected[t.i2];
      if (!a.valid || !b.valid || !d.valid) continue;
      uint8_t tr=80, tg=255, tb=120;
      if (c.color_by_distance) distance_color((a.depth + b.depth + d.depth) / 3.0, tr, tg, tb);
      const int pixels = canvas.filled_triangle(a, b, d, tr, tg, tb, mesh_a);
      if (pixels > 0) {
        ++st.triangles_drawn;
        st.mesh_pixels_drawn += pixels;
      }
    }
  }

  if (c.draw_wire) {
    for (uint32_t i = 0; i < st.triangles; ++i) {
      const auto& t = mesh.triangles[i];
      if (t.i0 >= st.vertices || t.i1 >= st.vertices || t.i2 >= st.vertices) continue;
      const Pt2& a = projected[t.i0];
      const Pt2& b = projected[t.i1];
      const Pt2& d = projected[t.i2];
      int drawn = 0;
      uint8_t er=80, eg=255, eb=120;
      if (a.valid && b.valid) { if (c.color_by_distance) distance_color((a.depth + b.depth) * 0.5, er, eg, eb); canvas.line(a.x, a.y, b.x, b.y, er, eg, eb, wire_a); ++drawn; }
      if (b.valid && d.valid) { if (c.color_by_distance) distance_color((b.depth + d.depth) * 0.5, er, eg, eb); canvas.line(b.x, b.y, d.x, d.y, er, eg, eb, wire_a); ++drawn; }
      if (d.valid && a.valid) { if (c.color_by_distance) distance_color((d.depth + a.depth) * 0.5, er, eg, eb); canvas.line(d.x, d.y, a.x, a.y, er, eg, eb, wire_a); ++drawn; }
      if (drawn > 0) {
        if (!c.draw_mesh) ++st.triangles_drawn;
        st.edges_drawn += drawn;
      }
    }
  }

  if (c.draw_points) {
    for (const auto& p : projected) {
      if (!p.valid) continue;
      uint8_t pr=120, pg=200, pb=255;
      if (c.color_by_distance) distance_color(p.depth, pr, pg, pb);
      st.points_drawn += canvas.disc(p.x, p.y, c.point_radius_px, p.depth, pr, pg, pb, point_a);
    }
  }
  if (c.draw_bbox && st.vertices > 0) draw_bbox(canvas, c, view_x, view_y, view_w, view_h, hmd_pose, mesh, st);
  if (draw_hud) draw_distance_hud(canvas, c, st, mesh);

  return st;
}

RenderStats merge_stereo_stats(const RenderStats& left, const RenderStats& right) {
  RenderStats out = left;
  out.finite_vertices = std::max(left.finite_vertices, right.finite_vertices);
  out.projected_vertices = left.projected_vertices + right.projected_vertices;
  out.clipped_vertices = left.clipped_vertices + right.clipped_vertices;
  out.edges_drawn = left.edges_drawn + right.edges_drawn;
  out.triangles_drawn = left.triangles_drawn + right.triangles_drawn;
  out.mesh_pixels_drawn = left.mesh_pixels_drawn + right.mesh_pixels_drawn;
  out.points_drawn = left.points_drawn + right.points_drawn;
  out.min_depth_m = std::min(left.min_depth_m, right.min_depth_m);
  out.max_depth_m = std::max(left.max_depth_m, right.max_depth_m);
  out.sum_depth_m = left.sum_depth_m + right.sum_depth_m;
  out.depth_samples = left.depth_samples + right.depth_samples;
  return out;
}

const char* overlay_error_name(vr::EVROverlayError e) {
  switch (e) {
    case vr::VROverlayError_None: return "None";
    case vr::VROverlayError_UnknownOverlay: return "UnknownOverlay";
    case vr::VROverlayError_InvalidHandle: return "InvalidHandle";
    case vr::VROverlayError_PermissionDenied: return "PermissionDenied";
    case vr::VROverlayError_OverlayLimitExceeded: return "OverlayLimitExceeded";
    case vr::VROverlayError_WrongVisibilityType: return "WrongVisibilityType";
    case vr::VROverlayError_KeyTooLong: return "KeyTooLong";
    case vr::VROverlayError_NameTooLong: return "NameTooLong";
    case vr::VROverlayError_KeyInUse: return "KeyInUse";
    case vr::VROverlayError_WrongTransformType: return "WrongTransformType";
    case vr::VROverlayError_InvalidTrackedDevice: return "InvalidTrackedDevice";
    case vr::VROverlayError_InvalidParameter: return "InvalidParameter";
    case vr::VROverlayError_ThumbnailCantBeDestroyed: return "ThumbnailCantBeDestroyed";
    case vr::VROverlayError_ArrayTooSmall: return "ArrayTooSmall";
    case vr::VROverlayError_RequestFailed: return "RequestFailed";
    case vr::VROverlayError_InvalidTexture: return "InvalidTexture";
    case vr::VROverlayError_UnableToLoadFile: return "UnableToLoadFile";
    case vr::VROverlayError_KeyboardAlreadyInUse: return "KeyboardAlreadyInUse";
    case vr::VROverlayError_NoNeighbor: return "NoNeighbor";
    case vr::VROverlayError_TooManyMaskPrimitives: return "TooManyMaskPrimitives";
    case vr::VROverlayError_BadMaskPrimitive: return "BadMaskPrimitive";
    default: return "Unknown";
  }
}

class SteamVrOverlay {
 public:
  SteamVrOverlay(const Config& cfg, Logger& log) : cfg_(cfg), log_(log) {}
  ~SteamVrOverlay() { shutdown(); }

  bool init() {
    log_.info("OpenVR env before VR_Init VR_OVERRIDE=", env_string("VR_OVERRIDE", "<unset>"),
              " VR_CONFIG_PATH=", env_string("VR_CONFIG_PATH", "<unset>"),
              " VR_LOG_PATH=", env_string("VR_LOG_PATH", "<unset>"),
              " LD_LIBRARY_PATH=", env_string("LD_LIBRARY_PATH", "<unset>"));
    vr::EVRInitError init_err = vr::VRInitError_None;
    vr_system_ = vr::VR_Init(&init_err, vr::VRApplication_Overlay);
    if (init_err != vr::VRInitError_None || !vr_system_) {
      log_.error("VR_Init failed: ", vr::VR_GetVRInitErrorAsEnglishDescription(init_err), " (", int(init_err), ")");
      log_.error("OpenVR init hint: start SteamVR from Steam first, or run the start script with SPATIAL_OVERLAY_AUTO_STEAMVR_ENV=1. If vrmonitor says libsteam_api.so is missing, check LD_LIBRARY_PATH and VR_OVERRIDE in the start-script log.");
      return false;
    }
    overlay_ = vr::VROverlay();
    if (!overlay_) {
      log_.error("VROverlay() returned null");
      return false;
    }

    vr::EVROverlayError oe = overlay_->CreateOverlay(cfg_.overlay_key.c_str(), cfg_.overlay_name.c_str(), &handle_);
    if (oe == vr::VROverlayError_KeyInUse) {
      log_.warn("overlay key already exists, trying FindOverlay key=", cfg_.overlay_key);
      oe = overlay_->FindOverlay(cfg_.overlay_key.c_str(), &handle_);
    }
    if (oe != vr::VROverlayError_None) {
      log_.error("Create/Find overlay failed: ", overlay_error_name(oe), " (", int(oe), ")");
      return false;
    }

    overlay_->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_None);
    overlay_->SetOverlayWidthInMeters(handle_, static_cast<float>(cfg_.overlay_width_m));
    overlay_->SetOverlayAlpha(handle_, static_cast<float>(std::clamp(cfg_.alpha, 0.0, 1.0)));
    overlay_->SetOverlaySortOrder(handle_, 100);
    const bool sbs_flag = cfg_.stereo_sbs && cfg_.stereo_openvr_flag;
    const auto sbs_err = overlay_->SetOverlayFlag(handle_, vr::VROverlayFlags_SideBySide_Parallel, sbs_flag);
    if (sbs_err != vr::VROverlayError_None) {
      log_.warn("SetOverlayFlag(SideBySide_Parallel) failed: ", overlay_error_name(sbs_err),
                " (", int(sbs_err), ") stereo_sbs=", (cfg_.stereo_sbs ? "1" : "0"));
    }
    set_transform();
    oe = overlay_->ShowOverlay(handle_);
    if (oe != vr::VROverlayError_None) {
      log_.error("ShowOverlay failed: ", overlay_error_name(oe), " (", int(oe), ")");
      return false;
    }

    if (cfg_.submit_mode == "opengl" || cfg_.submit_mode == "gl" || cfg_.submit_mode == "auto") {
      if (!init_gl_texture()) {
        if (cfg_.submit_mode == "opengl" || cfg_.submit_mode == "gl") {
          log_.warn("OpenGL texture submit requested but GL init failed; falling back to SetOverlayRaw so the app can still run");
        } else {
          log_.warn("OpenGL texture submit unavailable; falling back to SetOverlayRaw");
        }
      }
    }

    log_.info("OpenVR overlay initialized key=", cfg_.overlay_key,
              " handle=", handle_,
              " texture=", cfg_.width, "x", cfg_.height,
              " submit_mode=", effective_submit_mode(),
              " quad_width_m=", cfg_.overlay_width_m,
              " distance_m=", cfg_.overlay_distance_m,
              " stereo_sbs=", (cfg_.stereo_sbs ? "1" : "0"),
              " stereo_ipd_m=", cfg_.stereo_ipd_m,
              " stereo_openvr_flag=", (cfg_.stereo_openvr_flag ? "1" : "0"));
    return true;
  }

  bool hmd_pose(Pose34& out) {
    if (!vr_system_) return false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr_system_->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
                                                poses, vr::k_unMaxTrackedDeviceCount);
    const auto& p = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!p.bPoseIsValid || !p.bDeviceIsConnected) return false;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) out.m[r][c] = p.mDeviceToAbsoluteTracking.m[r][c];
    return true;
  }

  bool submit(RgbaCanvas& canvas) {
    if (!overlay_) return false;
    set_transform();

    // SetOverlayRaw is useful for a smoke test, but on Linux it can start returning
    // RequestFailed after many high-rate raw IPC uploads.  The stable path for a
    // continuously updated overlay is an OpenGL texture submitted with SetOverlayTexture.
    if (gl_ready_) {
      return submit_opengl(canvas);
    }
    return submit_raw(canvas);
  }

  void shutdown() {
    shutdown_gl();
    if (overlay_ && handle_ != vr::k_ulOverlayHandleInvalid) {
      overlay_->HideOverlay(handle_);
      overlay_->DestroyOverlay(handle_);
      handle_ = vr::k_ulOverlayHandleInvalid;
    }
    if (vr_system_) {
      vr::VR_Shutdown();
      vr_system_ = nullptr;
      overlay_ = nullptr;
    }
  }

 private:
  const char* effective_submit_mode() const {
    if (gl_ready_) return "opengl";
    return "raw";
  }

  bool init_gl_texture() {
#ifndef _WIN32
    x_display_ = XOpenDisplay(nullptr);
    if (!x_display_) {
      log_.warn("OpenGL submit init failed: XOpenDisplay(nullptr) returned null DISPLAY=", env_string("DISPLAY", "<unset>"));
      return false;
    }

    const int screen = DefaultScreen(x_display_);
    int visual_attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
    XVisualInfo* vi = glXChooseVisual(x_display_, screen, visual_attribs);
    if (!vi) {
      log_.warn("OpenGL submit init failed: glXChooseVisual returned null");
      return false;
    }

    Colormap cmap = XCreateColormap(x_display_, RootWindow(x_display_, vi->screen), vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap;
    swa.border_pixel = 0;
    swa.event_mask = 0;
    x_window_ = XCreateWindow(x_display_, RootWindow(x_display_, vi->screen),
                              0, 0, 1, 1, 0, vi->depth, InputOutput, vi->visual,
                              CWBorderPixel | CWColormap | CWEventMask, &swa);
    if (!x_window_) {
      log_.warn("OpenGL submit init failed: XCreateWindow returned 0");
      XFree(vi);
      return false;
    }

    gl_context_ = glXCreateContext(x_display_, vi, nullptr, GL_TRUE);
    XFree(vi);
    if (!gl_context_) {
      log_.warn("OpenGL submit init failed: glXCreateContext returned null");
      return false;
    }
    if (!glXMakeCurrent(x_display_, x_window_, gl_context_)) {
      log_.warn("OpenGL submit init failed: glXMakeCurrent failed");
      return false;
    }

    glGenTextures(1, &gl_tex_);
    if (gl_tex_ == 0) {
      log_.warn("OpenGL submit init failed: glGenTextures returned 0");
      return false;
    }
    glBindTexture(GL_TEXTURE_2D, gl_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cfg_.width, cfg_.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
      log_.warn("OpenGL submit init failed: glTexImage2D gl_error=0x", std::hex, err, std::dec);
      return false;
    }
    glFlush();
    gl_ready_ = true;
    log_.info("OpenGL texture submit initialized display=", DisplayString(x_display_),
              " tex_id=", gl_tex_,
              " size=", cfg_.width, "x", cfg_.height,
              " renderer=", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              " version=", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
#else
    return false;
#endif
  }

  const uint8_t* upload_pixels_for_steamvr(const RgbaCanvas& canvas) {
    const bool flip_x = cfg_.texture_flip_x ^ cfg_.texture_rotate_180;
    const bool flip_y = cfg_.texture_flip_y ^ cfg_.texture_rotate_180;
    if (!flip_x && !flip_y) return canvas.data();

    const uint32_t w = canvas.width();
    const uint32_t h = canvas.height();
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    upload_pixels_.resize(bytes);
    const uint8_t* src = canvas.data();
    for (uint32_t y = 0; y < h; ++y) {
      const uint32_t sy = flip_y ? (h - 1u - y) : y;
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t sx = flip_x ? (w - 1u - x) : x;
        const size_t di = (static_cast<size_t>(y) * w + x) * 4u;
        const size_t si = (static_cast<size_t>(sy) * w + sx) * 4u;
        upload_pixels_[di + 0] = src[si + 0];
        upload_pixels_[di + 1] = src[si + 1];
        upload_pixels_[di + 2] = src[si + 2];
        upload_pixels_[di + 3] = src[si + 3];
      }
    }
    if (!texture_orientation_logged_) {
      log_.info("texture orientation upload flip_x=", (flip_x ? "1" : "0"),
                " flip_y=", (flip_y ? "1" : "0"),
                " rotate_180=", (cfg_.texture_rotate_180 ? "1" : "0"));
      texture_orientation_logged_ = true;
    }
    return upload_pixels_.data();
  }

  bool submit_opengl(RgbaCanvas& canvas) {
#ifndef _WIN32
    if (!gl_ready_ || !x_display_ || !gl_context_ || gl_tex_ == 0) return submit_raw(canvas);
    if (!glXMakeCurrent(x_display_, x_window_, gl_context_)) {
      log_.warn("OpenGL submit failed: glXMakeCurrent failed, falling back to raw");
      return submit_raw(canvas);
    }
    glBindTexture(GL_TEXTURE_2D, gl_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    static_cast<GLsizei>(canvas.width()), static_cast<GLsizei>(canvas.height()),
                    GL_RGBA, GL_UNSIGNED_BYTE, upload_pixels_for_steamvr(canvas));
    const GLenum ge = glGetError();
    if (ge != GL_NO_ERROR) {
      log_.warn("OpenGL texture upload failed gl_error=0x", std::hex, ge, std::dec);
      return false;
    }
    glFlush();

    vr::Texture_t tex{};
    tex.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(gl_tex_));
    tex.eType = vr::TextureType_OpenGL;
    tex.eColorSpace = vr::ColorSpace_Auto;
    const auto oe = overlay_->SetOverlayTexture(handle_, &tex);
    if (oe != vr::VROverlayError_None) {
      ++submit_error_count_;
      log_.warn("SetOverlayTexture(OpenGL) failed: ", overlay_error_name(oe), " (", int(oe), ")",
                " failures=", submit_error_count_,
                " tex_id=", gl_tex_);
      return false;
    }
    submit_error_count_ = 0;
    return true;
#else
    return submit_raw(canvas);
#endif
  }

  bool submit_raw(RgbaCanvas& canvas) {
    void* raw_pixels_for_steamvr = static_cast<void*>(const_cast<uint8_t*>(upload_pixels_for_steamvr(canvas)));
    const auto oe = overlay_->SetOverlayRaw(handle_, raw_pixels_for_steamvr, canvas.width(), canvas.height(), 4);
    if (oe != vr::VROverlayError_None) {
      ++submit_error_count_;
      log_.warn("SetOverlayRaw failed: ", overlay_error_name(oe), " (", int(oe), ")",
                " failures=", submit_error_count_,
                " hint=use SPATIAL_OVERLAY_SUBMIT_MODE=opengl");
      return false;
    }
    submit_error_count_ = 0;
    return true;
  }

  void shutdown_gl() {
#ifndef _WIN32
    if (x_display_) {
      if (gl_context_) {
        glXMakeCurrent(x_display_, x_window_, gl_context_);
        if (gl_tex_ != 0) glDeleteTextures(1, &gl_tex_);
        glXMakeCurrent(x_display_, None, nullptr);
        glXDestroyContext(x_display_, gl_context_);
        gl_context_ = nullptr;
      }
      if (x_window_) {
        XDestroyWindow(x_display_, x_window_);
        x_window_ = 0;
      }
      XCloseDisplay(x_display_);
      x_display_ = nullptr;
    }
    gl_tex_ = 0;
    gl_ready_ = false;
#endif
  }

  void set_transform() {
    if (!overlay_) return;
    vr::HmdMatrix34_t m{};
    m.m[0][0] = 1.0f; m.m[0][1] = 0.0f; m.m[0][2] = 0.0f; m.m[0][3] = 0.0f;
    m.m[1][0] = 0.0f; m.m[1][1] = 1.0f; m.m[1][2] = 0.0f; m.m[1][3] = static_cast<float>(cfg_.overlay_y_offset_m);
    m.m[2][0] = 0.0f; m.m[2][1] = 0.0f; m.m[2][2] = 1.0f; m.m[2][3] = static_cast<float>(-std::abs(cfg_.overlay_distance_m));
    const auto oe = overlay_->SetOverlayTransformTrackedDeviceRelative(handle_, vr::k_unTrackedDeviceIndex_Hmd, &m);
    if (oe != vr::VROverlayError_None && transform_error_count_++ < 5) {
      log_.warn("SetOverlayTransformTrackedDeviceRelative failed: ", overlay_error_name(oe), " (", int(oe), ")");
    }
  }

  const Config& cfg_;
  Logger& log_;
  vr::IVRSystem* vr_system_ = nullptr;
  vr::IVROverlay* overlay_ = nullptr;
  vr::VROverlayHandle_t handle_ = vr::k_ulOverlayHandleInvalid;
  uint64_t submit_error_count_ = 0;
  uint64_t transform_error_count_ = 0;
  std::vector<uint8_t> upload_pixels_;
  bool texture_orientation_logged_ = false;

#ifndef _WIN32
  Display* x_display_ = nullptr;
  Window x_window_ = 0;
  GLXContext gl_context_ = nullptr;
  GLuint gl_tex_ = 0;
  bool gl_ready_ = false;
#endif
};

std::string mesh_flags(uint32_t flags) {
  std::string out;
  auto add = [&](const char* s) { if (!out.empty()) out += ","; out += s; };
  if (flags & xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_ACTIVE) add("active");
  if (flags & xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_UPDATED) add("updated");
  if (flags & xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_VOXEL_PROXY) add("voxel_proxy");
  if (flags & xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FLAG_QUANTIZED_FOR_UDP) add("quantized_udp");
  return out.empty() ? "none" : out;
}

void log_mesh(Logger& log, const Mesh& m, const char* prefix) {
  const double age_ms = m.timestamp_ns > 0 ? static_cast<double>(now_ns_u64() - m.timestamp_ns) / 1e6 : -1.0;
  log.info(prefix,
           " seq=", m.sequence,
           " age_ms=", std::fixed, std::setprecision(1), age_ms,
           " vc=", m.vertex_count,
           " tc=", m.triangle_count,
           " flags=", mesh_flags(m.status_flags),
           " bbox=[(", std::setprecision(3), m.bbox_min_x, ",", m.bbox_min_y, ",", m.bbox_min_z,
           ")-(", m.bbox_max_x, ",", m.bbox_max_y, ",", m.bbox_max_z,
           ")] voxel=", m.voxel_size_m,
           " conf=", m.confidence);
}


#ifndef _WIN32
std::string printable_magic(const char* magic, size_t n) {
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(magic[i]);
    out.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '.');
  }
  return out;
}

void log_ring_header_debug(Logger& log, const std::string& shm_name, const char* prefix) {
  try {
    const std::string posix_name = xr_runtime::normalize_posix_name(shm_name);
    const int fd = ::shm_open(posix_name.c_str(), O_RDONLY, 0666);
    if (fd < 0) {
      log.warn(prefix, " shm_open failed shm=", posix_name, " err=", std::strerror(errno));
      return;
    }
    struct FdGuard { int fd; ~FdGuard() { if (fd >= 0) ::close(fd); } } guard{fd};
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      log.warn(prefix, " fstat failed shm=", posix_name, " err=", std::strerror(errno));
      return;
    }

    xr_runtime::RingHeaderV1 h{};
    const ssize_t hn = ::pread(fd, &h, sizeof(h), 0);
    if (hn != static_cast<ssize_t>(sizeof(h))) {
      log.warn(prefix, " short ring header read shm=", posix_name, " read=", hn,
               " need=", sizeof(h), " size=", static_cast<long long>(st.st_size));
      return;
    }

    uint64_t legacy_latest_seq = 0;
    if (st.st_size >= 48) {
      const ssize_t legacy_read = ::pread(fd, &legacy_latest_seq, sizeof(legacy_latest_seq), 40);
      if (legacy_read != static_cast<ssize_t>(sizeof(legacy_latest_seq))) {
        legacy_latest_seq = 0;
      }
    }

    uint64_t chosen_seq = h.latest_sequence;
    const bool legacy_selected =
        legacy_latest_seq != 0 && legacy_latest_seq < (1ull << 32) &&
        (chosen_seq == 0 || chosen_seq > (1ull << 32));
    if (legacy_selected) chosen_seq = legacy_latest_seq;

    log.info(prefix,
             " shm=", posix_name,
             " size=", static_cast<long long>(st.st_size),
             " magic=", printable_magic(h.magic, sizeof(h.magic)),
             " version=", h.version,
             " header=", h.header_size,
             " slots=", h.slot_count,
             " stride=", h.slot_stride,
             " slot_header=", h.slot_header_size,
             " payload=", h.payload_size,
             " latest=", h.latest_sequence,
             " legacy40=", legacy_latest_seq,
             " chosen=", chosen_seq,
             " legacy_selected=", (legacy_selected ? "1" : "0"));

    if (chosen_seq == 0 || h.slot_count == 0 || h.slot_stride == 0 || h.slot_header_size == 0) {
      return;
    }

    const uint32_t idx = static_cast<uint32_t>((chosen_seq - 1) % h.slot_count);
    const off_t slot_off = static_cast<off_t>(h.header_size) + static_cast<off_t>(idx) * h.slot_stride;
    xr_runtime::RingSlotHeaderV1 slot{};
    const ssize_t sn = ::pread(fd, &slot, sizeof(slot), slot_off);
    if (sn != static_cast<ssize_t>(sizeof(slot))) {
      log.warn(prefix, " short slot header read idx=", idx, " off=", slot_off, " read=", sn);
      return;
    }

    Mesh payload{};
    const off_t payload_off = slot_off + static_cast<off_t>(h.slot_header_size);
    const ssize_t pn = ::pread(fd, &payload, sizeof(payload), payload_off);
    log.info(prefix,
             " slot idx=", idx,
             " seq_begin=", slot.seq_begin,
             " seq_end=", slot.seq_end,
             " slot_payload_size=", slot.payload_size,
             " slot_flags=", slot.flags,
             " payload_read=", pn,
             " mesh_seq=", payload.sequence,
             " mesh_ts=", payload.timestamp_ns,
             " vc=", payload.vertex_count,
             " tc=", payload.triangle_count,
             " mesh_flags=", mesh_flags(payload.status_flags));
  } catch (const std::exception& e) {
    log.warn(prefix, " exception while reading ring debug: ", e.what());
  }
}
#else
void log_ring_header_debug(Logger&, const std::string&, const char*) {}
#endif

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  try {
    if (!parse_args(argc, argv, cfg)) return 0;
  } catch (const std::exception& e) {
    std::cerr << "[xr_steamvr_spatial_overlay][ERROR] argument parse failed: " << e.what() << "\n";
    print_help();
    return 2;
  }

  Logger log(cfg.log_file);
  log.info("startup registry=", cfg.registry,
           " stream=", cfg.stream,
           " log_file=", cfg.log_file,
           " dry_run=", (cfg.dry_run ? "1" : "0"),
           " render_hz=", cfg.render_hz,
           " submit_mode=", cfg.submit_mode,
           " pose_source=", cfg.pose_source,
           " pose_registry=", cfg.pose_registry,
           " pose_stream=", cfg.pose_stream,
           " clear_no_mesh=", (cfg.clear_when_no_mesh ? "1" : "0"),
           " clear_no_pose=", (cfg.clear_when_no_pose ? "1" : "0"),
           " texture_flip_y=", (cfg.texture_flip_y ? "1" : "0"),
           " texture_flip_x=", (cfg.texture_flip_x ? "1" : "0"),
           " texture_rotate_180=", (cfg.texture_rotate_180 ? "1" : "0"),
           " max_mesh_age_ms=", cfg.max_mesh_age_ms,
           " draw_stale_mesh=", (cfg.draw_stale_mesh ? "1" : "0"),
           " test_pattern=", (cfg.visibility_test_pattern ? "1" : "0"),
           " force_visible_border=", (cfg.force_visible_border ? "1" : "0"));
  log.info("render config texture=", cfg.width, "x", cfg.height,
           " vertical_fov_deg=", cfg.vertical_fov_deg,
           " min_depth_m=", cfg.min_depth_m,
           " max_depth_m=", cfg.max_depth_m,
           " stereo_sbs=", (cfg.stereo_sbs ? "1" : "0"),
           " stereo_ipd_m=", cfg.stereo_ipd_m,
           " stereo_openvr_flag=", (cfg.stereo_openvr_flag ? "1" : "0"),
           " draw_mesh=", (cfg.draw_mesh ? "1" : "0"),
           " draw_wire=", (cfg.draw_wire ? "1" : "0"),
           " draw_points=", (cfg.draw_points ? "1" : "0"),
           " point_radius_px=", cfg.point_radius_px,
           " draw_bbox=", (cfg.draw_bbox ? "1" : "0"),
           " draw_distance_markers=", (cfg.draw_distance_markers ? "1" : "0"),
           " color_by_distance=", (cfg.color_by_distance ? "1" : "0"));

  std::unique_ptr<SteamVrOverlay> overlay;
  if (!cfg.dry_run) {
    overlay = std::make_unique<SteamVrOverlay>(cfg, log);
    if (!overlay->init()) {
      log.error("OpenVR init failed. Re-run with SPATIAL_OVERLAY_DRY_RUN=1 to debug SHM only.");
      return 1;
    }
  } else {
    log.warn("dry-run enabled: OpenVR overlay is disabled; SHM reader/logging only");
  }

  RgbaCanvas canvas(std::max(64, cfg.width), std::max(64, cfg.height));
  std::unique_ptr<MeshReader> reader;
  std::unique_ptr<RuntimePoseReader> pose_reader;
  uint64_t last_attach_try_ns = 0;
  uint64_t last_pose_attach_try_ns = 0;
  uint64_t last_log_ns = 0;
  uint64_t last_seen_seq = 0;
  uint64_t last_render_seq = 0;
  uint64_t frames = 0;
  uint64_t submit_ok = 0;
  uint64_t submit_fail = 0;
  uint64_t no_mesh = 0;
  uint64_t no_pose = 0;
  uint64_t last_seen_pose_seq = 0;
  uint64_t clear_submits = 0;
  uint64_t stale = 0;

  const auto frame_sleep = std::chrono::duration<double>(1.0 / std::max(1.0, cfg.render_hz));

  auto submit_clear_frame = [&](const char* reason) {
    canvas.clear();
    ++frames;
    ++clear_submits;
    bool ok = true;
    if (overlay) ok = overlay->submit(canvas);
    if (ok) ++submit_ok; else ++submit_fail;
    const uint64_t t = now_ns_u64();
    if (t - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
      log.info("clear frame reason=", reason,
               " frames=", frames,
               " clear_submits=", clear_submits,
               " submit_ok=", submit_ok,
               " submit_fail=", submit_fail,
               " no_pose=", no_pose,
               " no_mesh=", no_mesh);
      last_log_ns = t;
    }
  };

  while (true) {
    const uint64_t now = now_ns_u64();
    if (!reader && (last_attach_try_ns == 0 || (now - last_attach_try_ns) / 1e6 >= cfg.attach_retry_ms)) {
      last_attach_try_ns = now;
      try {
        log.info("attaching SHM stream registry=", cfg.registry, " stream=", cfg.stream);
        auto info = xr_runtime::stream_info_from_registry(cfg.registry, cfg.stream);
        reader = std::make_unique<MeshReader>(info, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME);
        if (info.stream_id != "runtime_spatial_proxy_mesh" || info.frame_id != "runtime_local") {
          log.warn("overlay must consume xr_runtime_adapter output; got stream=", info.stream_id,
                   " frame=", info.frame_id,
                   " expected stream=runtime_spatial_proxy_mesh frame=runtime_local");
        }
        log.info("attached stream=", info.stream_id,
                 " shm=", info.shm_name,
                 " frame=", info.frame_id,
                 " payload_size=", info.payload_size,
                 " slots=", info.slot_count,
                 " stride=", info.slot_stride);
      } catch (const std::exception& e) {
        log.warn("waiting for spatial proxy mesh stream: ", e.what());
      }
    }

    if (cfg.pose_source == "xr_runtime" && !pose_reader &&
        (last_pose_attach_try_ns == 0 || (now - last_pose_attach_try_ns) / 1e6 >= cfg.attach_retry_ms)) {
      last_pose_attach_try_ns = now;
      try {
        log.info("attaching runtime HMD pose stream registry=", cfg.pose_registry,
                 " stream=", cfg.pose_stream);
        auto pinfo = xr_runtime::stream_info_from_registry(cfg.pose_registry, cfg.pose_stream);
        pose_reader = std::make_unique<RuntimePoseReader>(pinfo, xr_runtime::RUNTIME_HMD_POSE_FORMAT_NAME);
        if (pinfo.stream_id != "runtime_hmd_pose" || pinfo.frame_id != "runtime_local") {
          log.warn("overlay projection should use xr_runtime_adapter runtime_hmd_pose; got stream=", pinfo.stream_id,
                   " frame=", pinfo.frame_id,
                   " expected stream=runtime_hmd_pose frame=runtime_local");
        }
        log.info("attached runtime HMD pose stream=", pinfo.stream_id,
                 " shm=", pinfo.shm_name,
                 " frame=", pinfo.frame_id,
                 " payload_size=", pinfo.payload_size,
                 " slots=", pinfo.slot_count);
      } catch (const std::exception& e) {
        log.warn("waiting for runtime HMD pose stream: ", e.what());
      }
    }

    std::optional<Mesh> mesh;
    if (reader) {
      try {
        mesh = reader->latest();
      } catch (const std::exception& e) {
        log.warn("SHM read failed, will reattach: ", e.what());
        reader.reset();
      }
    }

    if (!mesh) {
      ++no_mesh;
      if (now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
        log.info("waiting for mesh payload frames=", frames, " no_mesh=", no_mesh);
        if (reader) {
          log_ring_header_debug(log, reader->info().shm_name, "mesh ring debug");
        }
        last_log_ns = now;
      }
      if (cfg.clear_when_no_mesh) submit_clear_frame("no_mesh");
      std::this_thread::sleep_for(frame_sleep);
      continue;
    }

    if (mesh->sequence != last_seen_seq) {
      log_mesh(log, *mesh, "mesh update");
      last_seen_seq = mesh->sequence;
    }

    const double age_ms = mesh->timestamp_ns > 0 ? static_cast<double>(now - mesh->timestamp_ns) / 1e6 : 0.0;
    const bool mesh_is_stale = (cfg.max_mesh_age_ms > 0 && age_ms > cfg.max_mesh_age_ms);
    if (mesh_is_stale) {
      ++stale;
      if (now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
        log.warn("stale mesh seq=", mesh->sequence, " age_ms=", std::fixed, std::setprecision(1), age_ms,
                 " stale_count=", stale,
                 " action=", (cfg.draw_stale_mesh ? "draw_last_static_mesh" : "skip"));
        last_log_ns = now;
      }
      if (!cfg.draw_stale_mesh) {
        if (cfg.clear_on_stale_skip) submit_clear_frame("stale_mesh_skip");
        std::this_thread::sleep_for(frame_sleep);
        continue;
      }
    }

    Pose34 hmd{};
    bool pose_ok = true;
    if (cfg.pose_source == "xr_runtime") {
      pose_ok = false;
      std::optional<RuntimePose> runtime_pose;
      if (pose_reader) {
        try {
          runtime_pose = pose_reader->latest();
        } catch (const std::exception& e) {
          log.warn("runtime HMD pose SHM read failed, will reattach: ", e.what());
          pose_reader.reset();
        }
      }
      if (runtime_pose && runtime_pose_is_valid(*runtime_pose)) {
        hmd = pose34_from_runtime_pose(*runtime_pose);
        pose_ok = true;
        if (runtime_pose->sequence != last_seen_pose_seq) {
          if (cfg.pose_verbose || last_seen_pose_seq == 0 || now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
            log.info("runtime HMD pose update seq=", runtime_pose->sequence,
                     " p=(", std::fixed, std::setprecision(3), runtime_pose->px, ",", runtime_pose->py, ",", runtime_pose->pz, ")",
                     " q=(", std::setprecision(3), runtime_pose->qw, ",", runtime_pose->qx, ",", runtime_pose->qy, ",", runtime_pose->qz, ")",
                     " yaw_deg=", std::setprecision(1), runtime_pose_yaw_deg(*runtime_pose),
                     " flags=0x", std::hex, runtime_pose->flags, std::dec);
          }
          last_seen_pose_seq = runtime_pose->sequence;
        }
      }
      if (!pose_ok) {
        ++no_pose;
        if (now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
          log.warn("runtime HMD pose unavailable no_pose=", no_pose,
                   " pose_source=xr_runtime registry=", cfg.pose_registry,
                   " stream=", cfg.pose_stream,
                   " hint=check jq .streams.runtime_hmd_pose /tmp/runtime_tracking_streams.json");
          last_log_ns = now;
        }
        if (cfg.clear_when_no_pose) submit_clear_frame("no_runtime_pose");
        std::this_thread::sleep_for(frame_sleep);
        continue;
      }
    } else if (cfg.pose_source == "openvr") {
      if (overlay) {
        pose_ok = overlay->hmd_pose(hmd);
        if (!pose_ok) {
          ++no_pose;
          if (now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
            log.warn("OpenVR HMD pose unavailable no_pose=", no_pose,
                     " connected SteamVR? tracking universe standing? still waiting");
            last_log_ns = now;
          }
          if (cfg.clear_when_no_pose) submit_clear_frame("no_openvr_pose");
          std::this_thread::sleep_for(frame_sleep);
          continue;
        }
      } else {
        hmd.m[0][0] = 1.0; hmd.m[1][1] = 1.0; hmd.m[2][2] = 1.0;
      }
    } else {
      // identity pose is only for dry-run or projection debugging.
      hmd.m[0][0] = 1.0; hmd.m[1][1] = 1.0; hmd.m[2][2] = 1.0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    canvas.clear();
    RenderStats rs{};
    if (cfg.stereo_sbs) {
      const int eye_w = std::max(1, cfg.width / 2);
      const int eye_h = std::max(1, cfg.height);
      const double half_ipd = std::max(0.0, cfg.stereo_ipd_m) * 0.5;
      const Pose34 left_eye = pose34_offset_local_x(hmd, -half_ipd);
      const Pose34 right_eye = pose34_offset_local_x(hmd, half_ipd);
      const RenderStats left_stats = render_mesh(canvas, cfg, 0, 0, eye_w, eye_h, left_eye, *mesh, cfg.stereo_draw_hud);
      const RenderStats right_stats = render_mesh(canvas, cfg, eye_w, 0, eye_w, eye_h, right_eye, *mesh, cfg.stereo_draw_hud);
      rs = merge_stereo_stats(left_stats, right_stats);
    } else {
      rs = render_mesh(canvas, cfg, 0, 0, cfg.width, cfg.height, hmd, *mesh, true);
    }
    if (cfg.force_visible_border || cfg.visibility_test_pattern) {
      draw_visibility_pattern(canvas, frames, cfg.visibility_test_pattern);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double render_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++frames;

    bool ok = true;
    if (overlay) ok = overlay->submit(canvas);
    if (ok) ++submit_ok; else ++submit_fail;

    if (cfg.verbose_frames || now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9) || mesh->sequence != last_render_seq) {
      log.info("frame frames=", frames,
               " seq=", mesh->sequence,
               " vc=", rs.vertices,
               " tc=", rs.triangles,
               " finite=", rs.finite_vertices,
               " projected=", rs.projected_vertices,
               " clipped=", rs.clipped_vertices,
               " tri_drawn=", rs.triangles_drawn,
               " mesh_px=", rs.mesh_pixels_drawn,
               " point_px=", rs.points_drawn,
               " edges=", rs.edges_drawn,
               " dist_min_m=", std::fixed, std::setprecision(2), (std::isfinite(rs.min_depth_m) ? rs.min_depth_m : 0.0),
               " dist_max_m=", std::fixed, std::setprecision(2), rs.max_depth_m,
               " mesh_age_ms=", std::fixed, std::setprecision(1), rs.mesh_age_ms,
               " stale=", (mesh_is_stale ? "1" : "0"),
               " stereo_sbs=", (cfg.stereo_sbs ? "1" : "0"),
               " draw_stale=", (cfg.draw_stale_mesh ? "1" : "0"),
               " render_ms=", std::setprecision(3), render_ms,
               " submit_ok=", submit_ok,
               " submit_fail=", submit_fail,
               " no_pose=", no_pose,
               " no_mesh=", no_mesh,
               " clear_submits=", clear_submits,
               " pose_source=", cfg.pose_source,
               " pose_seq=", last_seen_pose_seq);
      last_log_ns = now;
      last_render_seq = mesh->sequence;
    }

    std::this_thread::sleep_for(frame_sleep);
  }

  return 0;
}
