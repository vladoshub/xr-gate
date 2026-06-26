#include <openvr.h>

#if defined(__linux__)
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#endif

#include <xr_video/contracts/stereo_video_contract.hpp>
#include <xr_video/shm/stereo_video_shm_reader.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

enum class VideoMode {
  Sbs,
  Left,
  Right,
};

enum class UploadBackend {
  OpenGl,
  Raw,
};

VideoMode parse_video_mode(const std::string& value) {
  if (value == "sbs") return VideoMode::Sbs;
  if (value == "left") return VideoMode::Left;
  if (value == "right") return VideoMode::Right;
  throw std::runtime_error("bad --video-mode: " + value + " (expected sbs|left|right)");
}

const char* video_mode_name(VideoMode mode) {
  switch (mode) {
    case VideoMode::Sbs: return "sbs";
    case VideoMode::Left: return "left";
    case VideoMode::Right: return "right";
  }
  return "unknown";
}

UploadBackend parse_upload_backend(const std::string& value) {
  if (value == "opengl" || value == "gl") return UploadBackend::OpenGl;
  if (value == "raw") return UploadBackend::Raw;
  throw std::runtime_error("bad --upload-backend: " + value + " (expected opengl|raw)");
}

const char* upload_backend_name(UploadBackend backend) {
  switch (backend) {
    case UploadBackend::OpenGl: return "opengl";
    case UploadBackend::Raw: return "raw";
  }
  return "unknown";
}

struct Config {
  std::string video_registry = "/tmp/runtime_video_streams.json";
  std::string video_stream = "runtime_stereo_video";

  std::string overlay_key = "xr_tracking.steamvr.video_overlay";
  std::string overlay_name = "XR Stereo Video Overlay";

  float width_m = 1.20f;
  float distance_m = 1.00f;
  float x_m = 0.0f;
  float y_m = 0.0f;
  float alpha = 0.70f;
  float target_fps = 30.0f;

  bool enable_sbs_flag = true;
  bool follow_hmd = true;
  bool hide_until_first_frame = true;
  bool upload_once = false;
  bool flip_v = false;

  UploadBackend upload_backend = UploadBackend::OpenGl;
  VideoMode video_mode = VideoMode::Sbs;
  uint32_t scale_divisor = 1;

  uint64_t print_every = 30;
  uint64_t reattach_sleep_ms = 500;
};

void print_usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n\n"
      << "Options:\n"
      << "  --video-registry PATH       Runtime video registry path (default: /tmp/runtime_video_streams.json)\n"
      << "  --video-stream ID           Runtime video stream id (default: runtime_stereo_video)\n"
      << "  --overlay-key KEY           OpenVR overlay key\n"
      << "  --overlay-name NAME         Human-readable overlay name\n"
      << "  --width-m VALUE             Overlay width in meters (default: 1.20)\n"
      << "  --distance-m VALUE          Distance in front of HMD in meters (default: 1.00)\n"
      << "  --x-m VALUE                 Horizontal offset relative to HMD, meters\n"
      << "  --y-m VALUE                 Vertical offset relative to HMD, meters\n"
      << "  --alpha VALUE               Overlay alpha 0..1 (default: 0.70)\n"
      << "  --target-fps VALUE          Upload loop target FPS (default: 30)\n"
      << "  --upload-backend MODE       Upload backend: opengl|raw (default: opengl)\n"
      << "  --video-mode MODE           Upload mode: sbs|left|right (default: sbs)\n"
      << "  --scale-divisor N           Downscale source by integer N before upload (default: 1)\n"
      << "  --upload-once               Upload the first frame once, then keep overlay static\n"
      << "  --flip-v                    Flip overlay texture vertically via OpenVR texture bounds\n"
      << "  --no-flip-v                 Disable vertical texture-bounds flip\n"
      << "  --print-every N             Print stats every N uploaded frames (default: 30, 0 disables)\n"
      << "  --no-sbs-flag               Do not enable OpenVR side-by-side stereo flag\n"
      << "  --no-follow-hmd             Use absolute standing-space transform instead of HMD-relative transform\n"
      << "  --show-before-first-frame   Show overlay immediately after creation\n"
      << "  --reattach-sleep-ms N       Sleep between SteamVR/SHM reattach attempts\n"
      << "  --help                      Show this help\n";
}

