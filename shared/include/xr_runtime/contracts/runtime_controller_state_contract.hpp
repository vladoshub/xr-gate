#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <xr_runtime/contracts/controller_input_contract.hpp>

namespace xr_runtime {

enum class RuntimeControllerMode : uint32_t {
  HAND_TRACKING_WITH_BUTTON_PRIORITY = 1,
  HAND_TRACKING_CONTROLLER_BUTTONS_ONLY = 2,
  HAND_POSITION_CONTROLLER_BUTTONS_STATIC_ORIENTATION = 3,
  CONTROLLER_ONLY_HMD_RELATIVE_POSE = 4,
  CONTROLLER_ONLY_POSE_INVALID = 5,
};

inline RuntimeControllerMode parse_runtime_controller_mode(const std::string& v) {
  if (v == "hand_tracking_with_button_priority") {
    return RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY;
  }
  if (v == "hand_tracking_controller_buttons_only") {
    return RuntimeControllerMode::HAND_TRACKING_CONTROLLER_BUTTONS_ONLY;
  }
  if (v == "hand_position_controller_buttons_static_orientation") {
    return RuntimeControllerMode::HAND_POSITION_CONTROLLER_BUTTONS_STATIC_ORIENTATION;
  }
  if (v == "controller_only_hmd_relative_pose") {
    return RuntimeControllerMode::CONTROLLER_ONLY_HMD_RELATIVE_POSE;
  }
  if (v == "controller_only_pose_invalid") {
    return RuntimeControllerMode::CONTROLLER_ONLY_POSE_INVALID;
  }

  // Backward-compatible aliases from the old controller-input composition flag.
  if (v == "hand_plus_controller") {
    return RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY;
  }
  if (v == "controller_buttons_only") {
    return RuntimeControllerMode::HAND_TRACKING_CONTROLLER_BUTTONS_ONLY;
  }
  if (v == "hand_tracking_only") {
    return RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY;
  }

  throw std::runtime_error(
      "--runtime-controller-mode must be one of: "
      "hand_tracking_with_button_priority, "
      "hand_tracking_controller_buttons_only, "
      "hand_position_controller_buttons_static_orientation, "
      "controller_only_hmd_relative_pose, "
      "controller_only_pose_invalid");
}

inline const char* runtime_controller_mode_name(RuntimeControllerMode mode) {
  switch (mode) {
    case RuntimeControllerMode::HAND_TRACKING_WITH_BUTTON_PRIORITY:
      return "hand_tracking_with_button_priority";
    case RuntimeControllerMode::HAND_TRACKING_CONTROLLER_BUTTONS_ONLY:
      return "hand_tracking_controller_buttons_only";
    case RuntimeControllerMode::HAND_POSITION_CONTROLLER_BUTTONS_STATIC_ORIENTATION:
      return "hand_position_controller_buttons_static_orientation";
    case RuntimeControllerMode::CONTROLLER_ONLY_HMD_RELATIVE_POSE:
      return "controller_only_hmd_relative_pose";
    case RuntimeControllerMode::CONTROLLER_ONLY_POSE_INVALID:
      return "controller_only_pose_invalid";
  }
  return "unknown";
}

enum RuntimeControllerStateFlags : uint32_t {
  RUNTIME_CONTROLLER_CONNECTED = 1u << 0,
  RUNTIME_CONTROLLER_POSE_VALID = 1u << 1,
  RUNTIME_CONTROLLER_INPUT_VALID = 1u << 2,
  RUNTIME_CONTROLLER_TRACKED = 1u << 3,
  RUNTIME_CONTROLLER_INPUT_STALE = 1u << 4,
  RUNTIME_CONTROLLER_POSE_STALE = 1u << 5,
  RUNTIME_CONTROLLER_SYNTHETIC_POSE = 1u << 6,
  RUNTIME_CONTROLLER_HMD_RELATIVE = 1u << 7,
  RUNTIME_CONTROLLER_POSE_INVALID = 1u << 8,
};

enum RuntimeControllerSourceBits : uint32_t {
  RUNTIME_CONTROLLER_SOURCE_HAND_POSITION = 1u << 0,
  RUNTIME_CONTROLLER_SOURCE_HAND_ORIENTATION = 1u << 1,
  RUNTIME_CONTROLLER_SOURCE_STATIC_ORIENTATION = 1u << 2,
  RUNTIME_CONTROLLER_SOURCE_HMD_RELATIVE_POSE = 1u << 3,
  RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT = 1u << 4,
  RUNTIME_CONTROLLER_SOURCE_HAND_GESTURES = 1u << 5,
  RUNTIME_CONTROLLER_SOURCE_SYNTHETIC_POSE = 1u << 6,
  RUNTIME_CONTROLLER_SOURCE_POSE_INVALID = 1u << 7,
  RUNTIME_CONTROLLER_SOURCE_LAST_GOOD_HAND_POSE = 1u << 8,
};

enum RuntimeControllerFrameFlags : uint32_t {
  RUNTIME_CONTROLLER_FRAME_LEFT_CONNECTED = 1u << 0,
  RUNTIME_CONTROLLER_FRAME_RIGHT_CONNECTED = 1u << 1,
  RUNTIME_CONTROLLER_FRAME_LEFT_POSE_VALID = 1u << 2,
  RUNTIME_CONTROLLER_FRAME_RIGHT_POSE_VALID = 1u << 3,
  RUNTIME_CONTROLLER_FRAME_LEFT_INPUT_VALID = 1u << 4,
  RUNTIME_CONTROLLER_FRAME_RIGHT_INPUT_VALID = 1u << 5,
};

constexpr uint32_t RUNTIME_CONTROLLER_STATE_MAGIC = 0x52544331u; // "RTC1"
constexpr const char* RUNTIME_CONTROLLER_STATE_FORMAT_NAME = "RUNTIME_CONTROLLER_STATE_V1";
constexpr uint32_t RUNTIME_CONTROLLER_STATE_FORMAT_VERSION = 1;

#pragma pack(push, 1)
struct RuntimeControllerSideStateV1 {
  uint32_t flags = 0;
  uint32_t role = CONTROLLER_SIDE_UNKNOWN;
  uint32_t source_mask = 0;
  uint32_t reserved0 = 0;

