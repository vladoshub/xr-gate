#include "capture_service_cpp/image_transform.hpp"
#include "capture_service_cpp/sources/camera_source.hpp"
#include "capture_service_cpp/sources/imu_source.hpp"
#include "capture_service_cpp/sources/tcp_input.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xr_capture_cpp {
namespace {

std::vector<TcpInputView> camera_views(const CameraSourceConfig& cfg, int channels) {
  const int width = cfg.primary.width;
  const int height = cfg.primary.height;
  if (cfg.layout == "mono") return {{"mono", width, height, channels}};
  std::vector<TcpInputView> views{{kLeftCameraView, width, height}, {kRightCameraView, width, height}};
  for (const auto& [id, size] : cfg.tcp_views) views.push_back({id, size.width, size.height});
  return views;
}

void validate_dimensions(const CameraSourceConfig& cfg, const ImageTransformConfig& transform) {
  const bool swap = image_rotation_swaps_dimensions(transform.rotate);
  const int width = swap ? cfg.primary.height : cfg.primary.width;
  const int height = swap ? cfg.primary.width : cfg.primary.height;
  if (width != cfg.output_width || height != cfg.output_height) {
    throw std::runtime_error("TCP camera output dimensions must match the input dimensions after rotation");
  }
}

class TcpCameraSource final : public ICameraSource {
 public:
  TcpCameraSource(const CameraSourceConfig& cfg, TcpInputRole role, int channels,
                  std::shared_ptr<TcpCaptureSession> session)
      : cfg_(cfg), views_(camera_views(cfg, channels)),
        input_(cfg.tcp, role, cfg.stall_exit_ms, views_, std::move(session)) {
    validate_dimensions(cfg_, cfg_.left_transform);
    if (cfg_.layout == "stereo") validate_dimensions(cfg_, cfg_.right_transform);
  }

  std::string name() const override { return "tcp"; }

  std::vector<StreamSpec> stream_specs(uint32_t slots) const override {
    const auto width = static_cast<uint32_t>(cfg_.output_width);
    const auto height = static_cast<uint32_t>(cfg_.output_height);
    const uint64_t bytes = static_cast<uint64_t>(width) * height;
    if (bytes > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("TCP camera output is too large");
    return {
        {cfg_.left_stream_id, kKindImage, "IMAGE", width, height, kFormatGray8, "GRAY8",
         static_cast<uint32_t>(bytes), slots, cfg_.left_frame_id, "TCP stereo left"},
        {cfg_.right_stream_id, kKindImage, "IMAGE", width, height, kFormatGray8, "GRAY8",
         static_cast<uint32_t>(bytes), slots, cfg_.right_frame_id, "TCP stereo right"},
    };
  }

  std::vector<CameraViewSpec> views() const override {
    std::vector<CameraViewSpec> result;
    for (const auto& view : views_) result.push_back({view.id, view.width, view.height});
    return result;
  }

  void open() override { input_.open(); }
  void close() override { input_.close(); }

  SourceReadStatus read(StereoFrame& frame, CameraViewSink* sink) override {
    frame = {};
    TcpInputSample sample;
    if (!input_.read(sample)) return SourceReadStatus::Timeout;
    frame.timestamp_ns = sample.timestamp_ns;
    if (cfg_.stereo_enabled || cfg_.layout == "mono") {
      frame.left = transform_image(image(0), cfg_.left_transform.rotate, cfg_.left_transform.flip);
      if (cfg_.layout == "stereo") {
        frame.right = transform_image(image(1), cfg_.right_transform.rotate, cfg_.right_transform.flip);
      }
    }
    if (sink) {
      for (size_t i = 0; i < views_.size(); ++i) {
        if (sink->wants(views_[i].id)) sink->submit(views_[i].id, image(i), frame.timestamp_ns);
      }
    }
    return SourceReadStatus::Data;
  }

 private:
  cv::Mat image(size_t index) {
    const auto& view = views_[index];
    return cv::Mat(view.height, view.width, CV_MAKETYPE(CV_8U, view.channels), input_.pixels(index));
  }

  CameraSourceConfig cfg_;
  std::vector<TcpInputView> views_;
  TcpSensorInput input_;
};

class TcpImuSource final : public IImuSource {
 public:
  TcpImuSource(const ImuSourceConfig& cfg, std::shared_ptr<TcpCaptureSession> session)
      : cfg_(cfg), input_(cfg.tcp, TcpInputRole::Imu, cfg.stall_exit_ms, {}, std::move(session)) {}

  std::string name() const override { return "tcp"; }

  std::vector<StreamSpec> stream_specs(uint32_t slots, uint32_t) const override {
    return {{cfg_.stream_id, kKindImu, "IMU", 0, 0, kFormatImuF32Le, "IMU_F32_LE", 24,
             slots, cfg_.frame_id, "TCP gyro rad/s and accelerometer m/s^2 sample"}};
  }

  void open() override { input_.open(); }
  void close() override { input_.close(); }

  SourceReadStatus read(ImuReadResult& result) override {
    result = {};
    TcpInputSample sample;
    if (!input_.read(sample)) return SourceReadStatus::Timeout;
    std::copy_n(sample.imu.begin(), 3, result.sample.gyro_rad_s.begin());
    std::copy_n(sample.imu.begin() + 3, 3, result.sample.accel_m_s2.begin());
    result.sample.timestamp_ns = sample.timestamp_ns;
    result.sample.source_timestamp_valid = true;
    result.sample.source_sequence = static_cast<uint32_t>(sample.sequence);
    result.has_sample = true;
    return SourceReadStatus::Data;
  }

 private:
  ImuSourceConfig cfg_;
  TcpSensorInput input_;
};

}  // namespace

std::unique_ptr<ICameraSource> make_tcp_camera_source(
    const CameraSourceConfig& cfg, TcpInputRole role, int channels, std::shared_ptr<TcpCaptureSession> session) {
  return std::make_unique<TcpCameraSource>(cfg, role, channels, std::move(session));
}

std::unique_ptr<IImuSource> make_tcp_imu_source(
    const ImuSourceConfig& cfg, std::shared_ptr<TcpCaptureSession> session) {
  return std::make_unique<TcpImuSource>(cfg, std::move(session));
}

}  // namespace xr_capture_cpp
