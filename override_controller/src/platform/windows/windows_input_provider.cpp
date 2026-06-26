#include "windows_input_provider.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace xr_override_controller {
namespace {

constexpr uint16_t kWinKeyType = 1;  // Matches EV_KEY fallback in main.cpp.
constexpr uint16_t kWinRelType = 2;  // Matches EV_REL fallback in main.cpp.
constexpr uint16_t kWinAbsType = 3;  // Matches EV_ABS fallback in main.cpp.

// Keep Windows synthetic codes in ranges that do not overlap normal VK codes.
constexpr uint16_t kXInputButtonCodeBase = 0x2200;
constexpr uint16_t kXInputLeftTrigger = 0x2300;
constexpr uint16_t kXInputRightTrigger = 0x2301;
constexpr uint16_t kXInputThumbLX = 0x2310;
constexpr uint16_t kXInputThumbLY = 0x2311;
constexpr uint16_t kXInputThumbRX = 0x2312;
constexpr uint16_t kXInputThumbRY = 0x2313;

constexpr uint16_t kRawMouseButtonBase = 0x2400;
constexpr uint16_t kRawMouseRelX = 0x2410;
constexpr uint16_t kRawMouseRelY = 0x2411;
constexpr uint16_t kRawMouseWheel = 0x2412;
constexpr uint16_t kRawHidBitBase = 0x5000;
constexpr uint16_t kRawHidBitEnd = 0x6000;

constexpr int kKeyboardPseudoFd = 1;
constexpr int kXInputPseudoFdBase = 100;
constexpr int kRawInputPseudoFdBase = 1000;

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

struct XInputButtonDef {
  uint16_t bit;
  uint16_t code;
  const char* name;
};

const XInputButtonDef kXInputButtons[] = {
#ifdef _WIN32
    {XINPUT_GAMEPAD_DPAD_UP, static_cast<uint16_t>(kXInputButtonCodeBase + 1), "XINPUT_DPAD_UP"},
    {XINPUT_GAMEPAD_DPAD_DOWN, static_cast<uint16_t>(kXInputButtonCodeBase + 2), "XINPUT_DPAD_DOWN"},
    {XINPUT_GAMEPAD_DPAD_LEFT, static_cast<uint16_t>(kXInputButtonCodeBase + 3), "XINPUT_DPAD_LEFT"},
    {XINPUT_GAMEPAD_DPAD_RIGHT, static_cast<uint16_t>(kXInputButtonCodeBase + 4), "XINPUT_DPAD_RIGHT"},
    {XINPUT_GAMEPAD_START, static_cast<uint16_t>(kXInputButtonCodeBase + 5), "XINPUT_START"},
    {XINPUT_GAMEPAD_BACK, static_cast<uint16_t>(kXInputButtonCodeBase + 6), "XINPUT_BACK"},
    {XINPUT_GAMEPAD_LEFT_THUMB, static_cast<uint16_t>(kXInputButtonCodeBase + 7), "XINPUT_LEFT_THUMB"},
    {XINPUT_GAMEPAD_RIGHT_THUMB, static_cast<uint16_t>(kXInputButtonCodeBase + 8), "XINPUT_RIGHT_THUMB"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, static_cast<uint16_t>(kXInputButtonCodeBase + 9), "XINPUT_LEFT_SHOULDER"},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, static_cast<uint16_t>(kXInputButtonCodeBase + 10), "XINPUT_RIGHT_SHOULDER"},
    {XINPUT_GAMEPAD_A, static_cast<uint16_t>(kXInputButtonCodeBase + 11), "XINPUT_A"},
    {XINPUT_GAMEPAD_B, static_cast<uint16_t>(kXInputButtonCodeBase + 12), "XINPUT_B"},
    {XINPUT_GAMEPAD_X, static_cast<uint16_t>(kXInputButtonCodeBase + 13), "XINPUT_X"},
    {XINPUT_GAMEPAD_Y, static_cast<uint16_t>(kXInputButtonCodeBase + 14), "XINPUT_Y"},
#endif
};

