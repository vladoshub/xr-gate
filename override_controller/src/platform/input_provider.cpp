#include <xr_override_controller/input_provider.hpp>

#include <memory>

#if defined(__linux__)
#include "linux/linux_evdev_input_provider.hpp"
#elif defined(_WIN32)
#include "windows/windows_input_provider.hpp"
#endif

namespace xr_override_controller {

std::unique_ptr<InputProvider> make_platform_input_provider() {
#if defined(__linux__)
  return std::make_unique<LinuxEvdevInputProvider>();
#elif defined(_WIN32)
  return std::make_unique<WindowsInputProvider>();
#else
  return nullptr;
#endif
}

}  // namespace xr_override_controller
