#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xr_override_controller {

class WindowsInputProvider final : public InputProvider {
 public:
  WindowsInputProvider();
  ~WindowsInputProvider() override;

  std::vector<DeviceInfo> scan_devices(bool open_readable) override;
  void flush_events(std::vector<DeviceInfo>& devices) override;
  std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices, int timeout_ms, bool include_stdin) override;
  std::string input_name(uint16_t type, uint16_t code) const override;
  InputBindingSpec make_input_spec(const DeviceInfo& device, uint16_t type, uint16_t code) const override;
  void close_devices(std::vector<DeviceInfo>& devices) override;

 private:
  struct PadSnapshot {
    bool connected = false;
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
  };

  void capture_keyboard_snapshot();
  PadSnapshot read_pad_snapshot(uint32_t user_index) const;

  void ensure_rawinput_window();
  void destroy_rawinput_window();
  void refresh_rawinput_device_cache();
  void pump_rawinput_messages(std::vector<DeviceInfo>* devices);
  void handle_rawinput_message(void* raw_input_handle, std::vector<DeviceInfo>* devices);
  void enqueue_rawinput_event(InputEvent ev);

  bool keyboard_previous_[256]{};
  std::array<PadSnapshot, 4> pad_previous_{};

  void* raw_hwnd_ = nullptr;
  bool raw_registered_ = false;
  bool raw_device_cache_valid_ = false;
  std::vector<DeviceInfo> raw_devices_cache_;
  std::deque<InputEvent> raw_event_queue_;
  std::map<std::string, std::vector<uint8_t>> raw_hid_previous_by_path_;
};

}  // namespace xr_override_controller