std::string vk_name(uint16_t code) {
  switch (code) {
    case VK_SPACE: return "VK_SPACE";
    case VK_RETURN: return "VK_RETURN";
    case VK_ESCAPE: return "VK_ESCAPE";
    case VK_TAB: return "VK_TAB";
    case VK_BACK: return "VK_BACK";
    case VK_UP: return "VK_UP";
    case VK_DOWN: return "VK_DOWN";
    case VK_LEFT: return "VK_LEFT";
    case VK_RIGHT: return "VK_RIGHT";
    default: break;
  }
  if (code >= 'A' && code <= 'Z') return std::string("VK_") + static_cast<char>(code);
  if (code >= '0' && code <= '9') return std::string("VK_") + static_cast<char>(code);
  if (code >= VK_F1 && code <= VK_F12) return "VK_F" + std::to_string(1 + code - VK_F1);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "VK_%u", static_cast<unsigned>(code));
  return buf;
}

std::string xinput_button_name_for_code(uint16_t code) {
  for (const auto& b : kXInputButtons) {
    if (b.code == code) return b.name;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "XINPUT_BUTTON_0x%04x", static_cast<unsigned>(code));
  return buf;
}

std::string xinput_axis_name(uint16_t code) {
  switch (code) {
    case kXInputLeftTrigger: return "XINPUT_LEFT_TRIGGER";
    case kXInputRightTrigger: return "XINPUT_RIGHT_TRIGGER";
    case kXInputThumbLX: return "XINPUT_THUMB_LX";
    case kXInputThumbLY: return "XINPUT_THUMB_LY";
    case kXInputThumbRX: return "XINPUT_THUMB_RX";
    case kXInputThumbRY: return "XINPUT_THUMB_RY";
    default: break;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "XINPUT_AXIS_0x%04x", static_cast<unsigned>(code));
  return buf;
}

std::string raw_mouse_button_name(uint16_t code) {
  const unsigned idx = static_cast<unsigned>(code - kRawMouseButtonBase + 1);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "RAW_MOUSE_BUTTON_%u", idx);
  return buf;
}

std::string raw_rel_name(uint16_t code) {
  switch (code) {
    case kRawMouseRelX: return "RAW_MOUSE_X";
    case kRawMouseRelY: return "RAW_MOUSE_Y";
    case kRawMouseWheel: return "RAW_MOUSE_WHEEL";
    default: break;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "RAW_REL_0x%04x", static_cast<unsigned>(code));
  return buf;
}

std::string raw_hid_bit_name(uint16_t code) {
  const unsigned bit_index = static_cast<unsigned>(code - kRawHidBitBase);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "RAW_HID_BIT_%u", bit_index);
  return buf;
}

int16_t apply_thumb_deadzone(int16_t value, int16_t deadzone) {
  if (std::abs(static_cast<int>(value)) < static_cast<int>(deadzone)) return 0;
  return value;
}

bool axis_changed(int old_value, int new_value, int threshold) {
  return std::abs(new_value - old_value) >= threshold;
}

std::string hex_ptr(uintptr_t value) {
  std::ostringstream os;
  os << "0x" << std::hex << std::setw(static_cast<int>(sizeof(uintptr_t) * 2)) << std::setfill('0') << value;
  return os.str();
}

#ifdef _WIN32
std::string rawinput_handle_path(HANDLE h) {
  return "rawinput://" + hex_ptr(reinterpret_cast<uintptr_t>(h));
}

std::string rawinput_device_name(HANDLE h) {
  UINT size = 0;
  if (::GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0) return {};
  std::string out(size, '\0');
  if (::GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, out.data(), &size) == static_cast<UINT>(-1)) return {};
  while (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

std::string rawinput_backend_for_type(DWORD type) {
  switch (type) {
    case RIM_TYPEKEYBOARD: return "rawinput_keyboard";
    case RIM_TYPEMOUSE: return "rawinput_mouse";
    case RIM_TYPEHID: return "rawinput_hid";
    default: return "rawinput_unknown";
  }
}

LRESULT CALLBACK rawinput_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return ::DefWindowProcA(hwnd, msg, wp, lp);
}
#endif

}  // namespace

