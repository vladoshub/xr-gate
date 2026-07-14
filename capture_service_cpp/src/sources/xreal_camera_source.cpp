#include "capture_service_cpp/sources/camera_source.hpp"

#include "capture_service_cpp/platform/camera_capture.hpp"
#include "capture_service_cpp/vendor/xreal_camera_decoder.hpp"
#include "capture_service_cpp/vendor/xreal_camera_transform.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace xr_capture_cpp {
namespace {

class XrealCameraSource final : public ICameraSource {
 public:
  explicit XrealCameraSource(RuntimeConfig cfg) : cfg_(std::move(cfg)) {}

  std::string name() const override { return "xreal_ultra"; }

  std::vector<StreamSpec> stream_specs(uint32_t slot_count) const override {
    const auto& c = cfg_.camera;
    return {
        StreamSpec{c.left_stream_id, kKindImage, "IMAGE", static_cast<uint32_t>(c.output_width),
                   static_cast<uint32_t>(c.output_height), kFormatGray8, "GRAY8",
                   static_cast<uint32_t>(c.output_width * c.output_height), slot_count,
                   c.left_frame_id, "Native C++ normalized stereo left camera frame"},
        StreamSpec{c.right_stream_id, kKindImage, "IMAGE", static_cast<uint32_t>(c.output_width),
                   static_cast<uint32_t>(c.output_height), kFormatGray8, "GRAY8",
                   static_cast<uint32_t>(c.output_width * c.output_height), slot_count,
                   c.right_frame_id, "Native C++ normalized stereo right camera frame"}};
  }

  void open() override {
    init_xreal_camera_tables();
    if (!capture_.open(cfg_.camera.primary, "XREAL camera")) {
      throw std::runtime_error("failed to open XREAL camera");
    }
    raw_eye_ = cv::Mat(cv::Size(kXrealEyeWidth, kXrealEyeHeight), CV_8UC1);
    transforms_ = resolve_xreal_eye_transforms(cfg_);
  }

  SourceReadStatus read(StereoFrame& output) override {
    output = {};
    cv::Mat frame;
    if (!capture_.read(frame) || frame.empty()) return SourceReadStatus::Timeout;

    bool is_right = false;
    if (!decode_xreal_eye(frame, raw_eye_, is_right)) {
      ++decode_fail_;
      return SourceReadStatus::Data;
    }
    ++decoded_;
    if (is_right) {
      latest_right_ = transform_xreal_gray_eye(raw_eye_, transforms_.right_rotate, transforms_.right_flip);
      have_right_ = true;
    } else {
      latest_left_ = transform_xreal_gray_eye(raw_eye_, transforms_.left_rotate, transforms_.left_flip);
      have_left_ = true;
    }
    if (!have_left_ || !have_right_) return SourceReadStatus::Data;

    validate_dimensions(latest_left_, "left");
    validate_dimensions(latest_right_, "right");
    output.left = latest_left_;
    output.right = latest_right_;
    output.timestamp_ns = steady_ns();
    have_left_ = false;
    have_right_ = false;
    ++published_pairs_;

    const uint64_t now = steady_ns();
    if (now - last_log_ns_ > 2000000000ULL) {
      std::cerr << "[capture_service_cpp] camera source=xreal_ultra decoded=" << decoded_
                << " decode_fail=" << decode_fail_ << " pairs=" << published_pairs_
                << " left_rotate=" << transforms_.left_rotate << " left_flip=" << transforms_.left_flip
                << " right_rotate=" << transforms_.right_rotate << " right_flip=" << transforms_.right_flip
                << std::endl;
      last_log_ns_ = now;
    }
    return SourceReadStatus::Data;
  }

  void close() override { capture_.release(); }

 private:
  void validate_dimensions(const cv::Mat& image, const char* side) const {
    if (image.cols != cfg_.camera.output_width || image.rows != cfg_.camera.output_height) {
      throw std::runtime_error(std::string("XREAL ") + side + " output dimensions are " +
                               std::to_string(image.cols) + "x" + std::to_string(image.rows) +
                               ", configured " + std::to_string(cfg_.camera.output_width) + "x" +
                               std::to_string(cfg_.camera.output_height));
    }
  }

  RuntimeConfig cfg_;
  CameraCapture capture_;
  cv::Mat raw_eye_;
  cv::Mat latest_left_;
  cv::Mat latest_right_;
  XrealEyeTransformConfig transforms_;
  bool have_left_ = false;
  bool have_right_ = false;
  uint64_t decoded_ = 0;
  uint64_t decode_fail_ = 0;
  uint64_t published_pairs_ = 0;
  uint64_t last_log_ns_ = steady_ns();
};

}  // namespace

std::unique_ptr<ICameraSource> make_xreal_camera_source(const RuntimeConfig& cfg) {
  return std::make_unique<XrealCameraSource>(cfg);
}

}  // namespace xr_capture_cpp
