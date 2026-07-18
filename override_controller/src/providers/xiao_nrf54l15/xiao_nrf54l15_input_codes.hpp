#pragma once

#include <cstdint>

namespace xr_override_controller::xiao_nrf54l15::input_codes {

// Linux input-event compatible numeric values. Keeping the numeric ABI in the
// provider makes trained bindings portable within override_controller without
// requiring <linux/input.h> in common/provider code.
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;

constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kAbsZ = 0x02;
constexpr uint16_t kAbsRz = 0x05;

constexpr uint16_t kKeyUp = 103;
constexpr uint16_t kKeyLeft = 105;
constexpr uint16_t kKeyRight = 106;
constexpr uint16_t kKeyDown = 108;

constexpr uint16_t kBtnSouth = 0x130;   // A
constexpr uint16_t kBtnEast = 0x131;    // B
constexpr uint16_t kBtnNorth = 0x133;   // C
constexpr uint16_t kBtnTl2 = 0x138;     // grip
constexpr uint16_t kBtnStart = 0x13b;   // menu
constexpr uint16_t kBtnThumbL = 0x13d;  // stick click
constexpr uint16_t kBtnTrigger = 0x120;

constexpr uint16_t kBusUsb = 0x03;

}  // namespace xr_override_controller::xiao_nrf54l15::input_codes
