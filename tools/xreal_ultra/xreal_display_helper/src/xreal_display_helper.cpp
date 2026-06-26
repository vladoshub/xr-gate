#include <hidapi/hidapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

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

void put_le16(std::array<uint8_t, 64>& out, size_t off, uint16_t v) {
  out[off + 0] = static_cast<uint8_t>(v & 0xff);
  out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void put_le32(std::array<uint8_t, 64>& out, size_t off, uint32_t v) {
  out[off + 0] = static_cast<uint8_t>(v & 0xff);
  out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
  out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
  out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint16_t get_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t crc32_adler_compatible(const uint8_t* data, size_t size) {
  uint32_t r = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    r ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      if ((r & 1u) != 0u) {
        r = (r >> 1u) ^ 0xedb88320u;
      } else {
        r >>= 1u;
      }
    }
  }
  return r ^ 0xffffffffu;
}

std::array<uint8_t, 64> serialize_mcu_packet(uint16_t cmd_id, const std::vector<uint8_t>& payload) {
  if (payload.size() > 42) {
    throw std::runtime_error("MCU payload too large");
  }

  std::array<uint8_t, 64> out{};
  const uint16_t length = static_cast<uint16_t>(payload.size() + 17);

  out[0] = 0xfd;
  put_le32(out, 1, 0);          // checksum placeholder
  put_le16(out, 5, length);     // length
  put_le32(out, 7, 0x1337);     // request_id
  put_le32(out, 11, 0);         // timestamp
  put_le16(out, 15, cmd_id);    // cmd_id
  // reserved: 17..21
  std::copy(payload.begin(), payload.end(), out.begin() + 22);

  const uint32_t crc = crc32_adler_compatible(out.data() + 5, length);
  put_le32(out, 1, crc);
  return out;
}

bool parse_mcu_packet(const uint8_t* data, size_t size, McuPacket& packet) {
  size_t off = 0;

#if defined(_WIN32)
  if (size >= 65 && data[0] == 0x00 && data[1] == 0xfd) {
    off = 1;
  }
#endif

  if (size < off + 64 || data[off + 0] != 0xfd) {
    return false;
  }

  const uint16_t length = get_le16(data + off + 5);
  if (length < 17 || length > 59) {
    return false;
  }

  const size_t payload_len = static_cast<size_t>(length - 17);
  if (payload_len > 42) {
    return false;
  }

  packet.cmd_id = get_le16(data + off + 15);
  packet.data.assign(data + off + 22, data + off + 22 + payload_len);
  return true;
}

std::string wide_to_utf8(const wchar_t* w) {
  if (!w) return {};
  std::string s;
  while (*w) {
    const wchar_t c = *w++;
    if (c >= 32 && c < 127) {
      s.push_back(static_cast<char>(c));
    } else {
      s.push_back('?');
    }
  }
  return s;
}

std::string hid_error_string(hid_device* dev) {
  const wchar_t* e = ::hid_error(dev);
  return e ? wide_to_utf8(e) : "unknown hidapi error";
}

std::string product_name(uint16_t pid) {
  switch (pid) {
    case kAirPid: return "XREAL Air";
    case kAir2Pid: return "XREAL Air 2";
    case kAir2ProPid: return "XREAL Air 2 Pro";
    case kAir2UltraPid: return "XREAL Air 2 Ultra";
    default: return "unknown";
  }
}

int mcu_interface_for_pid(uint16_t pid) {
  return pid == kAir2UltraPid ? 0 : 4;
}

bool supported_pid(uint16_t pid) {
  return pid == kAirPid || pid == kAir2Pid || pid == kAir2ProPid || pid == kAir2UltraPid;
}

std::string mode_name(uint8_t mode) {
  switch (mode) {
    case 1: return "2d/same";
    case 3: return "sbs/3d 60Hz";
    case 8: return "half-sbs";
    case 9: return "high-refresh-sbs 90Hz";
    case 10: return "high-refresh-2d 90Hz";
    case 11: return "high-refresh-2d 120Hz";
    default: return "unknown";
  }
}

uint8_t parse_mode_byte(const std::string& mode) {
  std::string m = mode;
  std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (m == "2d" || m == "same" || m == "same-on-both") return 1;
  if (m == "3d" || m == "sbs" || m == "stereo" || m == "60" || m == "60hz") return 3;
  if (m == "halfsbs" || m == "half-sbs" || m == "sbs2") return 8;
  if (m == "90" || m == "90hz" || m == "high-refresh-sbs" || m == "high-refresh-rate-3d") return 9;
  if (m == "120" || m == "120hz" || m == "high-refresh-2d" || m == "high-refresh-rate-2d") return 11;

  throw std::runtime_error("unknown mode: " + mode);
}

