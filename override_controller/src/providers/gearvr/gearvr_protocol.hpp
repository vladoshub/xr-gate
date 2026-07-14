#pragma once

#include <xr_override_controller/imu/controller_imu_processor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace xr_override_controller::gearvr {

inline constexpr std::string_view kServiceUuid = "4f63756c-7573-2054-6872-65656d6f7465";
inline constexpr std::string_view kNotifyUuid = "c8c51726-81bc-483b-a052-f7a14ea3d281";
inline constexpr std::string_view kCommandUuid = "c8c51726-81bc-483b-a052-f7a14ea3d282";

using Command = std::array<uint8_t, 2>;
inline constexpr Command kCommandOff{0x00, 0x00};
inline constexpr Command kCommandSensor{0x01, 0x00};
inline constexpr Command kCommandKeepAlive{0x04, 0x00};
inline constexpr Command kCommandLpmEnable{0x06, 0x00};
inline constexpr Command kCommandLpmDisable{0x07, 0x00};
inline constexpr Command kCommandVrMode{0x08, 0x00};

struct DecodedPacket {
  int touch_x = 0;
  int touch_y = 0;
  uint8_t buttons = 0;
  imu::Vec3f accel_m_s2;
  imu::Vec3f gyro_rad_s;
  imu::Vec3f magnetic_uT;
};

std::optional<DecodedPacket> decode_packet(const uint8_t* data, size_t size);
const std::array<Command, 8>& initialization_commands();

}  // namespace xr_override_controller::gearvr
