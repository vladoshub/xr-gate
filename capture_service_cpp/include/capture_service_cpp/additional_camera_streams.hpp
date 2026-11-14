#pragma once

#include "capture_service_cpp/sources/camera_source.hpp"
#include "capture_service_cpp/stream_publishers.hpp"

#include <memory>

namespace xr_capture_cpp {

class AdditionalCameraStreams final : public CameraViewSink {
 public:
  AdditionalCameraStreams(const RuntimeConfig& cfg, const ICameraSource& source,
                          const StreamPublishers& primary);
  ~AdditionalCameraStreams() override;

  const StreamPublishers& publishers() const;
  bool wants(const std::string& source_id) const noexcept override;
  void submit(const std::string& source_id, const cv::Mat& image, uint64_t timestamp_ns) noexcept override;
  bool finish_read() noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xr_capture_cpp
