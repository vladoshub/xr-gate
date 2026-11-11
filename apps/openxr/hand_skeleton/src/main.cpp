// SPDX-License-Identifier: MIT
// Standalone OpenXR hand diagnostics. No XR Gate SHM or controller poses are
// read here.
#define GLEW_NO_GLU
#include <GL/glew.h>
#include <GL/glx.h>
#include <SDL.h>
#include <SDL_syswm.h>
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t interrupted = 0;
static void signalHandler(int) { interrupted = 1; }
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void sdlCheck(bool ok, const char *call) {
  if (!ok)
    throw std::runtime_error(std::string(call) + ": " + SDL_GetError());
}
static XrInstance resultInstance = XR_NULL_HANDLE;
static void check(XrResult r, const char *call) {
  if (XR_FAILED(r)) {
    char message[XR_MAX_RESULT_STRING_SIZE]{};
    if (resultInstance)
      xrResultToString(resultInstance, r, message);
    throw std::runtime_error(std::string(call) + ": " +
                             (message[0] ? message : std::to_string(r)));
  }
}
#define XR(call) check((call), #call)
struct V {
  float x, y, z;
};
static V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static V cross(V a, V b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static float length(V a) {
  return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
static V normalized(V a) {
  float l = length(a);
  return l > 1e-7f ? a * (1.f / l) : V{0, 0, 0};
}
static bool finite(V a) {
  return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
static V vec(XrVector3f a) { return {a.x, a.y, a.z}; }
static V rotate(XrQuaternionf q, V v) {
  V u{q.x, q.y, q.z};
  return v + cross(u, v) * (2 * q.w) + cross(u, cross(u, v)) * 2;
}
struct M {
  float v[16]{};
};
static M identity() {
  M m;
  for (int i = 0; i < 4; i++)
    m.v[i * 5] = 1;
  return m;
}
static M operator*(const M &a, const M &b) {
  M c;
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++)
      for (int k = 0; k < 4; k++)
        c.v[j * 4 + i] += a.v[k * 4 + i] * b.v[j * 4 + k];
  return c;
}
static M viewMatrix(XrPosef pose) {
  auto q = pose.orientation;
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  M m = identity();
  V cols[] = {rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, 1})};
  for (int i = 0; i < 3; i++) {
    m.v[i * 4] = cols[i].x;
    m.v[i * 4 + 1] = cols[i].y;
    m.v[i * 4 + 2] = cols[i].z;
  }
  V t = rotate(q, vec(pose.position) * -1);
  m.v[12] = t.x;
  m.v[13] = t.y;
  m.v[14] = t.z;
  return m;
}
static M projection(XrFovf f) {
  const float n = .025f, z = 30.f, l = std::tan(f.angleLeft),
              r = std::tan(f.angleRight), b = std::tan(f.angleDown),
              t = std::tan(f.angleUp);
  M m;
  m.v[0] = 2 / (r - l);
  m.v[5] = 2 / (t - b);
  m.v[8] = (r + l) / (r - l);
  m.v[9] = (t + b) / (t - b);
  m.v[10] = -(z + n) / (z - n);
  m.v[11] = -1;
  m.v[14] = -2 * z * n / (z - n);
  return m;
}
struct Color {
  float r, g, b;
};
struct Vertex {
  V p;
  Color c;
};
using Mesh = std::vector<Vertex>;
static constexpr Color leftColor{.12f, .85f, 1.f}, rightColor{1.f, .45f, .12f},
    estimatedColor{1.f, .85f, .12f};
