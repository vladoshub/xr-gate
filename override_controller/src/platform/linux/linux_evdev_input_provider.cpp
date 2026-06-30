#include "linux_evdev_input_provider.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include <map>
#include <tuple>

namespace fs = std::filesystem;

namespace xr_override_controller {
namespace {

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string ioctl_string(int fd, unsigned long request, size_t max_len) {
  std::vector<char> buf(max_len, 0);
  if (ioctl(fd, request, buf.data()) < 0) return {};
  return std::string(buf.data());
}

std::string find_stable_symlink(const fs::path& dir, const fs::path& event_path) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  const auto wanted = fs::weakly_canonical(event_path, ec);
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_symlink(ec)) continue;
    const auto resolved = fs::weakly_canonical(entry.path(), ec);
    if (!ec && resolved == wanted) return entry.path().string();
  }
  return {};
}

std::string trim_copy(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t first = 0;
  while (first < s.size() && (s[first] == ' ' || s[first] == '\t')) ++first;
  return first == 0 ? s : s.substr(first);
}

std::string read_first_line(const fs::path& path) {
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) return {};
  return trim_copy(line);
}

uint16_t parse_u16_auto(const std::string& s) {
  if (s.empty()) return 0;
  try {
    size_t idx = 0;
    const unsigned long v = std::stoul(s, &idx, 16);
    (void)idx;
    return static_cast<uint16_t>(v & 0xffffu);
  } catch (...) {
    return 0;
  }
}

int open_evdev_readonly_nonblocking(const fs::path& path, std::string& open_error) {
  // Some kernels/libc combinations can reject a flag combination with EINVAL.
  // Try conservative fallbacks before declaring the device unreadable.
  const int variants[] = {
      O_RDONLY | O_NONBLOCK | O_CLOEXEC,
      O_RDONLY | O_NONBLOCK,
      O_RDONLY | O_CLOEXEC,
      O_RDONLY,
  };

  int first_errno = 0;
  int last_errno = 0;
  for (const int flags : variants) {
    errno = 0;
    const int fd = open(path.c_str(), flags);
    if (fd >= 0) {
      const int fd_flags = fcntl(fd, F_GETFD);
      if (fd_flags >= 0) (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

      const int file_flags = fcntl(fd, F_GETFL);
      if (file_flags >= 0) (void)fcntl(fd, F_SETFL, file_flags | O_NONBLOCK);

      open_error.clear();
      return fd;
    }

    if (first_errno == 0) first_errno = errno;
    last_errno = errno;

    // If access is denied, different flag variants will not help. Keep the
    // original error so the user gets the correct permission hint.
    if (errno == EACCES || errno == EPERM) break;
  }

  const int report_errno = first_errno != 0 ? first_errno : last_errno;
  open_error = report_errno != 0 ? std::strerror(report_errno) : "open failed";
  return -1;
}

DeviceFingerprint fingerprint_from_sysfs(const fs::path& event_path) {
  DeviceFingerprint fp;
  fp.platform = "linux";
  fp.backend = "evdev";
  fp.event_path = event_path.string();

  const auto event_name = event_path.filename().string();
  const fs::path dev_dir = fs::path("/sys/class/input") / event_name / "device";
  fp.name = read_first_line(dev_dir / "name");
  fp.phys = read_first_line(dev_dir / "phys");
  fp.uniq = read_first_line(dev_dir / "uniq");
  fp.bustype = parse_u16_auto(read_first_line(dev_dir / "id" / "bustype"));
  fp.vendor = parse_u16_auto(read_first_line(dev_dir / "id" / "vendor"));
  fp.product = parse_u16_auto(read_first_line(dev_dir / "id" / "product"));
  fp.version = parse_u16_auto(read_first_line(dev_dir / "id" / "version"));
  fp.by_id_path = find_stable_symlink("/dev/input/by-id", event_path);
  fp.by_path = find_stable_symlink("/dev/input/by-path", event_path);
  return fp;
}

std::string make_stable_material(const DeviceFingerprint& fp) {
  std::ostringstream os;
  os << fp.platform << '|' << fp.backend << '|' << fp.bustype << '|' << fp.vendor << '|'
     << fp.product << '|' << fp.version << '|' << fp.name << '|' << fp.uniq << '|'
     << fp.phys << '|' << fp.by_id_path << '|' << fp.by_path;
  return os.str();
}

DeviceFingerprint fingerprint_for_fd(int fd, const fs::path& event_path) {
  DeviceFingerprint fp = fingerprint_from_sysfs(event_path);

  char name[256] = {};
  if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && name[0] != '\0') fp.name = name;

  char phys[256] = {};
  if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) >= 0 && phys[0] != '\0') fp.phys = phys;

  char uniq[256] = {};
  if (ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq) >= 0 && uniq[0] != '\0') fp.uniq = uniq;

  input_id id{};
  if (ioctl(fd, EVIOCGID, &id) >= 0) {
    fp.bustype = id.bustype;
    fp.vendor = id.vendor;
    fp.product = id.product;
    fp.version = id.version;
  }

  fp.by_id_path = find_stable_symlink("/dev/input/by-id", event_path);
  fp.by_path = find_stable_symlink("/dev/input/by-path", event_path);
  fp.stable_hash = stable_hash64(make_stable_material(fp));
  return fp;
}

