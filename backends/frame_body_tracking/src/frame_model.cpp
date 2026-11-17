#include "frame_model.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace frame_body {
namespace {

constexpr double kVirtualFocal = 82.0;
constexpr double kVirtualCenter = (kInputSize - 1) * 0.5;
constexpr size_t kPlanePixels = kInputSize * kInputSize;
constexpr std::array<float, 3> kMean{0.485f, 0.456f, 0.406f};
constexpr std::array<float, 3> kStd{0.229f, 0.224f, 0.225f};
constexpr std::array<int64_t, 6> kImageShape{1, 1, 2, 3, kInputSize, kInputSize};
constexpr std::array<int64_t, 4> kIntrinsicShape{1, 2, 3, 3};
constexpr std::array<int64_t, 3> kDistortionShape{1, 2, 4};
constexpr std::array<int64_t, 5> kUvShape{1, 1, 2, kJointCount, 2};
constexpr std::array<int64_t, 5> kXyzShape{1, 1, 2, kJointCount, 3};

struct JointRange {
  double min_m;
  double max_m;
};

// Broad anatomical ranges from either headset camera. The minimum distance
// rejects collapsed arm predictions when arms are outside the view.
constexpr std::array<JointRange, kJointCount> kJointRanges{{
    {0.06, 0.75},
    {0.16, 1.05}, {0.28, 1.40}, {0.18, 1.55},
    {0.16, 1.05}, {0.28, 1.40}, {0.18, 1.55},
    {0.25, 1.80}, {0.30, 2.20}, {0.35, 2.60}, {0.35, 2.70},
    {0.25, 1.80}, {0.30, 2.20}, {0.35, 2.60}, {0.35, 2.70},
}};

template <size_t N>
void validate_tensor(const Ort::TypeInfo& type,
                     const std::array<int64_t, N>& shape,
                     const char* name) {
  const auto info = type.GetTensorTypeAndShapeInfo();
  const auto model_shape = info.GetShape();
  if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
      !std::ranges::equal(model_shape, shape)) {
    throw std::runtime_error(std::string("unexpected FRAME ONNX tensor shape or type for ") + name);
  }
}
}  // namespace

Eigen::Vector2d Camera::project(const Eigen::Vector3d& ray) const {
  const double norm_xy = ray.head<2>().norm();
  if (norm_xy < 1e-12) return principal_point;
  const double theta = std::atan2(norm_xy, ray.z());
  const double theta2 = theta * theta;
  const auto& k = distortion;
  const double distorted = theta * (1 + theta2 * (k[0] + theta2 * (k[1] + theta2 * (k[2] + theta2 * k[3]))));
  return principal_point + focal.cwiseProduct(ray.head<2>()) * (distorted / norm_xy);
}

bool Camera::contains(const Eigen::Vector2d& pixel) const {
  return pixel.x() >= 0 && pixel.y() >= 0 && pixel.x() <= width - 1 && pixel.y() <= height - 1;
}

Calibration read_calibration(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open Basalt calibration: " + path);
  const auto root = nlohmann::json::parse(input);
  const auto& value = root.at("value0");
  const auto& intrinsics = value.at("intrinsics");
  const auto& extrinsics = value.at("T_imu_cam");
  const auto& resolutions = value.at("resolution");
  if (intrinsics.size() != 2 || extrinsics.size() != 2 || resolutions.size() != 2) {
    throw std::runtime_error("FRAME requires exactly two calibrated cameras");
  }

  Calibration calibration;
  calibration.camera_time_offset_ns = value.value("cam_time_offset_ns", int64_t{0});
  for (size_t view = 0; view < calibration.cameras.size(); ++view) {
    auto& camera = calibration.cameras[view];
    if (intrinsics.at(view).at("camera_type") != "kb4") {
      throw std::runtime_error("FRAME requires Basalt kb4 camera intrinsics");
    }
    const auto& k = intrinsics.at(view).at("intrinsics");
    const auto& pose = extrinsics.at(view);
    camera.width = resolutions.at(view).at(0).get<int>();
    camera.height = resolutions.at(view).at(1).get<int>();
    camera.focal = {k.at("fx").get<double>(), k.at("fy").get<double>()};
    camera.principal_point = {k.at("cx").get<double>(), k.at("cy").get<double>()};
    camera.distortion = {k.at("k1").get<double>(), k.at("k2").get<double>(),
                         k.at("k3").get<double>(), k.at("k4").get<double>()};
    const Eigen::Vector3d position(pose.at("px").get<double>(), pose.at("py").get<double>(),
                                  pose.at("pz").get<double>());
    const Eigen::Quaterniond orientation(pose.at("qw").get<double>(), pose.at("qx").get<double>(),
                                         pose.at("qy").get<double>(), pose.at("qz").get<double>());
    if (camera.width < 1 || camera.height < 1 || !camera.focal.allFinite() ||
        camera.focal.minCoeff() <= 0 || !camera.principal_point.allFinite() ||
        !std::ranges::all_of(camera.distortion, [](double d) { return std::isfinite(d); }) ||
        !position.allFinite() || !orientation.coeffs().allFinite() || orientation.norm() < 1e-6) {
      throw std::runtime_error("invalid camera intrinsics or extrinsics");
    }
    camera.imu_from_camera.linear() = orientation.normalized().toRotationMatrix();
    camera.imu_from_camera.translation() = position;
  }
  const double baseline_m = (calibration.cameras[0].imu_from_camera.translation() -
                             calibration.cameras[1].imu_from_camera.translation()).norm();
  if (baseline_m < 1e-3) throw std::runtime_error("stereo camera baseline is missing");
  return calibration;
}