void arg_has_value(int i, int argc, const char* opt) {
  if (i + 1 < argc) return;
  std::ostringstream oss;
  oss << opt << " requires a value";
  throw std::runtime_error(oss.str());
}

template <typename T>
T parse_number(const char* s, const char* opt) {
  std::istringstream in(s);
  T value{};
  in >> value;
  if (!in || !in.eof()) {
    std::ostringstream oss;
    oss << "bad numeric value for " << opt << ": " << s;
    throw std::runtime_error(oss.str());
  }
  return value;
}

Config parse_args(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (a == "--video-registry") {
      arg_has_value(i, argc, a.c_str());
      cfg.video_registry = argv[++i];
    } else if (a == "--video-stream") {
      arg_has_value(i, argc, a.c_str());
      cfg.video_stream = argv[++i];
    } else if (a == "--overlay-key") {
      arg_has_value(i, argc, a.c_str());
      cfg.overlay_key = argv[++i];
    } else if (a == "--overlay-name") {
      arg_has_value(i, argc, a.c_str());
      cfg.overlay_name = argv[++i];
    } else if (a == "--width-m") {
      arg_has_value(i, argc, a.c_str());
      cfg.width_m = parse_number<float>(argv[++i], a.c_str());
    } else if (a == "--distance-m") {
      arg_has_value(i, argc, a.c_str());
      cfg.distance_m = parse_number<float>(argv[++i], a.c_str());
    } else if (a == "--x-m") {
      arg_has_value(i, argc, a.c_str());
      cfg.x_m = parse_number<float>(argv[++i], a.c_str());
    } else if (a == "--y-m") {
      arg_has_value(i, argc, a.c_str());
      cfg.y_m = parse_number<float>(argv[++i], a.c_str());
    } else if (a == "--alpha") {
      arg_has_value(i, argc, a.c_str());
      cfg.alpha = parse_number<float>(argv[++i], a.c_str());
    } else if (a == "--target-fps") {
      arg_has_value(i, argc, a.c_str());
      cfg.target_fps = parse_number<float>(argv[++i], a.c_str());
    } else if (a == "--upload-backend") {
      arg_has_value(i, argc, a.c_str());
      cfg.upload_backend = parse_upload_backend(argv[++i]);
    } else if (a == "--video-mode") {
      arg_has_value(i, argc, a.c_str());
      cfg.video_mode = parse_video_mode(argv[++i]);
    } else if (a == "--scale-divisor") {
      arg_has_value(i, argc, a.c_str());
      cfg.scale_divisor = parse_number<uint32_t>(argv[++i], a.c_str());
    } else if (a == "--upload-once") {
      cfg.upload_once = true;
    } else if (a == "--flip-v") {
      cfg.flip_v = true;
    } else if (a == "--no-flip-v") {
      cfg.flip_v = false;
    } else if (a == "--print-every") {
      arg_has_value(i, argc, a.c_str());
      cfg.print_every = parse_number<uint64_t>(argv[++i], a.c_str());
    } else if (a == "--reattach-sleep-ms") {
      arg_has_value(i, argc, a.c_str());
      cfg.reattach_sleep_ms = parse_number<uint64_t>(argv[++i], a.c_str());
    } else if (a == "--no-sbs-flag") {
      cfg.enable_sbs_flag = false;
    } else if (a == "--no-follow-hmd") {
      cfg.follow_hmd = false;
    } else if (a == "--show-before-first-frame") {
      cfg.hide_until_first_frame = false;
    } else {
      throw std::runtime_error("unknown argument: " + a);
    }
  }

  if (cfg.width_m <= 0.01f) throw std::runtime_error("--width-m must be > 0.01");
  if (cfg.distance_m <= 0.01f) throw std::runtime_error("--distance-m must be > 0.01");
  if (cfg.alpha < 0.0f || cfg.alpha > 1.0f) throw std::runtime_error("--alpha must be in range 0..1");
  if (cfg.target_fps <= 0.0f || cfg.target_fps > 240.0f) {
    throw std::runtime_error("--target-fps must be in range 0..240");
  }
  if (cfg.scale_divisor == 0 || cfg.scale_divisor > 16) {
    throw std::runtime_error("--scale-divisor must be in range 1..16");
  }
  if (cfg.video_mode != VideoMode::Sbs) {
    cfg.enable_sbs_flag = false;
  }
  if (cfg.reattach_sleep_ms == 0) cfg.reattach_sleep_ms = 1;
  return cfg;
}