static void triangle(Mesh &m, V a, V b, V c, Color color) {
  m.push_back({a, color});
  m.push_back({b, color});
  m.push_back({c, color});
}
static void rod(Mesh &m, V a, V b, float radius, Color color) {
  V d = b - a;
  if (length(d) < 1e-6f)
    return;
  V u = normalized(cross(d, std::abs(normalized(d).y) > .9f ? V{1, 0, 0}
                                                            : V{0, 1, 0})) *
        radius;
  V v = normalized(cross(d, u)) * radius;
  constexpr float tau = 6.28318530718f;
  for (int i = 0; i < 8; i++) {
    float t0 = tau * i / 8, t1 = tau * (i + 1) / 8;
    V o0 = u * std::cos(t0) + v * std::sin(t0),
      o1 = u * std::cos(t1) + v * std::sin(t1);
    triangle(m, a + o0, b + o0, b + o1, color);
    triangle(m, a + o0, b + o1, a + o1, color);
  }
}
static void ball(Mesh &m, V center, float radius, Color color) {
  constexpr float pi = 3.14159265359f;
  auto point = [&](int y, int x) {
    float a = pi * y / 6, b = 2 * pi * x / 10;
    return center + V{std::sin(a) * std::cos(b), std::cos(a),
                      std::sin(a) * std::sin(b)} *
                        radius;
  };
  for (int y = 0; y < 6; y++)
    for (int x = 0; x < 10; x++) {
      triangle(m, point(y, x), point(y + 1, x), point(y + 1, x + 1), color);
      triangle(m, point(y, x), point(y + 1, x + 1), point(y, x + 1), color);
    }
}
static constexpr int parents[XR_HAND_JOINT_COUNT_EXT] = {
    1,  -1, 1,  2, 3,  4,  1,  6,  7, 8,  9,  1,  11,
    12, 13, 14, 1, 16, 17, 18, 19, 1, 21, 22, 23, 24};
