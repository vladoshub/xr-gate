#include "capture_service_cpp/sources/camera_source.hpp"

#include "capture_service_cpp/image_transform.hpp"
#include "capture_service_cpp/interleaved_columns_decoder.hpp"
#include "capture_service_cpp/platform/camera_capture.hpp"

#include <iostream>
#include <sstream>
#include <memory>
#include <stdexcept>

namespace xr_capture_cpp {
namespace {

void decode_interleaved_columns_frame(const cv::Mat& frame,
                                      int eye_width,
                                      int eye_height,
                                      cv::Mat& first_eye,
                                      cv::Mat& second_eye) {
  if (frame.empty()) throw std::runtime_error("interleaved-columns frame is empty");
  if (frame.depth() != CV_8U) {
    throw std::runtime_error("interleaved-columns frame must have 8-bit elements");
  }

  const size_t eye_row_bytes = static_cast<size_t>(eye_width);
  const size_t source_row_bytes = eye_row_bytes * 2U;
  const size_t expected_bytes = source_row_bytes * static_cast<size_t>(eye_height);
  const size_t actual_bytes = frame.total() * frame.elemSize();

  const uint8_t* source = nullptr;
  size_t source_stride = 0;
  size_t source_size = 0;

  // OpenCV backends normally expose this UVC layout as either HxW CV_8UC2
  // or Hx(2W) CV_8UC1. Some raw backends return one continuous flat buffer.
  if (frame.rows == eye_height && frame.step[0] >= source_row_bytes && actual_bytes == expected_bytes) {
    source = frame.ptr<uint8_t>(0);
    source_stride = frame.step[0];
    source_size = source_stride * static_cast<size_t>(eye_height - 1) + source_row_bytes;
  } else if (frame.isContinuous() && actual_bytes == expected_bytes) {
    source = frame.ptr<uint8_t>(0);
    source_stride = source_row_bytes;
    source_size = expected_bytes;
  } else {
    std::ostringstream message;
    message << "interleaved-columns frame byte layout mismatch: got rows=" << frame.rows
            << " cols=" << frame.cols << " channels=" << frame.channels()
            << " elem_size=" << frame.elemSize() << " bytes=" << actual_bytes
            << ", expected " << eye_height << " rows and " << source_row_bytes
            << " bytes per row (" << expected_bytes << " bytes total)";
    throw std::runtime_error(message.str());
  }

  first_eye.create(eye_height, eye_width, CV_8UC1);
  second_eye.create(eye_height, eye_width, CV_8UC1);
  deinterleave_gray8_columns(source, source_size, source_stride, eye_width, eye_height,
                             first_eye.ptr<uint8_t>(0), first_eye.step[0],
                             second_eye.ptr<uint8_t>(0), second_eye.step[0]);
}

class OpenCvStereoCameraSource final : public ICameraSource {
 public:
  explicit OpenCvStereoCameraSource(RuntimeConfig cfg) : cfg_(std::move(cfg)) {}

  std::string name() const override { return "opencv:" + cfg_.camera.layout; }

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
    if (!primary_.open(cfg_.camera.primary, "primary stereo camera")) {
      throw std::runtime_error("failed to open primary stereo camera");
    }
    if (cfg_.camera.layout == "dual" && !secondary_.open(cfg_.camera.secondary, "secondary stereo camera")) {
      primary_.release();
      throw std::runtime_error("failed to open secondary stereo camera");
    }
  }

  SourceReadStatus read(StereoFrame& output) override {
    output = {};
    cv::Mat first;
    if (!primary_.read(first) || first.empty()) return SourceReadStatus::Timeout;

    cv::Mat left_raw;
    cv::Mat right_raw;
    if (cfg_.camera.layout == "dual") {
      cv::Mat second;
      if (!secondary_.read(second) || second.empty()) return SourceReadStatus::Timeout;
      if (cfg_.camera.stereo_order == "right_left") {
        right_raw = to_gray8(first);
        left_raw = to_gray8(second);
      } else {
        left_raw = to_gray8(first);
        right_raw = to_gray8(second);
      }
    } else if (cfg_.camera.layout == "interleaved_columns") {
      cv::Mat a;
      cv::Mat b;
      decode_interleaved_columns_frame(first, cfg_.camera.output_width, cfg_.camera.output_height, a, b);
      if (cfg_.camera.stereo_order == "right_left") {
        right_raw = a;
        left_raw = b;
      } else {
        left_raw = a;
        right_raw = b;
      }
    } else {
      const cv::Mat gray = to_gray8(first);
      if (cfg_.camera.layout == "side_by_side_horizontal") {
        if (gray.cols < 2 || gray.cols % 2 != 0) {
          throw std::runtime_error("horizontal side-by-side frame width must be even");
        }
        const int half = gray.cols / 2;
        cv::Mat a = gray(cv::Rect(0, 0, half, gray.rows));
        cv::Mat b = gray(cv::Rect(half, 0, half, gray.rows));
        if (cfg_.camera.stereo_order == "right_left") {
          right_raw = a;
          left_raw = b;
        } else {
          left_raw = a;
          right_raw = b;
        }
      } else {
        if (gray.rows < 2 || gray.rows % 2 != 0) {
          throw std::runtime_error("vertical side-by-side frame height must be even");
        }
        const int half = gray.rows / 2;
        cv::Mat a = gray(cv::Rect(0, 0, gray.cols, half));
        cv::Mat b = gray(cv::Rect(0, half, gray.cols, half));
        if (cfg_.camera.stereo_order == "right_left") {
          right_raw = a;
          left_raw = b;
        } else {
          left_raw = a;
          right_raw = b;
        }
      }
    }

    output.left = transform_gray_image(left_raw, cfg_.camera.left_transform.rotate, cfg_.camera.left_transform.flip);
    output.right = transform_gray_image(right_raw, cfg_.camera.right_transform.rotate, cfg_.camera.right_transform.flip);
    validate_dimensions(output.left, "left");
    validate_dimensions(output.right, "right");
    output.timestamp_ns = steady_ns();
    ++pairs_;

    const uint64_t now = steady_ns();
    if (now - last_log_ns_ > 2000000000ULL) {
      std::cerr << "[capture_service_cpp] camera source=opencv layout=" << cfg_.camera.layout
                << " pairs=" << pairs_ << " output=" << output.left.cols << "x" << output.left.rows
                << std::endl;
      last_log_ns_ = now;
    }
    return SourceReadStatus::Data;
  }

  void close() override {
    primary_.release();
    secondary_.release();
  }

 private:
  void validate_dimensions(const cv::Mat& image, const char* side) const {
    if (image.cols != cfg_.camera.output_width || image.rows != cfg_.camera.output_height) {
      throw std::runtime_error(std::string("OpenCV ") + side + " output dimensions are " +
                               std::to_string(image.cols) + "x" + std::to_string(image.rows) +
                               ", configured " + std::to_string(cfg_.camera.output_width) + "x" +
                               std::to_string(cfg_.camera.output_height));
    }
  }

  RuntimeConfig cfg_;
  CameraCapture primary_;
  CameraCapture secondary_;
  uint64_t pairs_ = 0;
  uint64_t last_log_ns_ = steady_ns();
};

}  // namespace

std::unique_ptr<ICameraSource> make_opencv_stereo_camera_source(const RuntimeConfig& cfg) {
  return std::make_unique<OpenCvStereoCameraSource>(cfg);
}

}  // namespace xr_capture_cpp
