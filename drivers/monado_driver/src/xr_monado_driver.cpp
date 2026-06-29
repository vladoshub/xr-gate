// XR Tracking Monado runtime driver.
//
// This device layer consumes the runtime-ready streams produced by
// xr_runtime_adapter and exposes one HMD plus two controller devices to Monado.

#include "xr_monado_driver/runtime_readers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

  XrRuntimeIo* io = nullptr;
  bool owns_io = false;
};

static struct xrt_binding_input_pair wmr_to_simple_inputs[] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_profile wmr_binding_profiles[] = {
    {XRT_DEVICE_SIMPLE_CONTROLLER, wmr_to_simple_inputs, ARRAY_SIZE(wmr_to_simple_inputs), nullptr, 0},
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

struct HmdDisplayProfile {
  uint32_t render_width = kDefaultRenderWidth;
  uint32_t render_height = kDefaultRenderHeight;
  uint32_t window_width = kDefaultWindowWidth;
  uint32_t window_height = kDefaultWindowHeight;
  float display_width_m = kDefaultDisplayWidthM;
  float display_height_m = kDefaultDisplayHeightM;
  float ipd_m = kDefaultIpdM;
  float lens_vertical_position_m = kDefaultLensVerticalPositionM;
  float fov_left = kDefaultFov;
  float fov_right = kDefaultFov;
  float fov_up = kDefaultFov;
  float fov_down = kDefaultFov;
  float refresh_hz = kDefaultRefreshHz;
};

HmdDisplayProfile load_hmd_display_profile_from_env() {
  HmdDisplayProfile profile{};

  profile.render_width = env_u32("XR_MONADO_RENDER_WIDTH", kDefaultRenderWidth, 64, 16384);
  profile.render_height = env_u32("XR_MONADO_RENDER_HEIGHT", kDefaultRenderHeight, 64, 16384);
  profile.window_width = env_u32("XR_MONADO_WINDOW_WIDTH", kDefaultWindowWidth, 64, 32768);
  profile.window_height = env_u32("XR_MONADO_WINDOW_HEIGHT", kDefaultWindowHeight, 64, 32768);
  profile.display_width_m = env_float("XR_MONADO_DISPLAY_WIDTH_M", kDefaultDisplayWidthM, 0.001f, 10.0f);
  profile.display_height_m = env_float("XR_MONADO_DISPLAY_HEIGHT_M", kDefaultDisplayHeightM, 0.001f, 10.0f);
  profile.ipd_m = env_float("XR_MONADO_IPD_M", kDefaultIpdM, 0.001f, 1.0f);
  profile.lens_vertical_position_m =
      env_float("XR_MONADO_LENS_VERTICAL_POSITION_M", kDefaultLensVerticalPositionM, -10.0f, 10.0f);
  profile.fov_left = env_float("XR_MONADO_FOV_LEFT", kDefaultFov, 0.01f, 10.0f);
  profile.fov_right = env_float("XR_MONADO_FOV_RIGHT", kDefaultFov, 0.01f, 10.0f);
  profile.fov_up = env_float("XR_MONADO_FOV_UP", kDefaultFov, 0.01f, 10.0f);
  profile.fov_down = env_float("XR_MONADO_FOV_DOWN", kDefaultFov, 0.01f, 10.0f);
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

struct xrt_quat quat_multiply(const struct xrt_quat& a, const struct xrt_quat& b) {
  return quatf(
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w);
}

struct xrt_quat quat_conjugate(const struct xrt_quat& q) {
  return quatf(q.w, -q.x, -q.y, -q.z);
}

struct xrt_vec3 quat_rotate_vec(const struct xrt_quat& q, const struct xrt_vec3& v) {
  const struct xrt_quat p = quatf(0.0f, v.x, v.y, v.z);
  const struct xrt_quat r = quat_multiply(quat_multiply(q, p), quat_conjugate(q));
  return vec3(r.x, r.y, r.z);
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

void relation_from_hand_side_hmd_relative(const xr_tracking::HandSideF32V2& side,
                                          const xr_runtime::RuntimeHmdPoseF64V1& hmd,
                                          struct xrt_space_relation* out_relation) {
  const struct xrt_vec3 hmd_pos = vec3(static_cast<float>(hmd.px),
                                      static_cast<float>(hmd.py),
                                      static_cast<float>(hmd.pz));
  const struct xrt_quat hmd_rot = quatf(static_cast<float>(hmd.qw),
                                       static_cast<float>(hmd.qx),
                                       static_cast<float>(hmd.qy),
                                       static_cast<float>(hmd.qz));

  const struct xrt_vec3 hand_rel = vec3(side.controller_px, side.controller_py, side.controller_pz);
  const struct xrt_vec3 hand_rel_rotated = quat_rotate_vec(hmd_rot, hand_rel);
  const struct xrt_vec3 position = vec3(hmd_pos.x + hand_rel_rotated.x,
                                       hmd_pos.y + hand_rel_rotated.y,
                                       hmd_pos.z + hand_rel_rotated.z);

  const struct xrt_quat hand_rel_rot = quatf(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz);
  const struct xrt_quat orientation = quat_multiply(hmd_rot, hand_rel_rot);

  struct xrt_vec3 linear_velocity{};
  struct xrt_vec3 angular_velocity{};
  const struct xrt_vec3* linear_velocity_ptr = nullptr;
  const struct xrt_vec3* angular_velocity_ptr = nullptr;

  if ((side.flags & xr_tracking::HAND_LINEAR_VELOCITY_VALID) != 0u) {
    linear_velocity = quat_rotate_vec(hmd_rot, vec3(side.vx, side.vy, side.vz));
    linear_velocity_ptr = &linear_velocity;
  }
  if ((side.flags & xr_tracking::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
    angular_velocity = quat_rotate_vec(hmd_rot, vec3(side.wx, side.wy, side.wz));
    angular_velocity_ptr = &angular_velocity;
  }

  relation_from_pose_velocity(out_relation, position, orientation, linear_velocity_ptr, angular_velocity_ptr);
}

bool controller_pose_input_name(enum xrt_input_name name) {
  switch (name) {
    case XRT_INPUT_WMR_GRIP_POSE:
    case XRT_INPUT_WMR_AIM_POSE:
    case XRT_INPUT_GENERIC_GRIP_POSE:
    case XRT_INPUT_GENERIC_AIM_POSE:
    case XRT_INPUT_SIMPLE_GRIP_POSE:
    case XRT_INPUT_SIMPLE_AIM_POSE:
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
        return XRT_SUCCESS;
      }
    }

    if (read_hand(io) && (io.have_hmd || read_hmd(io))) {
      const xr_tracking::HandSideF32V2& hand_side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
      if (runtime_hand_side_is_valid(io.last_hand.frame, hand_side, dev->left)) {
        relation_from_hand_side_hmd_relative(hand_side, io.last_hmd.pose, out_relation);
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

void clear_controller_inputs(struct xrt_device* xdev, int64_t now_ns) {
  for (uint32_t i = 0; i < xdev->input_count; ++i) {
    xdev->inputs[i].active = false;
    xdev->inputs[i].timestamp = now_ns;
    std::memset(&xdev->inputs[i].value, 0, sizeof(xdev->inputs[i].value));
  }
}

void publish_controller_inputs(struct xrt_device* xdev,
                               float trigger,
                               float grip,
                               float stick_x,
                               float stick_y,
                               bool menu_click,
                               bool stick_click,
                               int64_t now_ns) {
  for (uint32_t i = 0; i < xdev->input_count; ++i) {
    xdev->inputs[i].active = true;
    xdev->inputs[i].timestamp = now_ns;
    std::memset(&xdev->inputs[i].value, 0, sizeof(xdev->inputs[i].value));
  }

  const float trigger_value = std::clamp(trigger, 0.0f, 1.0f);
  const float grip_value = std::clamp(grip, 0.0f, 1.0f);

  // WMR exposes trigger as a value and squeeze as a click in this Monado path.
  // If the upstream runtime controller stream only reports digital button bits,
  // the caller promotes those bits into trigger/grip values before reaching this
  // helper. This mirrors the working SteamVR/OpenVR driver behavior.
  xdev->inputs[2].value.vec1.x = trigger_value;                           // WMR trigger value
  xdev->inputs[3].value.boolean = grip_value >= 0.5f;                     // WMR squeeze click
  xdev->inputs[4].value.vec2.x = std::clamp(stick_x, -1.0f, 1.0f);        // WMR thumbstick
  xdev->inputs[4].value.vec2.y = std::clamp(stick_y, -1.0f, 1.0f);
  xdev->inputs[5].value.boolean = stick_click;
  xdev->inputs[6].value.boolean = menu_click;
  xdev->inputs[7].value.boolean = stick_click;                            // Trackpad click fallback
  xdev->inputs[8].value.boolean = stick_click;                            // Trackpad touch fallback
  xdev->inputs[9].value.vec2.x = xdev->inputs[4].value.vec2.x;            // Trackpad mirrors stick
  xdev->inputs[9].value.vec2.y = xdev->inputs[4].value.vec2.y;
}

xrt_result_t xrtd_update_inputs(struct xrt_device* xdev) {
  XrTrackingDevice* dev = from_xdev(xdev);
  if (dev == nullptr || dev->io == nullptr || !dev->is_controller) {
    return XRT_SUCCESS;
  }

  const int64_t now_ns = monotonic_now_ns();
  XrRuntimeIo& io = *dev->io;
  const bool have_controller = read_controller_state(io);
  const bool have_hand = !have_controller && read_hand(io);

  if (have_controller) {
    const auto& side = dev->left ? io.last_controller.frame.left : io.last_controller.frame.right;
    const uint64_t buttons = side.buttons;
    const float trigger = std::clamp(side.trigger, 0.0f, 1.0f);
    const float grip = std::clamp(side.grip, 0.0f, 1.0f);
    float stick_x = std::clamp(side.thumbstick_x, -1.0f, 1.0f);
    float stick_y = std::clamp(side.thumbstick_y, -1.0f, 1.0f);

    const bool runtime_has_stick_axes =
    std::abs(stick_x) >= 0.05f || std::abs(stick_y) >= 0.05f;
// If xr_runtime_adapter already supplied axes (including HMD-space
// remapped D-pad axes), preserve them. Only synthesize axes from raw
// D-pad button bits when the runtime stream did not provide axes.
if (!runtime_has_stick_axes) {
  if (std::abs(stick_x) < 0.05f) {
    if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT) != 0ull) stick_x = -1.0f;
      if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT) != 0ull) stick_x = 1.0f;
  }
  if (std::abs(stick_y) < 0.05f) {
    if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN) != 0ull) stick_y = -1.0f;
      if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_UP) != 0ull) stick_y = 1.0f;
  }
}

    const bool trigger_click = trigger >= 0.55f ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_TRIGGER) != 0ull;
    const bool grip_click = grip >= 0.55f ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_GRIP) != 0ull;
    const bool thumbstick_click =
        (buttons & xr_runtime::CONTROLLER_BUTTON_THUMBSTICK) != 0ull ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_CENTER) != 0ull;
    const bool menu_click =
        (buttons & xr_runtime::CONTROLLER_BUTTON_MENU) != 0ull ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_SYSTEM) != 0ull;

    // Monado's WMR/simple binding path has no separate trigger-click input in
    // this device layout, so promote digital trigger/grip bits into their
    // corresponding analog/click values. This is the important part for
    // button-only controller_override modes.
    const float trigger_value = trigger_click ? std::max(trigger, 1.0f) : trigger;
    const float grip_value = grip_click ? std::max(grip, 1.0f) : grip;

    publish_controller_inputs(xdev, trigger_value, grip_value, stick_x, stick_y, menu_click, thumbstick_click, now_ns);
    return XRT_SUCCESS;
  }

  if (have_hand) {
    const xr_tracking::HandSideF32V2& hand_side = dev->left ? io.last_hand.frame.left : io.last_hand.frame.right;
    if (runtime_hand_side_is_valid(io.last_hand.frame, hand_side, dev->left)) {
      const float trigger = (hand_side.flags & xr_tracking::HAND_PINCH_VALID) != 0u
                                ? std::clamp(hand_side.pinch_strength, 0.0f, 1.0f)
                                : 0.0f;
      const float grip = (hand_side.flags & xr_tracking::HAND_GRAB_VALID) != 0u
                             ? std::clamp(hand_side.grab_strength, 0.0f, 1.0f)
                             : 0.0f;
      publish_controller_inputs(xdev, trigger, grip, 0.0f, 0.0f, false, false, now_ns);
      return XRT_SUCCESS;
    }
  }

  clear_controller_inputs(xdev, now_ns);
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
  info.fov[0] = profile.fov_left;
  info.fov[1] = profile.fov_right;
  info.fov[2] = profile.fov_up;
  info.fov[3] = profile.fov_down;

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
  }
}