std::string ev_key_name(uint16_t code) {
  switch (code) {
    case BTN_SOUTH: return "BTN_SOUTH";
    case BTN_EAST: return "BTN_EAST";
    case BTN_NORTH: return "BTN_NORTH";
    case BTN_WEST: return "BTN_WEST";
    case BTN_TL: return "BTN_TL";
    case BTN_TR: return "BTN_TR";
    case BTN_TL2: return "BTN_TL2";
    case BTN_TR2: return "BTN_TR2";
    case BTN_SELECT: return "BTN_SELECT";
    case BTN_START: return "BTN_START";
    case BTN_MODE: return "BTN_MODE";
    case BTN_THUMBL: return "BTN_THUMBL";
    case BTN_THUMBR: return "BTN_THUMBR";
    case BTN_LEFT: return "BTN_LEFT";
    case BTN_RIGHT: return "BTN_RIGHT";
    case BTN_MIDDLE: return "BTN_MIDDLE";
    case BTN_SIDE: return "BTN_SIDE";
    case BTN_EXTRA: return "BTN_EXTRA";
    case BTN_FORWARD: return "BTN_FORWARD";
    case BTN_BACK: return "BTN_BACK";
    case KEY_UP: return "KEY_UP";
    case KEY_DOWN: return "KEY_DOWN";
    case KEY_LEFT: return "KEY_LEFT";
    case KEY_RIGHT: return "KEY_RIGHT";
    case KEY_ENTER: return "KEY_ENTER";
    case KEY_KPENTER: return "KEY_KPENTER";
    case KEY_SPACE: return "KEY_SPACE";
    case KEY_ESC: return "KEY_ESC";
    case KEY_Q: return "KEY_Q";
    case KEY_E: return "KEY_E";
    case KEY_R: return "KEY_R";
    case KEY_F: return "KEY_F";
    default:
      if (code >= KEY_A && code <= KEY_Z) {
        char c = static_cast<char>('A' + (code - KEY_A));
        return std::string("KEY_") + c;
      }
      return "KEY_" + std::to_string(code);
  }
}

std::string ev_abs_name(uint16_t code) {
  switch (code) {
    case ABS_X: return "ABS_X";
    case ABS_Y: return "ABS_Y";
    case ABS_Z: return "ABS_Z";
    case ABS_RX: return "ABS_RX";
    case ABS_RY: return "ABS_RY";
    case ABS_RZ: return "ABS_RZ";
    case ABS_HAT0X: return "ABS_HAT0X";
    case ABS_HAT0Y: return "ABS_HAT0Y";
    default: return "ABS_" + std::to_string(code);
  }
}

std::string ev_rel_name(uint16_t code) {
  switch (code) {
    case REL_X: return "REL_X";
    case REL_Y: return "REL_Y";
    case REL_Z: return "REL_Z";
    case REL_RX: return "REL_RX";
    case REL_RY: return "REL_RY";
    case REL_RZ: return "REL_RZ";
    case REL_WHEEL: return "REL_WHEEL";
    case REL_HWHEEL: return "REL_HWHEEL";
    default: return "REL_" + std::to_string(code);
  }
}

