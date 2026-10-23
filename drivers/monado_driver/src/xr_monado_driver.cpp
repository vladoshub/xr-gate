// XR Tracking Monado runtime driver.
//
// This device layer consumes the runtime-ready streams produced by
// xr_runtime_adapter and exposes one HMD plus two controller devices to Monado.

#include "xr_monado_driver/runtime_readers.hpp"
#include <xr_runtime/input/controller_input_mapping.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include "xrt/xrt_device.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_tracking.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"
}

namespace xr::monado_driver {
namespace {

constexpr const char* kDriverName = "xr_tracking_runtime";
constexpr uint32_t kDefaultRenderWidth = 1920;
constexpr uint32_t kDefaultRenderHeight = 1080;
constexpr uint32_t kDefaultWindowWidth = 3840;
constexpr uint32_t kDefaultWindowHeight = 1080;
constexpr float kDefaultDisplayWidthM = 0.120f;
constexpr float kDefaultDisplayHeightM = 0.034f;
constexpr float kDefaultIpdM = 0.064f;
constexpr float kDefaultLensVerticalPositionM = 0.017f;
constexpr float kDefaultFov = 1.0f;
constexpr float kDefaultRefreshHz = 90.0f;
constexpr uint32_t kDefaultPoseMaxAgeMs = 250;
constexpr uint32_t kDefaultControllerMaxAgeMs = 250;
constexpr uint32_t kDefaultHandMaxAgeMs = 250;

struct XrRuntimeIo {
  // Pose, input and hand-tracker callbacks may run on different Monado threads.
  std::mutex mutex;
  RuntimePoseReaderConfig hmd_cfg;
  RuntimeControllerStateReaderConfig controller_cfg;
  RuntimeHandReaderConfig hand_cfg;

  std::unique_ptr<IRuntimePoseReader> hmd_reader;
  std::unique_ptr<IRuntimeControllerStateReader> controller_reader;
  std::unique_ptr<IRuntimeHandReader> hand_reader;

  RuntimePoseSample last_hmd{};
  RuntimeControllerStateSample last_controller{};
  RuntimeHandSample last_hand{};

  bool have_hmd = false;
  bool have_controller = false;
  bool have_hand = false;
};

struct XrTrackingDevice {
  struct xrt_device base;

  bool is_hmd = false;
  bool is_controller = false;
  bool left = false;
  bool hand_tracking_enabled = false;
  xr_runtime::ControllerInputMappingConfig input_cfg;
  struct xrt_binding_profile controller_profiles[4];