std::vector<uint8_t> run_command(hid_device* dev, uint16_t cmd_id, const std::vector<uint8_t>& data) {
  const auto packet = serialize_mcu_packet(cmd_id, data);

#if defined(_WIN32)
  std::array<uint8_t, 65> report{};
  std::copy(packet.begin(), packet.end(), report.begin() + 1);
  const int written = hid_write(dev, report.data(), report.size());
#else
  const int written = hid_write(dev, packet.data(), packet.size());
#endif

  if (written < 0) {
    throw std::runtime_error("hid_write failed: " + hid_error_string(dev));
  }

  for (int attempt = 0; attempt < 64; ++attempt) {
    std::array<uint8_t, 65> buf{};
    const int n = hid_read_timeout(dev, buf.data(), buf.size(), kCommandTimeoutMs);
    if (n < 0) {
      throw std::runtime_error("hid_read_timeout failed: " + hid_error_string(dev));
    }
    if (n == 0) {
      continue;
    }

    McuPacket response;
    if (!parse_mcu_packet(buf.data(), static_cast<size_t>(n), response)) {
      continue;
    }

    if (response.cmd_id == cmd_id) {
      return response.data;
    }
  }

  throw std::runtime_error("MCU command timed out or got too many unrelated packets");
}

uint8_t get_display_mode(hid_device* dev) {
  const auto response = run_command(dev, 0x07, {});
  if (response.size() < 2) {
    throw std::runtime_error("get display mode response too short");
  }
  return response[1];
}

void set_display_mode(hid_device* dev, uint8_t mode) {
  const auto response = run_command(dev, 0x08, {mode});
  if (response.empty() || response[0] != 0) {
    throw std::runtime_error("display mode setting unsuccessful");
  }
}

struct DeviceChoice {
  std::string path;
  uint16_t pid = 0;
  int interface_number = -1;
  std::string serial;
  std::string product;
};

DeviceChoice find_xreal_mcu_device() {
  hid_device_info* list = hid_enumerate(kXrealVid, 0);
  if (!list) {
    throw std::runtime_error("no XREAL/Nreal HID devices found");
  }

  DeviceChoice choice;
  bool found = false;

  for (hid_device_info* d = list; d; d = d->next) {
    if (d->vendor_id != kXrealVid || !supported_pid(d->product_id)) {
      continue;
    }

    const int target_iface = mcu_interface_for_pid(d->product_id);
    if (d->interface_number != target_iface) {
      continue;
    }

    choice.path = d->path ? d->path : "";
    choice.pid = d->product_id;
    choice.interface_number = d->interface_number;
    choice.serial = wide_to_utf8(d->serial_number);
    choice.product = wide_to_utf8(d->product_string);
    found = true;
    break;
  }

  hid_free_enumeration(list);

  if (!found) {
    throw std::runtime_error("supported XREAL glasses found, but MCU HID interface was not found");
  }

  return choice;
}

void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " --mode {90hz|60hz|3d|2d|half-sbs|120hz} [--keep-running] [--poll-seconds N]\n"
      << "\n"
      << "Examples:\n"
      << "  " << argv0 << " --mode 90hz --keep-running\n"
      << "  " << argv0 << " --mode 60hz --keep-running\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode_arg = "90hz";
  bool keep_running = false;
  int poll_seconds = 2;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--mode" || arg == "-m") {
      if (++i >= argc) {
        usage(argv[0]);
        return 2;
      }
      mode_arg = argv[i];
    } else if (arg == "--keep-running" || arg == "-k") {
      keep_running = true;
    } else if (arg == "--poll-seconds") {
      if (++i >= argc) {
        usage(argv[0]);
        return 2;
      }
      poll_seconds = std::max(1, std::stoi(argv[i]));
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  try {
    const uint8_t mode = parse_mode_byte(mode_arg);

    if (hid_init() != 0) {
      throw std::runtime_error("hid_init failed");
    }

    const DeviceChoice device = find_xreal_mcu_device();

    std::cout << "[xreal_display_helper] device: " << product_name(device.pid)
              << " pid=0x" << std::hex << std::setw(4) << std::setfill('0') << device.pid << std::dec
              << " iface=" << device.interface_number
              << " serial=" << (device.serial.empty() ? "(unknown)" : device.serial)
              << " product=" << (device.product.empty() ? "(unknown)" : device.product)
              << "\n";

    hid_device* dev = hid_open_path(device.path.c_str());
    if (!dev) {
      throw std::runtime_error("hid_open_path failed; check permissions/udev or try sudo");
    }

    const uint8_t before = get_display_mode(dev);
    std::cout << "[xreal_display_helper] display mode before: " << static_cast<int>(before)
              << " (" << mode_name(before) << ")\n";

    std::cout << "[xreal_display_helper] setting display mode: " << static_cast<int>(mode)
              << " (" << mode_name(mode) << ")\n";
    set_display_mode(dev, mode);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const uint8_t after = get_display_mode(dev);
    std::cout << "[xreal_display_helper] display mode after:  " << static_cast<int>(after)
              << " (" << mode_name(after) << ")\n";

    if (keep_running) {
      std::cout << "[xreal_display_helper] keep-running enabled; keep this process alive while using SteamVR.\n";
      std::cout << "[xreal_display_helper] verify with: xrandr --query | grep -A15 \"DisplayPort-2\"\n";

      while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(poll_seconds));
        try {
          const uint8_t current = get_display_mode(dev);
          std::cout << "[xreal_display_helper] alive; mode=" << static_cast<int>(current)
                    << " (" << mode_name(current) << ")\n";
        } catch (const std::exception& e) {
          std::cerr << "[xreal_display_helper] keepalive warning: " << e.what() << "\n";
        }
      }
    }

    hid_close(dev);
    hid_exit();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[xreal_display_helper] ERROR: " << e.what() << "\n";
    hid_exit();
    return 1;
  }
}
