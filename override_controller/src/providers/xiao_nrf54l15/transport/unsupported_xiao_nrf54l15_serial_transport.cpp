#include "../xiao_nrf54l15_serial_transport.hpp"

namespace xr_override_controller::xiao_nrf54l15 {

std::unique_ptr<SerialTransport> make_platform_serial_transport(
    const XiaoNrf54l15Options& options) {
  (void)options;
  return {};
}

}  // namespace xr_override_controller::xiao_nrf54l15
