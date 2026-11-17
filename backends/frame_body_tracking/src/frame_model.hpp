#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Geometry>
#include <onnxruntime_cxx_api.h>

#include <capture_client/contracts/messages.hpp>

namespace frame_body {

constexpr size_t kJointCount = 15;
constexpr size_t kInputSize = 256;

struct Camera {
  int width = 0;
  int height = 0;
  Eigen::Vector2d focal;
  Eigen::Vector2d principal_point;
  std::array<double, 4> distortion;
  Eigen::Isometry3d imu_from_camera = Eigen::Isometry3d::Identity();

  Eigen::Vector2d project(const Eigen::Vector3d& ray) const;
  bool contains(const Eigen::Vector2d& pixel) const;
};

struct Calibration {
  std::array<Camera, 2> cameras;
  int64_t camera_time_offset_ns = 0;
};

Calibration read_calibration(const std::string& path);

struct JointObservation {
  Eigen::Vector3d position_imu;
  float confidence = 0.0f;
};

using Observations = std::array<std::optional<JointObservation>, kJointCount>;

class FrameModel {
 public:
  FrameModel(const std::string& model_path, const Calibration& calibration,
             double downward_pitch_deg, int threads, double max_stereo_disagreement_m);

  Observations infer(const capture_client::ImageFrame& left,
                     const capture_client::ImageFrame& right);

 private:
  // Taps outside the original image have zero weight.
  struct RemapPixel {
    std::array<size_t, 4> offsets{};
    std::array<float, 4> weights{};
  };

  void build_remap(size_t view);
  void preprocess(size_t view, const capture_client::ImageFrame& image);
  Eigen::Vector3d camera_ray(const Eigen::Vector2d& virtual_pixel) const;

  Calibration calibration_;
  Eigen::Matrix3d camera_from_virtual_;
  double max_stereo_disagreement_m_;
  std::array<std::vector<RemapPixel>, 2> remap_;
  std::vector<float> images_;
  Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "frame_body_tracking"};
  Ort::Session session_{nullptr};
  Ort::MemoryInfo memory_info_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
};

}  // namespace frame_body