  XrRuntimeIo* io = nullptr;
  bool owns_io = false;
};

static struct xrt_binding_input_pair wmr_to_simple_inputs[] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_INDEX_TRIGGER_CLICK},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_input_pair wmr_to_vive_inputs[] = {
    {XRT_INPUT_VIVE_SQUEEZE_CLICK, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_VIVE_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_VIVE_TRIGGER_CLICK, XRT_INPUT_INDEX_TRIGGER_CLICK},
    {XRT_INPUT_VIVE_TRIGGER_VALUE, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_VIVE_TRACKPAD, XRT_INPUT_WMR_TRACKPAD},
    {XRT_INPUT_VIVE_TRACKPAD_CLICK, XRT_INPUT_WMR_TRACKPAD_CLICK},
    {XRT_INPUT_VIVE_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_VIVE_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_VIVE_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_input_pair wmr_inputs[] = {
    {XRT_INPUT_WMR_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_WMR_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
    {XRT_INPUT_WMR_TRIGGER_VALUE, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_WMR_SQUEEZE_CLICK, XRT_INPUT_WMR_SQUEEZE_CLICK},
    {XRT_INPUT_WMR_THUMBSTICK, XRT_INPUT_WMR_THUMBSTICK},
    {XRT_INPUT_WMR_THUMBSTICK_CLICK, XRT_INPUT_WMR_THUMBSTICK_CLICK},
    {XRT_INPUT_WMR_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_WMR_TRACKPAD_CLICK, XRT_INPUT_WMR_TRACKPAD_CLICK},
    {XRT_INPUT_WMR_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_WMR_TRACKPAD, XRT_INPUT_WMR_TRACKPAD},
};

static struct xrt_binding_input_pair touch_to_index_inputs[] = {
    {XRT_INPUT_INDEX_SYSTEM_CLICK, XRT_INPUT_TOUCH_SYSTEM_CLICK},
    {XRT_INPUT_INDEX_SYSTEM_TOUCH, XRT_INPUT_TOUCH_SYSTEM_CLICK},
    {XRT_INPUT_INDEX_A_CLICK, XRT_INPUT_TOUCH_A_CLICK},
    {XRT_INPUT_INDEX_A_TOUCH, XRT_INPUT_TOUCH_A_TOUCH},
    {XRT_INPUT_INDEX_B_CLICK, XRT_INPUT_TOUCH_B_CLICK},
    {XRT_INPUT_INDEX_B_TOUCH, XRT_INPUT_TOUCH_B_TOUCH},
    {XRT_INPUT_INDEX_SQUEEZE_VALUE, XRT_INPUT_TOUCH_SQUEEZE_VALUE},
    {XRT_INPUT_INDEX_SQUEEZE_FORCE, XRT_INPUT_INDEX_SQUEEZE_FORCE},
    {XRT_INPUT_INDEX_TRIGGER_CLICK, XRT_INPUT_INDEX_TRIGGER_CLICK},
    {XRT_INPUT_INDEX_TRIGGER_VALUE, XRT_INPUT_TOUCH_TRIGGER_VALUE},
    {XRT_INPUT_INDEX_TRIGGER_TOUCH, XRT_INPUT_TOUCH_TRIGGER_TOUCH},
    {XRT_INPUT_INDEX_THUMBSTICK, XRT_INPUT_TOUCH_THUMBSTICK},
    {XRT_INPUT_INDEX_THUMBSTICK_CLICK, XRT_INPUT_TOUCH_THUMBSTICK_CLICK},
    {XRT_INPUT_INDEX_THUMBSTICK_TOUCH, XRT_INPUT_TOUCH_THUMBSTICK_TOUCH},
    {XRT_INPUT_INDEX_TRACKPAD, XRT_INPUT_WMR_TRACKPAD},
    {XRT_INPUT_INDEX_TRACKPAD_FORCE, XRT_INPUT_INDEX_TRACKPAD_FORCE},
    {XRT_INPUT_INDEX_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD_TOUCH},
    {XRT_INPUT_INDEX_GRIP_POSE, XRT_INPUT_TOUCH_GRIP_POSE},
    {XRT_INPUT_INDEX_AIM_POSE, XRT_INPUT_TOUCH_AIM_POSE},
};

// Native Touch is tried first by Monado. Keep rich alternatives before Simple.
static struct xrt_binding_profile controller_binding_profiles[] = {
    {XRT_DEVICE_INDEX_CONTROLLER, touch_to_index_inputs, ARRAY_SIZE(touch_to_index_inputs), nullptr, 0},
    {XRT_DEVICE_WMR_CONTROLLER, wmr_inputs, ARRAY_SIZE(wmr_inputs), nullptr, 0},
    {XRT_DEVICE_VIVE_WAND, wmr_to_vive_inputs, ARRAY_SIZE(wmr_to_vive_inputs), nullptr, 0},
    {XRT_DEVICE_SIMPLE_CONTROLLER, wmr_to_simple_inputs, ARRAY_SIZE(wmr_to_simple_inputs), nullptr, 0},
};
constexpr const char* kControllerProfileNames[] = {"index", "wmr", "vive", "simple"};

// Touch provides face buttons and analog squeeze. Keep the WMR inputs as
// compatibility sources for the existing simple-controller/Vive/WMR bindings.
constexpr enum xrt_input_name kControllerInputNames[] = {
    XRT_INPUT_WMR_GRIP_POSE, XRT_INPUT_WMR_AIM_POSE,
    XRT_INPUT_WMR_TRIGGER_VALUE, XRT_INPUT_WMR_SQUEEZE_CLICK,
    XRT_INPUT_WMR_THUMBSTICK, XRT_INPUT_WMR_THUMBSTICK_CLICK,
    XRT_INPUT_WMR_MENU_CLICK, XRT_INPUT_WMR_TRACKPAD_CLICK,
    XRT_INPUT_WMR_TRACKPAD_TOUCH, XRT_INPUT_WMR_TRACKPAD,
    XRT_INPUT_TOUCH_GRIP_POSE, XRT_INPUT_TOUCH_AIM_POSE,
    XRT_INPUT_TOUCH_TRIGGER_VALUE, XRT_INPUT_TOUCH_TRIGGER_TOUCH,
    XRT_INPUT_TOUCH_SQUEEZE_VALUE, XRT_INPUT_TOUCH_THUMBSTICK,
    XRT_INPUT_TOUCH_THUMBSTICK_CLICK, XRT_INPUT_TOUCH_THUMBSTICK_TOUCH,
    XRT_INPUT_TOUCH_MENU_CLICK, XRT_INPUT_TOUCH_SYSTEM_CLICK,
    XRT_INPUT_TOUCH_A_CLICK, XRT_INPUT_TOUCH_A_TOUCH,
    XRT_INPUT_TOUCH_B_CLICK, XRT_INPUT_TOUCH_B_TOUCH,
    XRT_INPUT_TOUCH_X_CLICK, XRT_INPUT_TOUCH_X_TOUCH,
    XRT_INPUT_TOUCH_Y_CLICK, XRT_INPUT_TOUCH_Y_TOUCH,
    XRT_INPUT_INDEX_TRIGGER_CLICK, XRT_INPUT_INDEX_SQUEEZE_FORCE,
    XRT_INPUT_INDEX_TRACKPAD_FORCE,
};

XrTrackingDevice* from_xdev(struct xrt_device* xdev) {
  return reinterpret_cast<XrTrackingDevice*>(xdev);
}

struct xrt_quat quat_identity() {
  struct xrt_quat q{};
  q.x = 0.0f;
  q.y = 0.0f;
  q.z = 0.0f;
  q.w = 1.0f;
  return q;
}

struct xrt_vec3 vec3(float x, float y, float z) {
  struct xrt_vec3 v{};
  v.x = x;
  v.y = y;
  v.z = z;
  return v;
}

bool env_truthy(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) return false;
  return std::strcmp(value, "1") == 0 ||
         std::strcmp(value, "true") == 0 ||
         std::strcmp(value, "TRUE") == 0 ||
         std::strcmp(value, "yes") == 0 ||
         std::strcmp(value, "YES") == 0 ||
         std::strcmp(value, "on") == 0 ||
         std::strcmp(value, "ON") == 0;
}

uint32_t env_u32(const char* name, uint32_t fallback, uint32_t min_value, uint32_t max_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;

  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || end == nullptr || *end != '\0') return fallback;
  if (parsed < min_value || parsed > max_value) return fallback;
  return static_cast<uint32_t>(parsed);
}

float env_float(const char* name, float fallback, float min_value, float max_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;

  char* end = nullptr;
  const float parsed = std::strtof(value, &end);
  if (end == value || end == nullptr || *end != '\0') return fallback;
  if (!std::isfinite(parsed) || parsed < min_value || parsed > max_value) return fallback;
  return parsed;
}

bool env_has_value(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0';
}

float degrees_to_radians(float degrees) {
  constexpr float kPi = 3.14159265358979323846f;
  return degrees * (kPi / 180.0f);
}

struct EyeOpticsProfile {
  float lens_center_u = 0.5f;
  float lens_center_v = 0.5f;
  float fov_left = kDefaultFov;
  float fov_right = kDefaultFov;
  float fov_up = kDefaultFov;
  float fov_down = kDefaultFov;
};

struct HmdDisplayProfile {
  uint32_t render_width = kDefaultRenderWidth;
  uint32_t render_height = kDefaultRenderHeight;
  uint32_t window_width = kDefaultWindowWidth;
  uint32_t window_height = kDefaultWindowHeight;
  uint32_t rotation_deg = 0;
  std::string layout = "side_by_side_horizontal";
  float display_width_m = kDefaultDisplayWidthM;
  float display_height_m = kDefaultDisplayHeightM;
  float ipd_m = kDefaultIpdM;
  float inter_lens_distance_m = kDefaultIpdM;
  float screen_to_lens_distance_m = 0.0f;
  float eye_to_lens_distance_m = 0.0f;
  float lens_vertical_position_m = kDefaultLensVerticalPositionM;
  EyeOpticsProfile left_eye{};
  EyeOpticsProfile right_eye{};
  float refresh_hz = kDefaultRefreshHz;
};

std::string env_string(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;
  return value;
}

bool env_bool(const char* name, bool fallback) {
  if (!env_has_value(name)) return fallback;
  if (env_truthy(name)) return true;
  const std::string value = env_string(name, "");
  if (value == "0" || value == "false" || value == "FALSE" ||
      value == "no" || value == "NO" || value == "off" || value == "OFF") return false;
  std::fprintf(stderr, "[xr_tracking_monado][WARN] invalid %s; using default\n", name);
  return fallback;
}

xr_runtime::ControllerInputMappingConfig load_controller_input_config() {
  xr_runtime::ControllerInputMappingConfig cfg;
  cfg.left_ab_xy_alias = env_bool("XR_MONADO_CONTROLLER_LEFT_AB_XY_ALIAS", cfg.left_ab_xy_alias);
  cfg.trackpad_click_from_touch = env_bool("XR_MONADO_CONTROLLER_TRACKPAD_CLICK_FROM_TOUCH", false);
  // Preserve Monado's existing system-button publication; menu is independent.
  cfg.expose_system_button = env_bool("XR_MONADO_CONTROLLER_EXPOSE_SYSTEM_BUTTON", true);
  cfg.axis_activity_threshold = env_float("XR_MONADO_CONTROLLER_AXIS_THRESHOLD", cfg.axis_activity_threshold, 0.001f, 1.0f);
  cfg.analog_click_threshold = env_float("XR_MONADO_CONTROLLER_CLICK_THRESHOLD", cfg.analog_click_threshold, 0.001f, 1.0f);
  return cfg;
}

void configure_controller_binding_profiles(XrTrackingDevice* dev) {
  const std::string order = env_string("XR_MONADO_CONTROLLER_PROFILE_ORDER", "index,wmr,vive,simple");
  bool seen[ARRAY_SIZE(controller_binding_profiles)] = {};
  size_t count = 0;
  bool valid = true;
  size_t begin = 0;
  while (begin <= order.size()) {
    const size_t end = order.find(',', begin);
    std::string token = order.substr(begin, end == std::string::npos ? end : end - begin);
    const auto first = token.find_first_not_of(" \t\r\n");
    const auto last = token.find_last_not_of(" \t\r\n");
    token = first == std::string::npos ? "" : token.substr(first, last - first + 1);
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    size_t index = 0;
    while (index < ARRAY_SIZE(kControllerProfileNames) && token != kControllerProfileNames[index]) ++index;
    if (index == ARRAY_SIZE(kControllerProfileNames) || seen[index]) { valid = false; break; }
    seen[index] = true;
    dev->controller_profiles[count++] = controller_binding_profiles[index];
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  if (!valid || count == 0) {
    std::fprintf(stderr, "[xr_tracking_monado][WARN] invalid XR_MONADO_CONTROLLER_PROFILE_ORDER; using index,wmr,vive,simple\n");
    count = ARRAY_SIZE(controller_binding_profiles);
    std::copy(std::begin(controller_binding_profiles), std::end(controller_binding_profiles), dev->controller_profiles);
  }
  dev->base.binding_profiles = dev->controller_profiles;
  dev->base.binding_profile_count = count;
}

float load_eye_fov_deg(const char* name, float fallback_rad) {
  if (!env_has_value(name)) return fallback_rad;
  return degrees_to_radians(env_float(name, 0.0f, 0.01f, 89.9f));
}

HmdDisplayProfile load_hmd_display_profile_from_env() {
  HmdDisplayProfile profile{};

  // Side-by-side shorthand. Explicit render/window values below retain higher
  // precedence and preserve compatibility with existing launchers.
  uint32_t render_width_default = kDefaultRenderWidth;
  uint32_t render_height_default = kDefaultRenderHeight;
  uint32_t window_width_default = kDefaultWindowWidth;
  uint32_t window_height_default = kDefaultWindowHeight;
  if (env_has_value("XR_MONADO_EYE_WIDTH")) {
    render_width_default = env_u32("XR_MONADO_EYE_WIDTH", kDefaultRenderWidth, 64, 16384);
    window_width_default = render_width_default * 2u;
  }
  if (env_has_value("XR_MONADO_EYE_HEIGHT")) {
    render_height_default = env_u32("XR_MONADO_EYE_HEIGHT", kDefaultRenderHeight, 64, 16384);
    window_height_default = render_height_default;
  }

  profile.render_width = env_u32("XR_MONADO_RENDER_WIDTH", render_width_default, 64, 16384);
  profile.render_height = env_u32("XR_MONADO_RENDER_HEIGHT", render_height_default, 64, 16384);
  profile.window_width = env_u32("XR_MONADO_WINDOW_WIDTH", window_width_default, 64, 32768);
  profile.window_height = env_u32("XR_MONADO_WINDOW_HEIGHT", window_height_default, 64, 32768);
  profile.rotation_deg = env_u32("XR_MONADO_DISPLAY_ROTATION_DEG", 0, 0, 270);
  profile.layout = env_string("XR_MONADO_DISPLAY_LAYOUT", "side_by_side_horizontal");
  profile.display_width_m = env_float("XR_MONADO_DISPLAY_WIDTH_M", kDefaultDisplayWidthM, 0.001f, 10.0f);
  profile.display_height_m = env_float("XR_MONADO_DISPLAY_HEIGHT_M", kDefaultDisplayHeightM, 0.001f, 10.0f);
  profile.ipd_m = env_float("XR_MONADO_IPD_M", kDefaultIpdM, 0.001f, 1.0f);
  profile.inter_lens_distance_m =
      env_float("XR_MONADO_INTER_LENS_DISTANCE_M", profile.ipd_m, 0.001f, 1.0f);
  profile.screen_to_lens_distance_m =
      env_float("XR_MONADO_SCREEN_TO_LENS_DISTANCE_M", 0.0f, 0.0f, 10.0f);
  profile.eye_to_lens_distance_m =
      env_float("XR_MONADO_EYE_TO_LENS_DISTANCE_M", 0.0f, 0.0f, 10.0f);

  profile.left_eye.lens_center_u = env_float("XR_MONADO_LEFT_LENS_CENTER_U", 0.5f, 0.0f, 1.0f);
  profile.left_eye.lens_center_v = env_float("XR_MONADO_LEFT_LENS_CENTER_V", 0.5f, 0.0f, 1.0f);
  profile.right_eye.lens_center_u = env_float("XR_MONADO_RIGHT_LENS_CENTER_U", 0.5f, 0.0f, 1.0f);
  profile.right_eye.lens_center_v = env_float("XR_MONADO_RIGHT_LENS_CENTER_V", 0.5f, 0.0f, 1.0f);

  // Legacy shared FOV controls remain supported and form the fallback for both
  // eyes. New per-eye degree values from display/optics YAML override them.
  float fov_left_default = kDefaultFov;
  float fov_right_default = kDefaultFov;
  float fov_up_default = kDefaultFov;
  float fov_down_default = kDefaultFov;

  if (env_has_value("XR_MONADO_FOV_HORIZONTAL_DEG")) {
    const float full = env_float("XR_MONADO_FOV_HORIZONTAL_DEG", 0.0f, 0.02f, 179.8f);
    const float half_rad = degrees_to_radians(full * 0.5f);
    fov_left_default = half_rad;
    fov_right_default = half_rad;
  }
  if (env_has_value("XR_MONADO_FOV_VERTICAL_DEG")) {
    const float full = env_float("XR_MONADO_FOV_VERTICAL_DEG", 0.0f, 0.02f, 179.8f);
    const float half_rad = degrees_to_radians(full * 0.5f);
    fov_up_default = half_rad;
    fov_down_default = half_rad;
  }
  if (env_has_value("XR_MONADO_FOV_LEFT_DEG")) {
    fov_left_default = degrees_to_radians(env_float("XR_MONADO_FOV_LEFT_DEG", 0.0f, 0.01f, 89.9f));
  }
  if (env_has_value("XR_MONADO_FOV_RIGHT_DEG")) {
    fov_right_default = degrees_to_radians(env_float("XR_MONADO_FOV_RIGHT_DEG", 0.0f, 0.01f, 89.9f));
  }
  if (env_has_value("XR_MONADO_FOV_UP_DEG")) {
    fov_up_default = degrees_to_radians(env_float("XR_MONADO_FOV_UP_DEG", 0.0f, 0.01f, 89.9f));
  }
  if (env_has_value("XR_MONADO_FOV_DOWN_DEG")) {
    fov_down_default = degrees_to_radians(env_float("XR_MONADO_FOV_DOWN_DEG", 0.0f, 0.01f, 89.9f));
  }

  fov_left_default = env_float(
      "XR_MONADO_FOV_LEFT_RAD",
      env_float("XR_MONADO_FOV_LEFT", fov_left_default, 0.01f, 10.0f),
      0.01f,
      10.0f);
  fov_right_default = env_float(
      "XR_MONADO_FOV_RIGHT_RAD",
      env_float("XR_MONADO_FOV_RIGHT", fov_right_default, 0.01f, 10.0f),
      0.01f,
      10.0f);
  fov_up_default = env_float(
      "XR_MONADO_FOV_UP_RAD",
      env_float("XR_MONADO_FOV_UP", fov_up_default, 0.01f, 10.0f),
      0.01f,
      10.0f);
  fov_down_default = env_float(
      "XR_MONADO_FOV_DOWN_RAD",
      env_float("XR_MONADO_FOV_DOWN", fov_down_default, 0.01f, 10.0f),
      0.01f,
      10.0f);

  profile.left_eye.fov_left = load_eye_fov_deg("XR_MONADO_LEFT_EYE_FOV_LEFT_DEG", fov_left_default);
  profile.left_eye.fov_right = load_eye_fov_deg("XR_MONADO_LEFT_EYE_FOV_RIGHT_DEG", fov_right_default);
  profile.left_eye.fov_up = load_eye_fov_deg("XR_MONADO_LEFT_EYE_FOV_UP_DEG", fov_up_default);
  profile.left_eye.fov_down = load_eye_fov_deg("XR_MONADO_LEFT_EYE_FOV_DOWN_DEG", fov_down_default);
  profile.right_eye.fov_left = load_eye_fov_deg("XR_MONADO_RIGHT_EYE_FOV_LEFT_DEG", fov_left_default);
  profile.right_eye.fov_right = load_eye_fov_deg("XR_MONADO_RIGHT_EYE_FOV_RIGHT_DEG", fov_right_default);
  profile.right_eye.fov_up = load_eye_fov_deg("XR_MONADO_RIGHT_EYE_FOV_UP_DEG", fov_up_default);
  profile.right_eye.fov_down = load_eye_fov_deg("XR_MONADO_RIGHT_EYE_FOV_DOWN_DEG", fov_down_default);

  const float lens_center_v = 0.5f * (profile.left_eye.lens_center_v + profile.right_eye.lens_center_v);
  profile.lens_vertical_position_m = env_float(
      "XR_MONADO_LENS_VERTICAL_POSITION_M",
      profile.display_height_m * lens_center_v,
      -10.0f,
      10.0f);
  profile.refresh_hz = env_float("XR_MONADO_REFRESH_HZ", kDefaultRefreshHz, 1.0f, 1000.0f);

  return profile;
}

struct xrt_quat quatf(float w, float x, float y, float z) {
  struct xrt_quat q{};
  q.x = x;
  q.y = y;
  q.z = z;
  q.w = w;
  return q;
}

struct xrt_quat quat_normalize(struct xrt_quat q) {
  const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n <= 0.0f || !std::isfinite(n)) return quat_identity();
  q.w /= n;
  q.x /= n;
  q.y /= n;
  q.z /= n;
  return q;
}

bool quat_is_valid(const struct xrt_quat& q) {
  if (!std::isfinite(q.w) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
    return false;
  }
  const float n2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  return std::isfinite(n2) && n2 > 1.0e-8f;
}

void sanitize_pose(struct xrt_pose* pose) {
  if (pose == nullptr) return;
  if (!std::isfinite(pose->position.x)) pose->position.x = 0.0f;
  if (!std::isfinite(pose->position.y)) pose->position.y = 0.0f;
  if (!std::isfinite(pose->position.z)) pose->position.z = 0.0f;
  if (!quat_is_valid(pose->orientation)) {
    pose->orientation = quat_identity();
  } else {
    pose->orientation = quat_normalize(pose->orientation);
  }
}

void relation_zero(struct xrt_space_relation* out_relation) {
  std::memset(out_relation, 0, sizeof(*out_relation));
  out_relation->pose.orientation = quat_identity();
}

void force_pose_valid_tracked_flags(struct xrt_space_relation* relation) {
  if (relation == nullptr) return;

  uint32_t flags = static_cast<uint32_t>(relation->relation_flags);
  flags |= static_cast<uint32_t>(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
  flags |= static_cast<uint32_t>(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
  flags |= static_cast<uint32_t>(XRT_SPACE_RELATION_POSITION_VALID_BIT);
  flags |= static_cast<uint32_t>(XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
  relation->relation_flags = static_cast<xrt_space_relation_flags>(flags);
}

void relation_identity_valid(struct xrt_space_relation* out_relation) {
  relation_zero(out_relation);
  force_pose_valid_tracked_flags(out_relation);
}


void sanitize_tracking_origin_metadata(struct xrt_device* xdev) {
  if (xdev == nullptr || xdev->tracking_origin == nullptr) return;

  // Monado v25.1's xrt_tracking_origin does not expose an offset field here.
  // Keep this helper limited to stable metadata so the driver builds across the
  // pinned Monado revision while we diagnose the remaining xrLocateViews path.
  std::snprintf(xdev->tracking_origin->name,
                sizeof(xdev->tracking_origin->name),
                "xr_tracking_runtime_origin");
  xdev->tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
}

void sanitize_relation(struct xrt_space_relation* relation) {
  if (relation == nullptr) return;
  sanitize_pose(&relation->pose);
  // xrLocateViews validation is stricter than just having a finite quaternion in
  // the xrt_space_relation pose. If the relation flags say that orientation is
  // not valid, Monado can propagate an all-zero OpenXR XrView pose even though
  // the xrt pose we logged here is identity. During xr_tracking_runtime bring-up
  // we expose the latest/fallback HMD pose as valid+tracked so OpenXR clients can
  // render before the real runtime pose stream is available.
  if (quat_is_valid(relation->pose.orientation)) {
    force_pose_valid_tracked_flags(relation);
  }
}

void relation_from_pose_velocity(struct xrt_space_relation* out_relation,
                                 const struct xrt_vec3& position,
                                 const struct xrt_quat& orientation,
                                 const struct xrt_vec3* linear_velocity,
                                 const struct xrt_vec3* angular_velocity) {
  relation_zero(out_relation);
  out_relation->pose.position = position;
  out_relation->pose.orientation = quat_normalize(orientation);

  out_relation->relation_flags =
      static_cast<xrt_space_relation_flags>(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
                                            XRT_SPACE_RELATION_POSITION_VALID_BIT |
                                            XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
                                            XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

  if (linear_velocity != nullptr) {
    out_relation->linear_velocity = *linear_velocity;
    out_relation->relation_flags = static_cast<xrt_space_relation_flags>(
        out_relation->relation_flags | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
  }
  if (angular_velocity != nullptr) {
    out_relation->angular_velocity = *angular_velocity;
    out_relation->relation_flags = static_cast<xrt_space_relation_flags>(
        out_relation->relation_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
  }
}

bool read_hmd(XrRuntimeIo& io) {
  if (!io.hmd_reader) return false;
  RuntimePoseSample sample{};
  std::string error;
  if (!io.hmd_reader->read_latest(sample, &error)) return false;
  if (!runtime_pose_is_valid(sample.pose)) return false;
  if (!runtime_pose_sample_is_fresh(sample, io.hmd_cfg.max_age_ms)) return false;
  io.last_hmd = sample;
  io.have_hmd = true;
  return true;
}

bool read_controller_state(XrRuntimeIo& io) {
  if (!io.controller_reader) return false;
  RuntimeControllerStateSample sample{};
  std::string error;
  if (!io.controller_reader->read_latest(sample, &error)) return false;
  if (!runtime_controller_state_frame_is_valid(sample.frame)) return false;
  if (!runtime_controller_state_sample_is_fresh(sample, io.controller_cfg.max_age_ms)) return false;
  io.last_controller = sample;
  io.have_controller = true;
  return true;
}

bool read_hand(XrRuntimeIo& io) {
  if (!io.hand_reader) return false;
  RuntimeHandSample sample{};
  std::string error;
  if (!io.hand_reader->read_latest(sample, &error)) return false;
  if (!runtime_hand_frame_is_valid(sample.frame)) return false;
  if (!runtime_hand_sample_is_fresh(sample, io.hand_cfg.max_age_ms)) return false;
  io.last_hand = sample;
  io.have_hand = true;
  return true;
}

void relation_from_hmd_pose(const xr_runtime::RuntimeHmdPoseF64V1& pose,
                            struct xrt_space_relation* out_relation) {
  const struct xrt_vec3 position = vec3(static_cast<float>(pose.px),
                                       static_cast<float>(pose.py),
                                       static_cast<float>(pose.pz));
  const struct xrt_quat orientation = quatf(static_cast<float>(pose.qw),
                                           static_cast<float>(pose.qx),
                                           static_cast<float>(pose.qy),
                                           static_cast<float>(pose.qz));

  struct xrt_vec3 linear_velocity{};
  struct xrt_vec3 angular_velocity{};
  const struct xrt_vec3* linear_velocity_ptr = nullptr;
  const struct xrt_vec3* angular_velocity_ptr = nullptr;

  if ((pose.flags & xr_runtime::RUNTIME_HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
    linear_velocity = vec3(static_cast<float>(pose.vx), static_cast<float>(pose.vy), static_cast<float>(pose.vz));
    linear_velocity_ptr = &linear_velocity;
  }
  if ((pose.flags & xr_runtime::RUNTIME_HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) {
    angular_velocity = vec3(static_cast<float>(pose.wx), static_cast<float>(pose.wy), static_cast<float>(pose.wz));
    angular_velocity_ptr = &angular_velocity;
  }

  relation_from_pose_velocity(out_relation, position, orientation, linear_velocity_ptr, angular_velocity_ptr);
}

void relation_from_runtime_controller_side(const xr_runtime::RuntimeControllerSideStateV1& side,
                                           struct xrt_space_relation* out_relation) {
  const struct xrt_vec3 position = vec3(side.position[0], side.position[1], side.position[2]);
  const struct xrt_quat orientation = quatf(side.orientation_xyzw[3],
                                           side.orientation_xyzw[0],
                                           side.orientation_xyzw[1],
                                           side.orientation_xyzw[2]);
  const struct xrt_vec3 linear_velocity = vec3(side.linear_velocity[0], side.linear_velocity[1], side.linear_velocity[2]);
  const struct xrt_vec3 angular_velocity = vec3(side.angular_velocity[0], side.angular_velocity[1], side.angular_velocity[2]);
  relation_from_pose_velocity(out_relation, position, orientation, &linear_velocity, &angular_velocity);
}

void relation_from_hand_side(const xr_tracking::HandSideF32V2& side,
                             struct xrt_space_relation* out_relation) {
  // runtime_hand_tracking is already in runtime/world space, including the
  // HMD transform, origin and configured offsets. Applying the HMD again here
  // would double both the translation and rotation on the fallback path.
  const struct xrt_vec3 position = vec3(side.controller_px, side.controller_py, side.controller_pz);
  const struct xrt_quat orientation = quatf(side.controller_qw, side.controller_qx,
                                            side.controller_qy, side.controller_qz);
  struct xrt_vec3 linear_velocity{};
  struct xrt_vec3 angular_velocity{};
  const struct xrt_vec3* linear_velocity_ptr = nullptr;
  const struct xrt_vec3* angular_velocity_ptr = nullptr;

  if ((side.flags & xr_tracking::HAND_LINEAR_VELOCITY_VALID) != 0u) {
    linear_velocity = vec3(side.vx, side.vy, side.vz);
    linear_velocity_ptr = &linear_velocity;
  }
  if ((side.flags & xr_tracking::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
    angular_velocity = vec3(side.wx, side.wy, side.wz);
    angular_velocity_ptr = &angular_velocity;
  }

  relation_from_pose_velocity(out_relation, position, orientation, linear_velocity_ptr, angular_velocity_ptr);
}

bool runtime_hand_has_optical_joints(const RuntimeHandSample& sample,
                                     const xr_tracking::HandSideF32V2& side,
                                     bool left,
                                     uint32_t max_age_ms) {
  if (!runtime_hand_side_is_valid(sample.frame, side, left) ||
      !std::isfinite(side.confidence) ||
      side.status != static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Tracking) ||
      (side.flags & xr_tracking::HAND_JOINTS_VALID) == 0u ||
      (sample.frame.flags & xr_tracking::HAND_FLAG_JOINTS_VALID) == 0u ||
      side.joint_count != xr_tracking::HAND_JOINT_COUNT_V2 ||
      sample.frame.source_timestamp_ns == 0u) return false;
  // The runtime republishes at its tick rate. Check the optical source time as
  // well, so republishing cannot keep an old skeleton active indefinitely.
  const int64_t age_ns = sample.read_time_ns - static_cast<int64_t>(sample.frame.source_timestamp_ns);
  return max_age_ms == 0u || age_ns <= static_cast<int64_t>(max_age_ms) * 1000000LL;
}

bool finite_position(const struct xrt_vec3& position) {
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

constexpr uint32_t kJointPoseValid = XRT_SPACE_RELATION_POSITION_VALID_BIT |
                                      XRT_SPACE_RELATION_ORIENTATION_VALID_BIT;
constexpr uint32_t kJointPoseTracked = XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
                                        XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;

bool runtime_joint_to_monado(const xr_tracking::HandJointF32V2& src,
                             struct xrt_hand_joint_value& dst) {
  dst = {};
  dst.relation.pose.orientation = quat_identity();
  const auto position = vec3(src.px, src.py, src.pz);
  const auto orientation = quatf(src.qw, src.qx, src.qy, src.qz);
  // The 21-joint stream has producers using XRT relation flags and producers
  // using skeleton26 flags. Both use the low two bits for pose validity (in
  // opposite order). Only publish a complete, finite pose from either form.
  if ((src.flags & 3u) != 3u || !std::isfinite(src.confidence) || src.confidence <= 0.0f ||
      !finite_position(position) || !quat_is_valid(orientation)) return false;
  dst.relation.pose.position = position;
  dst.relation.pose.orientation = quat_normalize(orientation);
  uint32_t flags = kJointPoseValid;
  flags |= src.flags & kJointPoseTracked;
  // skeleton26 TRACKED is bit 3; the stream carries no per-joint velocities.
  if ((src.flags & 8u) != 0u && (src.flags & kJointPoseTracked) == 0u) flags |= kJointPoseTracked;
  dst.relation.relation_flags = static_cast<xrt_space_relation_flags>(flags);
  dst.radius = std::isfinite(src.radius_m) && src.radius_m > 0.0f ? src.radius_m : 0.008f;
  return true;
}

bool runtime_hand_to_joint_set(const xr_tracking::HandSideF32V2& side,
                               struct xrt_hand_joint_set& out) {
  out = {};
  for (auto& joint : out.values.hand_joint_set_default) joint.relation.pose.orientation = quat_identity();
  relation_zero(&out.hand_pose);
  if (side.joint_count != xr_tracking::HAND_JOINT_COUNT_V2) return false;

  // Inverse of Mercury's native 26 -> 21 mapping. The runtime carries wrist,
  // four thumb joints and four joints per remaining finger.
  constexpr enum xrt_hand_joint joint_map[] = {
      XRT_HAND_JOINT_WRIST,
      XRT_HAND_JOINT_THUMB_METACARPAL, XRT_HAND_JOINT_THUMB_PROXIMAL,
      XRT_HAND_JOINT_THUMB_DISTAL, XRT_HAND_JOINT_THUMB_TIP,
      XRT_HAND_JOINT_INDEX_PROXIMAL, XRT_HAND_JOINT_INDEX_INTERMEDIATE,
      XRT_HAND_JOINT_INDEX_DISTAL, XRT_HAND_JOINT_INDEX_TIP,
      XRT_HAND_JOINT_MIDDLE_PROXIMAL, XRT_HAND_JOINT_MIDDLE_INTERMEDIATE,
      XRT_HAND_JOINT_MIDDLE_DISTAL, XRT_HAND_JOINT_MIDDLE_TIP,
      XRT_HAND_JOINT_RING_PROXIMAL, XRT_HAND_JOINT_RING_INTERMEDIATE,
      XRT_HAND_JOINT_RING_DISTAL, XRT_HAND_JOINT_RING_TIP,
      XRT_HAND_JOINT_LITTLE_PROXIMAL, XRT_HAND_JOINT_LITTLE_INTERMEDIATE,
      XRT_HAND_JOINT_LITTLE_DISTAL, XRT_HAND_JOINT_LITTLE_TIP,
  };
  static_assert(ARRAY_SIZE(joint_map) == xr_tracking::HAND_JOINT_COUNT_V2);
  for (uint32_t i = 0; i < ARRAY_SIZE(joint_map); ++i) {
    if (side.joints[i].joint_id == i) {
      runtime_joint_to_monado(side.joints[i], out.values.hand_joint_set_default[joint_map[i]]);
    }
  }
  auto& wrist = out.values.hand_joint_set_default[XRT_HAND_JOINT_WRIST];
  const auto palm_position = vec3(side.palm_px, side.palm_py, side.palm_pz);
  if ((wrist.relation.relation_flags & kJointPoseValid) != kJointPoseValid ||
      !finite_position(palm_position)) {
    out = {};
    return false;
  }
  wrist.radius = side.joints[0].radius_m > 0.0f ? wrist.radius : 0.018f;

  auto& palm = out.values.hand_joint_set_default[XRT_HAND_JOINT_PALM];
  palm.relation.pose.position = palm_position;
  // The runtime owns the final palm pose and any configured corrections.
  // Do not replace its orientation with another joint or a controller pose.
  const auto palm_orientation = quatf(side.palm_qw, side.palm_qx, side.palm_qy, side.palm_qz);
  uint32_t palm_flags = XRT_SPACE_RELATION_POSITION_VALID_BIT;
  if (quat_is_valid(palm_orientation)) {
    palm.relation.pose.orientation = quat_normalize(palm_orientation);
    palm_flags |= XRT_SPACE_RELATION_ORIENTATION_VALID_BIT;
  }
  // An invalid palm quaternion keeps an identity placeholder with orientation
  // validity cleared. The measured wrist/finger joints remain available.
  palm.relation.relation_flags = static_cast<xrt_space_relation_flags>(palm_flags);
  palm.radius = 0.020f;

  constexpr enum xrt_hand_joint metacarpals[] = {
      XRT_HAND_JOINT_INDEX_METACARPAL, XRT_HAND_JOINT_MIDDLE_METACARPAL,
      XRT_HAND_JOINT_RING_METACARPAL, XRT_HAND_JOINT_LITTLE_METACARPAL,
  };
  for (auto metacarpal : metacarpals) {
    const auto& proximal = out.values.hand_joint_set_default[metacarpal + 1];
    if ((proximal.relation.relation_flags & kJointPoseValid) != kJointPoseValid) continue;
    auto& joint = out.values.hand_joint_set_default[metacarpal];
    joint = proximal;
    const auto& a = wrist.relation.pose.position;
    const auto& b = proximal.relation.pose.position;
    joint.relation.pose.position = vec3(a.x + 0.25f * (b.x - a.x),
                                         a.y + 0.25f * (b.y - a.y),
                                         a.z + 0.25f * (b.z - a.z));
    // These four joints are reconstructed, never advertised as measured.
    joint.relation.relation_flags = static_cast<xrt_space_relation_flags>(kJointPoseValid);
  }

  // All joints already use runtime/world coordinates. Keep the base transform
  // at identity; Monado applies the device tracking origin exactly once.
  out.hand_pose.relation_flags = static_cast<xrt_space_relation_flags>(kJointPoseValid | kJointPoseTracked);
  out.is_active = true;
  return true;
}

xrt_result_t xrtd_hand_tracking_feature(struct xrt_device* xdev, enum xrt_device_feature_type type) {
  const auto* dev = from_xdev(xdev);
  if (!dev || !dev->is_controller || !dev->hand_tracking_enabled) return XRT_ERROR_FEATURE_NOT_SUPPORTED;
  const auto expected = dev->left ? XRT_DEVICE_FEATURE_HAND_TRACKING_LEFT : XRT_DEVICE_FEATURE_HAND_TRACKING_RIGHT;
  // Tracking already runs in xr_runtime_adapter. Monado's feature-use callbacks
  // acknowledge use of this hand without starting/stopping the shared backend.
  return type == expected ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t xrtd_get_hand_tracking(struct xrt_device* xdev,
                                    enum xrt_input_name name,
                                    int64_t desired_timestamp_ns,
                                    struct xrt_hand_joint_set* out_value,
                                    int64_t* out_timestamp_ns) {
  (void)desired_timestamp_ns;  // Return the measured sample time, without finger extrapolation.
  *out_value = {};
  *out_timestamp_ns = 0;
  auto* dev = from_xdev(xdev);
  if (!dev || !dev->io || !dev->is_controller || !dev->hand_tracking_enabled) return XRT_ERROR_INPUT_UNSUPPORTED;
  const auto expected = dev->left ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
  if (name != expected) return XRT_ERROR_INPUT_UNSUPPORTED;

  auto& io = *dev->io;
  std::lock_guard<std::mutex> lock(io.mutex);
  if (!read_hand(io)) return XRT_SUCCESS;
  const auto& side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
  if (!runtime_hand_has_optical_joints(io.last_hand, side, dev->left, io.hand_cfg.max_age_ms)) return XRT_SUCCESS;
  if (runtime_hand_to_joint_set(side, *out_value)) {
    *out_timestamp_ns = static_cast<int64_t>(io.last_hand.frame.source_timestamp_ns);
  }
  return XRT_SUCCESS;
}

bool controller_pose_input_name(enum xrt_input_name name) {
  switch (name) {
    case XRT_INPUT_WMR_GRIP_POSE:
    case XRT_INPUT_WMR_AIM_POSE:
    case XRT_INPUT_GENERIC_GRIP_POSE:
    case XRT_INPUT_GENERIC_AIM_POSE:
    case XRT_INPUT_SIMPLE_GRIP_POSE:
    case XRT_INPUT_SIMPLE_AIM_POSE:
    case XRT_INPUT_VIVE_GRIP_POSE:
    case XRT_INPUT_VIVE_AIM_POSE:
    case XRT_INPUT_TOUCH_GRIP_POSE:
    case XRT_INPUT_TOUCH_AIM_POSE:
    case XRT_INPUT_INDEX_GRIP_POSE:
    case XRT_INPUT_INDEX_AIM_POSE:
      return true;
    default:
      return false;
  }
}

xrt_result_t xrtd_get_tracked_pose(struct xrt_device* xdev,
                                   const enum xrt_input_name name,
                                   const int64_t at_timestamp_ns,
                                   struct xrt_space_relation* const out_relation) {
  (void)at_timestamp_ns;
  XrTrackingDevice* dev = from_xdev(xdev);
  relation_zero(out_relation);

  if (dev == nullptr || dev->io == nullptr) {
    return XRT_SUCCESS;
  }

  XrRuntimeIo& io = *dev->io;
  std::lock_guard<std::mutex> lock(io.mutex);

  if (dev->is_hmd) {
    if (name != XRT_INPUT_GENERIC_HEAD_POSE) return XRT_ERROR_INPUT_UNSUPPORTED;
    if (!read_hmd(io)) {
      // Keep OpenXR clients alive before xr_runtime_adapter publishes a fresh pose.
      // Returning XRT_SUCCESS with an all-zero/invalid pose can make xrLocateViews()
      // fail validation, so expose a sane identity head pose as a fallback.
      relation_identity_valid(out_relation);
      return XRT_SUCCESS;
    }
    relation_from_hmd_pose(io.last_hmd.pose, out_relation);
    sanitize_relation(out_relation);
    return XRT_SUCCESS;
  }

  if (dev->is_controller) {
    if (!controller_pose_input_name(name)) return XRT_ERROR_INPUT_UNSUPPORTED;

    if (read_controller_state(io)) {
      const auto& side = dev->left ? io.last_controller.frame.left : io.last_controller.frame.right;
      if (runtime_controller_side_has_pose(side, dev->left)) {
        relation_from_runtime_controller_side(side, out_relation);
      }
      // A fresh composed frame is authoritative, including an explicitly
      // invalid pose in controller-only/calibration/lost-hand modes.
      return XRT_SUCCESS;
    }

    if (read_hand(io)) {
      const xr_tracking::HandSideF32V2& hand_side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
      if (runtime_hand_side_is_valid(io.last_hand.frame, hand_side, dev->left)) {
        relation_from_hand_side(hand_side, out_relation);
        return XRT_SUCCESS;
      }
    }
  }

  return XRT_SUCCESS;
}


xrt_result_t xrtd_get_view_poses(struct xrt_device* xdev,
                                    const struct xrt_vec3* default_eye_relation,
                                    const int64_t at_timestamp_ns,
                                    const enum xrt_view_type view_type,
                                    const uint32_t view_count,
                                    struct xrt_space_relation* out_head_relation,
                                    struct xrt_fov* out_fovs,
                                    struct xrt_pose* out_poses) {
  // Use Monado's normal helper for the final plumbing. It is the documented
  // helper for HMD drivers and knows how Monado expects xrt_hmd_parts::views,
  // xrt_hmd_parts::fov, and the default eye relation to be used. Our tracked
  // pose callback now always returns a valid identity fallback, so the original
  // reason for bypassing the helper is gone.
  xrt_result_t xret = u_device_get_view_poses(xdev,
                                              default_eye_relation,
                                              at_timestamp_ns,
                                              view_type,
                                              view_count,
                                              out_head_relation,
                                              out_fovs,
                                              out_poses);
  if (xret != XRT_SUCCESS) {
    return xret;
  }

  // Be defensive for OpenXR validation: sanitize both the head relation and the
  // per-eye poses after the helper filled them. xrLocateViews requires valid
  // unit quaternions in the returned XrView poses.
  if (out_head_relation != nullptr) {
    sanitize_relation(out_head_relation);
    if (!quat_is_valid(out_head_relation->pose.orientation)) {
      relation_identity_valid(out_head_relation);
    }
    force_pose_valid_tracked_flags(out_head_relation);
  }

  if (out_poses != nullptr) {
    for (uint32_t i = 0; i < view_count; ++i) {
      sanitize_pose(&out_poses[i]);
      if (!quat_is_valid(out_poses[i].orientation)) {
        out_poses[i].orientation = quat_identity();
      }
    }
  }

  if (env_truthy("XR_TRACKING_MONADO_DEBUG_VIEW_POSES")) {
    const bool has_head = out_head_relation != nullptr;
    const bool has_fovs = out_fovs != nullptr;
    const bool has_poses = out_poses != nullptr;
    const struct xrt_quat head_q = has_head ? out_head_relation->pose.orientation : quat_identity();
    const struct xrt_quat view0_q = has_poses && view_count > 0 ? out_poses[0].orientation : quat_identity();
    const struct xrt_quat view1_q = has_poses && view_count > 1 ? out_poses[1].orientation : quat_identity();
    const uint32_t head_flags = has_head ? static_cast<uint32_t>(out_head_relation->relation_flags) : 0u;
    const bool has_origin = xdev != nullptr && xdev->tracking_origin != nullptr;
    std::fprintf(stderr,
                 "[xr_tracking_runtime] get_view_poses helper view_count=%u "
                 "ptrs(head=%d fovs=%d poses=%d origin=%d) head_flags=0x%08x "
                 "head_q=(%f,%f,%f,%f) view0_q=(%f,%f,%f,%f) view1_q=(%f,%f,%f,%f)\n",
                 view_count,
                 has_head ? 1 : 0,
                 has_fovs ? 1 : 0,
                 has_poses ? 1 : 0,
                 has_origin ? 1 : 0,
                 head_flags,
                 head_q.x,
                 head_q.y,
                 head_q.z,
                 head_q.w,
                 view0_q.x,
                 view0_q.y,
                 view0_q.z,
                 view0_q.w,
                 view1_q.x,
                 view1_q.y,
                 view1_q.z,
                 view1_q.w);
    std::fflush(stderr);
  }

  return XRT_SUCCESS;
}

bool is_hand_tracking_input(enum xrt_input_name name) {
  return name == XRT_INPUT_HT_UNOBSTRUCTED_LEFT || name == XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
}

void clear_controller_inputs(struct xrt_device* xdev, int64_t now_ns) {
  for (uint32_t i = 0; i < xdev->input_count; ++i) {
    auto& input = xdev->inputs[i];
    if (is_hand_tracking_input(input.name)) continue;
    input.active = false;
    input.timestamp = now_ns;
    std::memset(&input.value, 0, sizeof(input.value));
  }
}

void publish_controller_inputs(struct xrt_device* xdev,
                               const xr_runtime::RuntimeControllerSideStateV1& side,
                               int64_t timestamp_ns) {
  const auto* dev = from_xdev(xdev);
  const auto state = xr_runtime::map_controller_input(&side, dev->left, dev->input_cfg);
  for (uint32_t i = 0; i < xdev->input_count; ++i) {
    auto& input = xdev->inputs[i];
    if (is_hand_tracking_input(input.name)) continue;
    input.active = true;
    input.timestamp = timestamp_ns;
    std::memset(&input.value, 0, sizeof(input.value));
    switch (input.name) {
      case XRT_INPUT_WMR_TRIGGER_VALUE:
      case XRT_INPUT_TOUCH_TRIGGER_VALUE: input.value.vec1.x = state.trigger; break;
      case XRT_INPUT_TOUCH_SQUEEZE_VALUE:
      case XRT_INPUT_INDEX_SQUEEZE_FORCE: input.value.vec1.x = state.grip; break;
      case XRT_INPUT_INDEX_TRACKPAD_FORCE: input.value.vec1.x = state.stick_click ? 1.0f : 0.0f; break;
      case XRT_INPUT_INDEX_TRIGGER_CLICK: input.value.boolean = state.trigger_click; break;
      case XRT_INPUT_WMR_SQUEEZE_CLICK: input.value.boolean = state.grip_click; break;
      case XRT_INPUT_WMR_THUMBSTICK:
      case XRT_INPUT_WMR_TRACKPAD:
      case XRT_INPUT_TOUCH_THUMBSTICK: input.value.vec2 = {state.x, state.y}; break;
      case XRT_INPUT_WMR_THUMBSTICK_CLICK:
      case XRT_INPUT_TOUCH_THUMBSTICK_CLICK: input.value.boolean = state.stick_click; break;
      case XRT_INPUT_TOUCH_THUMBSTICK_TOUCH: input.value.boolean = state.stick_touch; break;
      case XRT_INPUT_WMR_TRACKPAD_CLICK: input.value.boolean = state.trackpad_click; break;
      case XRT_INPUT_WMR_TRACKPAD_TOUCH: input.value.boolean = state.trackpad_touch; break;
      case XRT_INPUT_WMR_MENU_CLICK:
      case XRT_INPUT_TOUCH_MENU_CLICK: input.value.boolean = state.menu; break;
      case XRT_INPUT_TOUCH_SYSTEM_CLICK: input.value.boolean = state.system; break;
      case XRT_INPUT_TOUCH_TRIGGER_TOUCH: input.value.boolean = state.trigger_touch; break;
      case XRT_INPUT_TOUCH_A_CLICK: input.value.boolean = state.a; break;
      case XRT_INPUT_TOUCH_A_TOUCH: input.value.boolean = state.a_touch; break;
      case XRT_INPUT_TOUCH_B_CLICK: input.value.boolean = state.b; break;
      case XRT_INPUT_TOUCH_B_TOUCH: input.value.boolean = state.b_touch; break;
      case XRT_INPUT_TOUCH_X_CLICK: input.value.boolean = state.x_button; break;
      case XRT_INPUT_TOUCH_X_TOUCH: input.value.boolean = state.x_touch; break;
      case XRT_INPUT_TOUCH_Y_CLICK: input.value.boolean = state.y_button; break;
      case XRT_INPUT_TOUCH_Y_TOUCH: input.value.boolean = state.y_touch; break;
      default: break;  // Pose inputs have no scalar/button value.
    }
  }
}

bool runtime_controller_side_has_input(const xr_runtime::RuntimeControllerSideStateV1& side, bool left) {
  const uint32_t role = left ? xr_runtime::CONTROLLER_SIDE_LEFT : xr_runtime::CONTROLLER_SIDE_RIGHT;
  const uint32_t required = xr_runtime::RUNTIME_CONTROLLER_CONNECTED | xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  return side.role == role && (side.flags & required) == required &&
         (side.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_STALE) == 0u;
}

xrt_result_t xrtd_update_inputs(struct xrt_device* xdev) {
  XrTrackingDevice* dev = from_xdev(xdev);
  if (dev == nullptr || dev->io == nullptr || !dev->is_controller) return XRT_SUCCESS;

  const int64_t now_ns = monotonic_now_ns();
  XrRuntimeIo& io = *dev->io;
  std::lock_guard<std::mutex> lock(io.mutex);
  const bool have_controller = read_controller_state(io);
  const bool have_hand = (dev->hand_tracking_enabled || !have_controller) && read_hand(io);
  clear_controller_inputs(xdev, now_ns);

  if (have_controller) {
    const auto& side = dev->left ? io.last_controller.frame.left : io.last_controller.frame.right;
    if (runtime_controller_side_has_input(side, dev->left)) {
      publish_controller_inputs(xdev, side, static_cast<int64_t>(io.last_controller.frame.timestamp_ns));
    }
  } else if (have_hand) {
    const auto& side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
    if (runtime_hand_side_is_valid(io.last_hand.frame, side, dev->left)) {
      xr_runtime::RuntimeControllerSideStateV1 input{};
      input.flags = xr_runtime::RUNTIME_CONTROLLER_CONNECTED | xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
      input.role = dev->left ? xr_runtime::CONTROLLER_SIDE_LEFT : xr_runtime::CONTROLLER_SIDE_RIGHT;
      input.buttons = side.reserved0 & xr_runtime::CONTROLLER_BUTTON_KNOWN_MASK;
      if ((side.flags & xr_tracking::HAND_PINCH_VALID) != 0u) {
        input.trigger = side.pinch_strength;
        if (side.pinch_active != 0u) input.buttons |= xr_runtime::CONTROLLER_BUTTON_TRIGGER;
      }
      if ((side.flags & xr_tracking::HAND_GRAB_VALID) != 0u) {
        input.grip = side.grab_strength;
        if (side.grab_active != 0u) input.buttons |= xr_runtime::CONTROLLER_BUTTON_GRIP;
      }
      publish_controller_inputs(xdev, input, now_ns);
    }
  }

  // Pose availability is independent of buttons/gestures being non-neutral.
  const auto& controller_side = dev->left ? io.last_controller.frame.left : io.last_controller.frame.right;
  const auto& hand_side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
  const bool have_pose = have_controller ? runtime_controller_side_has_pose(controller_side, dev->left)
      : (have_hand && runtime_hand_side_is_valid(io.last_hand.frame, hand_side, dev->left));
  for (uint32_t i = 0; i < ARRAY_SIZE(kControllerInputNames); ++i) {
    if (controller_pose_input_name(xdev->inputs[i].name)) xdev->inputs[i].active = have_pose;
  }

  if (dev->hand_tracking_enabled) {
    const auto& side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
    auto& input = xdev->inputs[ARRAY_SIZE(kControllerInputNames)];
    input.active = have_hand && runtime_hand_has_optical_joints(io.last_hand, side, dev->left, io.hand_cfg.max_age_ms);
    input.timestamp = have_hand ? static_cast<int64_t>(io.last_hand.frame.source_timestamp_ns) : now_ns;
  }
  return XRT_SUCCESS;
}

void xrtd_destroy(struct xrt_device* xdev) {
  XrTrackingDevice* dev = from_xdev(xdev);
  if (dev != nullptr) {
    u_var_remove_root(dev);
    if (dev->owns_io) {
      delete dev->io;
      dev->io = nullptr;
    }
  }
  u_device_free(xdev);
}

void setup_hmd_display(struct xrt_device* xdev) {
  const HmdDisplayProfile profile = load_hmd_display_profile_from_env();

  struct u_device_simple_info info{};
  info.display.w_pixels = profile.window_width;
  info.display.h_pixels = profile.window_height;
  info.display.w_meters = profile.display_width_m;
  info.display.h_meters = profile.display_height_m;
  info.lens_horizontal_separation_meters = profile.ipd_m;
  info.lens_vertical_position_meters = profile.lens_vertical_position_m;
  // u_device_simple_info::fov contains one scalar FOV value per view.
  // XRT_MAX_VIEWS is 2 in Monado, so writing directional values to indices
  // 2 and 3 corrupts adjacent stack data. Use one conservative horizontal
  // bootstrap FOV per eye; the exact asymmetric directional FOV is applied to
  // xdev->hmd->distortion.fov after u_device_setup_split_side_by_side().
  static_assert(XRT_MAX_VIEWS >= 2, "XR tracking HMD requires two Monado views");
  info.fov[0] = std::max(profile.left_eye.fov_left, profile.left_eye.fov_right);
  info.fov[1] = std::max(profile.right_eye.fov_left, profile.right_eye.fov_right);

  std::fprintf(stderr,
               "[xr_tracking_monado] display layout=%s rotation=%u window=%ux%u "
               "render_per_eye=%ux%u refresh=%.3fHz ipd=%.6fm inter_lens=%.6fm "
               "screen_to_lens=%.6fm eye_to_lens=%.6fm\n",
               profile.layout.c_str(),
               profile.rotation_deg,
               profile.window_width,
               profile.window_height,
               profile.render_width,
               profile.render_height,
               static_cast<double>(profile.refresh_hz),
               static_cast<double>(profile.ipd_m),
               static_cast<double>(profile.inter_lens_distance_m),
               static_cast<double>(profile.screen_to_lens_distance_m),
               static_cast<double>(profile.eye_to_lens_distance_m));
  std::fprintf(stderr,
               "[xr_tracking_monado] left_eye center=(%.6f,%.6f) "
               "fov_rad=(left=%.6f right=%.6f up=%.6f down=%.6f) "
               "right_eye center=(%.6f,%.6f) "
               "fov_rad=(left=%.6f right=%.6f up=%.6f down=%.6f)\n",
               static_cast<double>(profile.left_eye.lens_center_u),
               static_cast<double>(profile.left_eye.lens_center_v),
               static_cast<double>(profile.left_eye.fov_left),
               static_cast<double>(profile.left_eye.fov_right),
               static_cast<double>(profile.left_eye.fov_up),
               static_cast<double>(profile.left_eye.fov_down),
               static_cast<double>(profile.right_eye.lens_center_u),
               static_cast<double>(profile.right_eye.lens_center_v),
               static_cast<double>(profile.right_eye.fov_left),
               static_cast<double>(profile.right_eye.fov_right),
               static_cast<double>(profile.right_eye.fov_up),
               static_cast<double>(profile.right_eye.fov_down));

  u_device_setup_split_side_by_side(xdev, &info);

  // MVP passthrough profile: no lens distortion.
  // Without this Monado tries to generate a mesh through xdev->compute_distortion,
  // but the default u_device no-impl function returns an error and compositor init fails.
  u_distortion_mesh_set_none(xdev);

  if (xdev->hmd != nullptr) {
    xdev->hmd->screens[0].nominal_frame_interval_ns = static_cast<uint64_t>(
        static_cast<double>(U_TIME_1S_IN_NS) / static_cast<double>(profile.refresh_hz));
    xdev->hmd->screens[0].w_pixels = static_cast<int>(profile.window_width);
    xdev->hmd->screens[0].h_pixels = static_cast<int>(profile.window_height);
    xdev->hmd->views[0].display.w_pixels = profile.render_width;
    xdev->hmd->views[0].display.h_pixels = profile.render_height;
    xdev->hmd->views[1].display.w_pixels = profile.render_width;
    xdev->hmd->views[1].display.h_pixels = profile.render_height;

    std::fprintf(stderr,
                 "[xr_tracking_monado] configured views left=%ux%u right=%ux%u\n",
                 xdev->hmd->views[0].display.w_pixels,
                 xdev->hmd->views[0].display.h_pixels,
                 xdev->hmd->views[1].display.w_pixels,
                 xdev->hmd->views[1].display.h_pixels);

    if (profile.layout == "top_bottom_vertical") {
      xdev->hmd->views[0].viewport.x_pixels = 0;
      xdev->hmd->views[0].viewport.y_pixels = 0;
      xdev->hmd->views[0].viewport.w_pixels = profile.window_width;
      xdev->hmd->views[0].viewport.h_pixels = profile.window_height / 2u;
      xdev->hmd->views[1].viewport.x_pixels = 0;
      xdev->hmd->views[1].viewport.y_pixels = profile.window_height / 2u;
      xdev->hmd->views[1].viewport.w_pixels = profile.window_width;
      xdev->hmd->views[1].viewport.h_pixels = profile.window_height / 2u;
    }

    // Rotation is represented in the Monado per-view output matrix. The
    // default profile is 0 degrees, preserving the historical identity matrix.
    const auto set_view_rotation = [](struct xrt_view* view, uint32_t degrees) {
      if (view == nullptr) return;
      switch (degrees) {
        case 90:
          view->rot.v[0] = 0.0f;  view->rot.v[1] = -1.0f;
          view->rot.v[2] = 1.0f;  view->rot.v[3] = 0.0f;
          break;
        case 180:
          view->rot.v[0] = -1.0f; view->rot.v[1] = 0.0f;
          view->rot.v[2] = 0.0f;  view->rot.v[3] = -1.0f;
          break;
        case 270:
          view->rot.v[0] = 0.0f;  view->rot.v[1] = 1.0f;
          view->rot.v[2] = -1.0f; view->rot.v[3] = 0.0f;
          break;
        default:
          view->rot.v[0] = 1.0f;  view->rot.v[1] = 0.0f;
          view->rot.v[2] = 0.0f;  view->rot.v[3] = 1.0f;
          break;
      }
    };
    set_view_rotation(&xdev->hmd->views[0], profile.rotation_deg);
    set_view_rotation(&xdev->hmd->views[1], profile.rotation_deg);

    // xrt_fov follows the OpenXR sign convention: left/down are negative,
    // right/up are positive. This permits asymmetric per-eye optics profiles.
    xdev->hmd->distortion.fov[0].angle_left = -profile.left_eye.fov_left;
    xdev->hmd->distortion.fov[0].angle_right = profile.left_eye.fov_right;
    xdev->hmd->distortion.fov[0].angle_up = profile.left_eye.fov_up;
    xdev->hmd->distortion.fov[0].angle_down = -profile.left_eye.fov_down;
    xdev->hmd->distortion.fov[1].angle_left = -profile.right_eye.fov_left;
    xdev->hmd->distortion.fov[1].angle_right = profile.right_eye.fov_right;
    xdev->hmd->distortion.fov[1].angle_up = profile.right_eye.fov_up;
    xdev->hmd->distortion.fov[1].angle_down = -profile.right_eye.fov_down;
  }
}

XrRuntimeIo* create_runtime_io() {
  auto* io = new XrRuntimeIo();
  const auto transport = env_string("XR_MONADO_TRANSPORT", "shm");
  xr::openvr_driver::RuntimeTcpConfig tcp;
  tcp.host = env_string("XR_MONADO_TCP_HOST", "127.0.0.1");
  tcp.port = static_cast<uint16_t>(env_u32("XR_MONADO_TCP_PORT", 45810, 1, 65535));
  tcp.reconnect_ms = env_u32("XR_MONADO_TCP_RECONNECT_MS", 500, 10, 60000);
  tcp.timeout_ms = env_u32("XR_MONADO_TCP_TIMEOUT_MS", 1000, 50, 60000);
  io->hmd_cfg.transport = io->controller_cfg.transport = io->hand_cfg.transport = transport;
  io->hmd_cfg.tcp = io->controller_cfg.tcp = io->hand_cfg.tcp = tcp;
  io->hmd_cfg.shm_name = env_string("XR_MONADO_HMD_SHM", "runtime_hmd_pose");
  io->hmd_cfg.max_age_ms = kDefaultPoseMaxAgeMs;
  io->controller_cfg.shm_name = env_string("XR_MONADO_CONTROLLER_SHM", "runtime_controller_state");
  io->controller_cfg.max_age_ms = kDefaultControllerMaxAgeMs;
  io->hand_cfg.shm_name = env_string("XR_MONADO_HAND_SHM", "runtime_hand_tracking");
  io->hand_cfg.max_age_ms = kDefaultHandMaxAgeMs;
  io->hmd_reader = create_runtime_pose_reader(io->hmd_cfg);
  io->controller_reader = create_runtime_controller_state_reader(io->controller_cfg);
  io->hand_reader = create_runtime_hand_reader(io->hand_cfg);
  if (!io->hmd_reader || !io->controller_reader || !io->hand_reader) {
    delete io;
    throw std::runtime_error("unsupported XR_MONADO_TRANSPORT: " + transport);
  }
  std::fprintf(stderr, "[xr_tracking_monado] transport=%s endpoint=%s:%u\n",
               transport.c_str(), tcp.host.c_str(), unsigned(tcp.port));
  return io;
}

}  // namespace
}  // namespace xr::monado_driver

extern "C" struct xrt_device* xr_tracking_runtime_hmd_create(void) {
  using namespace xr::monado_driver;

  auto* dev = U_DEVICE_ALLOCATE(XrTrackingDevice, U_DEVICE_ALLOC_HMD, 1, 0);
  if (dev == nullptr) return nullptr;

  dev->is_hmd = true;
  dev->is_controller = false;
  dev->io = create_runtime_io();
  dev->owns_io = true;

  struct xrt_device* xdev = &dev->base;
  sanitize_tracking_origin_metadata(xdev);
  xdev->name = XRT_DEVICE_GENERIC_HMD;
  xdev->device_type = XRT_DEVICE_TYPE_HMD;
  xdev->supported.orientation_tracking = true;
  xdev->supported.position_tracking = true;
  std::snprintf(xdev->str, sizeof(xdev->str), "%s HMD", kDriverName);
  std::snprintf(xdev->serial, sizeof(xdev->serial), "xr-tracking-monado-hmd-001");
  xdev->inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
  xdev->inputs[0].active = true;
  xdev->update_inputs = u_device_noop_update_inputs;
  u_device_populate_function_pointers(xdev, xrtd_get_tracked_pose, xrtd_destroy);
  setup_hmd_display(xdev);
  // OpenXR clients call xrLocateViews(), which reaches xrt_device_get_view_poses().
  // Use Monado's generic SBS helper but sanitize/fallback the returned poses so
  // clients do not fail validation while xr_runtime_adapter is not publishing a
  // fresh HMD pose yet.
  xdev->get_view_poses = xrtd_get_view_poses;
  u_var_add_root(dev, "XR Tracking Runtime HMD", true);
  return xdev;
}

extern "C" struct xrt_device* xr_tracking_runtime_controller_create(struct xrt_device* hmd_xdev, bool left) {
  using namespace xr::monado_driver;

  XrRuntimeIo* io = nullptr;
  if (hmd_xdev != nullptr) {
    XrTrackingDevice* hmd = from_xdev(hmd_xdev);
    if (hmd != nullptr) io = hmd->io;
  }
  const bool owns_io = io == nullptr;
  if (owns_io) io = create_runtime_io();
  const bool hand_tracking_enabled = env_truthy("XR_MONADO_HAND_TRACKING");
  const uint32_t kInputCount = ARRAY_SIZE(kControllerInputNames) + (hand_tracking_enabled ? 1u : 0u);
  auto* dev = U_DEVICE_ALLOCATE(XrTrackingDevice, U_DEVICE_ALLOC_NO_FLAGS, kInputCount, 0);
  if (dev == nullptr) {
    if (owns_io) delete io;
    return nullptr;
  }

  dev->is_hmd = false;
  dev->is_controller = true;
  dev->left = left;
  dev->hand_tracking_enabled = hand_tracking_enabled;
  dev->input_cfg = load_controller_input_config();
  dev->io = io;
  dev->owns_io = owns_io;

  struct xrt_device* xdev = &dev->base;
  sanitize_tracking_origin_metadata(xdev);
  xdev->name = XRT_DEVICE_TOUCH_CONTROLLER;
  xdev->device_type = left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
  xdev->supported.orientation_tracking = true;
  xdev->supported.position_tracking = true;
  xdev->supported.hand_tracking = hand_tracking_enabled;
  configure_controller_binding_profiles(dev);

  std::snprintf(xdev->str, sizeof(xdev->str), "%s %s controller", kDriverName, left ? "left" : "right");
  std::snprintf(xdev->serial, sizeof(xdev->serial), "xr-tracking-monado-%s-controller-001", left ? "left" : "right");

  for (uint32_t i = 0; i < ARRAY_SIZE(kControllerInputNames); ++i) {
    xdev->inputs[i].name = kControllerInputNames[i];
  }
  if (hand_tracking_enabled) {
    xdev->inputs[ARRAY_SIZE(kControllerInputNames)].name =
        left ? XRT_INPUT_HT_UNOBSTRUCTED_LEFT : XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
  }

  u_device_populate_function_pointers(xdev, xrtd_get_tracked_pose, xrtd_destroy);
  // populate_function_pointers installs a noop update_inputs callback.
  xdev->update_inputs = xrtd_update_inputs;
  if (hand_tracking_enabled) {
    xdev->get_hand_tracking = xrtd_get_hand_tracking;
    xdev->begin_feature = xrtd_hand_tracking_feature;
    xdev->end_feature = xrtd_hand_tracking_feature;
  }
  std::fprintf(stderr, "[xr_tracking_monado] %s controller profile=touch hand_tracking=%s\n",
               left ? "left" : "right", hand_tracking_enabled ? "enabled" : "disabled");
  std::fprintf(stderr, "[xr_tracking_monado] controller input left_ab_xy_alias=%d trackpad_click_from_touch=%d axis_threshold=%.3f click_threshold=%.3f\n",
               dev->input_cfg.left_ab_xy_alias, dev->input_cfg.trackpad_click_from_touch,
               dev->input_cfg.axis_activity_threshold, dev->input_cfg.analog_click_threshold);
  u_var_add_root(dev, left ? "XR Tracking Runtime Left Controller" : "XR Tracking Runtime Right Controller", true);
  return xdev;
}
