#pragma once

#include <hidapi/hidapi.h>

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace xreal_display_helper {

constexpr uint16_t kXrealVid = 0x3318;
constexpr uint16_t kAirPid = 0x0424;
constexpr uint16_t kAir2Pid = 0x0428;
constexpr uint16_t kAir2ProPid = 0x0432;
constexpr uint16_t kAir2UltraPid = 0x0426;

constexpr int kCommandTimeoutMs = 1000;

struct McuPacket {
  uint16_t cmd_id = 0;
  std::vector<uint8_t> data;
};

struct DeviceChoice {
  std::string path;
  uint16_t pid = 0;
  int interface_number = -1;
  std::string serial;
  std::string product;
};

std::string wide_to_utf8(const wchar_t* w);
std::string hid_error_string(hid_device* dev);
std::string product_name(uint16_t pid);
std::string mode_name(uint8_t mode);
uint8_t parse_mode_byte(const std::string& mode);

bool supported_pid(uint16_t pid);
int mcu_interface_for_pid(uint16_t pid);
DeviceChoice find_xreal_mcu_device();

std::array<uint8_t, 64> serialize_mcu_packet(uint16_t cmd_id, const std::vector<uint8_t>& payload);
bool parse_mcu_packet_at(const uint8_t* data, size_t size, size_t offset, McuPacket& packet);

std::vector<uint8_t> run_command(hid_device* dev, uint16_t cmd_id, const std::vector<uint8_t>& data);
uint8_t get_display_mode(hid_device* dev);
void set_display_mode(hid_device* dev, uint8_t mode);

// Platform hooks. Implemented under src/platform/linux or src/platform/windows.
int platform_write_mcu_packet(hid_device* dev, const std::array<uint8_t, 64>& packet);
bool platform_parse_mcu_packet(const uint8_t* data, size_t size, McuPacket& packet);
std::string platform_hid_open_failure_hint();
void platform_print_keep_running_hint(std::ostream& os);

}  // namespace xreal_display_helper
