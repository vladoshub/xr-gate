#include <openvr.h>

#ifndef _WIN32
#include <X11/Xlib.h>
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#endif

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Mesh = xr_spatial::RuntimeSpatialProxyMeshF32V1;
using MeshReader = xr_runtime::TrackingRingReader<Mesh>;

uint64_t now_ns_u64() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string wall_time_string() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
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

 private:
  template <typename... Args>
  void line(const char* level, Args&&... args) {
    std::ostringstream msg;
    (msg << ... << args);
    const std::string s = "[" + wall_time_string() + "][xr_steamvr_spatial_scene][" + level + "] " + msg.str();
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
  std::string registry = env_string("SPATIAL_SCENE_REGISTRY", "/tmp/runtime_tracking_streams.json");
  std::string stream = env_string("SPATIAL_SCENE_STREAM", "runtime_spatial_proxy_mesh");
  std::string log_file = env_string("SPATIAL_SCENE_LOG_FILE", "/tmp/xr_steamvr_spatial_scene.log");
  int width = env_int("SPATIAL_SCENE_WIDTH", 1280);
  int height = env_int("SPATIAL_SCENE_HEIGHT", 1280);
  double render_hz = env_double("SPATIAL_SCENE_RENDER_HZ", 60.0);
  double log_every_sec = env_double("SPATIAL_SCENE_LOG_EVERY_SEC", 1.0);
  double attach_retry_ms = env_double("SPATIAL_SCENE_ATTACH_RETRY_MS", 500.0);
  double max_mesh_age_ms = env_double("SPATIAL_SCENE_MAX_MESH_AGE_MS", 3000.0);
  bool draw_stale_mesh = env_bool("SPATIAL_SCENE_DRAW_STALE_MESH", true);
  bool dry_run = env_bool("SPATIAL_SCENE_DRY_RUN", false);
  bool verbose_frames = env_bool("SPATIAL_SCENE_VERBOSE_FRAMES", false);
  bool draw_mesh = env_bool("SPATIAL_SCENE_DRAW_MESH", true);
  bool draw_points = env_bool("SPATIAL_SCENE_DRAW_POINTS", false);
  bool draw_wire = env_bool("SPATIAL_SCENE_DRAW_WIRE", false);
  bool color_by_distance = env_bool("SPATIAL_SCENE_COLOR_BY_DISTANCE", true);
  double point_size = env_double("SPATIAL_SCENE_POINT_SIZE", 3.0);
  double alpha = env_double("SPATIAL_SCENE_ALPHA", 0.70);
  double near_m = env_double("SPATIAL_SCENE_NEAR_M", 0.05);
  double far_m = env_double("SPATIAL_SCENE_FAR_M", 8.0);
};

void print_help() {
  std::cout << "xr_steamvr_spatial_scene\n"
            << "  Reads runtime_spatial_proxy_mesh from SHM and submits real stereo 3D geometry to OpenVR compositor.\n\n"
            << "Options mirror env vars:\n"
            << "  --registry PATH\n"
            << "  --stream NAME\n"
            << "  --log-file PATH\n"
            << "  --width N --height N\n"
            << "  --render-hz HZ\n"
            << "  --dry-run\n"
            << "  --verbose-frames\n"
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
    else if (a == "--width") c.width = std::stoi(need("--width"));
    else if (a == "--height") c.height = std::stoi(need("--height"));
    else if (a == "--render-hz") c.render_hz = std::stod(need("--render-hz"));
    else if (a == "--dry-run") c.dry_run = true;
    else if (a == "--verbose-frames") c.verbose_frames = true;
    else throw std::runtime_error("unknown argument: " + a);
  }
  return true;
}

const char* vr_init_error_name(vr::EVRInitError e) {
  return vr::VR_GetVRInitErrorAsEnglishDescription(e);
}