XrRuntimeIo* create_runtime_io() {
  auto* io = new XrRuntimeIo();
  io->hmd_cfg.shm_name = "runtime_hmd_pose";
  io->hmd_cfg.max_age_ms = kDefaultPoseMaxAgeMs;
  io->controller_cfg.shm_name = "runtime_controller_state";
  io->controller_cfg.max_age_ms = kDefaultControllerMaxAgeMs;
  io->hand_cfg.shm_name = "runtime_hand_tracking";
  io->hand_cfg.max_age_ms = kDefaultHandMaxAgeMs;
  io->hmd_reader = create_runtime_pose_reader(io->hmd_cfg);
  io->controller_reader = create_runtime_controller_state_reader(io->controller_cfg);
  io->hand_reader = create_runtime_hand_reader(io->hand_cfg);
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
  if (io == nullptr) io = create_runtime_io();

  constexpr uint32_t kInputCount = 10;
  auto* dev = U_DEVICE_ALLOCATE(XrTrackingDevice, U_DEVICE_ALLOC_NO_FLAGS, kInputCount, 0);
  if (dev == nullptr) return nullptr;

  dev->is_hmd = false;
  dev->is_controller = true;
  dev->left = left;
  dev->io = io;
  dev->owns_io = (hmd_xdev == nullptr);

  struct xrt_device* xdev = &dev->base;
  sanitize_tracking_origin_metadata(xdev);
  xdev->name = XRT_DEVICE_WMR_CONTROLLER;
  xdev->device_type = left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
  xdev->supported.orientation_tracking = true;
  xdev->supported.position_tracking = true;
  xdev->binding_profiles = wmr_binding_profiles;
  xdev->binding_profile_count = ARRAY_SIZE(wmr_binding_profiles);

  std::snprintf(xdev->str, sizeof(xdev->str), "%s %s controller", kDriverName, left ? "left" : "right");
  std::snprintf(xdev->serial, sizeof(xdev->serial), "xr-tracking-monado-%s-controller-001", left ? "left" : "right");

  xdev->inputs[0].name = XRT_INPUT_WMR_GRIP_POSE;
  xdev->inputs[1].name = XRT_INPUT_WMR_AIM_POSE;
  xdev->inputs[2].name = XRT_INPUT_WMR_TRIGGER_VALUE;
  xdev->inputs[3].name = XRT_INPUT_WMR_SQUEEZE_CLICK;
  xdev->inputs[4].name = XRT_INPUT_WMR_THUMBSTICK;
  xdev->inputs[5].name = XRT_INPUT_WMR_THUMBSTICK_CLICK;
  xdev->inputs[6].name = XRT_INPUT_WMR_MENU_CLICK;
  xdev->inputs[7].name = XRT_INPUT_WMR_TRACKPAD_CLICK;
  xdev->inputs[8].name = XRT_INPUT_WMR_TRACKPAD_TOUCH;
  xdev->inputs[9].name = XRT_INPUT_WMR_TRACKPAD;

  xdev->update_inputs = xrtd_update_inputs;
  u_device_populate_function_pointers(xdev, xrtd_get_tracked_pose, xrtd_destroy);
  u_var_add_root(dev, left ? "XR Tracking Runtime Left Controller" : "XR Tracking Runtime Right Controller", true);
  return xdev;
}