bool interesting_event(const input_event& ev) {
  if (ev.type == EV_KEY) return ev.value == 0 || ev.value == 1;
  if (ev.type == EV_ABS) return true;
  if (ev.type == EV_REL) return ev.value != 0;
  return false;
}

constexpr size_t kBitsPerWord = sizeof(unsigned long) * 8u;
constexpr size_t kKeyWords = (KEY_MAX + 1u + kBitsPerWord - 1u) / kBitsPerWord;
using KeyBits = std::array<unsigned long, kKeyWords>;

bool read_key_bits(int fd, KeyBits& bits) {
  bits.fill(0);
  return ioctl(fd, EVIOCGKEY(static_cast<int>(bits.size() * sizeof(bits[0]))), bits.data()) >= 0;
}

bool has_pressed_key(int fd) {
  KeyBits keys{};
  if (!read_key_bits(fd, keys)) return false;
  for (unsigned long word : keys) {
    if (word != 0) return true;
  }
  return false;
}

void drain_fd_events(int fd) {
  input_event ev{};
  while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {}
}

bool wait_for_pressed_keys_before_grab(int fd,
                                      size_t idx,
                                      const DeviceFingerprint& fp,
                                      std::ostream* log) {
  if (fd < 0 || !has_pressed_key(fd)) return true;

  if (log) {
    *log << "[override_controller][WARN] input device [" << idx << "] " << short_device_label(fp)
         << " still has pressed keys/buttons; waiting for release before EVIOCGRAB to avoid stuck input.\n";
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < deadline) {
    drain_fd_events(fd);
    if (!has_pressed_key(fd)) {
      // Give the Linux input stack a tiny window to deliver key/button-up events
      // before this process takes the exclusive EVIOCGRAB. This matters most for
      // keyboards and terminals, but it is harmless for gamepads/remotes too.
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      drain_fd_events(fd);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (log) {
    *log << "[override_controller][WARN] input device [" << idx
         << "] still reports pressed keys/buttons; NOT enabling EVIOCGRAB for this device "
            "to avoid stuck input. Release the key/button and restart override_controller if "
            "you need this device to be blocked.\n";
  }
  return false;
}

InputEvent make_stop_event(size_t device_index, uint16_t code) {
  InputEvent out;
  out.device_index = device_index;
  out.type = EV_KEY;
  out.code = code;
  out.value = 1;
  out.timestamp_ns = now_ns();
  out.stop_requested = true;
  return out;
}

}  // namespace

std::vector<DeviceInfo> LinuxEvdevInputProvider::scan_devices(bool open_readable) {
  std::vector<DeviceInfo> out;
  std::error_code ec;
  const fs::path input_dir("/dev/input");
  if (!fs::exists(input_dir, ec)) return out;

  for (const auto& entry : fs::directory_iterator(input_dir, ec)) {
    if (ec) break;
    const auto name = entry.path().filename().string();
    if (name.rfind("event", 0) != 0) continue;

    DeviceInfo dev;
    std::string open_error;
    const int fd = open_evdev_readonly_nonblocking(entry.path(), open_error);
    if (fd < 0) {
      dev.fingerprint = fingerprint_from_sysfs(entry.path());
      dev.fingerprint.stable_hash = stable_hash64(make_stable_material(dev.fingerprint));
      dev.open_error = open_error;
      dev.readable = false;
      out.push_back(std::move(dev));
      continue;
    }

    dev.fingerprint = fingerprint_for_fd(fd, entry.path());
    dev.readable = true;
    if (open_readable) {
      dev.fd = fd;
    } else {
      close(fd);
    }
    out.push_back(std::move(dev));
  }

  std::sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
    return a.fingerprint.event_path < b.fingerprint.event_path;
  });
  return out;
}

void LinuxEvdevInputProvider::flush_events(std::vector<DeviceInfo>& devices) {
  input_event ev{};
  for (auto& d : devices) {
    if (d.fd < 0) continue;
    while (read(d.fd, &ev, sizeof(ev)) == sizeof(ev)) {}
  }
}