std::string overlay_error_name(vr::EVROverlayError e) {
  if (vr::VROverlay()) {
    const char* name = vr::VROverlay()->GetOverlayErrorNameFromEnum(e);
    if (name) return name;
  }
  return std::to_string(static_cast<int>(e));
}

void throw_overlay_error(const std::string& where, vr::EVROverlayError e) {
  if (e == vr::VROverlayError_None) return;
  throw std::runtime_error(where + " failed: " + overlay_error_name(e));
}

vr::HmdMatrix34_t make_transform(const Config& cfg) {
  vr::HmdMatrix34_t m{};
  m.m[0][0] = 1.0f;
  m.m[1][1] = 1.0f;
  m.m[2][2] = 1.0f;
  m.m[0][3] = cfg.x_m;
  m.m[1][3] = cfg.y_m;
  // In OpenVR convention HMD forward is -Z, so positive distance means in front of the face.
  m.m[2][3] = -cfg.distance_m;
  return m;
}

#if defined(__linux__)
std::string gl_error_name(GLenum e) {
  std::ostringstream oss;
  oss << "0x" << std::hex << static_cast<unsigned int>(e);
  return oss.str();
}

void throw_gl_error(const std::string& where) {
  const GLenum e = glGetError();
  if (e == GL_NO_ERROR) return;
  throw std::runtime_error(where + " GL error: " + gl_error_name(e));
}