const char* compositor_error_name(vr::EVRCompositorError e) {
  switch (e) {
    case vr::VRCompositorError_None: return "None";
    case vr::VRCompositorError_RequestFailed: return "RequestFailed";
    case vr::VRCompositorError_IncompatibleVersion: return "IncompatibleVersion";
    case vr::VRCompositorError_DoNotHaveFocus: return "DoNotHaveFocus";
    case vr::VRCompositorError_InvalidTexture: return "InvalidTexture";
    case vr::VRCompositorError_IsNotSceneApplication: return "IsNotSceneApplication";
    case vr::VRCompositorError_TextureIsOnWrongDevice: return "TextureIsOnWrongDevice";
    case vr::VRCompositorError_TextureUsesUnsupportedFormat: return "TextureUsesUnsupportedFormat";
    case vr::VRCompositorError_SharedTexturesNotSupported: return "SharedTexturesNotSupported";
    case vr::VRCompositorError_IndexOutOfRange: return "IndexOutOfRange";
    case vr::VRCompositorError_AlreadySubmitted: return "AlreadySubmitted";
    case vr::VRCompositorError_InvalidBounds: return "InvalidBounds";
    default: return "Unknown";
  }
}

struct Mat44 {
  double m[4][4]{};
};

Mat44 identity44() {
  Mat44 r{};
  for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
  return r;
}

Mat44 mat44_from_openvr34(const vr::HmdMatrix34_t& a) {
  Mat44 r = identity44();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) r.m[row][col] = a.m[row][col];
  }
  return r;
}

Mat44 mat44_from_openvr44(const vr::HmdMatrix44_t& a) {
  Mat44 r{};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) r.m[row][col] = a.m[row][col];
  }
  return r;
}

Mat44 mul44(const Mat44& a, const Mat44& b) {
  Mat44 r{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    }
  }
  return r;
}

Mat44 inverse_rigid44(const Mat44& t) {
  Mat44 r = identity44();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) r.m[row][col] = t.m[col][row];
  }
  for (int row = 0; row < 3; ++row) {
    r.m[row][3] = -(r.m[row][0] * t.m[0][3] + r.m[row][1] * t.m[1][3] + r.m[row][2] * t.m[2][3]);
  }
  return r;
}

void load_gl_matrix(const Mat44& m) {
  GLdouble v[16];
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) v[col * 4 + row] = m.m[row][col];
  }
  glLoadMatrixd(v);
}

void distance_color(float x, float y, float z, bool enabled) {
  if (!enabled) {
    glColor4f(0.1f, 1.0f, 0.35f, 0.70f);
    return;
  }
  const float d = std::sqrt(x * x + y * y + z * z);
  if (d < 0.50f) glColor4f(1.0f, 0.15f, 0.10f, 0.75f);
  else if (d < 1.00f) glColor4f(1.0f, 0.85f, 0.10f, 0.72f);
  else if (d < 1.50f) glColor4f(0.15f, 1.0f, 0.25f, 0.70f);
  else if (d < 2.50f) glColor4f(0.10f, 0.65f, 1.0f, 0.68f);
  else glColor4f(0.55f, 0.35f, 1.0f, 0.65f);
}

