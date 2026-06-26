#include <xr_override_controller/types.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace xr_override_controller {

std::string to_string(ControllerSide side) {
  switch (side) {
    case ControllerSide::Left: return "left";
    case ControllerSide::Right: return "right";
  }
  return "unknown";
}

std::string to_string(ControllerAction action) {
  switch (action) {
    case ControllerAction::Trigger: return "trigger";
    case ControllerAction::Grip: return "grip";
    case ControllerAction::Menu: return "menu";
    case ControllerAction::A: return "a";
    case ControllerAction::B: return "b";
    case ControllerAction::X: return "x";
    case ControllerAction::Y: return "y";
    case ControllerAction::System: return "system";
    case ControllerAction::ThumbstickClick: return "thumbstick_click";
    case ControllerAction::DpadUp: return "dpad_up";
    case ControllerAction::DpadDown: return "dpad_down";
    case ControllerAction::DpadLeft: return "dpad_left";
    case ControllerAction::DpadRight: return "dpad_right";
    case ControllerAction::DpadCenter: return "dpad_center";
    case ControllerAction::ThumbstickX: return "thumbstick_x";
    case ControllerAction::ThumbstickY: return "thumbstick_y";
  }
  return "unknown";
}

ControllerSide parse_side(const std::string& s) {
  if (s == "left" || s == "l") return ControllerSide::Left;
  if (s == "right" || s == "r") return ControllerSide::Right;
  throw std::runtime_error("side must be left or right: " + s);
}

ControllerAction parse_action(const std::string& s) {
  if (s == "trigger") return ControllerAction::Trigger;
  if (s == "grip" || s == "grab") return ControllerAction::Grip;
  if (s == "menu") return ControllerAction::Menu;
  if (s == "a") return ControllerAction::A;
  if (s == "b") return ControllerAction::B;
  if (s == "x") return ControllerAction::X;
  if (s == "y") return ControllerAction::Y;
  if (s == "system") return ControllerAction::System;
  if (s == "thumbstick" || s == "thumbstick_click") return ControllerAction::ThumbstickClick;
  if (s == "dpad_up" || s == "up") return ControllerAction::DpadUp;
  if (s == "dpad_down" || s == "down") return ControllerAction::DpadDown;
  if (s == "dpad_left") return ControllerAction::DpadLeft;
  if (s == "dpad_right") return ControllerAction::DpadRight;
  if (s == "dpad_center" || s == "dpad_press") return ControllerAction::DpadCenter;
  if (s == "thumbstick_x" || s == "stick_x") return ControllerAction::ThumbstickX;
  if (s == "thumbstick_y" || s == "stick_y") return ControllerAction::ThumbstickY;
  throw std::runtime_error("unknown controller action: " + s);
}

uint64_t button_bit_for_action(ControllerAction action) {
  switch (action) {
    case ControllerAction::Trigger: return kButtonTrigger;
    case ControllerAction::Grip: return kButtonGrip;
    case ControllerAction::Menu: return kButtonMenu;
    case ControllerAction::A: return kButtonA;
    case ControllerAction::B: return kButtonB;
    case ControllerAction::X: return kButtonX;
    case ControllerAction::Y: return kButtonY;
    case ControllerAction::System: return kButtonSystem;
    case ControllerAction::ThumbstickClick: return kButtonThumbstick;
    case ControllerAction::DpadUp: return kButtonDpadUp;
    case ControllerAction::DpadDown: return kButtonDpadDown;
    case ControllerAction::DpadLeft: return kButtonDpadLeft;
    case ControllerAction::DpadRight: return kButtonDpadRight;
    case ControllerAction::DpadCenter: return kButtonDpadCenter;
    case ControllerAction::ThumbstickX:
    case ControllerAction::ThumbstickY:
      return 0;
  }
  return 0;
}

bool is_axis_action(ControllerAction action) {
  return action == ControllerAction::ThumbstickX || action == ControllerAction::ThumbstickY;
}

uint64_t stable_hash64(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex_u64(uint64_t v) {
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << v;
  return os.str();
}

std::string short_device_label(const DeviceFingerprint& fp) {
  std::ostringstream os;
  os << fp.name;
  if (fp.vendor || fp.product) {
    os << " [" << std::hex << std::setw(4) << std::setfill('0') << fp.vendor
       << ":" << std::setw(4) << fp.product << std::dec << "]";
  }
  if (!fp.uniq.empty()) os << " uniq=" << fp.uniq;
  else if (!fp.by_id_path.empty()) os << " by-id=" << fp.by_id_path;
  else if (!fp.by_path.empty()) os << " by-path=" << fp.by_path;
  else os << " path=" << fp.event_path;
  return os.str();
}

int fingerprint_match_score(const DeviceFingerprint& wanted, const DeviceFingerprint& candidate) {
  int score = 0;
  if (!wanted.platform.empty() && wanted.platform == candidate.platform) score += 5;
  if (!wanted.backend.empty() && wanted.backend == candidate.backend) score += 5;
  if (!wanted.uniq.empty() && wanted.uniq == candidate.uniq) score += 140;
  if (!wanted.by_id_path.empty() && wanted.by_id_path == candidate.by_id_path) score += 120;
  if (!wanted.by_path.empty() && wanted.by_path == candidate.by_path) score += 95;
  if (!wanted.phys.empty() && wanted.phys == candidate.phys) score += 85;
  if (wanted.vendor != 0 && wanted.vendor == candidate.vendor) score += 20;
  if (wanted.product != 0 && wanted.product == candidate.product) score += 20;
  if (wanted.bustype != 0 && wanted.bustype == candidate.bustype) score += 10;
  if (wanted.version != 0 && wanted.version == candidate.version) score += 5;
  if (!wanted.name.empty() && wanted.name == candidate.name) score += 25;
  if (wanted.stable_hash != 0 && wanted.stable_hash == candidate.stable_hash) score += 40;
  if (!wanted.event_path.empty() && wanted.event_path == candidate.event_path) score += 15;
  return score;
}

}  // namespace xr_override_controller