FrameModel::FrameModel(const std::string& model_path, const Calibration& calibration,
                       double downward_pitch_deg, int threads, double max_stereo_disagreement_m)
    : calibration_(calibration),
      max_stereo_disagreement_m_(max_stereo_disagreement_m),
      images_(2 * 3 * kPlanePixels) {
  const double pitch = downward_pitch_deg * std::numbers::pi / 180.0;
  camera_from_virtual_ = (Eigen::AngleAxisd(-pitch, Eigen::Vector3d::UnitX()) *
                         Eigen::AngleAxisd(std::numbers::pi, Eigen::Vector3d::UnitZ())).toRotationMatrix();

  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(threads);
  options.SetInterOpNumThreads(1);
  options.AddConfigEntry("session.intra_op.allow_spinning", "0");
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  session_ = Ort::Session(env_, model_path.c_str(), options);

  if (session_.GetInputCount() != 3 || session_.GetOutputCount() != 2) {
    throw std::runtime_error("expected FRAME backbone model with inputs images, K, d and outputs uv, xyz");
  }
  Ort::AllocatorWithDefaultOptions allocator;
  for (size_t i = 0; i < session_.GetInputCount(); ++i) {
    const std::string name = session_.GetInputNameAllocated(i, allocator).get();
    if (name == "images") validate_tensor(session_.GetInputTypeInfo(i), kImageShape, "images");
    else if (name == "K") validate_tensor(session_.GetInputTypeInfo(i), kIntrinsicShape, "K");
    else if (name == "d") validate_tensor(session_.GetInputTypeInfo(i), kDistortionShape, "d");
    else throw std::runtime_error("unexpected FRAME ONNX input: " + name);
  }
  for (size_t i = 0; i < session_.GetOutputCount(); ++i) {
    const std::string name = session_.GetOutputNameAllocated(i, allocator).get();
    if (name == "uv") validate_tensor(session_.GetOutputTypeInfo(i), kUvShape, "uv");
    else if (name == "xyz") validate_tensor(session_.GetOutputTypeInfo(i), kXyzShape, "xyz");
    else throw std::runtime_error("unexpected FRAME ONNX output: " + name);
  }
  for (size_t view = 0; view < remap_.size(); ++view) build_remap(view);
}

Eigen::Vector3d FrameModel::camera_ray(const Eigen::Vector2d& virtual_pixel) const {
  const Eigen::Vector2d xy = (virtual_pixel.array() - kVirtualCenter).matrix() / kVirtualFocal;
  const double theta = xy.norm();
  const double scale = theta > 1e-12 ? std::sin(theta) / theta : 1.0;
  return camera_from_virtual_ * Eigen::Vector3d(xy.x() * scale, xy.y() * scale, std::cos(theta));
}

void FrameModel::build_remap(size_t view) {
  const auto& camera = calibration_.cameras[view];
  auto& map = remap_[view];
  map.resize(kPlanePixels);
  for (size_t y = 0; y < kInputSize; ++y) {
    for (size_t x = 0; x < kInputSize; ++x) {
      const Eigen::Vector3d ray = camera_ray(Eigen::Vector2d(x, y));
      if (ray.z() <= 1e-6) continue;
      const Eigen::Vector2d uv = camera.project(ray);
      // OpenCV INTER_LINEAR in the replay uses a 1/32-pixel coefficient table.
      const double px = std::round(uv.x() * 32.0) / 32.0;
      const double py = std::round(uv.y() * 32.0) / 32.0;
      if (px <= -1 || py <= -1 || px >= camera.width || py >= camera.height) continue;
      const int x0 = static_cast<int>(std::floor(px));
      const int y0 = static_cast<int>(std::floor(py));
      const double dx = px - x0;
      const double dy = py - y0;
      auto& entry = map[y * kInputSize + x];
      for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
          const int ix = x0 + sx;
          const int iy = y0 + sy;
          if (ix < 0 || iy < 0 || ix >= camera.width || iy >= camera.height) continue;
          const size_t i = sy * 2 + sx;
          entry.offsets[i] = static_cast<size_t>(iy) * camera.width + ix;
          entry.weights[i] = static_cast<float>((sx ? dx : 1 - dx) * (sy ? dy : 1 - dy));
        }
      }
    }
  }
}