void LinuxEvdevInputProvider::close_devices(std::vector<DeviceInfo>& devices) {
  pending_events_.clear();
  left_ctrl_down_ = false;
  right_ctrl_down_ = false;
  for (auto& d : devices) {
    if (d.fd >= 0) {
      (void)ioctl(d.fd, EVIOCGRAB, 0);
      close(d.fd);
      d.fd = -1;
    }
    d.readable = false;
  }
}

std::optional<InputEvent> LinuxEvdevInputProvider::wait_event(std::vector<DeviceInfo>& devices,
                                                                      int timeout_ms,
                                                                      bool include_stdin) {
  if (!pending_events_.empty()) {
    InputEvent out = pending_events_.front();
    pending_events_.pop_front();
    return out;
  }

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = timeout_ms >= 0 ? start + std::chrono::milliseconds(timeout_ms)
                                      : std::chrono::steady_clock::time_point::max();

  while (true) {
    fd_set rfds;
    FD_ZERO(&rfds);
    int max_fd = -1;

    if (include_stdin) {
      FD_SET(STDIN_FILENO, &rfds);
      max_fd = std::max(max_fd, STDIN_FILENO);
    }

    for (const auto& d : devices) {
      if (d.fd < 0) continue;
      FD_SET(d.fd, &rfds);
      max_fd = std::max(max_fd, d.fd);
    }

    if (max_fd < 0) return std::nullopt;

    timeval tv{};
    timeval* tv_ptr = nullptr;
    if (timeout_ms >= 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return std::nullopt;
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      tv.tv_sec = static_cast<long>(remaining.count() / 1000);
      tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
      tv_ptr = &tv;
    }

    const int rc = select(max_fd + 1, &rfds, nullptr, nullptr, tv_ptr);
    if (rc == 0) return std::nullopt;
    if (rc < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }

    if (include_stdin && FD_ISSET(STDIN_FILENO, &rfds)) {
      std::string dummy;
      std::getline(std::cin, dummy);
      InputEvent ev;
      ev.device_index = std::numeric_limits<size_t>::max();
      ev.timestamp_ns = now_ns();
      return ev;
    }

    // Realtime input must not replay a kernel evdev backlog after the service stalls.
    // Drain every ready fd and compact to the latest value per (device,type,code).
    // This preserves final held state for sticks/buttons while dropping old transient
    // press/release/axis history that would otherwise appear in SteamVR as a seconds-long queue.
    std::map<std::tuple<size_t, uint16_t, uint16_t>, size_t> compact_index;
    std::vector<InputEvent> compacted;
    bool saw_ready_device = false;

    for (size_t i = 0; i < devices.size(); ++i) {
      auto& d = devices[i];
      if (d.fd < 0 || !FD_ISSET(d.fd, &rfds)) continue;
      saw_ready_device = true;

      input_event ev{};
      ssize_t n = 0;
      while ((n = read(d.fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_KEY) {
          const bool key_down = ev.value == 1 || ev.value == 2;
          if (ev.code == KEY_LEFTCTRL) {
            left_ctrl_down_ = key_down;
          } else if (ev.code == KEY_RIGHTCTRL) {
            right_ctrl_down_ = key_down;
          }

          // Reserved escape hatch for EVIOCGRAB. When a keyboard is grabbed,
          // the terminal may not receive Ctrl+C/SIGINT, so detect it directly
          // from raw evdev before event compaction. Esc is a single-key fallback.
          if (key_down && (ev.code == KEY_ESC ||
                           (ev.code == KEY_C && (left_ctrl_down_ || right_ctrl_down_)))) {
            return make_stop_event(i, static_cast<uint16_t>(ev.code));
          }
        }

        if (!interesting_event(ev)) continue;

        InputEvent out;
        out.device_index = i;
        out.type = static_cast<uint16_t>(ev.type);
        out.code = static_cast<uint16_t>(ev.code);
        out.value = ev.value;
        out.timestamp_ns = 0;

        const auto key = std::make_tuple(out.device_index, out.type, out.code);
        const auto it = compact_index.find(key);
        if (it == compact_index.end()) {
          compact_index.emplace(key, compacted.size());
          compacted.push_back(out);
        } else {
          compacted[it->second] = out;
        }
      }

      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        d.open_error = std::strerror(errno);
        d.readable = false;
        if (d.fd >= 0) {
          (void)ioctl(d.fd, EVIOCGRAB, 0);
          close(d.fd);
          d.fd = -1;
        }
      }
    }

    if (!compacted.empty()) {
      const int64_t ts = now_ns();
      for (auto& ev : compacted) {
        ev.timestamp_ns = ts;
        pending_events_.push_back(ev);
      }
      InputEvent out = pending_events_.front();
      pending_events_.pop_front();
      return out;
    }

    // A device can wake select() with only EV_SYN/EV_MSC/noise or with data
    // that was drained by another read. Do not report that as a user-facing
    // timeout; keep waiting until the original deadline expires.
    if (!saw_ready_device && timeout_ms == 0) return std::nullopt;
  }
}

