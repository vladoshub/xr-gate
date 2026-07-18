#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/imu_transform.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xr_capture_cpp {
std::atomic<bool> g_stop{false};
std::atomic<int> g_exit_code{kExitOk};
}

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_near(float actual, float expected, const std::string& message) {
  if (std::fabs(actual - expected) > 1e-5F) {
    throw std::runtime_error(message + ": actual=" + std::to_string(actual) +
                             " expected=" + std::to_string(expected));
  }
}

xr_capture_cpp::RuntimeConfig parse_config_text(const std::string& name,
                                                const std::string& transform_yaml) {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / name;
  {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("failed to create temporary config");
    out << "service:\n"
           "  publish: [shm]\n"
           "camera:\n"
           "  enabled: false\n"
           "imu:\n"
           "  enabled: true\n"
           "  driver: serial\n"
        << transform_yaml <<
           "  raw:\n"
           "    enabled: false\n"
           "  serial:\n"
           "    port: /dev/ttyACM0\n"
           "    protocol: xr_controller_v1\n";
  }

  std::vector<std::string> storage{"imu_transform_smoke", "--config", path.string()};
  std::vector<char*> args;
  for (auto& item : storage) args.push_back(item.data());
  try {
    xr_capture_cpp::RuntimeConfig cfg =
        xr_capture_cpp::parse_args(static_cast<int>(args.size()), args.data());
    std::error_code ignored;
    fs::remove(path, ignored);
    return cfg;
  } catch (...) {
    std::error_code ignored;
    fs::remove(path, ignored);
    throw;
  }
}

bool config_fails(const std::string& name, const std::string& transform_yaml) {
  try {
    (void)parse_config_text(name, transform_yaml);
    return false;
  } catch (const std::runtime_error&) {
    return true;
  }
}

}  // namespace

int main() {
  using namespace xr_capture_cpp;

  const RuntimeConfig identity = parse_config_text("imu_transform_identity.yaml", "");
  require(identity.imu.transform.mode == ImuTransformMode::Identity,
          "omitted transform must remain identity");
  ImuSample identity_sample;
  identity_sample.gyro_rad_s = {{1.0F, 2.0F, 3.0F}};
  identity_sample.accel_m_s2 = {{4.0F, 5.0F, 6.0F}};
  apply_imu_transform(identity.imu.transform, identity_sample);
  require(identity_sample.gyro_rad_s == std::array<float, 3>{{1.0F, 2.0F, 3.0F}},
          "identity gyro changed");
  require(identity_sample.accel_m_s2 == std::array<float, 3>{{4.0F, 5.0F, 6.0F}},
          "identity accel changed");

  const RuntimeConfig axes = parse_config_text(
      "imu_transform_axes.yaml", "  transform:\n    axes: [x, -z, y]\n");
  require(axes.imu.transform.mode == ImuTransformMode::Axes, "axes mode not selected");
  ImuSample axes_sample;
  axes_sample.gyro_rad_s = {{1.0F, 2.0F, 3.0F}};
  axes_sample.accel_m_s2 = {{4.0F, 5.0F, 6.0F}};
  apply_imu_transform(axes.imu.transform, axes_sample);
  require(axes_sample.gyro_rad_s == std::array<float, 3>{{1.0F, -3.0F, 2.0F}},
          "axes gyro mapping");
  require(axes_sample.accel_m_s2 == std::array<float, 3>{{4.0F, -6.0F, 5.0F}},
          "axes accel mapping");

  constexpr double kSqrtHalf = 0.7071067811865475244;
  const RuntimeConfig quaternion = parse_config_text(
      "imu_transform_quaternion.yaml",
      "  transform:\n    quaternion_xyzw: [0, 0, 0.7071067811865476, 0.7071067811865476]\n");
  require(quaternion.imu.transform.mode == ImuTransformMode::Quaternion,
          "quaternion mode not selected");
  require(std::fabs(quaternion.imu.transform.quaternion_xyzw[2] - kSqrtHalf) < 1e-12,
          "quaternion was not normalized");
  ImuSample q_sample;
  q_sample.gyro_rad_s = {{1.0F, 0.0F, 0.0F}};
  q_sample.accel_m_s2 = {{0.0F, 1.0F, 0.0F}};
  apply_imu_transform(quaternion.imu.transform, q_sample);
  require_near(q_sample.gyro_rad_s[0], 0.0F, "quaternion gyro x");
  require_near(q_sample.gyro_rad_s[1], 1.0F, "quaternion gyro y");
  require_near(q_sample.accel_m_s2[0], -1.0F, "quaternion accel x");
  require_near(q_sample.accel_m_s2[1], 0.0F, "quaternion accel y");

  require(config_fails(
              "imu_transform_both.yaml",
              "  transform:\n"
              "    axes: [x, -z, y]\n"
              "    quaternion_xyzw: [0, 0, 0, 1]\n"),
          "axes and quaternion must be mutually exclusive");
  require(config_fails("imu_transform_reflection.yaml",
                       "  transform:\n    axes: [x, y, -z]\n"),
          "reflection axes mapping must be rejected");
  require(config_fails("imu_transform_duplicate.yaml",
                       "  transform:\n    axes: [x, x, z]\n"),
          "duplicate source axes must be rejected");
  require(config_fails("imu_transform_zero_quaternion.yaml",
                       "  transform:\n    quaternion_xyzw: [0, 0, 0, 0]\n"),
          "zero quaternion must be rejected");

  return 0;
}