struct Hand {
  std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
  bool active = false;
  int valid = 0, tracked = 0, oriented = 0;
};
static bool validPosition(const XrHandJointLocationEXT &j) {
  return (j.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
         finite(vec(j.pose.position));
}
static bool validOrientation(const XrHandJointLocationEXT &j) {
  if (!(j.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    return false;
  auto q = j.pose.orientation;
  float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return std::isfinite(n) && std::abs(n - 1) < .05f;
}
static void countHand(Hand &h) {
  h.valid = h.tracked = h.oriented = 0;
  if (!h.active)
    return;
  for (auto &j : h.joints) {
    if (validPosition(j)) {
      h.valid++;
      if (j.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
        h.tracked++;
    }
    if (validOrientation(j))
      h.oriented++;
  }
}
static bool sourceJoint(int i) {
  return i != 0 && i != 6 && i != 11 && i != 16 && i != 21;
}
static Hand displayedHand(Hand h, bool source21) {
  if (source21)
    for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++)
      if (!sourceJoint(i))
        h.joints[i].locationFlags = 0;
  countHand(h);
  return h;
}
static void skeleton(Mesh &mesh, const Hand &h, Color color, bool axes,
                     bool radii, bool source21) {
  if (!h.active)
    return;
  for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++) {
    const auto &j = h.joints[i];
    if ((source21 && !sourceJoint(i)) || !validPosition(j))
      continue;
    V p = vec(j.pose.position);
    Color c = (j.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
                  ? color
                  : estimatedColor;
    float radius = radii && std::isfinite(j.radius) && j.radius > 0
                       ? std::clamp(j.radius, .001f, .03f)
                       : .004f;
    ball(mesh, p, radius, c);
    int parent = parents[i];
    while (source21 && parent >= 0 && !sourceJoint(parent))
      parent = parents[parent];
    if (parent >= 0 && validPosition(h.joints[parent]))
      rod(mesh, p, vec(h.joints[parent].pose.position), .002f, color);
    if (axes && validOrientation(j)) {
      rod(mesh, p, p + rotate(j.pose.orientation, {.025f, 0, 0}), .0007f,
          {1, .08f, .08f});
      rod(mesh, p, p + rotate(j.pose.orientation, {0, .025f, 0}), .0007f,
          {.08f, 1, .08f});
      rod(mesh, p, p + rotate(j.pose.orientation, {0, 0, .025f}), .0007f,
          {.15f, .3f, 1});
    }
  }
}
// Minimal built-in 5x7 font: the diagnostic panel has no external assets.
static std::array<uint8_t, 7> glyph(char c) {
  switch (c) {
#define F(ch, a, b, c, d, e, f, g)                                             \
  case ch:                                                                     \
    return {a, b, c, d, e, f, g}
    F('A', 14, 17, 17, 31, 17, 17, 17);
    F('C', 14, 17, 16, 16, 16, 17, 14);
    F('D', 30, 17, 17, 17, 17, 17, 30);
    F('E', 31, 16, 16, 30, 16, 16, 31);
    F('F', 31, 16, 16, 30, 16, 16, 16);
    F('G', 14, 17, 16, 23, 17, 17, 14);
    F('H', 17, 17, 17, 31, 17, 17, 17);
    F('I', 14, 4, 4, 4, 4, 4, 14);
    F('J', 7, 2, 2, 2, 18, 18, 12);
    F('K', 17, 18, 20, 24, 20, 18, 17);
    F('L', 16, 16, 16, 16, 16, 16, 31);
    F('M', 17, 27, 21, 21, 17, 17, 17);
    F('N', 17, 25, 21, 19, 17, 17, 17);
    F('O', 14, 17, 17, 17, 17, 17, 14);
    F('P', 30, 17, 17, 30, 16, 16, 16);
    F('R', 30, 17, 17, 30, 20, 18, 17);
    F('S', 15, 16, 16, 14, 1, 1, 30);
    F('T', 31, 4, 4, 4, 4, 4, 4);
    F('U', 17, 17, 17, 17, 17, 17, 14);
    F('V', 17, 17, 17, 17, 17, 10, 4);
    F('X', 17, 17, 10, 4, 10, 17, 17);
    F('Y', 17, 17, 10, 4, 4, 4, 4);
    F('0', 14, 17, 19, 21, 25, 17, 14);
    F('1', 4, 12, 4, 4, 4, 4, 14);
    F('2', 14, 17, 1, 2, 4, 8, 31);
    F('3', 30, 1, 1, 14, 1, 1, 30);
    F('4', 2, 6, 10, 18, 31, 2, 2);
    F('5', 31, 16, 16, 30, 1, 1, 30);
    F('6', 14, 16, 16, 30, 17, 17, 14);
    F('7', 31, 1, 2, 4, 8, 8, 8);
    F('8', 14, 17, 17, 14, 17, 17, 14);
    F('9', 14, 17, 17, 15, 1, 1, 14);
    F('/', 1, 1, 2, 4, 8, 16, 16);
    F('-', 0, 0, 0, 31, 0, 0, 0);
#undef F
  default:
    return {};
  }
}
static void textMesh(Mesh &m, const std::string &s, float x, float y,
                     float size, Color color) {
  for (char c : s) {
    auto rows = glyph(c);
    for (int r = 0; r < 7; r++)
      for (int col = 0; col < 5; col++)
        if (rows[r] & (1 << (4 - col))) {
          V a{x + col * size, y - r * size, -.7f}, b = a + V{size, 0, 0},
                                                   d = a + V{0, -size, 0},
                                                   e = d + V{size, 0, 0};
          triangle(m, a, d, b, color);
          triangle(m, b, d, e, color);
        }
    x += 6 * size;
  }
}
class Renderer {
  GLuint program = 0, vao = 0, vbo = 0;
  GLint mvp = -1;

public:
  void init() {
    const char *vs = "#version 330 core\nlayout(location=0) in vec3 "
                     "p;layout(location=1) in vec3 c;uniform mat4 mvp;out vec3 "
                     "color;void main(){color=c;gl_Position=mvp*vec4(p,1);}";
    const char *fs = "#version 330 core\nin vec3 color;out vec4 outColor;void "
                     "main(){outColor=vec4(color,1);}";
    auto shader = [](GLenum type, const char *src) {
      GLuint s = glCreateShader(type);
      glShaderSource(s, 1, &src, nullptr);
      glCompileShader(s);
      GLint ok = 0;
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error(log);
      }
      return s;
    };
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    require(ok, "Shader link failed");
    mvp = glGetUniformLocation(program, "mvp");
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, p)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, c)));
  }
  void draw(const Mesh &m, const M &transform) {
    if (m.empty())
      return;
    glUseProgram(program);
    glUniformMatrix4fv(mvp, 1, GL_FALSE, transform.v);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m.size() * sizeof(Vertex)), m.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m.size()));
  }
  void destroy() {
    if (vbo)
      glDeleteBuffers(1, &vbo);
    if (vao)
      glDeleteVertexArrays(1, &vao);
    if (program)
      glDeleteProgram(program);
  }
};
struct Eye {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  int width = 0, height = 0;
  std::vector<XrSwapchainImageOpenGLKHR> images;
  std::vector<GLuint> fbos, depth;
};
struct Options {
  bool demo = false, mirror = true;
  int frames = 0, joints = 21;
};
class App {
  Options options;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace space = XR_NULL_HANDLE, headSpace = XR_NULL_HANDLE;
  XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::array<XrHandTrackerEXT, 2> trackers{};
  PFN_xrCreateHandTrackerEXT createHand = nullptr;
  PFN_xrDestroyHandTrackerEXT destroyHand = nullptr;
  PFN_xrLocateHandJointsEXT locateHand = nullptr;
  std::array<Hand, 2> hands{};
  std::array<Eye, 2> eyes{};
  SDL_Window *window = nullptr;
  SDL_GLContext context = nullptr;
  Renderer renderer;
  bool running = false, quit = false, axes = false, radii = false, grid = true,
       source21 = true;
  uint64_t frameCount = 0;
  std::chrono::steady_clock::time_point lastStatus{};
  template <class T> void proc(const char *name, T &p) {
    XR(xrGetInstanceProcAddr(instance, name,
                             reinterpret_cast<PFN_xrVoidFunction *>(&p)));
  }
  void initXR() {
    uint32_t n = 0;
    XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XR(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()));
    const char *extensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
                                XR_EXT_HAND_TRACKING_EXTENSION_NAME};
    for (auto name : extensions)
      require(
          std::any_of(
              props.begin(), props.end(),
              [&](auto &p) { return std::strcmp(name, p.extensionName) == 0; }),
          std::string("Runtime lacks ") + name +
              ". For XR Gate restart Monado with XR_MONADO_HAND_TRACKING=1.");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ci.applicationInfo.applicationName,
                  XR_MAX_APPLICATION_NAME_SIZE, "XR Gate Hand Skeleton");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = extensions;
    XR(xrCreateInstance(&ci, &instance));
    resultInstance = instance;
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    XR(xrGetInstanceProperties(instance, &ip));
    std::cout << "Runtime: " << ip.runtimeName << '\n';
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR(xrGetSystem(instance, &si, &system));
    XrSystemHandTrackingPropertiesEXT hp{
        XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    sp.next = &hp;
    XR(xrGetSystemProperties(instance, system, &sp));
    require(hp.supportsHandTracking,
            "Hand tracking is disabled on this system. Restart XR Gate Monado "
            "with XR_MONADO_HAND_TRACKING=1.");
    PFN_xrGetOpenGLGraphicsRequirementsKHR reqFunc = nullptr;
    proc("xrGetOpenGLGraphicsRequirementsKHR", reqFunc);
    XrGraphicsRequirementsOpenGLKHR req{
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    XR(reqFunc(instance, system, &req));
    require(req.minApiVersionSupported <= XR_MAKE_VERSION(3, 3, 0) &&
                req.maxApiVersionSupported >= XR_MAKE_VERSION(3, 3, 0),
            "This example needs OpenGL 3.3 support in the runtime.");
    proc("xrCreateHandTrackerEXT", createHand);
    proc("xrDestroyHandTrackerEXT", destroyHand);
    proc("xrLocateHandJointsEXT", locateHand);
  }
  void initGL() {
    // Explicit X11 because this example uses XrGraphicsBindingOpenGLXlibKHR.
    require(SDL_setenv("SDL_VIDEODRIVER", "x11", 1) == 0,
            "Cannot select X11 SDL backend");
    sdlCheck(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    window = SDL_CreateWindow(
        "XR Gate Hand Skeleton", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1000, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
            (options.mirror ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN));
    sdlCheck(window, "SDL_CreateWindow");
    context = SDL_GL_CreateContext(window);
    sdlCheck(context, "SDL_GL_CreateContext");
    SDL_GL_SetSwapInterval(0);
    glewExperimental = GL_TRUE;
    require(glewInit() == GLEW_OK, "GLEW initialization failed");
    while (glGetError() != GL_NO_ERROR) {
    }
    std::cout << "OpenGL: " << glGetString(GL_VERSION) << '\n';
    renderer.init();
  }
  void initSession() {
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    sdlCheck(SDL_GetWindowWMInfo(window, &wm) == SDL_TRUE,
             "SDL_GetWindowWMInfo");
    require(wm.subsystem == SDL_SYSWM_X11, "X11 window required");
    auto display = wm.info.x11.display;
    GLXContext glContext = glXGetCurrentContext();
    int configId = 0;
    require(glXQueryContext(display, glContext, GLX_FBCONFIG_ID, &configId) ==
                Success,
            "Cannot query GLX framebuffer config");
    int count = 0;
    int attrs[] = {GLX_FBCONFIG_ID, configId, None};
    GLXFBConfig *configs =
        glXChooseFBConfig(display, DefaultScreen(display), attrs, &count);
    require(configs && count > 0, "Cannot find GLX framebuffer config");
    GLXFBConfig config = configs[0];
    XFree(configs);
    XVisualInfo *visual = glXGetVisualFromFBConfig(display, config);
    require(visual, "Cannot find GLX visual");
    XrGraphicsBindingOpenGLXlibKHR binding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    binding.xDisplay = display;
    binding.visualid = static_cast<uint32_t>(visual->visualid);
    binding.glxFBConfig = config;
    binding.glxDrawable = glXGetCurrentDrawable();
    binding.glxContext = glContext;
    XFree(visual);
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = system;
    XR(xrCreateSession(instance, &ci, &session));
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    XR(xrCreateReferenceSpace(session, &rs, &space));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR(xrCreateReferenceSpace(session, &rs, &headSpace));
    uint32_t n = 0;
    XR(xrEnumerateEnvironmentBlendModes(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n,
        nullptr));
    std::vector<XrEnvironmentBlendMode> modes(n);
    XR(xrEnumerateEnvironmentBlendModes(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
        modes.data()));
    require(n > 0, "No blend modes");
    blend = std::find(modes.begin(), modes.end(),
                      XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != modes.end()
                ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE
                : modes[0];
    for (int i = 0; i < 2; i++) {
      XrHandTrackerCreateInfoEXT h{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
      h.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
      h.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
      XR(createHand(session, &h, &trackers[i]));
    }
    XR(xrEnumerateViewConfigurationViews(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n,
        nullptr));
    require(n == 2, "Stereo view configuration required");
    std::array<XrViewConfigurationView, 2> views{
        {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    XR(xrEnumerateViewConfigurationViews(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n,
        views.data()));
    uint32_t nf = 0;
    XR(xrEnumerateSwapchainFormats(session, 0, &nf, nullptr));
    std::vector<int64_t> formats(nf);
    XR(xrEnumerateSwapchainFormats(session, nf, &nf, formats.data()));
    int64_t format = 0;
    for (int64_t f : {GL_SRGB8_ALPHA8, GL_RGBA8})
      if (std::find(formats.begin(), formats.end(), f) != formats.end()) {
        format = f;
        break;
      }
    require(format != 0, "Runtime has no RGBA8/sRGB8 OpenGL swapchain format");
    if (format == GL_SRGB8_ALPHA8)
      glEnable(GL_FRAMEBUFFER_SRGB);
    for (int i = 0; i < 2; i++) {
      Eye &e = eyes[i];
      e.width = static_cast<int>(views[i].recommendedImageRectWidth);
      e.height = static_cast<int>(views[i].recommendedImageRectHeight);
      XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
      sc.format = format;
      sc.sampleCount = 1;
      sc.width = e.width;
      sc.height = e.height;
      sc.faceCount = sc.arraySize = sc.mipCount = 1;
      XR(xrCreateSwapchain(session, &sc, &e.swapchain));
      XR(xrEnumerateSwapchainImages(e.swapchain, 0, &n, nullptr));
      e.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
      XR(xrEnumerateSwapchainImages(
          e.swapchain, n, &n,
          reinterpret_cast<XrSwapchainImageBaseHeader *>(e.images.data())));
      e.fbos.resize(n);
      e.depth.resize(n);
      glGenFramebuffers(n, e.fbos.data());
      glGenRenderbuffers(n, e.depth.data());
      for (uint32_t j = 0; j < n; j++) {
        glBindFramebuffer(GL_FRAMEBUFFER, e.fbos[j]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, e.images[j].image, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, e.depth[j]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e.width,
                              e.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, e.depth[j]);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                    GL_FRAMEBUFFER_COMPLETE,
                "Incomplete eye framebuffer");
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  void events() {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        quit = true;
      if (event.type == SDL_KEYDOWN && !event.key.repeat)
        switch (event.key.keysym.sym) {
        case SDLK_ESCAPE:
          quit = true;
          break;
        case SDLK_a:
          axes = !axes;
          break;
        case SDLK_r:
          radii = !radii;
          break;
        case SDLK_g:
          grid = !grid;
          break;
        case SDLK_j:
          source21 = !source21;
          break;
        default:
          break;
        }
    }
    if (!instance)
      return;
    for (;;) {
      XrEventDataBuffer eventData{XR_TYPE_EVENT_DATA_BUFFER};
      XrResult r = xrPollEvent(instance, &eventData);
      if (r == XR_EVENT_UNAVAILABLE)
        break;
      check(r, "xrPollEvent");
      if (eventData.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        quit = true;
      if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto &e =
            *reinterpret_cast<XrEventDataSessionStateChanged *>(&eventData);
        std::cout << "Session state: " << e.state << '\n';
        if (e.state == XR_SESSION_STATE_READY && !running) {
          XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO};
          b.primaryViewConfigurationType =
              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          XR(xrBeginSession(session, &b));
          running = true;
        }
        if (e.state == XR_SESSION_STATE_STOPPING && running) {
          XR(xrEndSession(session));
          running = false;
        }
        if (e.state == XR_SESSION_STATE_EXITING ||
            e.state == XR_SESSION_STATE_LOSS_PENDING)
          quit = true;
      }
    }
  }
  void locate(XrTime time) {
    for (int i = 0; i < 2; i++) {
      Hand &h = hands[i];
      h = Hand{};
      XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
      li.baseSpace = space;
      li.time = time;
      XrHandJointLocationsEXT result{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
      result.jointCount = XR_HAND_JOINT_COUNT_EXT;
      result.jointLocations = h.joints.data();
      XR(locateHand(trackers[i], &li, &result));
      h.active = result.isActive;
      countHand(h);
    }
  }
  Mesh scene() {
    Mesh mesh;
    mesh.reserve(30000);
    if (grid)
      for (int i = -10; i <= 10; i++) {
        float d = i * .2f;
        Color c{.07f, .12f, .16f};
        rod(mesh, {d, -1.5f, -2}, {d, -1.5f, 2}, .001f, c);
        rod(mesh, {-2, -1.5f, d}, {2, -1.5f, d}, .001f, c);
      }
    skeleton(mesh, hands[0], leftColor, axes, radii, source21);
    skeleton(mesh, hands[1], rightColor, axes, radii, source21);
    return mesh;
  }
  Mesh panel() {
    Mesh m;
    float y = -.16f;
    if (options.demo) {
      textMesh(m, "DEMO - SYNTHETIC", -.19f, y, .002f, estimatedColor);
      y -= .025f;
    }
    for (int i = 0; i < 2; i++) {
      const auto h = displayedHand(hands[i], source21);
      std::string s = std::string(i == 0 ? "L " : "R ") +
                      (h.active ? "VALID " + std::to_string(h.valid) +
                                      (source21 ? "/21 TRACK " : "/26 TRACK ") +
                                      std::to_string(h.tracked)
                                : "LOST");
      textMesh(m, s, -.19f, y, .002f, i == 0 ? leftColor : rightColor);
      y -= .025f;
    }
    textMesh(m, source21 ? "J JOINTS 21" : "J JOINTS 26", -.19f, y, .0015f,
             {.7f, .75f, .8f});
    y -= .02f;
    textMesh(m,
             std::string("A AXES ") + (axes ? "ON" : "OFF") + "  R RADII " +
                 (radii ? "ON" : "OFF"),
             -.19f, y, .0015f, {.7f, .75f, .8f});
    return m;
  }
  void draw(GLuint fbo, int w, int h, const M &vp, const Mesh &mesh,
            const M &panelTransform, const Mesh &hud) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(.012f, .019f, .03f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.draw(mesh, vp);
    glDisable(GL_DEPTH_TEST);
    renderer.draw(hud, panelTransform);
    glEnable(GL_DEPTH_TEST);
  }
  void status() {
    auto now = std::chrono::steady_clock::now();
    if (now - lastStatus < std::chrono::seconds(1))
      return;
    lastStatus = now;
    std::string s = options.demo ? "XR Gate Hands [DEMO]" : "XR Gate Hands";
    s += source21 ? " [21 joints]" : " [26 joints]";
    for (int i = 0; i < 2; i++) {
      auto h = displayedHand(hands[i], source21);
      s += std::string(i == 0 ? " | L " : " | R ") +
           (h.active ? "valid=" + std::to_string(h.valid) +
                           " tracked=" + std::to_string(h.tracked) +
                           " orient=" + std::to_string(h.oriented)
                     : "LOST");
    }
    SDL_SetWindowTitle(window, s.c_str());
    std::cout << s << std::endl;
  }
  void xrFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR(xrWaitFrame(session, &wi, &fs));
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XR(xrBeginFrame(session, &bi));
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = fs.predictedDisplayTime;
    end.environmentBlendMode = blend;
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = space;
    XR(xrLocateViews(session, &li, &state, 2, &n, views.data()));
    const auto valid =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if (!fs.shouldRender || n != 2 || (state.viewStateFlags & valid) != valid) {
      XR(xrEndFrame(session, &end));
      return;
    }
    locate(fs.predictedDisplayTime);
    Mesh mesh = scene(), hud = panel();
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    XR(xrLocateSpace(headSpace, space, fs.predictedDisplayTime, &head));
    const auto headFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                           XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool headValid = (head.locationFlags & headFlags) == headFlags;
    // inverse(viewMatrix(head)) is the VIEW-space to LOCAL-space transform.
    M headToLocal = identity();
    if (headValid) {
      V cols[] = {rotate(head.pose.orientation, {1, 0, 0}),
                  rotate(head.pose.orientation, {0, 1, 0}),
                  rotate(head.pose.orientation, {0, 0, 1})};
      for (int c = 0; c < 3; c++) {
        headToLocal.v[c * 4] = cols[c].x;
        headToLocal.v[c * 4 + 1] = cols[c].y;
        headToLocal.v[c * 4 + 2] = cols[c].z;
      }
      headToLocal.v[12] = head.pose.position.x;
      headToLocal.v[13] = head.pose.position.y;
      headToLocal.v[14] = head.pose.position.z;
    } else
      hud.clear();
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{
        {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for (int i = 0; i < 2; i++) {
      auto &e = eyes[i];
      uint32_t index = 0;
      XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      XR(xrAcquireSwapchainImage(e.swapchain, &ai, &index));
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait.timeout = XR_INFINITE_DURATION;
      XR(xrWaitSwapchainImage(e.swapchain, &wait));
      M vp = projection(views[i].fov) * viewMatrix(views[i].pose);
      draw(e.fbos[index], e.width, e.height, vp, mesh, vp * headToLocal, hud);
      if (i == 0 && options.mirror) {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        if (w > 0 && h > 0) {
          glBindFramebuffer(GL_READ_FRAMEBUFFER, e.fbos[index]);
          glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
          glClear(GL_COLOR_BUFFER_BIT);
          float scale = std::min(float(w) / e.width, float(h) / e.height);
          int mw = int(e.width * scale), mh = int(e.height * scale),
              x = (w - mw) / 2, y = (h - mh) / 2;
          glBlitFramebuffer(0, 0, e.width, e.height, x, y, x + mw, y + mh,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR);
          SDL_GL_SwapWindow(window);
        }
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glFlush();
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      XR(xrReleaseSwapchainImage(e.swapchain, &release));
      auto &pv = projectionViews[i];
      pv.pose = views[i].pose;
      pv.fov = views[i].fov;
      pv.subImage.swapchain = e.swapchain;
      pv.subImage.imageRect.extent = {e.width, e.height};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space;
    layer.viewCount = 2;
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader *layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer)};
    end.layerCount = 1;
    end.layers = layers;
    XR(xrEndFrame(session, &end));
    status();
    frameCount++;
  }
  void demoFrame() {
    for (int side = 0; side < 2; side++) {
      Hand &h = hands[side];
      h = Hand{};
      h.active = true;
      float x = side == 0 ? -.16f : .16f;
      auto set = [&](int j, V p) {
        auto &a = h.joints[j];
        a.pose.position = {p.x, p.y, p.z};
        a.pose.orientation.w = 1;
        a.radius = .005f;
        a.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                          XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                          XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                          XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
      };
      set(1, {x, -.04f, -.55f});
      set(0, {x, .01f, -.55f});
      int starts[] = {2, 6, 11, 16, 21};
      for (int finger = 0; finger < 5; finger++) {
        int count = finger == 0 ? 4 : 5;
        for (int k = 0; k < count; k++) {
          float fx = (finger - 2) * .024f * (side == 0 ? 1 : -1),
                fy = .007f + k * .026f;
          if (finger == 0) {
            fx =
                (side == 0 ? -.045f : .045f) + (side == 0 ? -1 : 1) * k * .014f;
            fy = -.02f + k * .019f;
          }
          set(starts[finger] + k, {x + fx, fy, -.55f});
        }
      }
      for (int j : {0, 6, 11, 16, 21})
        h.joints[j].locationFlags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
      countHand(h);
    }
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w > 0 && h > 0) {
      float t = .7f, aspect = float(w) / h;
      M p = projection({std::atan(-t * aspect), std::atan(t * aspect),
                        std::atan(t), std::atan(-t)});
      draw(0, w, h, p, scene(), p, panel());
      SDL_GL_SwapWindow(window);
    }
    status();
    frameCount++;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

public:
  explicit App(Options o) : options(o), source21(o.joints == 21) {}
  ~App() {
    if (context) {
      SDL_GL_MakeCurrent(window, context);
      for (auto &e : eyes) {
        if (!e.fbos.empty())
          glDeleteFramebuffers(static_cast<GLsizei>(e.fbos.size()),
                               e.fbos.data());
        if (!e.depth.empty())
          glDeleteRenderbuffers(static_cast<GLsizei>(e.depth.size()),
                                e.depth.data());
      }
      renderer.destroy();
    }
    for (auto &e : eyes)
      if (e.swapchain)
        xrDestroySwapchain(e.swapchain);
    for (auto h : trackers)
      if (h && destroyHand)
        destroyHand(h);
    if (headSpace)
      xrDestroySpace(headSpace);
    if (space)
      xrDestroySpace(space);
    if (session)
      xrDestroySession(session);
    if (instance)
      xrDestroyInstance(instance);
    resultInstance = XR_NULL_HANDLE;
    if (context)
      SDL_GL_DeleteContext(context);
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
  }
  void run() {
    if (!options.demo)
      initXR();
    initGL();
    if (!options.demo)
      initSession();
    std::cout << "Left cyan; right orange; valid untracked joints yellow. J "
                 "21/26 joints, A axes, R radii, G grid, Esc exit.\n";
    while (!quit && !interrupted) {
      events();
      if (quit)
        break;
      if (options.demo)
        demoFrame();
      else if (running)
        xrFrame();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (options.frames > 0 &&
          frameCount >= static_cast<uint64_t>(options.frames))
        quit = true;
    }
  }
};
int main(int argc, char **argv) {
  try {
    Options options;
    for (int i = 1; i < argc; i++) {
      std::string s = argv[i];
      if (s == "--demo")
        options.demo = true;
      else if (s == "--no-mirror")
        options.mirror = false;
      else if (s == "--joints" && i + 1 < argc) {
        std::string value = argv[++i];
        require(value == "21" || value == "26", "--joints must be 21 or 26");
        options.joints = value == "21" ? 21 : 26;
      } else if (s == "--frames" && i + 1 < argc) {
        std::string value = argv[++i];
        size_t parsed = 0;
        options.frames = std::stoi(value, &parsed);
        require(parsed == value.size() && options.frames > 0,
                "--frames must be a positive integer");
      } else if (s == "--help") {
        std::cout << "XR Gate Hand Skeleton (Linux X11/OpenGL "
                     "3.3)\n--joints 21|26  Select displayed skeleton (default "
                     "21)\n--no-mirror  Hide desktop mirror\n--demo       "
                     "Synthetic rendering only, no OpenXR runtime\n--frames N  "
                     " Exit after N rendered frames\nJ: 21/26 joints, A: joint "
                     "axes (RGB=XYZ), R: runtime radii, G: grid, Esc: exit\n";
        return 0;
      } else
        throw std::runtime_error("Unknown or incomplete option: " + s);
    }
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    App app(options);
    app.run();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "[hands] " << e.what()
              << "\nCheck XR_RUNTIME_JSON, the running Monado service, "
                 "XR_MONADO_HAND_TRACKING=1, and DISPLAY/X11.\n";
    return 1;
  }
}
