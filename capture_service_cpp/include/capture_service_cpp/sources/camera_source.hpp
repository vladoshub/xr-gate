#pragma once

#include "capture_service_cpp/common.hpp"

#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace xr_capture_cpp {
class XrealDeviceClock;
class TcpCaptureSession;
enum class TcpInputRole;

struct CameraViewSpec {
  std::string source_id;
  int width = 0;
  int height = 0;
};

std::string resolve_camera_view_id(const CameraSourceConfig& cfg, const std::vector<CameraViewSpec>& views,
                                   const std::string& id);

class CameraViewSink {
 public:
  virtual ~CameraViewSink() = default;
  virtual bool wants(const std::string& source_id) const noexcept = 0;
  // Decoded GRAY8 pixels before the stereo output transforms. The driver can
  // reuse this buffer as soon as submit returns. Timestamps use the capture
  // clock, and views with the same hardware exposure can share a timestamp.
  virtual void submit(const std::string& source_id, const cv::Mat& image, uint64_t timestamp_ns) noexcept = 0;
  // Publish the views submitted by one read together when the worker takes
  // its next batch. A read may emit a view without emitting a stereo pair.
  virtual bool finish_read() noexcept = 0;
};

inline const std::string kLeftCameraView = "left";
inline const std::string kRightCameraView = "right";

struct StereoFrame {
  cv::Mat left;
  cv::Mat right;
  uint64_t timestamp_ns = 0;
  bool complete() const { return !left.empty() && !right.empty(); }
};

class ICameraSource {
 public:
  virtual ~ICameraSource() = default;
  virtual std::string name() const = 0;
  virtual std::vector<StreamSpec> stream_specs(uint32_t slot_count) const = 0;
  virtual std::vector<CameraViewSpec> views() const { return {}; }
  virtual void open() = 0;
  // Submit decoded views before returning; the caller commits the read to the sink.
  virtual SourceReadStatus read(StereoFrame& frame, CameraViewSink* view_sink = nullptr) = 0;
  virtual void close() = 0;
};

std::unique_ptr<ICameraSource> create_camera_source(
    const RuntimeConfig& cfg, std::shared_ptr<XrealDeviceClock> xreal_clock = {},
    std::shared_ptr<TcpCaptureSession> tcp_session = {});

std::unique_ptr<ICameraSource> make_tcp_camera_source(
    const CameraSourceConfig& cfg, TcpInputRole role, int channels = 1,
    std::shared_ptr<TcpCaptureSession> session = {});

}  // namespace xr_capture_cpp