bool finite_vertex(const xr_spatial::RuntimeSpatialProxyVertexF32V1& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct DrawStats {
  int finite_vertices = 0;
  int triangles_drawn = 0;
  int points_drawn = 0;
};

DrawStats draw_mesh_gl(const Mesh& mesh, const Config& cfg) {
  DrawStats st{};
  const uint32_t vc = std::min<uint32_t>(mesh.vertex_count, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_VERTICES);
  const uint32_t tc = std::min<uint32_t>(mesh.triangle_count, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_MAX_TRIANGLES);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_LIGHTING);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);

  if (cfg.draw_mesh && tc > 0) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBegin(GL_TRIANGLES);
    for (uint32_t i = 0; i < tc; ++i) {
      const auto tri = mesh.triangles[i];
      if (tri.i0 >= vc || tri.i1 >= vc || tri.i2 >= vc) continue;
      const auto& a = mesh.vertices[tri.i0];
      const auto& b = mesh.vertices[tri.i1];
      const auto& c = mesh.vertices[tri.i2];
      if (!finite_vertex(a) || !finite_vertex(b) || !finite_vertex(c)) continue;
      const float cx = (a.x + b.x + c.x) / 3.0f;
      const float cy = (a.y + b.y + c.y) / 3.0f;
      const float cz = (a.z + b.z + c.z) / 3.0f;
      distance_color(cx, cy, cz, cfg.color_by_distance);
      glVertex3f(a.x, a.y, a.z);
      glVertex3f(b.x, b.y, b.z);
      glVertex3f(c.x, c.y, c.z);
      ++st.triangles_drawn;
    }
    glEnd();
  }

  if (cfg.draw_wire && tc > 0) {
    glLineWidth(1.0f);
    glColor4f(0.0f, 1.0f, 0.25f, static_cast<float>(std::clamp(cfg.alpha, 0.05, 1.0)));
    glBegin(GL_LINES);
    for (uint32_t i = 0; i < tc; ++i) {
      const auto tri = mesh.triangles[i];
      if (tri.i0 >= vc || tri.i1 >= vc || tri.i2 >= vc) continue;
      const auto& a = mesh.vertices[tri.i0];
      const auto& b = mesh.vertices[tri.i1];
      const auto& c = mesh.vertices[tri.i2];
      if (!finite_vertex(a) || !finite_vertex(b) || !finite_vertex(c)) continue;
      glVertex3f(a.x, a.y, a.z); glVertex3f(b.x, b.y, b.z);
      glVertex3f(b.x, b.y, b.z); glVertex3f(c.x, c.y, c.z);
      glVertex3f(c.x, c.y, c.z); glVertex3f(a.x, a.y, a.z);
    }
    glEnd();
  }

  if (cfg.draw_points) {
    glPointSize(static_cast<float>(std::max(1.0, cfg.point_size)));
    glBegin(GL_POINTS);
    for (uint32_t i = 0; i < vc; ++i) {
      const auto& v = mesh.vertices[i];
      if (!finite_vertex(v)) continue;
      // Organized-grid invalid cells are usually zeroed.  Avoid drawing a bright ball at origin.
      if (std::abs(v.x) < 1e-6f && std::abs(v.y) < 1e-6f && std::abs(v.z) < 1e-6f) continue;
      distance_color(v.x, v.y, v.z, cfg.color_by_distance);
      glVertex3f(v.x, v.y, v.z);
      ++st.points_drawn;
    }
    glEnd();
  }

  for (uint32_t i = 0; i < vc; ++i) if (finite_vertex(mesh.vertices[i])) ++st.finite_vertices;
  return st;
}

#ifndef _WIN32
class GlContext {
 public:
  GlContext(int w, int h, Logger& log) : w_(w), h_(h), log_(log) {}
  ~GlContext() { shutdown(); }

