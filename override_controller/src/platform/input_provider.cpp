#include <xr_override_controller/input_provider.hpp>

#include "composite_input_provider.hpp"
#include "../providers/gearvr/gearvr_input_provider.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include "linux/linux_evdev_input_provider.hpp"
#elif defined(_WIN32)
#include "windows/windows_input_provider.hpp"
#endif

namespace xr_override_controller {
namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace

std::unique_ptr<InputProvider> make_input_provider(const InputProviderOptions& options) {
  std::vector<std::unique_ptr<InputProvider>> enabled;
  std::vector<std::string> names = options.providers;
  if (names.empty()) {
#if defined(__linux__)
    names.push_back("evdev");
#elif defined(_WIN32)
    names.push_back("windows");
#endif
  }

  for (std::string name : names) {
    name = lower_copy(name);
#if defined(__linux__)
    if (name == "evdev" || name == "linux_evdev") {
      enabled.push_back(std::make_unique<LinuxEvdevInputProvider>());
    } else if (name == "gearvr_ble" || name == "gearvr") {
      enabled.push_back(std::make_unique<gearvr::GearVrInputProvider>(options));
    } else {
      throw std::runtime_error("unknown Linux input provider: " + name);
    }
#elif defined(_WIN32)
    if (name == "windows" || name == "rawinput" || name == "xinput") {
      enabled.push_back(std::make_unique<WindowsInputProvider>());
    } else if (name == "gearvr_ble" || name == "gearvr") {
      enabled.push_back(std::make_unique<gearvr::GearVrInputProvider>(options));
    } else {
      throw std::runtime_error("unknown Windows input provider: " + name);
    }
#else
    (void)name;
    throw std::runtime_error("no input providers for this platform yet");
#endif
  }

  if (enabled.size() == 1) return std::move(enabled.front());
  return std::make_unique<CompositeInputProvider>(std::move(enabled));
}

}  // namespace xr_override_controller
