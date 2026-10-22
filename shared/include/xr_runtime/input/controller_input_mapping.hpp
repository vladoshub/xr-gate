#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>

namespace xr_runtime {

// These are virtual controller semantics, independent of the physical backend.
struct ControllerInputMappingConfig {
  bool left_ab_xy_alias = true;
  bool trackpad_click_from_touch = false;
  bool expose_system_button = false;
  float axis_activity_threshold = 0.05f;
  float analog_click_threshold = 0.55f;
};

struct ControllerInputState {
  float trigger = 0.0f, grip = 0.0f, x = 0.0f, y = 0.0f;
  bool trigger_click = false, trigger_touch = false;
  bool grip_click = false, grip_touch = false;
  bool stick_click = false, stick_touch = false;
  bool trackpad_click = false, trackpad_touch = false;
  bool menu = false, system = false;
  bool a = false, b = false, x_button = false, y_button = false;
  bool a_touch = false, b_touch = false, x_touch = false, y_touch = false;
};

inline float finite_clamp(float value, float low, float high) {
  return std::isfinite(value) ? std::clamp(value, low, high) : 0.0f;
}

inline ControllerInputState map_controller_input(
    const xr_runtime::RuntimeControllerSideStateV1* side, bool left,
    const ControllerInputMappingConfig& cfg) {
  ControllerInputState out;
  constexpr uint32_t required = xr_runtime::RUNTIME_CONTROLLER_CONNECTED |
                                xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
  if (side == nullptr || (side->flags & required) != required ||
      (side->flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_STALE) != 0u) return out;

  const auto pressed = [side](uint64_t mask) { return (side->buttons & mask) != 0; };
  const auto touched = [side](uint64_t mask) { return ((side->touches | side->buttons) & mask) != 0; };
  const float axis_threshold = std::isfinite(cfg.axis_activity_threshold)
      ? std::clamp(cfg.axis_activity_threshold, 0.001f, 1.0f) : 0.05f;
  const float click_threshold = std::isfinite(cfg.analog_click_threshold)
      ? std::clamp(cfg.analog_click_threshold, 0.001f, 1.0f) : 0.55f;

  out.trigger = finite_clamp(side->trigger, 0.0f, 1.0f);
  out.grip = finite_clamp(side->grip, 0.0f, 1.0f);
  // A digital-only source still supplies a useful analog pull to bindings.
  if (out.trigger == 0.0f && pressed(CONTROLLER_BUTTON_TRIGGER)) out.trigger = 1.0f;
  if (out.grip == 0.0f && pressed(CONTROLLER_BUTTON_GRIP)) out.grip = 1.0f;
  out.trigger_click = pressed(CONTROLLER_BUTTON_TRIGGER) || out.trigger >= click_threshold;
  out.grip_click = pressed(CONTROLLER_BUTTON_GRIP) || out.grip >= click_threshold;
  out.trigger_touch = touched(CONTROLLER_BUTTON_TRIGGER) || out.trigger > 0.0f;
  out.grip_touch = touched(CONTROLLER_BUTTON_GRIP) || out.grip > 0.0f;

  out.x = finite_clamp(side->thumbstick_x, -1.0f, 1.0f);
  out.y = finite_clamp(side->thumbstick_y, -1.0f, 1.0f);
  if (std::abs(out.x) < axis_threshold) {
    out.x = static_cast<float>(pressed(CONTROLLER_BUTTON_DPAD_RIGHT)) -
            static_cast<float>(pressed(CONTROLLER_BUTTON_DPAD_LEFT));
  }
  if (std::abs(out.y) < axis_threshold) {
    out.y = static_cast<float>(pressed(CONTROLLER_BUTTON_DPAD_UP)) -
            static_cast<float>(pressed(CONTROLLER_BUTTON_DPAD_DOWN));
  }
  out.stick_click = pressed(CONTROLLER_BUTTON_THUMBSTICK | CONTROLLER_BUTTON_DPAD_CENTER);
  out.stick_touch = touched(CONTROLLER_BUTTON_THUMBSTICK | CONTROLLER_BUTTON_DPAD_CENTER |
      CONTROLLER_BUTTON_DPAD_UP | CONTROLLER_BUTTON_DPAD_DOWN |
      CONTROLLER_BUTTON_DPAD_LEFT | CONTROLLER_BUTTON_DPAD_RIGHT) ||
      std::abs(out.x) >= axis_threshold || std::abs(out.y) >= axis_threshold;
  out.trackpad_touch = out.stick_touch;
  // Direction, capacitive touch and displacement never imply a physical click.
  // The opt-in switch is only for restoring older custom Vive bindings.
  out.trackpad_click = out.stick_click || (cfg.trackpad_click_from_touch && out.stick_touch);

  out.menu = pressed(CONTROLLER_BUTTON_MENU);
  out.system = cfg.expose_system_button && pressed(CONTROLLER_BUTTON_SYSTEM);
  out.a = pressed(CONTROLLER_BUTTON_A);
  out.b = pressed(CONTROLLER_BUTTON_B);
  out.x_button = pressed(CONTROLLER_BUTTON_X);
  out.y_button = pressed(CONTROLLER_BUTTON_Y);
  out.a_touch = touched(CONTROLLER_BUTTON_A);
  out.b_touch = touched(CONTROLLER_BUTTON_B);
  out.x_touch = touched(CONTROLLER_BUTTON_X);
  out.y_touch = touched(CONTROLLER_BUTTON_Y);
  if (left && cfg.left_ab_xy_alias) {
    out.a = out.x_button = out.a || out.x_button;
    out.b = out.y_button = out.b || out.y_button;
    out.a_touch = out.x_touch = out.a_touch || out.x_touch;
    out.b_touch = out.y_touch = out.b_touch || out.y_touch;
  }
  return out;
}

}  // namespace xr_runtime
