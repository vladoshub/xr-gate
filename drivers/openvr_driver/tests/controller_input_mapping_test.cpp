#include "controller_input_mapping.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace xr::openvr_driver;
using namespace xr_runtime;

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

RuntimeControllerSideStateV1 sample() {
  RuntimeControllerSideStateV1 side;
  side.flags = RUNTIME_CONTROLLER_CONNECTED | RUNTIME_CONTROLLER_INPUT_VALID;
  return side;
}

void neutral(const ControllerInputState& s) {
  require(s.trigger == 0 && s.grip == 0 && s.x == 0 && s.y == 0 &&
      !s.trigger_click && !s.trigger_touch && !s.grip_click && !s.grip_touch &&
      !s.stick_click && !s.stick_touch && !s.trackpad_click && !s.trackpad_touch &&
      !s.a && !s.b && !s.x_button && !s.y_button &&
      !s.a_touch && !s.b_touch && !s.x_touch && !s.y_touch && !s.menu && !s.system,
      "released/disconnected input must be completely neutral");
}
}  // namespace

int main() {
  try {
    ControllerInputMappingConfig cfg;
    for (bool left : {false, true}) {
      auto side = sample();
      side.touches = CONTROLLER_BUTTON_THUMBSTICK;
      auto s = map_controller_input(&side, left, cfg);
      require(s.stick_touch && s.trackpad_touch && !s.stick_click && !s.trackpad_click,
              "centre touch must not press the stick or trackpad");
      for (const auto axis : {-1.0f, -0.5f, 0.5f, 1.0f}) {
        side.thumbstick_x = axis;
        side.thumbstick_y = -axis;
        s = map_controller_input(&side, left, cfg);
        require(s.x == axis && s.y == -axis && s.stick_touch && s.trackpad_touch,
                "directions must retain their signed coordinates");
        require(!s.stick_click && !s.trackpad_click,
                "touch/axis movement must not also fire a general click action");
      }
      for (const auto direction : {CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_DOWN,
                                   CONTROLLER_BUTTON_DPAD_LEFT, CONTROLLER_BUTTON_DPAD_RIGHT}) {
        side = sample();
        side.buttons = direction;
        s = map_controller_input(&side, left, cfg);
        require(std::abs(s.x) + std::abs(s.y) == 1.0f && s.stick_touch,
                "digital directions must supply axes and touch");
        require(!s.stick_click && !s.trackpad_click, "a D-pad direction must not press centre");
      }
      for (const auto click : {CONTROLLER_BUTTON_THUMBSTICK, CONTROLLER_BUTTON_DPAD_CENTER}) {
        side = sample(); side.buttons = click;
        s = map_controller_input(&side, left, cfg);
        require(s.stick_click && s.trackpad_click && s.stick_touch && s.trackpad_touch,
                "physical click must remain available in both layouts");
      }
      side = sample();
      side.buttons = CONTROLLER_BUTTON_DPAD_LEFT | CONTROLLER_BUTTON_DPAD_RIGHT |
                     CONTROLLER_BUTTON_DPAD_UP | CONTROLLER_BUTTON_DPAD_DOWN;
      s = map_controller_input(&side, left, cfg);
      require(s.x == 0 && s.y == 0 && !s.trackpad_click, "opposing directions must cancel");
      side = sample();
      side.buttons = CONTROLLER_BUTTON_TRIGGER | CONTROLLER_BUTTON_GRIP;
      s = map_controller_input(&side, left, cfg);
      require(s.trigger == 1 && s.grip == 1 && s.trigger_click && s.grip_click,
              "digital triggers/grips must also drive pull actions");
      side.trigger = 0.25f; side.grip = 0.3f;
      s = map_controller_input(&side, left, cfg);
      require(s.trigger == 0.25f && s.grip == 0.3f, "real analog values must not become binary");
      side = sample(); side.trigger = 0.6f; side.grip = 0.4f;
      s = map_controller_input(&side, left, cfg);
      require(s.trigger_click && !s.grip_click && s.trigger_touch && s.grip_touch,
              "analog pull must use the configured click threshold");
      side.trigger = 2; side.grip = -1; side.thumbstick_x = 5; side.thumbstick_y = -5;
      s = map_controller_input(&side, left, cfg);
      require(s.trigger == 1 && s.grip == 0 && s.x == 1 && s.y == -1, "axes must be clamped");
      side = sample();
      side.trigger = side.grip = side.thumbstick_x = side.thumbstick_y =
          std::numeric_limits<float>::quiet_NaN();
      neutral(map_controller_input(&side, left, cfg));
      side = sample();
      side.buttons = CONTROLLER_BUTTON_KNOWN_MASK;
      side.touches = CONTROLLER_BUTTON_KNOWN_MASK;
      for (const auto flags : {uint32_t{0}, uint32_t{RUNTIME_CONTROLLER_CONNECTED},
              uint32_t{RUNTIME_CONTROLLER_INPUT_VALID},
              uint32_t{side.flags | RUNTIME_CONTROLLER_INPUT_STALE}}) {
        side.flags = flags;
        neutral(map_controller_input(&side, left, cfg));
      }
      neutral(map_controller_input(nullptr, left, cfg));
      side = sample(); neutral(map_controller_input(&side, left, cfg));

      // A configured face button must never also press or touch another one.
      const uint64_t faces[] = {CONTROLLER_BUTTON_A, CONTROLLER_BUTTON_B,
                                CONTROLLER_BUTTON_X, CONTROLLER_BUTTON_Y};
      for (size_t i = 0; i < 4; ++i) {
        for (bool touch_only : {false, true}) {
          side = sample();
          if (touch_only) side.touches = faces[i]; else side.buttons = faces[i];
          s = map_controller_input(&side, left, cfg);
          const bool clicks[] = {s.a, s.b, s.x_button, s.y_button};
          const bool touches[] = {s.a_touch, s.b_touch, s.x_touch, s.y_touch};
          for (size_t j = 0; j < 4; ++j) {
            require(clicks[j] == (!touch_only && i == j), "face presses must remain independent by default");
            require(touches[j] == (i == j), "face touches must remain independent by default");
          }
          side.buttons = side.touches = 0;
          neutral(map_controller_input(&side, left, cfg));
        }
      }
    }

    // Legacy profiles can still request the old aliases explicitly.
    cfg.left_ab_xy_alias = true;
    auto side = sample(); side.buttons = CONTROLLER_BUTTON_A | CONTROLLER_BUTTON_B;
    auto s = map_controller_input(&side, true, cfg);
    require(s.x_button && s.y_button && s.x_touch && s.y_touch, "left A/B must support Touch X/Y bindings");
    s = map_controller_input(&side, false, cfg);
    require(s.a && s.b && !s.x_button && !s.y_button, "right A/B must keep their own identities");
    side.buttons = CONTROLLER_BUTTON_X | CONTROLLER_BUTTON_Y;
    s = map_controller_input(&side, true, cfg);
    require(s.a && s.b && s.x_button && s.y_button, "canonical left X/Y must also support existing A/B bindings");
    cfg.left_ab_xy_alias = false;
    s = map_controller_input(&side, true, cfg);
    require(!s.a && !s.b && s.x_button && s.y_button, "aliasing must be optional");
    side = sample(); side.touches = CONTROLLER_BUTTON_A;
    s = map_controller_input(&side, false, cfg);
    require(s.a_touch && !s.a, "capacitive face-button touch must not become a press");
    cfg.trackpad_click_from_touch = true;
    side = sample(); side.touches = CONTROLLER_BUTTON_THUMBSTICK;
    s = map_controller_input(&side, true, cfg);
    require(s.trackpad_click && !s.stick_click, "legacy click synthesis must be opt-in and trackpad-only");
    cfg.trackpad_click_from_touch = false;
    cfg.analog_click_threshold = 0.8f;
    cfg.axis_activity_threshold = 0.2f;
    side = sample(); side.trigger = 0.7f; side.thumbstick_x = 0.1f;
    s = map_controller_input(&side, true, cfg);
    require(!s.trigger_click && s.x == 0 && !s.stick_touch, "input thresholds must be configurable");
    side = sample(); side.buttons = CONTROLLER_BUTTON_SYSTEM;
    require(!map_controller_input(&side, true, cfg).system, "system input must remain disabled by default");
    cfg.expose_system_button = true;
    require(map_controller_input(&side, true, cfg).system, "explicit system input setting must work");
    std::cout << "Virtual controller touch, click, direction, button and release semantics passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
