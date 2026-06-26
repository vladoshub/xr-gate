#include "xreal_display_helper/display_helper.hpp"

#include <array>
#include <cstdint>
#include <ostream>
#include <string>

namespace xreal_display_helper {

int platform_write_mcu_packet(hid_device* dev, const std::array<uint8_t, 64>& packet) {
  return hid_write(dev, packet.data(), packet.size());
}

bool platform_parse_mcu_packet(const uint8_t* data, size_t size, McuPacket& packet) {
  return parse_mcu_packet_at(data, size, 0, packet);
}

std::string platform_hid_open_failure_hint() {
  return "check permissions/udev or try sudo";
}

void platform_print_keep_running_hint(std::ostream& os) {
  os << "[xreal_display_helper] verify with: xrandr --query | grep -A15 \"DisplayPort-2\"\n";
}

}  // namespace xreal_display_helper