class GlTextureUploader {
 public:
  GlTextureUploader() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) throw std::runtime_error("XOpenDisplay failed; OpenGL overlay upload needs X11/GLX");

    const int screen = DefaultScreen(display_);
    const int fb_attrs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 0,
        GLX_STENCIL_SIZE, 0,
        None};

    int fb_count = 0;
    GLXFBConfig* fb_configs = glXChooseFBConfig(display_, screen, fb_attrs, &fb_count);
    if (!fb_configs || fb_count <= 0) {
      if (fb_configs) XFree(fb_configs);
      throw std::runtime_error("glXChooseFBConfig failed");
    }
    fb_config_ = fb_configs[0];
    XFree(fb_configs);

    const int pb_attrs[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None};
    pbuffer_ = glXCreatePbuffer(display_, fb_config_, pb_attrs);
    if (!pbuffer_) throw std::runtime_error("glXCreatePbuffer failed");

    context_ = glXCreateNewContext(display_, fb_config_, GLX_RGBA_TYPE, nullptr, True);
    if (!context_) throw std::runtime_error("glXCreateNewContext failed");

    make_current();
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::cerr << "[steamvr_video_overlay] OpenGL upload context ready version="
              << (version ? version : "unknown") << "\n";
  }

  ~GlTextureUploader() { shutdown(); }

  GlTextureUploader(const GlTextureUploader&) = delete;
  GlTextureUploader& operator=(const GlTextureUploader&) = delete;

  vr::EVROverlayError upload_to_overlay(vr::VROverlayHandle_t handle,
                                        const std::vector<uint8_t>& rgba,
                                        uint32_t width,
                                        uint32_t height) {
    if (!vr::VROverlay()) return vr::VROverlayError_RequestFailed;
    if (rgba.empty() || width == 0 || height == 0) return vr::VROverlayError_InvalidTexture;

    make_current();
    ensure_texture(width, height);

    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height),
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    rgba.data());
    throw_gl_error("glTexSubImage2D");

    // Keep the first implementation conservative. We can relax this to glFlush/fences later.
    glFinish();

    vr::Texture_t texture{};
    texture.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(texture_));
    texture.eType = vr::TextureType_OpenGL;
    texture.eColorSpace = vr::ColorSpace_Gamma;
    return vr::VROverlay()->SetOverlayTexture(handle, &texture);
  }

 private:
  void make_current() {
    if (!display_ || !context_ || !pbuffer_) throw std::runtime_error("OpenGL context is not initialized");
    if (!glXMakeContextCurrent(display_, pbuffer_, pbuffer_, context_)) {
      throw std::runtime_error("glXMakeContextCurrent failed");
    }
  }

  void ensure_texture(uint32_t width, uint32_t height) {
    if (texture_ == 0) {
      glGenTextures(1, &texture_);
      throw_gl_error("glGenTextures");
      glBindTexture(GL_TEXTURE_2D, texture_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      throw_gl_error("glTexParameteri");
    } else {
      glBindTexture(GL_TEXTURE_2D, texture_);
    }

    if (texture_width_ != width || texture_height_ != height) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D,
                   0,
                   GL_RGBA8,
                   static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height),
                   0,
                   GL_RGBA,
                   GL_UNSIGNED_BYTE,
                   nullptr);
      throw_gl_error("glTexImage2D");
      texture_width_ = width;
      texture_height_ = height;
      std::cerr << "[steamvr_video_overlay] OpenGL texture allocated "
                << texture_width_ << "x" << texture_height_ << " id=" << texture_ << "\n";
    }
  }

  void shutdown() {
    if (display_ && context_ && pbuffer_) {
      glXMakeContextCurrent(display_, pbuffer_, pbuffer_, context_);
      if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
      }
      glXMakeContextCurrent(display_, None, None, nullptr);
    }
    if (display_ && context_) {
      glXDestroyContext(display_, context_);
      context_ = nullptr;
    }
    if (display_ && pbuffer_) {
      glXDestroyPbuffer(display_, pbuffer_);
      pbuffer_ = 0;
    }
    if (display_) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
  }

  Display* display_ = nullptr;
  GLXFBConfig fb_config_ = nullptr;
  GLXContext context_ = nullptr;
  GLXPbuffer pbuffer_ = 0;
  GLuint texture_ = 0;
  uint32_t texture_width_ = 0;
  uint32_t texture_height_ = 0;
};
#else
class GlTextureUploader {
 public:
  GlTextureUploader() { throw std::runtime_error("OpenGL texture backend is currently implemented only for Linux/GLX"); }
};
#endif

class OpenVrOverlaySession {
 public:
  explicit OpenVrOverlaySession(const Config& cfg) : cfg_(cfg) {
    vr::EVRInitError init_error = vr::VRInitError_None;
    vr::VR_Init(&init_error, vr::VRApplication_Overlay);
    if (init_error != vr::VRInitError_None) {
      const char* name = vr::VR_GetVRInitErrorAsEnglishDescription(init_error);
      throw std::runtime_error(std::string("VR_Init(VRApplication_Overlay) failed: ") +
                               (name ? name : std::to_string(static_cast<int>(init_error))));
    }
    if (!vr::VROverlay()) throw std::runtime_error("IVROverlay interface is not available");

    vr::EVROverlayError e = vr::VROverlay()->FindOverlay(cfg_.overlay_key.c_str(), &handle_);
    if (e == vr::VROverlayError_UnknownOverlay) {
      e = vr::VROverlay()->CreateOverlay(cfg_.overlay_key.c_str(), cfg_.overlay_name.c_str(), &handle_);
    }
    throw_overlay_error("Find/CreateOverlay", e);

    throw_overlay_error("SetOverlayInputMethod",
                        vr::VROverlay()->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_None));
    throw_overlay_error("SetOverlayAlpha", vr::VROverlay()->SetOverlayAlpha(handle_, cfg_.alpha));
    throw_overlay_error("SetOverlayWidthInMeters",
                        vr::VROverlay()->SetOverlayWidthInMeters(handle_, cfg_.width_m));

    if (cfg_.enable_sbs_flag) {
      throw_overlay_error("SetOverlayFlag(SideBySide_Parallel)",
                          vr::VROverlay()->SetOverlayFlag(
                              handle_, vr::VROverlayFlags_SideBySide_Parallel, true));
    }