  bool init() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      log_.error("XOpenDisplay failed; set DISPLAY and run inside an X11 SteamVR session");
      return false;
    }

    int fb_attribs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT | GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER, False,
        None};
    int fb_count = 0;
    GLXFBConfig* fbc = glXChooseFBConfig(display_, DefaultScreen(display_), fb_attribs, &fb_count);
    if (!fbc || fb_count <= 0) {
      log_.error("glXChooseFBConfig failed");
      if (fbc) XFree(fbc);
      return false;
    }
    fb_config_ = fbc[0];
    XFree(fbc);

    XVisualInfo* vi = glXGetVisualFromFBConfig(display_, fb_config_);
    if (!vi) {
      log_.error("glXGetVisualFromFBConfig failed");
      return false;
    }
    Colormap cmap = XCreateColormap(display_, RootWindow(display_, vi->screen), vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap;
    swa.event_mask = 0;
    window_ = XCreateWindow(display_, RootWindow(display_, vi->screen), 0, 0, 64, 64, 0, vi->depth,
                            InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!window_) {
      log_.error("XCreateWindow failed");
      return false;
    }

    context_ = glXCreateNewContext(display_, fb_config_, GLX_RGBA_TYPE, nullptr, True);
    if (!context_) {
      log_.error("glXCreateNewContext failed");
      return false;
    }
    if (!glXMakeCurrent(display_, window_, context_)) {
      log_.error("glXMakeCurrent failed");
      return false;
    }

    if (!create_eye(left_) || !create_eye(right_)) return false;
    log_.info("OpenGL scene context ready vendor=", reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
              " renderer=", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              " version=", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
  }

  struct EyeTarget {
    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depth = 0;
  };

  EyeTarget& eye(vr::EVREye e) { return e == vr::Eye_Left ? left_ : right_; }
  int width() const { return w_; }
  int height() const { return h_; }

  void bind_eye(vr::EVREye e) {
    EyeTarget& t = eye(e);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, w_, h_);
  }

  void shutdown() {
    if (display_ && context_) glXMakeCurrent(display_, None, nullptr);
    destroy_eye(left_);
    destroy_eye(right_);
    if (context_) { glXDestroyContext(display_, context_); context_ = nullptr; }
    if (display_ && window_) { XDestroyWindow(display_, window_); window_ = 0; }
    if (display_) { XCloseDisplay(display_); display_ = nullptr; }
  }

 private:
  bool create_eye(EyeTarget& t) {
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);

    glGenTextures(1, &t.color);
    glBindTexture(GL_TEXTURE_2D, t.color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);

    glGenRenderbuffers(1, &t.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w_, h_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      log_.error("FBO incomplete");
      return false;
    }
    return true;
  }

  void destroy_eye(EyeTarget& t) {
    if (t.depth) glDeleteRenderbuffers(1, &t.depth);
    if (t.color) glDeleteTextures(1, &t.color);
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    t = EyeTarget{};
  }

  int w_ = 0;
  int h_ = 0;
  Logger& log_;
  Display* display_ = nullptr;
  GLXFBConfig fb_config_ = nullptr;
  Window window_ = 0;
  GLXContext context_ = nullptr;
  EyeTarget left_{};
  EyeTarget right_{};
};
#else
class GlContext {
 public:
  GlContext(int, int, Logger&) {}
  bool init() { return false; }
};
#endif

class OpenVrScene {
 public:
  OpenVrScene(const Config& cfg, Logger& log) : cfg_(cfg), log_(log), gl_(cfg.width, cfg.height, log) {}
  ~OpenVrScene() { shutdown(); }

  bool init() {
    vr::EVRInitError init_err = vr::VRInitError_None;
    vr_system_ = vr::VR_Init(&init_err, vr::VRApplication_Scene);
    if (init_err != vr::VRInitError_None) {
      log_.error("VR_Init(Scene) failed: ", vr_init_error_name(init_err), " (", int(init_err), ")");
      return false;
    }
    compositor_ = vr::VRCompositor();
    if (!compositor_) {
      log_.error("VRCompositor returned null");
      return false;
    }
    if (!gl_.init()) return false;
    log_.info("OpenVR scene initialized render_target=", cfg_.width, "x", cfg_.height);
    return true;
  }

  bool render_submit(const Mesh& mesh, DrawStats& left_stats, DrawStats& right_stats) {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    compositor_->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    const auto& hmd_pose = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd_pose.bPoseIsValid) {
      ++no_hmd_pose_;
      if (no_hmd_pose_ < 10 || no_hmd_pose_ % 90 == 0) {
        log_.warn("OpenVR HMD pose is not valid no_hmd_pose=", no_hmd_pose_);
      }
      return false;
    }

    const Mat44 head_to_world = mat44_from_openvr34(hmd_pose.mDeviceToAbsoluteTracking);
    left_stats = render_eye(vr::Eye_Left, head_to_world, mesh);
    right_stats = render_eye(vr::Eye_Right, head_to_world, mesh);