  uint64_t source_device_hash = 0;
  uint64_t last_input_ns = 0;
  uint64_t last_pose_ns = 0;

  float position[3] = {0.0f, 0.0f, 0.0f};
  float orientation_xyzw[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  float linear_velocity[3] = {0.0f, 0.0f, 0.0f};
  float angular_velocity[3] = {0.0f, 0.0f, 0.0f};

  uint64_t buttons = 0;
  uint64_t touches = 0;
  uint64_t changed_buttons = 0;

  float trigger = 0.0f;
  float grip = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;

  uint32_t press_counters[32] = {};
  uint32_t release_counters[32] = {};

  char debug_source[64] = {};
};

struct RuntimeControllerStateFrameV1 {
  uint32_t magic = RUNTIME_CONTROLLER_STATE_MAGIC;
  uint32_t version = RUNTIME_CONTROLLER_STATE_FORMAT_VERSION;
  uint32_t size_bytes = sizeof(RuntimeControllerStateFrameV1);
  uint32_t flags = 0;

  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;

  RuntimeControllerSideStateV1 left;
  RuntimeControllerSideStateV1 right;
};
#pragma pack(pop)

static_assert(sizeof(RuntimeControllerSideStateV1) == 452,
              "RuntimeControllerSideStateV1 must stay ABI-stable at 452 bytes");
static_assert(sizeof(RuntimeControllerStateFrameV1) == 936,
              "RuntimeControllerStateFrameV1 must stay ABI-stable at 936 bytes");

constexpr uint32_t RUNTIME_CONTROLLER_STATE_PAYLOAD_SIZE = sizeof(RuntimeControllerStateFrameV1);

}  // namespace xr_runtime
