#include "xreal_display_helper/display_helper.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <string>

namespace xreal_display_helper {

int platform_write_mcu_packet(hid_device* dev, const std::array<uint8_t, 64>& packet) {
  // Windows hidapi expects the leading report ID byte. XREAL MCU uses report 0.
  std::array<uint8_t, 65> report{};
  std::copy(packet.begin(), packet.end(), report.begin() + 1);
  return hid_write(dev, report.data(), report.size());
}

bool platform_parse_mcu_packet(const uint8_t* data, size_t size, McuPacket& packet) {
  // Windows hidapi may include a leading report ID byte in the read buffer.
  if (size >= 65 && data[0] == 0x00 && data[1] == 0xfd) {
    return parse_mcu_packet_at(data, size, 1, packet);
  }
  return parse_mcu_packet_at(data, size, 0, packet);
}

std::string platform_hid_open_failure_hint() {
  return "check that no other process owns the XREAL HID interface and try --list-devices/debug enumeration if needed";
}

void platform_print_keep_running_hint(std::ostream& os) {
  os << "[xreal_display_helper] on Windows, keep this console/process alive while SteamVR is running.\n";
}

}  // namespace xreal_display_helper