    glFlush();

    vr::Texture_t left_tex{};
    left_tex.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(gl_.eye(vr::Eye_Left).color));
    left_tex.eType = vr::TextureType_OpenGL;
    left_tex.eColorSpace = vr::ColorSpace_Gamma;
    vr::Texture_t right_tex{};
    right_tex.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(gl_.eye(vr::Eye_Right).color));
    right_tex.eType = vr::TextureType_OpenGL;
    right_tex.eColorSpace = vr::ColorSpace_Gamma;

    const auto le = compositor_->Submit(vr::Eye_Left, &left_tex);
    const auto re = compositor_->Submit(vr::Eye_Right, &right_tex);
    compositor_->PostPresentHandoff();

    if (le != vr::VRCompositorError_None || re != vr::VRCompositorError_None) {
      if (submit_error_count_++ < 10 || submit_error_count_ % 90 == 0) {
        log_.warn("compositor submit failed left=", compositor_error_name(le), "(", int(le), ")",
                  " right=", compositor_error_name(re), "(", int(re), ")");
      }
      return false;
    }
    return true;
  }

  void shutdown() {
    gl_.shutdown();
    if (vr_system_) {
      vr::VR_Shutdown();
      vr_system_ = nullptr;
      compositor_ = nullptr;
    }
  }

 private:
  DrawStats render_eye(vr::EVREye eye, const Mat44& head_to_world, const Mesh& mesh) {
    gl_.bind_eye(eye);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const Mat44 projection = mat44_from_openvr44(vr_system_->GetProjectionMatrix(eye, static_cast<float>(cfg_.near_m), static_cast<float>(cfg_.far_m)));
    const Mat44 eye_to_head = mat44_from_openvr34(vr_system_->GetEyeToHeadTransform(eye));
    const Mat44 eye_to_world = mul44(head_to_world, eye_to_head);
    const Mat44 world_to_eye = inverse_rigid44(eye_to_world);

    glMatrixMode(GL_PROJECTION);
    load_gl_matrix(projection);
    glMatrixMode(GL_MODELVIEW);
    load_gl_matrix(world_to_eye);

    return draw_mesh_gl(mesh, cfg_);
  }

  const Config& cfg_;
  Logger& log_;
  GlContext gl_;
  vr::IVRSystem* vr_system_ = nullptr;
  vr::IVRCompositor* compositor_ = nullptr;
  uint64_t no_hmd_pose_ = 0;
  uint64_t submit_error_count_ = 0;
};

