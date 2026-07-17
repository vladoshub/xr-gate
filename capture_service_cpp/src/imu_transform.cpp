#include "capture_service_cpp/imu_transform.hpp"

#include <sstream>

namespace xr_capture_cpp {

std::array<float, 3> rotate_imu_vector(const ImuTransformConfig& transform,
                                       const std::array<float, 3>& value) {
  const auto& r = transform.rotation_matrix;
  std::array<float, 3> out{};
  for (size_t row = 0; row < 3; ++row) {
    const size_t base = row * 3;
    out[row] = static_cast<float>(r[base] * static_cast<double>(value[0]) +
                                  r[base + 1] * static_cast<double>(value[1]) +
                                  r[base + 2] * static_cast<double>(value[2]));
  }
  return out;
}

void apply_imu_transform(const ImuTransformConfig& transform, ImuSample& sample) {
  if (transform.mode == ImuTransformMode::Identity) return;
  sample.gyro_rad_s = rotate_imu_vector(transform, sample.gyro_rad_s);
  sample.accel_m_s2 = rotate_imu_vector(transform, sample.accel_m_s2);
}

std::string imu_transform_description(const ImuTransformConfig& transform) {
  if (transform.mode == ImuTransformMode::Identity) return "identity";
  if (transform.mode == ImuTransformMode::Axes) {
    return "axes=[" + transform.axes[0] + "," + transform.axes[1] + "," +
           transform.axes[2] + "]";
  }
  std::ostringstream os;
  os << "quaternion_xyzw=[" << transform.quaternion_xyzw[0] << ','
     << transform.quaternion_xyzw[1] << ',' << transform.quaternion_xyzw[2] << ','
     << transform.quaternion_xyzw[3] << ']';
  return os.str();
}

}  // namespace xr_capture_cpp
