#include "capture_service_cpp/sources/camera_source.hpp"

#include <stdexcept>

namespace xr_capture_cpp {

std::unique_ptr<ICameraSource> make_xreal_camera_source(const RuntimeConfig& cfg);
std::unique_ptr<ICameraSource> make_opencv_stereo_camera_source(const RuntimeConfig& cfg);

std::unique_ptr<ICameraSource> create_camera_source(const RuntimeConfig& cfg) {
  if (cfg.camera.driver == "xreal_ultra") return make_xreal_camera_source(cfg);
  if (cfg.camera.driver == "opencv") return make_opencv_stereo_camera_source(cfg);
  throw std::runtime_error("unsupported camera source: " + cfg.camera.driver);
}

}  // namespace xr_capture_cpp