void log_mesh(Logger& log, const Mesh& mesh, const char* prefix) {
  log.info(prefix,
           " seq=", mesh.sequence,
           " kind=", mesh.mesh_kind,
           " grid=", mesh.grid_width, "x", mesh.grid_height,
           " valid=", mesh.valid_point_count,
           " vc=", mesh.vertex_count,
           " tc=", mesh.triangle_count,
           " flags=0x", std::hex, mesh.status_flags, std::dec,
           " bbox=[(", std::fixed, std::setprecision(3), mesh.bbox_min_x, ",", mesh.bbox_min_y, ",", mesh.bbox_min_z,
           ")-(", mesh.bbox_max_x, ",", mesh.bbox_max_y, ",", mesh.bbox_max_z, ")]",
           " age_ms=", std::setprecision(1), (mesh.timestamp_ns ? double(now_ns_u64() - mesh.timestamp_ns) / 1e6 : 0.0));
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg{};
  try {
    if (!parse_args(argc, argv, cfg)) return 0;
  } catch (const std::exception& e) {
    std::cerr << "argument error: " << e.what() << "\n";
    print_help();
    return 2;
  }

  Logger log(cfg.log_file);
  log.info("starting registry=", cfg.registry,
           " stream=", cfg.stream,
           " texture=", cfg.width, "x", cfg.height,
           " render_hz=", cfg.render_hz,
           " dry_run=", cfg.dry_run,
           " draw_mesh=", cfg.draw_mesh,
           " draw_points=", cfg.draw_points,
           " draw_wire=", cfg.draw_wire,
           " alpha=", cfg.alpha);

  std::unique_ptr<OpenVrScene> scene;
  if (!cfg.dry_run) {
    scene = std::make_unique<OpenVrScene>(cfg, log);
    if (!scene->init()) {
      log.error("OpenVR scene init failed. Re-run with SPATIAL_SCENE_DRY_RUN=1 to debug SHM only.");
      return 3;
    }
  } else {
    log.warn("dry-run enabled: OpenVR scene submit disabled; SHM reader/logging only");
  }

  std::unique_ptr<MeshReader> reader;
  uint64_t last_attach_try_ns = 0;
  uint64_t last_log_ns = 0;
  uint64_t last_seen_seq = 0;
  uint64_t frames = 0;
  uint64_t submit_ok = 0;
  uint64_t submit_fail = 0;
  uint64_t no_mesh = 0;
  uint64_t stale = 0;
  const auto frame_sleep = std::chrono::duration<double>(1.0 / std::max(1.0, cfg.render_hz));

  while (true) {
    const uint64_t now = now_ns_u64();
    if (!reader && (last_attach_try_ns == 0 || (now - last_attach_try_ns) / 1e6 >= cfg.attach_retry_ms)) {
      last_attach_try_ns = now;
      try {
        log.info("attaching SHM stream registry=", cfg.registry, " stream=", cfg.stream);
        auto info = xr_runtime::stream_info_from_registry(cfg.registry, cfg.stream);
        reader = std::make_unique<MeshReader>(info, xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME);
        if (info.stream_id != "runtime_spatial_proxy_mesh" || info.frame_id != "runtime_local") {
          log.warn("scene app should consume xr_runtime_adapter output; got stream=", info.stream_id,
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
        last_log_ns = now;
      }
      std::this_thread::sleep_for(frame_sleep);
      continue;
    }

    if (mesh->sequence != last_seen_seq) {
      log_mesh(log, *mesh, "mesh update");
      last_seen_seq = mesh->sequence;
    }

    const double age_ms = mesh->timestamp_ns > 0 ? static_cast<double>(now - mesh->timestamp_ns) / 1e6 : 0.0;
    const bool mesh_is_stale = cfg.max_mesh_age_ms > 0 && age_ms > cfg.max_mesh_age_ms;
    if (mesh_is_stale) {
      ++stale;
      if (!cfg.draw_stale_mesh) {
        if (now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
          log.warn("stale mesh skip seq=", mesh->sequence, " age_ms=", std::fixed, std::setprecision(1), age_ms);
          last_log_ns = now;
        }
        std::this_thread::sleep_for(frame_sleep);
        continue;
      }
    }

    DrawStats left{}, right{};
    bool ok = true;
    if (scene) ok = scene->render_submit(*mesh, left, right);
    if (ok) ++submit_ok; else ++submit_fail;
    ++frames;

    if (cfg.verbose_frames || now - last_log_ns > static_cast<uint64_t>(cfg.log_every_sec * 1e9)) {
      log.info("frame frames=", frames,
               " seq=", mesh->sequence,
               " vc=", mesh->vertex_count,
               " tc=", mesh->triangle_count,
               " left_tri=", left.triangles_drawn,
               " right_tri=", right.triangles_drawn,
               " left_pts=", left.points_drawn,
               " right_pts=", right.points_drawn,
               " mesh_age_ms=", std::fixed, std::setprecision(1), age_ms,
               " stale=", (mesh_is_stale ? "1" : "0"),
               " submit_ok=", submit_ok,
               " submit_fail=", submit_fail,
               " no_mesh=", no_mesh);
      last_log_ns = now;
    }

    std::this_thread::sleep_for(frame_sleep);
  }

  return 0;
}
