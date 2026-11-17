// SPDX-License-Identifier: MIT
// OpenXR diagnostics for hand joints and FRAME body tracker devices.
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
#include <string_view>
#include <thread>
#include <vector>

#include "monado_xdev_space.hpp"

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
static constexpr float previewDistance = 1.45f;
static constexpr XrExtent2Df previewSize{1.20f, .675f};

static M previewProjection() {
  const float horizontal = std::atan(previewSize.width / (2 * previewDistance));
  const float vertical = std::atan(previewSize.height / (2 * previewDistance));
  return projection({-horizontal, horizontal, vertical, -vertical});
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

enum BodyJoint {
  Neck, LeftShoulder, LeftElbow, LeftWrist, RightShoulder, RightElbow, RightWrist,
  LeftHip, LeftKnee, LeftFoot, LeftToe, RightHip, RightKnee, RightFoot, RightToe,
  Waist, BodyJointCount,
};

struct BodyPart {
  std::string_view id;
  int parent;
  Color color;
};

static constexpr Color torsoColor{.86f, .91f, .96f};
static constexpr std::array<BodyPart, BodyJointCount> bodyParts{{
    {"frame_neck", Waist, torsoColor},
    {"frame_left_shoulder", Neck, leftColor},
    {"frame_left_elbow", LeftShoulder, leftColor},
    {"frame_left_wrist", LeftElbow, leftColor},
    {"frame_right_shoulder", Neck, rightColor},
    {"frame_right_elbow", RightShoulder, rightColor},
    {"frame_right_wrist", RightElbow, rightColor},
    {"frame_left_hip", Waist, leftColor},
    {"frame_left_knee", LeftHip, leftColor},
    {"frame_left_foot", LeftKnee, leftColor},
    {"frame_left_toe", LeftFoot, leftColor},
    {"frame_right_hip", Waist, rightColor},
    {"frame_right_knee", RightHip, rightColor},
    {"frame_right_foot", RightKnee, rightColor},
    {"frame_right_toe", RightFoot, rightColor},
    {"frame_waist", -1, torsoColor},
}};

using Body = std::array<XrSpaceLocation, BodyJointCount>;

static bool validPosition(const XrSpaceLocation &joint) {
  return (joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
         finite(vec(joint.pose.position));
}

static bool trackedPosition(const XrSpaceLocation &joint) {
  return validPosition(joint) &&
         (joint.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
}

static Body previewBody(Body body) {
  const auto first = std::find_if(body.begin(), body.end(),
                                  [](const auto &joint) { return validPosition(joint); });
  if (first == body.end())
    return body;

  V low = vec(first->pose.position), high = low;
  for (auto joint = first + 1; joint != body.end(); ++joint) {
    if (!validPosition(*joint))
      continue;
    const V p = vec(joint->pose.position);
    low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
    high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
  }

  const V center = low * .5f + high * .5f;
  const V halfExtent = high * .5f - low * .5f;
  constexpr V previewCenter{0, -.025f, -previewDistance};
  const float maxExtent = std::max({halfExtent.x, halfExtent.y, halfExtent.z, .001f});
  const float scale = std::min(.32f, .23f / maxExtent);
  for (auto &joint : body) {
    if (!validPosition(joint))
      continue;
    const V p = (vec(joint.pose.position) - center) * scale + previewCenter;
    joint.pose.position = {p.x, p.y, p.z};
  }
  return body;
}

static void bodySkeleton(Mesh &mesh, const Body &body) {
  for (size_t i = 0; i < body.size(); i++) {
    const auto &joint = body[i];
    if (!validPosition(joint))
      continue;
    const auto &part = bodyParts[i];
    const bool tracked = trackedPosition(joint);
    const V p = vec(joint.pose.position);
    ball(mesh, p, .016f, tracked ? part.color : estimatedColor);
    if (part.parent >= 0 && validPosition(body[part.parent])) {
      const auto &parent = body[part.parent];
      rod(mesh, p, vec(parent.pose.position), .008f,
          tracked && trackedPosition(parent) ? part.color : estimatedColor);
    }
  }
}

static Body demoBody() {
  static constexpr std::array<V, BodyJointCount> positions{{
      {0, .65f, -1.6f},
      {-.22f, .51f, -1.6f}, {-.38f, .25f, -1.58f}, {-.40f, .02f, -1.44f},
      {.22f, .51f, -1.6f}, {.38f, .25f, -1.58f}, {.42f, .02f, -1.44f},
      {-.13f, .02f, -1.6f}, {-.17f, -.38f, -1.50f},
      {-.19f, -.80f, -1.56f}, {-.19f, -.82f, -1.70f},
      {.13f, .02f, -1.6f}, {.17f, -.38f, -1.50f},
      {.19f, -.80f, -1.56f}, {.19f, -.82f, -1.70f},
      {0, .02f, -1.6f},
  }};
  Body body{};
  for (size_t i = 0; i < body.size(); i++) {
    body[i].type = XR_TYPE_SPACE_LOCATION;
    body[i].locationFlags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    const auto p = positions[i];
    body[i].pose.position = {p.x, p.y, p.z};
  }
  return body;
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
  bool body = false, demo = false, mirror = true;
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
  XrXDevListMNDX bodyDevices = XR_NULL_HANDLE;
  PFN_xrDestroyXDevListMNDX destroyBodyDevices = nullptr;
  std::array<XrSpace, BodyJointCount> bodySpaces{};
  Body body{};
  std::array<Eye, 2> eyes{};
  SDL_Window *window = nullptr;
  SDL_GLContext context = nullptr;
  Renderer renderer;
  bool running = false, quit = false, axes = false, radii = false, grid = true,
       source21 = true;
  bool bodyPreview;
  uint64_t frameCount = 0;
  std::chrono::steady_clock::time_point lastStatus{};
  template <class T> void proc(const char *name, T &p) {
    PFN_xrVoidFunction function = nullptr;
    XR(xrGetInstanceProcAddr(instance, name, &function));
    require(function, std::string("Missing OpenXR function ") + name);
    p = reinterpret_cast<T>(function);
  }
  void initXR() {
    uint32_t n = 0;
    XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XR(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()));
    const char *extensions[] = {
        XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
        options.body ? XR_MNDX_XDEV_SPACE_EXTENSION_NAME
                     : XR_EXT_HAND_TRACKING_EXTENSION_NAME};
    for (auto name : extensions)
      require(
          std::any_of(
              props.begin(), props.end(),
              [&](auto &p) { return std::strcmp(name, p.extensionName) == 0; }),
          std::string("Runtime lacks ") + name);
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ci.applicationInfo.applicationName,
                  XR_MAX_APPLICATION_NAME_SIZE, "XR Gate Skeleton");
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
    XrSystemXDevSpacePropertiesMNDX bp{XR_TYPE_SYSTEM_XDEV_SPACE_PROPERTIES_MNDX};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    sp.next = options.body ? static_cast<void *>(&bp) : static_cast<void *>(&hp);
    XR(xrGetSystemProperties(instance, system, &sp));
    if (options.body)
      require(bp.supportsXDevSpace, "This OpenXR system lacks Monado device spaces");
    else
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
    if (!options.body) {
      proc("xrCreateHandTrackerEXT", createHand);
      proc("xrDestroyHandTrackerEXT", destroyHand);
      proc("xrLocateHandJointsEXT", locateHand);
    }
  }
  void initBodySpaces() {
    PFN_xrCreateXDevListMNDX createList = nullptr;
    PFN_xrEnumerateXDevsMNDX enumerateDevices = nullptr;
    PFN_xrGetXDevPropertiesMNDX getProperties = nullptr;
    PFN_xrCreateXDevSpaceMNDX createSpace = nullptr;
    proc("xrCreateXDevListMNDX", createList);
    proc("xrDestroyXDevListMNDX", destroyBodyDevices);
    proc("xrEnumerateXDevsMNDX", enumerateDevices);
    proc("xrGetXDevPropertiesMNDX", getProperties);
    proc("xrCreateXDevSpaceMNDX", createSpace);

    const XrCreateXDevListInfoMNDX info{XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX};
    XR(createList(session, &info, &bodyDevices));
    uint32_t count = 0;
    XR(enumerateDevices(bodyDevices, 0, &count, nullptr));
    std::vector<XrXDevIdMNDX> ids(count);
    XR(enumerateDevices(bodyDevices, count, &count, ids.data()));
    constexpr std::string_view prefix = "xr-gate-body/id/";
    size_t registered = 0;
    for (uint32_t i = 0; i < count; i++) {
      XrGetXDevInfoMNDX query{XR_TYPE_GET_XDEV_INFO_MNDX};
      query.id = ids[i];
      XrXDevPropertiesMNDX properties{XR_TYPE_XDEV_PROPERTIES_MNDX};
      XR(getProperties(bodyDevices, &query, &properties));
      const std::string_view serial(properties.serial,
                                    strnlen(properties.serial, sizeof(properties.serial)));
      if (!properties.canCreateSpace || serial.substr(0, prefix.size()) != prefix)
        continue;
      const auto id = serial.substr(prefix.size());
      const auto part = std::find_if(bodyParts.begin(), bodyParts.end(),
                                     [&](const BodyPart &p) { return p.id == id; });
      if (part == bodyParts.end())
        continue;
      auto &jointSpace = bodySpaces[part - bodyParts.begin()];
      if (jointSpace)
        continue;
      XrCreateXDevSpaceInfoMNDX create{XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX};
      create.xdevList = bodyDevices;
      create.id = ids[i];
      create.offset.orientation.w = 1.f;
      XR(createSpace(session, &create, &jointSpace));
      registered++;
    }
    require(registered > 0, "No FRAME joints registered by Monado");
    std::cout << "FRAME joints registered by Monado: " << registered << "/"
              << bodySpaces.size() << '\n';
  }
  void initGL() {
    // Live OpenXR uses a GLX graphics binding. The demo also supports SDL's
    // offscreen driver for a software-rendering smoke test.
    if (!options.demo)
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
        options.body ? "XR Gate Body Skeleton" : "XR Gate Hand Skeleton",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1000, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
            (options.mirror ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN));
    sdlCheck(window, "SDL_CreateWindow");
    context = SDL_GL_CreateContext(window);
    sdlCheck(context, "SDL_GL_CreateContext");
    SDL_GL_SetSwapInterval(0);
    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    // GLEW loads the core GL functions before querying the GLX display.
    // An SDL offscreen EGL context can use the core functions without GLX.
    const bool offscreenDemo = options.demo &&
        std::strcmp(SDL_GetCurrentVideoDriver(), "offscreen") == 0;
    require(glewStatus == GLEW_OK ||
                (offscreenDemo && glewStatus == GLEW_ERROR_NO_GLX_DISPLAY &&
                 GLEW_VERSION_3_3),
            "GLEW error: " +
                std::string(reinterpret_cast<const char *>(glewGetErrorString(glewStatus))));
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
    if (options.body) {
      initBodySpaces();
    } else {
      for (int i = 0; i < 2; i++) {
        XrHandTrackerCreateInfoEXT h{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        h.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        h.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XR(createHand(session, &h, &trackers[i]));
      }
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
        case SDLK_v:
          if (options.body)
            bodyPreview = !bodyPreview;
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
    if (options.body) {
      for (size_t i = 0; i < bodySpaces.size(); i++) {
        body[i] = {XR_TYPE_SPACE_LOCATION};
        if (bodySpaces[i])
          XR(xrLocateSpace(bodySpaces[i], space, time, &body[i]));
      }
      return;
    }
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
    if (grid && !bodyPreview)
      for (int i = -10; i <= 10; i++) {
        float d = i * .2f;
        Color c{.07f, .12f, .16f};
        rod(mesh, {d, -1.5f, -2}, {d, -1.5f, 2}, .001f, c);
        rod(mesh, {-2, -1.5f, d}, {2, -1.5f, d}, .001f, c);
      }
    if (options.body)
      bodySkeleton(mesh, bodyPreview ? previewBody(body) : body);
    else {
      skeleton(mesh, hands[0], leftColor, axes, radii, source21);
      skeleton(mesh, hands[1], rightColor, axes, radii, source21);
    }
    return mesh;
  }
  std::string bodyStatus() const {
    const auto valid = std::count_if(body.begin(), body.end(),
                                     [](const auto &joint) { return validPosition(joint); });
    if (!valid)
      return "FRAME LOST";
    const auto tracked = std::count_if(body.begin(), body.end(), trackedPosition);
    return "FRAME VALID " + std::to_string(valid) + "/" +
           std::to_string(body.size()) + " TRACK " + std::to_string(tracked);
  }
  Mesh panel() {
    Mesh m;
    float y = options.body ? .155f : -.16f;
    if (options.demo) {
      textMesh(m, "DEMO - SYNTHETIC", -.19f, y, .002f, estimatedColor);
      y -= .025f;
    }
    if (options.body) {
      const std::string label = bodyStatus();
      if (bodyPreview) {
        constexpr float size = .0028f;
        const bool lost = label == "FRAME LOST";
        textMesh(m, label, -3 * size * label.size(), lost ? .010f : y, size,
                 lost ? estimatedColor : torsoColor);
      } else {
        textMesh(m, label, -.19f, y, .0018f, torsoColor);
      }
      return m;
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
    if (options.body) {
      auto s = std::string(options.demo ? "XR Gate Body [DEMO] | " : "XR Gate Body | ") +
               (bodyPreview ? "Preview | " : "World | ") +
               bodyStatus();
      SDL_SetWindowTitle(window, s.c_str());
      std::cout << s << std::endl;
      return;
    }
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
  XrSwapchainSubImage renderEye(Eye &eye, bool mirror, const M &vp,
                                const Mesh &mesh, const M &hudTransform,
                                const Mesh &hud) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR(xrAcquireSwapchainImage(eye.swapchain, &acquire, &index));
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    XR(xrWaitSwapchainImage(eye.swapchain, &wait));
    draw(eye.fbos[index], eye.width, eye.height, vp, mesh, hudTransform, hud);
    if (mirror) {
      int w = 0, h = 0;
      SDL_GL_GetDrawableSize(window, &w, &h);
      if (w > 0 && h > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, eye.fbos[index]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        float scale = std::min(float(w) / eye.width, float(h) / eye.height);
        int mw = int(eye.width * scale), mh = int(eye.height * scale),
            x = (w - mw) / 2, y = (h - mh) / 2;
        glBlitFramebuffer(0, 0, eye.width, eye.height, x, y, x + mw, y + mh,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        SDL_GL_SwapWindow(window);
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR(xrReleaseSwapchainImage(eye.swapchain, &release));
    XrSwapchainSubImage image{};
    image.swapchain = eye.swapchain;
    image.imageRect.extent = {eye.width, eye.height};
    return image;
  }
  void renderPreview(XrFrameEndInfo end) {
    locate(end.displayTime);
    const M camera = previewProjection();
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = headSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.pose.orientation.w = 1;
    layer.pose.position.z = -previewDistance;
    layer.size = previewSize;
    // Render a single image in the panel's own camera. Its contents and size
    // are independent of the eye poses and FOV returned by xrLocateViews.
    layer.subImage = renderEye(eyes[0], options.mirror, camera, scene(), camera, panel());
    const auto *header = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
    end.layerCount = 1;
    end.layers = &header;
    XR(xrEndFrame(session, &end));
  }
  bool renderWorld(XrFrameEndInfo end) {
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = end.displayTime;
    li.space = space;
    XR(xrLocateViews(session, &li, &state, 2, &n, views.data()));
    const auto valid =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if (n != 2 || (state.viewStateFlags & valid) != valid) {
      XR(xrEndFrame(session, &end));
      return false;
    }
    locate(end.displayTime);
    Mesh mesh = scene(), hud = panel();
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    XR(xrLocateSpace(headSpace, space, end.displayTime, &head));
    const auto headFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                           XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    M headToRender = identity();
    if ((head.locationFlags & headFlags) == headFlags) {
      // Place the head-relative HUD into the LOCAL projection layer.
      V cols[] = {rotate(head.pose.orientation, {1, 0, 0}),
                  rotate(head.pose.orientation, {0, 1, 0}),
                  rotate(head.pose.orientation, {0, 0, 1})};
      for (int c = 0; c < 3; c++) {
        headToRender.v[c * 4] = cols[c].x;
        headToRender.v[c * 4 + 1] = cols[c].y;
        headToRender.v[c * 4 + 2] = cols[c].z;
      }
      headToRender.v[12] = head.pose.position.x;
      headToRender.v[13] = head.pose.position.y;
      headToRender.v[14] = head.pose.position.z;
    } else {
      hud.clear();
    }
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{
        {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for (int i = 0; i < 2; i++) {
      auto &pv = projectionViews[i];
      pv.pose = views[i].pose;
      pv.fov = views[i].fov;
      const M vp = projection(pv.fov) * viewMatrix(pv.pose);
      pv.subImage = renderEye(eyes[i], i == 0 && options.mirror, vp, mesh,
                             vp * headToRender, hud);
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space;
    layer.viewCount = 2;
    layer.views = projectionViews.data();
    const auto *header = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
    end.layerCount = 1;
    end.layers = &header;
    XR(xrEndFrame(session, &end));
    return true;
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
    if (!fs.shouldRender) {
      XR(xrEndFrame(session, &end));
      return;
    }
    if (bodyPreview)
      renderPreview(end);
    else if (!renderWorld(end))
      return;
    status();
    frameCount++;
  }
  void demoHands() {
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
  }
  void demoFrame() {
    if (options.body)
      body = demoBody();
    else
      demoHands();
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w > 0 && h > 0) {
      float t = .7f, aspect = float(w) / h;
      const M p = bodyPreview
                      ? previewProjection()
                      : projection({std::atan(-t * aspect), std::atan(t * aspect),
                                    std::atan(t), std::atan(-t)});
      draw(0, w, h, p, scene(), p, panel());
      SDL_GL_SwapWindow(window);
    }
    status();
    frameCount++;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

public:
  explicit App(Options o)
      : options(o), source21(o.joints == 21), bodyPreview(o.body) {}
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
    for (auto jointSpace : bodySpaces)
      if (jointSpace)
        xrDestroySpace(jointSpace);
    if (bodyDevices && destroyBodyDevices)
      destroyBodyDevices(bodyDevices);
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
    std::cout << "Left cyan; right orange; valid untracked joints yellow. ";
    if (options.body)
      std::cout << "V preview/world. ";
    else
      std::cout << "J 21/26 joints, A axes, R radii. ";
    std::cout << "G grid, Esc exit.\n";
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
  Options options;
  try {
    for (int i = 1; i < argc; i++) {
      std::string s = argv[i];
      if (s == "--demo")
        options.demo = true;
      else if (s == "--body")
        options.body = true;
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
        std::cout << "XR Gate Skeleton (Linux X11/OpenGL 3.3)\n"
                     "--body        Show Monado FRAME body trackers\n"
                     "--joints 21|26  Select hand skeleton (default 21)\n"
                     "--no-mirror  Hide desktop mirror\n--demo       "
                     "Synthetic rendering only, no OpenXR runtime\n--frames N  "
                     " Exit after N rendered frames\nV switches body preview/world. "
                     "J 21/26 joints, A joint axes, R runtime radii for hands. "
                     "G grid, Esc exit.\n";
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
    std::cerr << "[skeleton] " << e.what()
              << "\nCheck XR_RUNTIME_JSON and DISPLAY/X11.\n";
    if (options.body)
      std::cerr << "Start XR Gate with FRAME first, then start Monado with "
                   "XR_MONADO_BODY_TRACKERS=1.\n";
    else
      std::cerr << "Start Monado with XR_MONADO_HAND_TRACKING=1.\n";
    return 1;
  }
}
