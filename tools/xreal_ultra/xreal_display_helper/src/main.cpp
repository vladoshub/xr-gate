#include "xreal_display_helper/display_helper.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

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
  using namespace xreal_display_helper;

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
      throw std::runtime_error("hid_open_path failed; " + platform_hid_open_failure_hint());
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
      platform_print_keep_running_hint(std::cout);

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
