#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace xr_override_controller::gearvr {

struct TouchpadOptions {
  std::string mode = "absolute_stick";
  double deadzone = 0.12;
  double radius = 90.0;
  bool invert_x = false;
  bool invert_y = true;
};

class TouchpadProcessor {
 public:
  using EmitFn = std::function<void(uint16_t type, uint16_t code, int32_t value)>;

  explicit TouchpadProcessor(TouchpadOptions options = {});

  void process(int touch_x, int touch_y, const EmitFn& emit);
  void release(const EmitFn& emit);
  void reset();

 private:
  int scale_touch(double delta) const;

  TouchpadOptions options_;
  bool touched_ = false;
  int anchor_x_ = 0;
  int anchor_y_ = 0;
  int last_x_ = 0;
  int last_y_ = 0;
  uint16_t dpad_code_ = 0;
};

}  // namespace xr_override_controller::gearvr
