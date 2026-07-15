#include "gearvr_touchpad.hpp"

#include "gearvr_input_codes.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace xr_override_controller::gearvr {
namespace codes = input_codes;
namespace {

int clamp_axis(double value) {
  return static_cast<int>(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
}

}  // namespace

TouchpadProcessor::TouchpadProcessor(TouchpadOptions options)
    : options_(std::move(options)) {}

int TouchpadProcessor::scale_touch(double delta) const {
  double normalized = std::clamp(delta / std::max(1.0, options_.radius), -1.0, 1.0);
  if (std::abs(normalized) < options_.deadzone) {
    normalized = 0.0;
  } else {
    const double sign = normalized < 0.0 ? -1.0 : 1.0;
    normalized = sign * ((std::abs(normalized) - options_.deadzone) /
                         std::max(1.0e-6, 1.0 - options_.deadzone));
  }
  return clamp_axis(normalized);
}

void TouchpadProcessor::process(int touch_x, int touch_y, const EmitFn& emit) {
  const bool touched = !(touch_x == 0 && touch_y == 0);
  if (!touched) {
    release(emit);
    return;
  }

  if (!touched_) {
    touched_ = true;
    anchor_x_ = touch_x;
    anchor_y_ = touch_y;
  }

  if (options_.mode == "dpad") {
    const int dx = touch_x - anchor_x_;
    const int dy = touch_y - anchor_y_;
    const double threshold = std::max(1.0, options_.radius * options_.deadzone);
    uint16_t code = 0;
    if (std::max(std::abs(dx), std::abs(dy)) >= threshold) {
      if (std::abs(dx) >= std::abs(dy)) code = dx > 0 ? codes::kKeyRight : codes::kKeyLeft;
      else code = dy > 0 ? codes::kKeyDown : codes::kKeyUp;
    }
    if (code != dpad_code_) {
      if (dpad_code_ != 0) emit(codes::kEvKey, dpad_code_, 0);
      if (code != 0) emit(codes::kEvKey, code, 1);
      dpad_code_ = code;
    }
    return;
  }

  int x = 0;
  int y = 0;
  if (options_.mode == "raw") {
    x = clamp_axis(touch_x / 315.0 * 2.0 - 1.0);
    y = clamp_axis(touch_y / 315.0 * 2.0 - 1.0);
  } else if (options_.mode == "absolute_stick") {
    x = scale_touch(touch_x - 157);
    y = scale_touch(touch_y - 157);
  } else {
    x = scale_touch(touch_x - anchor_x_);
    y = scale_touch(touch_y - anchor_y_);
  }
  if (options_.invert_x) x = -x;
  if (options_.invert_y) y = -y;
  // Emit only axes that actually changed. In particular, a vertical swipe
  // must not generate a leading neutral ABS_X event that the training wizard
  // can mistake for the Y-axis binding.
  if (x != last_x_) {
    emit(codes::kEvAbs, codes::kAbsX, x);
    last_x_ = x;
  }
  if (y != last_y_) {
    emit(codes::kEvAbs, codes::kAbsY, y);
    last_y_ = y;
  }
}

void TouchpadProcessor::release(const EmitFn& emit) {
  if (!touched_) return;
  if (dpad_code_ != 0) {
    emit(codes::kEvKey, dpad_code_, 0);
  } else {
    if (last_x_ != 0) emit(codes::kEvAbs, codes::kAbsX, 0);
    if (last_y_ != 0) emit(codes::kEvAbs, codes::kAbsY, 0);
  }
  reset();
}

void TouchpadProcessor::reset() {
  touched_ = false;
  anchor_x_ = 0;
  anchor_y_ = 0;
  last_x_ = 0;
  last_y_ = 0;
  dpad_code_ = 0;
}

}  // namespace xr_override_controller::gearvr
