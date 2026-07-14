#include "../gearvr_ble_transport.hpp"

#include <stdexcept>

namespace xr_override_controller::gearvr {

std::unique_ptr<BleTransport> make_platform_ble_transport(const InputProviderOptions&) {
  throw std::runtime_error(
      "Gear VR BLE transport is not implemented on Windows yet; add the C++/WinRT "
      "BleTransport backend without changing GearVrInputProvider");
}

}  // namespace xr_override_controller::gearvr
