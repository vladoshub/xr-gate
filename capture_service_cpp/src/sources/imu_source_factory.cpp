#include "capture_service_cpp/sources/imu_source.hpp"

#include <stdexcept>

namespace xr_capture_cpp {

std::unique_ptr<IImuSource> make_xreal_hid_imu_source(const RuntimeConfig& cfg);
std::unique_ptr<IImuSource> make_serial_imu_source(const RuntimeConfig& cfg);
std::unique_ptr<IImuSource> make_synthetic_imu_source(const RuntimeConfig& cfg);

std::unique_ptr<IImuSource> create_imu_source(const RuntimeConfig& cfg) {
  if (cfg.imu.driver == "xreal_hid") return make_xreal_hid_imu_source(cfg);
  if (cfg.imu.driver == "serial") return make_serial_imu_source(cfg);
  if (cfg.imu.driver == "synthetic") return make_synthetic_imu_source(cfg);
  throw std::runtime_error("unsupported IMU source: " + cfg.imu.driver);
}

}  // namespace xr_capture_cpp