    apply_texture_bounds();
    update_transform();

    if (!cfg_.hide_until_first_frame) {
      throw_overlay_error("ShowOverlay", vr::VROverlay()->ShowOverlay(handle_));
      visible_ = true;
    }

    std::cerr << "[steamvr_video_overlay] OpenVR overlay ready key=" << cfg_.overlay_key
              << " width_m=" << cfg_.width_m
              << " distance_m=" << cfg_.distance_m
              << " alpha=" << cfg_.alpha
              << " upload_backend=" << upload_backend_name(cfg_.upload_backend)
              << " mode=" << video_mode_name(cfg_.video_mode)
              << " scale_divisor=" << cfg_.scale_divisor
              << " upload_once=" << (cfg_.upload_once ? "yes" : "no")
              << " flip_v=" << (cfg_.flip_v ? "yes" : "no")
              << " sbs=" << (cfg_.enable_sbs_flag ? "yes" : "no") << "\n";
  }

  ~OpenVrOverlaySession() { shutdown(); }

  OpenVrOverlaySession(const OpenVrOverlaySession&) = delete;
  OpenVrOverlaySession& operator=(const OpenVrOverlaySession&) = delete;

  void upload_rgba(const std::vector<uint8_t>& rgba,
                   uint32_t width,
                   uint32_t height,
                   GlTextureUploader* gl_uploader) {
    if (rgba.empty() || width == 0 || height == 0) return;
    if (rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
      throw std::runtime_error("bad RGBA buffer size");
    }

    update_transform();

    if (cfg_.upload_backend == UploadBackend::OpenGl) {
      if (!gl_uploader) throw std::runtime_error("OpenGL uploader is not initialized");
      throw_overlay_error("SetOverlayTexture(OpenGL)", gl_uploader->upload_to_overlay(handle_, rgba, width, height));
    } else {
      auto* data = const_cast<uint8_t*>(rgba.data());
      const vr::EVROverlayError e = vr::VROverlay()->SetOverlayRaw(handle_, data, width, height, 4);
      throw_overlay_error("SetOverlayRaw", e);
    }

    if (!visible_) {
      throw_overlay_error("ShowOverlay", vr::VROverlay()->ShowOverlay(handle_));
      visible_ = true;
    }
  }

 private:
  void apply_texture_bounds() {
    vr::VRTextureBounds_t bounds{};
    bounds.uMin = 0.0f;
    bounds.uMax = 1.0f;
    if (cfg_.flip_v) {
      // CPU image buffers are top-left-origin, while OpenGL/OpenVR texture sampling for
      // TextureType_OpenGL is effectively bottom-left-origin on this SteamVR/Linux path.
      // Flip only the sampled V range; do not copy/flip pixels every frame.
      bounds.vMin = 1.0f;
      bounds.vMax = 0.0f;
    } else {
      bounds.vMin = 0.0f;
      bounds.vMax = 1.0f;
    }
    throw_overlay_error("SetOverlayTextureBounds",
                        vr::VROverlay()->SetOverlayTextureBounds(handle_, &bounds));
  }

  void update_transform() {
    const vr::HmdMatrix34_t transform = make_transform(cfg_);
    if (cfg_.follow_hmd) {
      throw_overlay_error("SetOverlayTransformTrackedDeviceRelative",
                          vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
                              handle_, vr::k_unTrackedDeviceIndex_Hmd, &transform));
    } else {
      throw_overlay_error("SetOverlayTransformAbsolute",
                          vr::VROverlay()->SetOverlayTransformAbsolute(
                              handle_, vr::TrackingUniverseStanding, &transform));
    }
  }

  void shutdown() {
    if (handle_ != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) {
      vr::VROverlay()->HideOverlay(handle_);
      // Keep overlay registration in SteamVR so settings/debug tools can preserve it.
      handle_ = vr::k_ulOverlayHandleInvalid;
    }
    vr::VR_Shutdown();
  }

  Config cfg_;
  vr::VROverlayHandle_t handle_ = vr::k_ulOverlayHandleInvalid;
  bool visible_ = false;
};

