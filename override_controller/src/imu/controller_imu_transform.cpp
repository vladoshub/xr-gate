#include <xr_override_controller/imu/controller_imu_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace xr_override_controller::imu {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

struct Mat3 {
  double v[3][3]{};
};

Mat3 identity() {
  Mat3 out{};
  out.v[0][0] = 1.0;
  out.v[1][1] = 1.0;
  out.v[2][2] = 1.0;
  return out;
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
  Mat3 out{};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      for (int k = 0; k < 3; ++k) out.v[row][col] += a.v[row][k] * b.v[k][col];
    }
  }
  return out;
}

Mat3 transpose(const Mat3& in) {
  Mat3 out{};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) out.v[row][col] = in.v[col][row];
  }
  return out;
}

Mat3 rotation_x(double radians) {
  Mat3 out = identity();
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  out.v[1][1] = c;
  out.v[1][2] = -s;
  out.v[2][1] = s;
  out.v[2][2] = c;
  return out;
}

Mat3 rotation_y(double radians) {
  Mat3 out = identity();
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  out.v[0][0] = c;
  out.v[0][2] = s;
  out.v[2][0] = -s;
  out.v[2][2] = c;
  return out;
}

Mat3 rotation_z(double radians) {
  Mat3 out = identity();
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  out.v[0][0] = c;
  out.v[0][1] = -s;
  out.v[1][0] = s;
  out.v[1][1] = c;
  return out;
}

Mat3 make_basis_transform(const OrientationTransformConfig& config) {
  const double scale = kPi / 180.0;
  Mat3 inversion = identity();
  inversion.v[0][0] = config.invert_x ? -1.0 : 1.0;
  inversion.v[1][1] = config.invert_y ? -1.0 : 1.0;
  inversion.v[2][2] = config.invert_z ? -1.0 : 1.0;

  // Same Euler convention used by the runtime configs: intrinsic XYZ values
  // composed as Rz * Ry * Rx, then applied after optional source-axis flips.
  const Mat3 rotation = multiply(
      rotation_z(config.basis_rotation.rz_deg * scale),
      multiply(rotation_y(config.basis_rotation.ry_deg * scale),
               rotation_x(config.basis_rotation.rx_deg * scale)));
  return multiply(rotation, inversion);
}

void transform_vector(const Mat3& basis, float value[3]) {
  const double x = value[0];
  const double y = value[1];
  const double z = value[2];
  value[0] = static_cast<float>(basis.v[0][0] * x + basis.v[0][1] * y + basis.v[0][2] * z);
  value[1] = static_cast<float>(basis.v[1][0] * x + basis.v[1][1] * y + basis.v[1][2] * z);
  value[2] = static_cast<float>(basis.v[2][0] * x + basis.v[2][1] * y + basis.v[2][2] * z);
}

Mat3 quaternion_to_matrix(double x, double y, double z, double w) {
  const double norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(norm > 1.0e-12) || !std::isfinite(norm)) return identity();
  x /= norm;
  y /= norm;
  z /= norm;
  w /= norm;

  Mat3 out{};
  out.v[0][0] = 1.0 - 2.0 * (y * y + z * z);
  out.v[0][1] = 2.0 * (x * y - z * w);
  out.v[0][2] = 2.0 * (x * z + y * w);
  out.v[1][0] = 2.0 * (x * y + z * w);
  out.v[1][1] = 1.0 - 2.0 * (x * x + z * z);
  out.v[1][2] = 2.0 * (y * z - x * w);
  out.v[2][0] = 2.0 * (x * z - y * w);
  out.v[2][1] = 2.0 * (y * z + x * w);
  out.v[2][2] = 1.0 - 2.0 * (x * x + y * y);
  return out;
}

void matrix_to_quaternion(const Mat3& m, float out_xyzw[4]) {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
  const double trace = m.v[0][0] + m.v[1][1] + m.v[2][2];
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    w = 0.25 * s;
    x = (m.v[2][1] - m.v[1][2]) / s;
    y = (m.v[0][2] - m.v[2][0]) / s;
    z = (m.v[1][0] - m.v[0][1]) / s;
  } else if (m.v[0][0] > m.v[1][1] && m.v[0][0] > m.v[2][2]) {
    const double s = std::sqrt(std::max(0.0, 1.0 + m.v[0][0] - m.v[1][1] - m.v[2][2])) * 2.0;
    w = (m.v[2][1] - m.v[1][2]) / s;
    x = 0.25 * s;
    y = (m.v[0][1] + m.v[1][0]) / s;
    z = (m.v[0][2] + m.v[2][0]) / s;
  } else if (m.v[1][1] > m.v[2][2]) {
    const double s = std::sqrt(std::max(0.0, 1.0 + m.v[1][1] - m.v[0][0] - m.v[2][2])) * 2.0;
    w = (m.v[0][2] - m.v[2][0]) / s;
    x = (m.v[0][1] + m.v[1][0]) / s;
    y = 0.25 * s;
    z = (m.v[1][2] + m.v[2][1]) / s;
  } else {
    const double s = std::sqrt(std::max(0.0, 1.0 + m.v[2][2] - m.v[0][0] - m.v[1][1])) * 2.0;
    w = (m.v[1][0] - m.v[0][1]) / s;
    x = (m.v[0][2] + m.v[2][0]) / s;
    y = (m.v[1][2] + m.v[2][1]) / s;
    z = 0.25 * s;
  }

  const double norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(norm > 1.0e-12) || !std::isfinite(norm)) {
    out_xyzw[0] = 0.0f;
    out_xyzw[1] = 0.0f;
    out_xyzw[2] = 0.0f;
    out_xyzw[3] = 1.0f;
    return;
  }
  out_xyzw[0] = static_cast<float>(x / norm);
  out_xyzw[1] = static_cast<float>(y / norm);
  out_xyzw[2] = static_cast<float>(z / norm);
  out_xyzw[3] = static_cast<float>(w / norm);
}

}  // namespace

xr_runtime::ControllerImuStateV1 apply_orientation_transform(
    xr_runtime::ControllerImuStateV1 state,
    const OrientationTransformConfig& config) {
  if (!config.enabled) return state;

  const Mat3 basis = make_basis_transform(config);
  const size_t sample_capacity = sizeof(state.samples) / sizeof(state.samples[0]);
  const size_t sample_count = std::min<size_t>(state.sample_count, sample_capacity);
  for (size_t i = 0; i < sample_count; ++i) {
    transform_vector(basis, state.samples[i].angular_velocity_rad_s);
    transform_vector(basis, state.samples[i].specific_force_m_s2);
  }
  transform_vector(basis, state.magnetic_field_uT);

  if ((state.data_flags & xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID) != 0u) {
    const Mat3 orientation = quaternion_to_matrix(
        state.orientation_xyzw[0], state.orientation_xyzw[1],
        state.orientation_xyzw[2], state.orientation_xyzw[3]);
    // A basis conversion applies to both ends of the orientation mapping. This
    // remains valid for reflections (an odd number of invert_* flags), because
    // basis * R * basis^T is still a proper rotation.
    const Mat3 transformed = multiply(multiply(basis, orientation), transpose(basis));
    matrix_to_quaternion(transformed, state.orientation_xyzw);
  }
  return state;
}

}  // namespace xr_override_controller::imu