std::string LinuxEvdevInputProvider::input_name(uint16_t type, uint16_t code) const {
  if (type == EV_KEY) return ev_key_name(code);
  if (type == EV_ABS) return ev_abs_name(code);
  if (type == EV_REL) return ev_rel_name(code);
  return "EV" + std::to_string(type) + ":" + std::to_string(code);
}

InputBindingSpec LinuxEvdevInputProvider::make_input_spec(const DeviceInfo& device,
                                                          uint16_t type,
                                                          uint16_t code) const {
  InputBindingSpec spec;
  spec.type = type;
  spec.code = code;
  spec.name = input_name(type, code);
  if (type == EV_ABS) spec.kind = InputKind::AbsAxis;
  else if (type == EV_REL) spec.kind = InputKind::RelAxis;
  else spec.kind = InputKind::Key;
  if (device.fd >= 0 && type == EV_ABS) {
    input_absinfo abs{};
    if (ioctl(device.fd, EVIOCGABS(code), &abs) >= 0) {
      spec.abs_min = abs.minimum;
      spec.abs_max = abs.maximum;
      spec.abs_flat = abs.flat;
    }
  }
  return spec;
}

bool LinuxEvdevInputProvider::set_device_grab(std::vector<DeviceInfo>& devices,
                                              const std::set<size_t>& device_indices,
                                              bool enabled,
                                              std::ostream* log) {
  bool any_ok = false;
  for (const size_t idx : device_indices) {
    if (idx >= devices.size()) continue;
    auto& d = devices[idx];
    if (d.fd < 0) {
      if (log) {
        *log << "[override_controller][WARN] cannot " << (enabled ? "grab" : "ungrab")
             << " unopened device [" << idx << "] " << short_device_label(d.fingerprint) << "\n";
      }
      continue;
    }

    if (enabled) {
      // Avoid grabbing while a key/button is already down. If EVIOCGRAB is enabled
      // before the key/button-up reaches the original consumer, that consumer can
      // see a permanently pressed input until this process exits. This is most
      // visible with keyboards/terminals, but the check is cheap and safe for all
      // EV_KEY-capable devices.
      if (!wait_for_pressed_keys_before_grab(d.fd, idx, d.fingerprint, log)) {
        continue;
      }
    }

    errno = 0;
    if (ioctl(d.fd, EVIOCGRAB, enabled ? 1 : 0) < 0) {
      if (log) {
        *log << "[override_controller][WARN] EVIOCGRAB " << (enabled ? "enable" : "disable")
             << " failed for [" << idx << "] " << short_device_label(d.fingerprint)
             << ": " << std::strerror(errno) << "\n";
      }
      continue;
    }

    any_ok = true;
    if (log) {
      *log << "[override_controller] " << (enabled ? "grabbed" : "released")
           << " input device [" << idx << "] " << short_device_label(d.fingerprint) << "\n";
      if (enabled) {
        *log << "[override_controller][WARN] Input device [" << idx
             << "] is blocked by override_controller. Press Ctrl+C or Esc to stop "
                "override_controller and release the device.\n";
      }
    }
  }
  return any_ok;
}

}  // namespace xr_override_controller