uint8_t pixel_component(const xr_video::StereoVideoFrame& frame,
                        const std::vector<uint8_t>& src,
                        uint32_t x,
                        uint32_t y,
                        uint32_t channel,
                        bool left) {
  const auto fmt = static_cast<xr_video::StereoVideoPixelFormat>(frame.header.pixel_format);
  const uint32_t bpp = xr_video::bytes_per_pixel(fmt);
  const uint32_t width = frame.header.width;
  const uint32_t stride = left ? frame.header.stride_left : frame.header.stride_right;
  const uint32_t effective_stride = stride != 0 ? stride : width * bpp;
  const size_t offset = static_cast<size_t>(y) * effective_stride + static_cast<size_t>(x) * bpp;
  if (offset + bpp > src.size()) return 0;

  switch (fmt) {
    case xr_video::StereoVideoPixelFormat::Gray8:
      return src[offset];
    case xr_video::StereoVideoPixelFormat::Rgb8:
      return src[offset + channel];
    case xr_video::StereoVideoPixelFormat::Bgr8:
      return src[offset + (2u - channel)];
    default:
      return 0;
  }
}

void write_rgba_pixel(std::vector<uint8_t>& dst,
                      uint32_t dst_width,
                      uint32_t x,
                      uint32_t y,
                      uint8_t r,
                      uint8_t g,
                      uint8_t b) {
  const size_t off = (static_cast<size_t>(y) * dst_width + x) * 4u;
  dst[off + 0] = r;
  dst[off + 1] = g;
  dst[off + 2] = b;
  dst[off + 3] = 255;
}

std::vector<uint8_t> make_upload_rgba(const xr_video::StereoVideoFrame& frame,
                                      VideoMode mode,
                                      uint32_t scale_divisor,
                                      uint32_t& out_width,
                                      uint32_t& out_height) {
  xr_video::validate_frame(frame);

  const uint32_t src_width = frame.header.width;
  const uint32_t src_height = frame.header.height;
  const uint32_t scale = scale_divisor == 0 ? 1u : scale_divisor;
  const uint32_t eye_width = std::max<uint32_t>(1u, src_width / scale);
  const uint32_t eye_height = std::max<uint32_t>(1u, src_height / scale);

  out_width = (mode == VideoMode::Sbs) ? eye_width * 2u : eye_width;
  out_height = eye_height;

  std::vector<uint8_t> rgba(static_cast<size_t>(out_width) * out_height * 4u);

  auto copy_eye = [&](bool left, uint32_t dst_x_base) {
    for (uint32_t y = 0; y < eye_height; ++y) {
      const uint32_t sy = std::min<uint32_t>(src_height - 1u, y * scale);
      for (uint32_t x = 0; x < eye_width; ++x) {
        const uint32_t sx = std::min<uint32_t>(src_width - 1u, x * scale);
        const auto& src = left ? frame.left : frame.right;
        const uint8_t r = pixel_component(frame, src, sx, sy, 0, left);
        const uint8_t g = pixel_component(frame, src, sx, sy, 1, left);
        const uint8_t b = pixel_component(frame, src, sx, sy, 2, left);
        write_rgba_pixel(rgba, out_width, dst_x_base + x, y, r, g, b);
      }
    }
  };

  switch (mode) {
    case VideoMode::Sbs:
      copy_eye(true, 0);
      copy_eye(false, eye_width);
      break;
    case VideoMode::Left:
      copy_eye(true, 0);
      break;
    case VideoMode::Right:
      copy_eye(false, 0);
      break;
  }

  return rgba;
}

