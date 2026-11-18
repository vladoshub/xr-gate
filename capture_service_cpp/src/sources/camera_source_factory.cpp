#include "capture_service_cpp/sources/camera_source.hpp"
#include "capture_service_cpp/sources/tcp_input.hpp"

#include <algorithm>
#include <stdexcept>

namespace xr_capture_cpp {

std::unique_ptr<ICameraSource> make_xreal_camera_source(
    const RuntimeConfig& cfg, std::shared_ptr<XrealDeviceClock> clock);
std::unique_ptr<ICameraSource> make_opencv_stereo_camera_source(const RuntimeConfig& cfg);

std::unique_ptr<ICameraSource> create_camera_source(
    const RuntimeConfig& cfg, std::shared_ptr<XrealDeviceClock> xreal_clock,
    std::shared_ptr<TcpCaptureSession> tcp_session) {
  if (cfg.camera.driver == "xreal_ultra") return make_xreal_camera_source(cfg, std::move(xreal_clock));
  if (cfg.camera.driver == "opencv") return make_opencv_stereo_camera_source(cfg);
  if (cfg.camera.driver == "tcp") {
    return make_tcp_camera_source(cfg.camera, TcpInputRole::Primary, 1, std::move(tcp_session));
  }
  throw std::runtime_error("unsupported camera source: " + cfg.camera.driver);
}

std::string resolve_camera_view_id(const CameraSourceConfig& cfg, const std::vector<CameraViewSpec>& views,
                                   const std::string& id) {
  if (std::any_of(views.begin(), views.end(), [&](const CameraViewSpec& view) { return view.source_id == id; })) {
    return id;
  }
  if (id == cfg.left_stream_id) return kLeftCameraView;
  if (id == cfg.right_stream_id) return kRightCameraView;
  return id;
}

}  // namespace xr_capture_cpp