WindowsInputProvider::WindowsInputProvider() = default;

WindowsInputProvider::~WindowsInputProvider() {
  destroy_rawinput_window();
}

WindowsInputProvider::PadSnapshot WindowsInputProvider::read_pad_snapshot(uint32_t user_index) const {
  PadSnapshot out;
#ifdef _WIN32
  XINPUT_STATE state{};
  const DWORD rc = ::XInputGetState(static_cast<DWORD>(user_index), &state);
  if (rc != ERROR_SUCCESS) return out;
  out.connected = true;
  out.packet = state.dwPacketNumber;
  out.buttons = state.Gamepad.wButtons;
  out.left_trigger = state.Gamepad.bLeftTrigger;
  out.right_trigger = state.Gamepad.bRightTrigger;
  out.thumb_lx = apply_thumb_deadzone(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
  out.thumb_ly = apply_thumb_deadzone(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
  out.thumb_rx = apply_thumb_deadzone(state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
  out.thumb_ry = apply_thumb_deadzone(state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
#else
  (void)user_index;
#endif
  return out;
}

void WindowsInputProvider::capture_keyboard_snapshot() {
#ifdef _WIN32
  for (int key : kTrackedKeys) {
    keyboard_previous_[key & 0xff] = (::GetAsyncKeyState(key) & 0x8000) != 0;
  }
#endif
}

void WindowsInputProvider::ensure_rawinput_window() {
#ifdef _WIN32
  if (raw_hwnd_ != nullptr) return;

  HINSTANCE instance = ::GetModuleHandleA(nullptr);
  const char* class_name = "XRTrackingOverrideControllerRawInputWindow";

  WNDCLASSEXA wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = rawinput_window_proc;
  wc.hInstance = instance;
  wc.lpszClassName = class_name;
  (void)::RegisterClassExA(&wc);

  HWND hwnd = ::CreateWindowExA(
      0, class_name, "XR Tracking Override Controller RawInput", WS_OVERLAPPED,
      0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
  if (!hwnd) return;

  RAWINPUTDEVICE devices[] = {
      {0x01, 0x06, RIDEV_INPUTSINK, hwnd},  // Generic Desktop / Keyboard
      {0x01, 0x02, RIDEV_INPUTSINK, hwnd},  // Generic Desktop / Mouse
      {0x01, 0x04, RIDEV_INPUTSINK, hwnd},  // Generic Desktop / Joystick
      {0x01, 0x05, RIDEV_INPUTSINK, hwnd},  // Generic Desktop / Game Pad
      {0x0c, 0x01, RIDEV_INPUTSINK, hwnd},  // Consumer Control
  };
#if defined(RIDEV_DEVNOTIFY)
  for (auto& d : devices) d.dwFlags |= RIDEV_DEVNOTIFY;
#endif

  raw_hwnd_ = hwnd;
  raw_registered_ = ::RegisterRawInputDevices(devices, static_cast<UINT>(sizeof(devices) / sizeof(devices[0])), sizeof(RAWINPUTDEVICE)) != FALSE;
#else
  raw_hwnd_ = nullptr;
  raw_registered_ = false;
#endif
}

void WindowsInputProvider::destroy_rawinput_window() {
#ifdef _WIN32
  if (raw_hwnd_ != nullptr) {
    HWND hwnd = reinterpret_cast<HWND>(raw_hwnd_);
    ::DestroyWindow(hwnd);
    raw_hwnd_ = nullptr;
  }
#endif
  raw_registered_ = false;
  raw_device_cache_valid_ = false;
  raw_event_queue_.clear();
}

void WindowsInputProvider::refresh_rawinput_device_cache() {
  raw_devices_cache_.clear();
  raw_device_cache_valid_ = true;
#ifdef _WIN32
  ensure_rawinput_window();

  UINT count = 0;
  if (::GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) return;
  std::vector<RAWINPUTDEVICELIST> list(count);
  const UINT got = ::GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
  if (got == static_cast<UINT>(-1)) return;

  for (UINT i = 0; i < got; ++i) {
    const HANDLE handle = list[i].hDevice;
    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT info_size = sizeof(info);
    if (::GetRawInputDeviceInfoA(handle, RIDI_DEVICEINFO, &info, &info_size) == static_cast<UINT>(-1)) continue;

    DeviceInfo d;
    d.fingerprint.platform = "windows";
    d.fingerprint.backend = rawinput_backend_for_type(info.dwType);
    d.fingerprint.event_path = rawinput_handle_path(handle);
    d.fingerprint.name = rawinput_device_name(handle);
    if (d.fingerprint.name.empty()) d.fingerprint.name = d.fingerprint.backend;

    if (info.dwType == RIM_TYPEHID) {
      d.fingerprint.vendor = static_cast<uint16_t>(info.hid.dwVendorId & 0xffffu);
      d.fingerprint.product = static_cast<uint16_t>(info.hid.dwProductId & 0xffffu);
      d.fingerprint.version = static_cast<uint16_t>(info.hid.dwVersionNumber & 0xffffu);
      d.fingerprint.bustype = static_cast<uint16_t>(info.hid.usUsagePage);
      d.fingerprint.phys = "usage=" + std::to_string(info.hid.usUsagePage) + ":" + std::to_string(info.hid.usUsage);
    } else if (info.dwType == RIM_TYPEKEYBOARD) {
      d.fingerprint.product = static_cast<uint16_t>(info.keyboard.dwType & 0xffffu);
      d.fingerprint.version = static_cast<uint16_t>(info.keyboard.dwSubType & 0xffffu);
      d.fingerprint.phys = "keyboard_mode=" + std::to_string(info.keyboard.dwKeyboardMode);
    } else if (info.dwType == RIM_TYPEMOUSE) {
      d.fingerprint.product = static_cast<uint16_t>(info.mouse.dwId & 0xffffu);
      d.fingerprint.phys = "mouse_buttons=" + std::to_string(info.mouse.dwNumberOfButtons);
    }

    const std::string material = d.fingerprint.platform + "|" + d.fingerprint.backend + "|" +
                                 d.fingerprint.event_path + "|" + d.fingerprint.name + "|" +
                                 d.fingerprint.phys + "|" + std::to_string(d.fingerprint.vendor) + "|" +
                                 std::to_string(d.fingerprint.product) + "|" + std::to_string(d.fingerprint.version);
    d.fingerprint.stable_hash = stable_hash64(material);
    raw_devices_cache_.push_back(std::move(d));
  }
#endif
}

std::vector<DeviceInfo> WindowsInputProvider::scan_devices(bool open_readable) {
  std::vector<DeviceInfo> out;

  ensure_rawinput_window();
  if (!raw_device_cache_valid_) refresh_rawinput_device_cache();

  for (size_t i = 0; i < raw_devices_cache_.size(); ++i) {
    DeviceInfo d = raw_devices_cache_[i];
    d.fd = open_readable ? static_cast<int>(kRawInputPseudoFdBase + i) : -1;
    d.readable = open_readable && raw_registered_;
    if (!raw_registered_) d.open_error = "RegisterRawInputDevices failed; using keyboard polling/XInput fallback only";
    out.push_back(std::move(d));
  }

  DeviceInfo keyboard;
  keyboard.fingerprint.platform = "windows";
  keyboard.fingerprint.backend = "keyboard";
  keyboard.fingerprint.name = "Windows keyboard polling fallback";
  keyboard.fingerprint.stable_hash = stable_hash64("windows|keyboard-polling-fallback");
  keyboard.fd = open_readable ? kKeyboardPseudoFd : -1;
  keyboard.readable = open_readable;
  out.push_back(std::move(keyboard));

  for (uint32_t i = 0; i < pad_previous_.size(); ++i) {
    const PadSnapshot snap = read_pad_snapshot(i);
    pad_previous_[i] = snap;
    if (!snap.connected) continue;
    DeviceInfo d;
    d.fingerprint.platform = "windows";
    d.fingerprint.backend = "xinput";
    d.fingerprint.name = "XInput Controller " + std::to_string(i);
    d.fingerprint.event_path = "xinput://" + std::to_string(i);
    d.fingerprint.product = static_cast<uint16_t>(i + 1);
    d.fingerprint.stable_hash = stable_hash64("windows|xinput|" + std::to_string(i));
    d.fd = open_readable ? static_cast<int>(kXInputPseudoFdBase + i) : -1;
    d.readable = open_readable;
    out.push_back(std::move(d));
  }

  return out;
}

void WindowsInputProvider::flush_events(std::vector<DeviceInfo>& devices) {
  pump_rawinput_messages(&devices);
  raw_event_queue_.clear();
  raw_hid_previous_by_path_.clear();
  capture_keyboard_snapshot();
  for (uint32_t i = 0; i < pad_previous_.size(); ++i) {
    pad_previous_[i] = read_pad_snapshot(i);
  }
}

void WindowsInputProvider::enqueue_rawinput_event(InputEvent ev) {
  raw_event_queue_.push_back(ev);
}

void WindowsInputProvider::pump_rawinput_messages(std::vector<DeviceInfo>* devices) {
#ifdef _WIN32
  ensure_rawinput_window();
  MSG msg{};
  while (::PeekMessageA(&msg, reinterpret_cast<HWND>(raw_hwnd_), 0, 0, PM_REMOVE)) {
    if (msg.message == WM_INPUT) {
      handle_rawinput_message(reinterpret_cast<void*>(msg.lParam), devices);
      ::DefWindowProcA(msg.hwnd, msg.message, msg.wParam, msg.lParam);
      continue;
    }
    if (msg.message == WM_INPUT_DEVICE_CHANGE) {
      raw_device_cache_valid_ = false;
      continue;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageA(&msg);
  }
#else
  (void)devices;
#endif
}

void WindowsInputProvider::handle_rawinput_message(void* raw_input_handle, std::vector<DeviceInfo>* devices) {
#ifdef _WIN32
  UINT size = 0;
  HRAWINPUT hraw = reinterpret_cast<HRAWINPUT>(raw_input_handle);
  if (::GetRawInputData(hraw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) return;
  std::vector<uint8_t> buffer(size);
  if (::GetRawInputData(hraw, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;

  const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
  const std::string path = rawinput_handle_path(raw->header.hDevice);

  size_t dev_idx = static_cast<size_t>(-1);
  if (devices) {
    for (size_t i = 0; i < devices->size(); ++i) {
      if ((*devices)[i].fingerprint.event_path == path && (*devices)[i].readable) {
        dev_idx = i;
        break;
      }
    }
  }
  if (dev_idx == static_cast<size_t>(-1)) return;

  const auto make_event = [&](uint16_t type, uint16_t code, int32_t value) {
    InputEvent ev;
    ev.device_index = dev_idx;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ev.timestamp_ns = now_ns();
    enqueue_rawinput_event(ev);
  };

  if (raw->header.dwType == RIM_TYPEKEYBOARD) {
    const RAWKEYBOARD& k = raw->data.keyboard;
    if (k.VKey == 0 || k.VKey == 255) return;
    const bool up = (k.Flags & RI_KEY_BREAK) != 0;
    make_event(kWinKeyType, static_cast<uint16_t>(k.VKey), up ? 0 : 1);
    return;
  }

  if (raw->header.dwType == RIM_TYPEMOUSE) {
    const RAWMOUSE& m = raw->data.mouse;
    const USHORT flags = m.usButtonFlags;
    const struct { USHORT down; USHORT up; uint16_t code; } buttons[] = {
        {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_1_UP, static_cast<uint16_t>(kRawMouseButtonBase + 0)},
        {RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_2_UP, static_cast<uint16_t>(kRawMouseButtonBase + 1)},
        {RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_3_UP, static_cast<uint16_t>(kRawMouseButtonBase + 2)},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, static_cast<uint16_t>(kRawMouseButtonBase + 3)},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, static_cast<uint16_t>(kRawMouseButtonBase + 4)},
    };
    for (const auto& b : buttons) {
      if (flags & b.down) make_event(kWinKeyType, b.code, 1);
      if (flags & b.up) make_event(kWinKeyType, b.code, 0);
    }
    if (m.lLastX != 0) make_event(kWinRelType, kRawMouseRelX, m.lLastX);
    if (m.lLastY != 0) make_event(kWinRelType, kRawMouseRelY, m.lLastY);
    if (flags & RI_MOUSE_WHEEL) make_event(kWinRelType, kRawMouseWheel, static_cast<SHORT>(m.usButtonData));
    return;
  }

  if (raw->header.dwType == RIM_TYPEHID) {
    const RAWHID& hid = raw->data.hid;
    const uint32_t total = hid.dwSizeHid * hid.dwCount;
    if (total == 0) return;
    const uint8_t* data = hid.bRawData;
    std::vector<uint8_t>& prev = raw_hid_previous_by_path_[path];
    if (prev.size() < total) prev.resize(total, 0);

    const uint32_t max_bytes = std::min<uint32_t>(total, (kRawHidBitEnd - kRawHidBitBase) / 8);
    for (uint32_t byte_i = 0; byte_i < max_bytes; ++byte_i) {
      const uint8_t old_v = prev[byte_i];
      const uint8_t new_v = data[byte_i];
      const uint8_t diff = static_cast<uint8_t>(old_v ^ new_v);
      if (diff == 0) continue;
      for (uint32_t bit = 0; bit < 8; ++bit) {
        if ((diff & (1u << bit)) == 0) continue;
        const uint32_t bit_index = byte_i * 8u + bit;
        make_event(kWinKeyType, static_cast<uint16_t>(kRawHidBitBase + bit_index), (new_v & (1u << bit)) ? 1 : 0);
      }
    }
    prev.assign(data, data + total);
  }
#else
  (void)raw_input_handle;
  (void)devices;
#endif
}

std::optional<InputEvent> WindowsInputProvider::wait_event(std::vector<DeviceInfo>& devices,
                                                           int timeout_ms,
                                                           bool) {
#ifdef _WIN32
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
  while (std::chrono::steady_clock::now() <= deadline) {
    pump_rawinput_messages(&devices);
    if (!raw_event_queue_.empty()) {
      InputEvent ev = raw_event_queue_.front();
      raw_event_queue_.pop_front();
      return ev;
    }

    for (size_t dev_idx = 0; dev_idx < devices.size(); ++dev_idx) {
      if (!devices[dev_idx].readable || devices[dev_idx].fd < 0) continue;
      const auto& fp = devices[dev_idx].fingerprint;

      if (fp.backend == "keyboard") {
        for (int key : kTrackedKeys) {
          const int idx = key & 0xff;
          const bool down = (::GetAsyncKeyState(key) & 0x8000) != 0;
          if (down != keyboard_previous_[idx]) {
            keyboard_previous_[idx] = down;
            InputEvent ev;
            ev.device_index = dev_idx;
            ev.type = kWinKeyType;
            ev.code = static_cast<uint16_t>(key);
            ev.value = down ? 1 : 0;
            ev.timestamp_ns = now_ns();
            return ev;
          }
        }
        continue;
      }

      if (fp.backend != "xinput") continue;
      if (fp.event_path.rfind("xinput://", 0) != 0) continue;
      const uint32_t user_index = static_cast<uint32_t>(std::stoul(fp.event_path.substr(9)));
      if (user_index >= pad_previous_.size()) continue;

      const PadSnapshot prev = pad_previous_[user_index];
      const PadSnapshot snap = read_pad_snapshot(user_index);
      if (!snap.connected) continue;
      pad_previous_[user_index] = snap;

      const uint16_t changed_buttons = static_cast<uint16_t>(prev.buttons ^ snap.buttons);
      if (changed_buttons != 0) {
        for (const auto& b : kXInputButtons) {
          if ((changed_buttons & b.bit) == 0) continue;
          InputEvent ev;
          ev.device_index = dev_idx;
          ev.type = kWinKeyType;
          ev.code = b.code;
          ev.value = (snap.buttons & b.bit) ? 1 : 0;
          ev.timestamp_ns = now_ns();
          return ev;
        }
      }

      const auto make_abs = [&](uint16_t code, int value) {
        InputEvent ev;
        ev.device_index = dev_idx;
        ev.type = kWinAbsType;
        ev.code = code;
        ev.value = value;
        ev.timestamp_ns = now_ns();
        return ev;
      };

      if (axis_changed(prev.left_trigger, snap.left_trigger, 4)) return make_abs(kXInputLeftTrigger, snap.left_trigger);
      if (axis_changed(prev.right_trigger, snap.right_trigger, 4)) return make_abs(kXInputRightTrigger, snap.right_trigger);
      if (axis_changed(prev.thumb_lx, snap.thumb_lx, 1024)) return make_abs(kXInputThumbLX, snap.thumb_lx);
      if (axis_changed(prev.thumb_ly, snap.thumb_ly, 1024)) return make_abs(kXInputThumbLY, snap.thumb_ly);
      if (axis_changed(prev.thumb_rx, snap.thumb_rx, 1024)) return make_abs(kXInputThumbRX, snap.thumb_rx);
      if (axis_changed(prev.thumb_ry, snap.thumb_ry, 1024)) return make_abs(kXInputThumbRY, snap.thumb_ry);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
#else
  (void)devices;
  (void)timeout_ms;
#endif
  return std::nullopt;
}

std::string WindowsInputProvider::input_name(uint16_t type, uint16_t code) const {
  if (type == kWinKeyType) {
    if (code >= kXInputButtonCodeBase && code < static_cast<uint16_t>(kXInputButtonCodeBase + 0x0100)) {
      return xinput_button_name_for_code(code);
    }
    if (code >= kRawMouseButtonBase && code < static_cast<uint16_t>(kRawMouseButtonBase + 5)) {
      return raw_mouse_button_name(code);
    }
    if (code >= kRawHidBitBase && code < kRawHidBitEnd) return raw_hid_bit_name(code);
    return vk_name(code);
  }
  if (type == kWinAbsType) return xinput_axis_name(code);
  if (type == kWinRelType) return raw_rel_name(code);
  return "unknown";
}

InputBindingSpec WindowsInputProvider::make_input_spec(const DeviceInfo&, uint16_t type, uint16_t code) const {
  InputBindingSpec spec;
  spec.type = type;
  spec.code = code;
  spec.name = input_name(type, code);
  if (type == kWinAbsType) {
    spec.kind = InputKind::AbsAxis;
    if (code == kXInputLeftTrigger || code == kXInputRightTrigger) {
      spec.abs_min = 0;
      spec.abs_max = 255;
      spec.abs_flat = 4;
    } else {
      spec.abs_min = -32768;
      spec.abs_max = 32767;
      spec.abs_flat = 1024;
    }
  } else if (type == kWinRelType) {
    spec.kind = InputKind::RelAxis;
    spec.abs_direction = 0;
  } else {
    spec.kind = InputKind::Key;
  }
  return spec;
}

void WindowsInputProvider::close_devices(std::vector<DeviceInfo>& devices) {
  for (auto& d : devices) d.fd = -1;
}

}  // namespace xr_override_controller