std::unique_ptr<xr_video::StereoVideoShmReader> try_open_reader(const Config& cfg) {
  const auto info = xr_video::stereo_video_stream_from_registry(cfg.video_registry, cfg.video_stream);
  auto reader = std::make_unique<xr_video::StereoVideoShmReader>(info);
  std::cerr << "[steamvr_video_overlay] Attached video SHM stream=" << cfg.video_stream
            << " shm=" << info.shm_name
            << " size=" << info.width << "x" << info.height
            << " slots=" << info.slot_count << "\n";
  return reader;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  try {
    const Config cfg = parse_args(argc, argv);

    std::cerr << "[steamvr_video_overlay] Starting\n"
              << "  video_registry=" << cfg.video_registry << "\n"
              << "  video_stream=" << cfg.video_stream << "\n"
              << "  overlay_key=" << cfg.overlay_key << "\n";

    std::unique_ptr<OpenVrOverlaySession> overlay;
    std::unique_ptr<GlTextureUploader> gl_uploader;
    if (cfg.upload_backend == UploadBackend::OpenGl) {
      gl_uploader = std::make_unique<GlTextureUploader>();
    }
    std::unique_ptr<xr_video::StereoVideoShmReader> reader;
    uint64_t last_sequence = 0;
    uint64_t uploaded = 0;
    uint64_t skipped_same_sequence = 0;
    bool uploaded_first_frame = false;
    auto last_print = std::chrono::steady_clock::now();
    const auto frame_period = std::chrono::duration<double>(1.0 / static_cast<double>(cfg.target_fps));

    while (!g_stop.load()) {
      if (!overlay) {
        try {
          overlay = std::make_unique<OpenVrOverlaySession>(cfg);
        } catch (const std::exception& e) {
          std::cerr << "[steamvr_video_overlay][WAIT] SteamVR overlay attach failed: " << e.what() << "\n";
          std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reattach_sleep_ms));
          continue;
        }
      }

      if (!reader) {
        try {
          reader = try_open_reader(cfg);
          last_sequence = 0;
        } catch (const std::exception& e) {
          std::cerr << "[steamvr_video_overlay][WAIT] video SHM attach failed: " << e.what() << "\n";
          std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reattach_sleep_ms));
          continue;
        }
      }

      const auto loop_start = std::chrono::steady_clock::now();
      if (cfg.upload_once && uploaded_first_frame) {
        std::this_thread::sleep_until(loop_start + frame_period);
        continue;
      }
      try {
        const auto frame = reader->latest();
        if (!frame) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }
        if (frame->header.sequence == last_sequence) {
          ++skipped_same_sequence;
          std::this_thread::sleep_until(loop_start + frame_period);
          continue;
        }

        uint32_t upload_width = 0;
        uint32_t upload_height = 0;
        const std::vector<uint8_t> rgba = make_upload_rgba(
            *frame, cfg.video_mode, cfg.scale_divisor, upload_width, upload_height);
        overlay->upload_rgba(rgba, upload_width, upload_height, gl_uploader.get());

        last_sequence = frame->header.sequence;
        ++uploaded;
        uploaded_first_frame = true;

        if (cfg.print_every != 0 && uploaded % cfg.print_every == 0) {
          const auto now = std::chrono::steady_clock::now();
          const double dt = std::chrono::duration<double>(now - last_print).count();
          last_print = now;
          const uint64_t age_ns = xr_video::monotonic_now_ns() > frame->header.source_timestamp_ns
                                      ? xr_video::monotonic_now_ns() - frame->header.source_timestamp_ns
                                      : 0;
          std::cerr << std::fixed << std::setprecision(2)
                    << "[steamvr_video_overlay] uploaded=" << uploaded
                    << " seq=" << frame->header.sequence
                    << " src=" << frame->header.width << "x" << frame->header.height
                    << " upload=" << upload_width << "x" << upload_height
                    << " mode=" << video_mode_name(cfg.video_mode)
                    << " backend=" << upload_backend_name(cfg.upload_backend)
                    << " flip_v=" << (cfg.flip_v ? "yes" : "no")
                    << " age_ms=" << xr_video::ns_to_ms(static_cast<int64_t>(age_ns))
                    << " skipped_same=" << skipped_same_sequence
                    << " interval_s=" << dt << "\n";
          skipped_same_sequence = 0;
        }
      } catch (const std::exception& e) {
        std::cerr << "[steamvr_video_overlay][WARN] loop failed, reattaching: " << e.what() << "\n";
        reader.reset();
        overlay.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reattach_sleep_ms));
        continue;
      }

      std::this_thread::sleep_until(loop_start + frame_period);
    }

    std::cerr << "[steamvr_video_overlay] Stopping\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[steamvr_video_overlay][ERROR] " << e.what() << "\n";
    return 1;
  }
}
