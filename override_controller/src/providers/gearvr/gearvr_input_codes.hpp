#pragma once

#include <cstdint>

namespace xr_override_controller::gearvr::input_codes {

// Linux input-event compatible numeric codes. Keeping these values in the
// platform-neutral Gear VR provider lets the same trained bindings work on
// Linux and Windows without including <linux/input.h> in common code.
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kKeyUp = 103;
constexpr uint16_t kKeyLeft = 105;
constexpr uint16_t kKeyRight = 106;
constexpr uint16_t kKeyDown = 108;
constexpr uint16_t kKeyVolumeDown = 114;
constexpr uint16_t kKeyVolumeUp = 115;
constexpr uint16_t kKeyBack = 158;
constexpr uint16_t kKeyHomepage = 172;
constexpr uint16_t kBtnLeft = 0x110;
constexpr uint16_t kBtnTrigger = 0x120;
constexpr uint16_t kBtnTouch = 0x14a;
constexpr uint16_t kBusBluetooth = 0x05;

}  // namespace xr_override_controller::gearvr::input_codes
