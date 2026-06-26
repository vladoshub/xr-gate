#include "windows_keyboard_input_provider.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <thread>

namespace xr_override_controller {
namespace {
constexpr uint16_t kWinKeyType = 1;

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const int kTrackedKeys[] = {
    VK_SPACE, VK_RETURN, VK_ESCAPE, VK_TAB, VK_BACK,
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
    VK_OEM_1,VK_OEM_2,VK_OEM_3,VK_OEM_4,VK_OEM_5,VK_OEM_6,VK_OEM_7,VK_OEM_PLUS,VK_OEM_MINUS,
};
}  // namespace

std::vector<DeviceInfo> WindowsKeyboardInputProvider::scan_devices(bool open_readable) {
  DeviceInfo d;
  d.fingerprint.platform = "windows";
  d.fingerprint.backend = "keyboard";
  d.fingerprint.name = "Windows keyboard";
  d.fingerprint.stable_hash = stable_hash64("windows|keyboard");
  d.readable = open_readable;
  return {d};
}

void WindowsKeyboardInputProvider::flush_events(std::vector<DeviceInfo>&) {
#ifdef _WIN32
  for (int key : kTrackedKeys) {
    previous_[key & 0xff] = (::GetAsyncKeyState(key) & 0x8000) != 0;
  }
#endif
}

std::optional<InputEvent> WindowsKeyboardInputProvider::wait_event(std::vector<DeviceInfo>& devices,
                                                                    int timeout_ms,
                                                                    bool) {
#ifdef _WIN32
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() <= deadline) {
    if (!devices.empty() && devices[0].readable) {
      for (int key : kTrackedKeys) {
        const int idx = key & 0xff;
        const bool down = (::GetAsyncKeyState(key) & 0x8000) != 0;
        if (down != previous_[idx]) {
          previous_[idx] = down;
          InputEvent ev;
          ev.device_index = 0;
          ev.type = kWinKeyType;
          ev.code = static_cast<uint16_t>(key);
          ev.value = down ? 1 : 0;
          ev.timestamp_ns = now_ns();
          return ev;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
#else
  (void)devices;
  (void)timeout_ms;
#endif
  return std::nullopt;
}

std::string WindowsKeyboardInputProvider::input_name(uint16_t type, uint16_t code) const {
  if (type != kWinKeyType) return "unknown";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "VK_%u", static_cast<unsigned>(code));
  return buf;
}

InputBindingSpec WindowsKeyboardInputProvider::make_input_spec(const DeviceInfo&, uint16_t type, uint16_t code) const {
  InputBindingSpec spec;
  spec.kind = InputKind::Key;
  spec.type = type;
  spec.code = code;
  spec.name = input_name(type, code);
  return spec;
}

}  // namespace xr_override_controller
