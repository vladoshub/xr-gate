#pragma once

#include "capture_service_cpp/common.hpp"

#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace xr_capture_cpp {

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
  virtual void open() = 0;
  virtual SourceReadStatus read(StereoFrame& frame) = 0;
  virtual void close() = 0;
};

std::unique_ptr<ICameraSource> create_camera_source(const RuntimeConfig& cfg);

}  // namespace xr_capture_cpp