void FrameModel::preprocess(size_t view, const capture_client::ImageFrame& image) {
  const auto& camera = calibration_.cameras[view];
  if (image.width != static_cast<uint32_t>(camera.width) ||
      image.height != static_cast<uint32_t>(camera.height) ||
      image.gray8.size() != static_cast<size_t>(camera.width) * camera.height) {
    throw std::runtime_error("captured GRAY8 dimensions do not match FRAME calibration for camera" +
                             std::to_string(view));
  }
  const auto& source = image.gray8;
  const auto& map = remap_[view];
  for (size_t pixel = 0; pixel < kPlanePixels; ++pixel) {
    const auto& entry = map[pixel];
    float gray = 0.0f;
    for (size_t tap = 0; tap < entry.weights.size(); ++tap) {
      gray += source[entry.offsets[tap]] * entry.weights[tap];
    }
    gray = std::round(gray) / 255.0f;
    for (size_t channel = 0; channel < kMean.size(); ++channel) {
      images_[(view * 3 + channel) * kPlanePixels + pixel] = (gray - kMean[channel]) / kStd[channel];
    }
  }
}

Observations FrameModel::infer(const capture_client::ImageFrame& left,
                              const capture_client::ImageFrame& right) {
  preprocess(0, left);
  preprocess(1, right);

  const std::array<const char*, 3> input_names{"images", "K", "d"};
  const std::array<const char*, 2> output_names{"uv", "xyz"};
  constexpr float f = kVirtualFocal / kInputSize;
  constexpr float c = kVirtualCenter / kInputSize;
  std::array<float, 18> intrinsics{f, 0, c, 0, f, c, 0, 0, 1, f, 0, c, 0, f, c, 0, 0, 1};
  std::array<float, 8> distortion{};
  std::array<Ort::Value, 3> inputs{
      Ort::Value::CreateTensor<float>(memory_info_, images_.data(), images_.size(), kImageShape.data(), kImageShape.size()),
      Ort::Value::CreateTensor<float>(memory_info_, intrinsics.data(), intrinsics.size(),
                                      kIntrinsicShape.data(), kIntrinsicShape.size()),
      Ort::Value::CreateTensor<float>(memory_info_, distortion.data(), distortion.size(),
                                      kDistortionShape.data(), kDistortionShape.size()),
  };
  const auto outputs = session_.Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(),
                                    inputs.size(), output_names.data(), output_names.size());
  const auto* uv_data = outputs[0].GetTensorData<float>();
  const auto* xyz_data = outputs[1].GetTensorData<float>();

  Observations joints;
  for (size_t joint = 0; joint < kJointCount; ++joint) {
    std::array<Eigen::Vector3d, 2> positions;
    size_t valid_views = 0;
    for (size_t view = 0; view < positions.size(); ++view) {
      const auto& camera = calibration_.cameras[view];
      const size_t index = view * kJointCount + joint;
      const Eigen::Vector2d uv(uv_data[2 * index], uv_data[2 * index + 1]);
      const Eigen::Vector3d xyz(xyz_data[3 * index], xyz_data[3 * index + 1], xyz_data[3 * index + 2]);
      if (!uv.allFinite() || uv.minCoeff() < 0 || uv.maxCoeff() > 1 || !xyz.allFinite() || xyz.z() <= 0) break;
      const Eigen::Vector3d ray = camera_ray(uv * (kInputSize - 1));
      if (ray.z() <= 1e-6 || !camera.contains(camera.project(ray))) break;
      const double distance_m = xyz.norm();
      if (distance_m < kJointRanges[joint].min_m || distance_m > kJointRanges[joint].max_m) break;
      positions[view] = camera.imu_from_camera * (camera_from_virtual_ * xyz);
      ++valid_views;
    }
    if (valid_views != positions.size()) continue;
    const double disagreement_m = (positions[0] - positions[1]).norm();
    if (disagreement_m >= max_stereo_disagreement_m_) continue;
    const float confidence = static_cast<float>(1.0 - disagreement_m / max_stereo_disagreement_m_);
    joints[joint] = JointObservation{(positions[0] + positions[1]) * 0.5, confidence};
  }
  return joints;
}

}  // namespace frame_body
