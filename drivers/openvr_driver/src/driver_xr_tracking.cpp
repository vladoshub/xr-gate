#include <openvr_driver.h>

#include "runtime_pose_reader.hpp"
#include "runtime_hand_reader.hpp"
#include "runtime_controller_state_reader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <cmath>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define XR_DRIVER_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__) || defined(__APPLE__)
#define XR_DRIVER_EXPORT extern "C" __attribute__((visibility("default")))
#else
#error "Unsupported platform"
#endif

namespace xr::openvr_driver {
namespace {

constexpr const char* kDriverSettingsSection = "xr_tracking";
constexpr const char* kSerialNumberKey = "serialNumber";
constexpr const char* kModelNumberKey = "modelNumber";
constexpr const char* kRuntimePoseTransportKey = "runtimePoseTransport";
constexpr const char* kRuntimePoseRegistryPathKey = "runtimePoseRegistryPath";
constexpr const char* kRuntimePoseStreamKey = "runtimePoseStream";
constexpr const char* kRuntimePoseShmNameKey = "runtimePoseShmName";
constexpr const char* kRuntimePoseMaxAgeMsKey = "runtimePoseMaxAgeMs";
constexpr const char* kRuntimePoseUdpBindHostKey = "runtimePoseUdpBindHost";
constexpr const char* kRuntimePoseUdpPortKey = "runtimePoseUdpPort";
constexpr const char* kRuntimePoseOptionalKey = "runtimePoseOptional";
constexpr const char* kWindowXKey = "windowX";
constexpr const char* kWindowYKey = "windowY";
constexpr const char* kWindowWidthKey = "windowWidth";
constexpr const char* kWindowHeightKey = "windowHeight";
constexpr const char* kRenderWidthKey = "renderWidth";
constexpr const char* kRenderHeightKey = "renderHeight";
constexpr const char* kDisplayFrequencyKey = "displayFrequency";
constexpr float kDefaultDisplayFrequencyHz = 60.0f;  // direct acquire AMD/iGPU test
constexpr const char* kSecondsFromVsyncToPhotonsKey = "secondsFromVsyncToPhotons";
constexpr const char* kIpdMetersKey = "ipdMeters";
constexpr const char* kDisplayDebugModeKey = "displayDebugMode";
constexpr const char* kIsDisplayOnDesktopKey = "isDisplayOnDesktop";
constexpr const char* kIsDisplayRealDisplayKey = "isDisplayRealDisplay";
constexpr const char* kProjectionLeftKey = "projectionLeft";
constexpr const char* kProjectionRightKey = "projectionRight";
constexpr const char* kProjectionTopKey = "projectionTop";
constexpr const char* kProjectionBottomKey = "projectionBottom";
constexpr const char* kPoseSmoothingAlphaKey = "poseSmoothingAlpha";
constexpr const char* kHandControllerModeKey = "handControllerMode";
constexpr const char* kRuntimeHandTransportKey = "runtimeHandTransport";
constexpr const char* kRuntimeHandRegistryPathKey = "runtimeHandRegistryPath";
constexpr const char* kRuntimeHandStreamKey = "runtimeHandStream";
constexpr const char* kRuntimeHandShmNameKey = "runtimeHandShmName";
constexpr const char* kRuntimeHandMaxAgeMsKey = "runtimeHandMaxAgeMs";
constexpr const char* kRuntimeHandUdpBindHostKey = "runtimeHandUdpBindHost";
constexpr const char* kRuntimeHandUdpPortKey = "runtimeHandUdpPort";
constexpr const char* kRuntimeControllerStateTransportKey = "runtimeControllerStateTransport";
constexpr const char* kRuntimeControllerStateRegistryPathKey = "runtimeControllerStateRegistryPath";
constexpr const char* kRuntimeControllerStateStreamKey = "runtimeControllerStateStream";
constexpr const char* kRuntimeControllerStateShmNameKey = "runtimeControllerStateShmName";
constexpr const char* kRuntimeControllerStateMaxAgeMsKey = "runtimeControllerStateMaxAgeMs";
constexpr const char* kRuntimeControllerInputHoldMsKey = "runtimeControllerInputHoldMs";
constexpr const char* kRuntimeControllerStateUdpBindHostKey = "runtimeControllerStateUdpBindHost";
constexpr const char* kRuntimeControllerStateUdpPortKey = "runtimeControllerStateUdpPort";
constexpr const char* kHandControllerPoseModeKey = "handControllerPoseMode";
constexpr const char* kHandControllerPoseYOffsetKey = "handControllerPoseYOffset";
constexpr const char* kHandControllerRenderModelKey = "handControllerRenderModel";
constexpr const char* kHandControllerExposeSystemButtonKey = "handControllerExposeSystemButton";
constexpr const char* kHiddenHandControllerRenderModel = "xr_tracking_hidden_controller";
constexpr const char* kLeftHandSerialKey = "leftHandSerial";
constexpr const char* kRightHandSerialKey = "rightHandSerial";
constexpr const char* kCoordinateModeKey = "coordinateMode";
constexpr uint64_t kXrTrackingUniverseId = 42424242ULL;

enum class CoordinateMode {
  RuntimeReady,
  LegacyDriverTransform,
};

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

CoordinateMode parse_coordinate_mode(std::string value) {
  value = lower_ascii(std::move(value));
  if (value == "legacy" || value == "legacy_driver_transform" || value == "driver_transform" ||
      value == "openvr_legacy") {
    return CoordinateMode::LegacyDriverTransform;
  }
  return CoordinateMode::RuntimeReady;
}

const char* coordinate_mode_name(CoordinateMode mode) {
  return mode == CoordinateMode::LegacyDriverTransform ? "legacy_driver_transform" : "runtime_ready";
}

vr::HmdQuaternion_t quat(double w, double x, double y, double z) {
  vr::HmdQuaternion_t q{};
  q.w = w;
  q.x = x;
  q.y = y;
  q.z = z;
  return q;
}

vr::HmdQuaternion_t quat_conjugate(const vr::HmdQuaternion_t& q) {
  return quat(q.w, -q.x, -q.y, -q.z);
}

vr::HmdQuaternion_t quat_multiply(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b) {
  return quat(
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w);
}

vr::HmdQuaternion_t quat_normalize(const vr::HmdQuaternion_t& q) {
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n <= 0.0) {
    return quat(1.0, 0.0, 0.0, 0.0);
  }
  return quat(q.w / n, q.x / n, q.y / n, q.z / n);
}

double quat_dot(const vr::HmdQuaternion_t& a, const vr::HmdQuaternion_t& b) {
  return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

vr::HmdQuaternion_t quat_slerp_shortest(vr::HmdQuaternion_t a,
                                        vr::HmdQuaternion_t b,
                                        double alpha) {
  alpha = std::clamp(alpha, 0.0, 1.0);

  double dot = quat_dot(a, b);
  if (dot < 0.0) {
    b.w = -b.w;
    b.x = -b.x;
    b.y = -b.y;
    b.z = -b.z;
    dot = -dot;
  }

  if (dot > 0.9995) {
    return quat_normalize(quat(
        a.w + alpha * (b.w - a.w),
        a.x + alpha * (b.x - a.x),
        a.y + alpha * (b.y - a.y),
        a.z + alpha * (b.z - a.z)));
  }

  const double theta_0 = std::acos(std::clamp(dot, -1.0, 1.0));
  const double theta = theta_0 * alpha;
  const double sin_theta = std::sin(theta);
  const double sin_theta_0 = std::sin(theta_0);

  const double s0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
  const double s1 = sin_theta / sin_theta_0;

  return quat_normalize(quat(
      s0 * a.w + s1 * b.w,
      s0 * a.x + s1 * b.x,
      s0 * a.y + s1 * b.y,
      s0 * a.z + s1 * b.z));
}

// runtime_local -> OpenVR tracking space.
// Candidate basis:
//   OpenVR X = runtime X
//   OpenVR Y = runtime Z
//   OpenVR Z = -runtime Y
std::array<double, 3> runtime_vec_to_openvr(double x, double y, double z) {
  return {-x, z, y};
}

std::array<double, 3> runtime_ready_vec(double x, double y, double z) {
  return {x, y, z};
}

std::array<double, 3> vec_for_coordinate_mode(CoordinateMode mode, double x, double y, double z) {
  if (mode == CoordinateMode::LegacyDriverTransform) {
    return runtime_vec_to_openvr(x, y, z);
  }
  return runtime_ready_vec(x, y, z);
}

vr::HmdQuaternion_t runtime_quat_to_openvr(const vr::HmdQuaternion_t& q_runtime) {
  constexpr double kSqrtHalf = 0.70710678118654752440;

  // C = rotation about X by -90 degrees.
  const vr::HmdQuaternion_t c = quat(kSqrtHalf, -kSqrtHalf, 0.0, 0.0);
  const vr::HmdQuaternion_t c_inv = quat_conjugate(c);

  const vr::HmdQuaternion_t q_rx = quat_multiply(quat_multiply(c, q_runtime), c_inv);

  // Additional 180 deg basis rotation around OpenVR Y.
  // Keeps yaw direction, flips pitch direction and forward/back axis.
  const vr::HmdQuaternion_t y180 = quat(0.0, 0.0, 1.0, 0.0);
  const vr::HmdQuaternion_t y180_inv = quat_conjugate(y180);

  return quat_normalize(quat_multiply(quat_multiply(y180, q_rx), y180_inv));
}

vr::HmdQuaternion_t quat_for_coordinate_mode(CoordinateMode mode, const vr::HmdQuaternion_t& q_runtime) {
  if (mode == CoordinateMode::LegacyDriverTransform) {
    return runtime_quat_to_openvr(q_runtime);
  }
  return quat_normalize(q_runtime);
}

std::array<double, 3> quat_rotate_vec(const vr::HmdQuaternion_t& q, const std::array<double, 3>& v) {
  const vr::HmdQuaternion_t p = quat(0.0, v[0], v[1], v[2]);
  const vr::HmdQuaternion_t r = quat_multiply(quat_multiply(q, p), quat_conjugate(q));
  return {r.x, r.y, r.z};
}

std::array<double, 3> runtime_hmd_position_for_driver(const xr_runtime::RuntimeHmdPoseF64V1& hmd,
                                                       CoordinateMode mode) {
  const auto p = vec_for_coordinate_mode(mode, hmd.px, hmd.py, hmd.pz);
  if (mode == CoordinateMode::LegacyDriverTransform) {
    return {p[0], p[1] + 1.60, p[2]};
  }
  return p;
}

vr::HmdQuaternion_t runtime_hmd_rotation_for_driver(const xr_runtime::RuntimeHmdPoseF64V1& hmd,
                                                    CoordinateMode mode) {
  return quat_for_coordinate_mode(mode, quat(hmd.qw, hmd.qx, hmd.qy, hmd.qz));
}

std::string get_string_setting(const char* key, const std::string& fallback) {
  char value[1024]{};
  vr::VRSettings()->GetString(kDriverSettingsSection, key, value, sizeof(value));
  if (value[0] == '\0') return fallback;
  return std::string(value);
}

int32_t get_int_setting(const char* key, int32_t fallback) {
  vr::EVRSettingsError err = vr::VRSettingsError_None;
  const int32_t v = vr::VRSettings()->GetInt32(kDriverSettingsSection, key, &err);
  return err == vr::VRSettingsError_None ? v : fallback;
}

float get_float_setting(const char* key, float fallback) {
  vr::EVRSettingsError err = vr::VRSettingsError_None;
  const float v = vr::VRSettings()->GetFloat(kDriverSettingsSection, key, &err);
  return err == vr::VRSettingsError_None ? v : fallback;
}

bool get_bool_setting(const char* key, bool fallback) {
  vr::EVRSettingsError err = vr::VRSettingsError_None;
  const bool v = vr::VRSettings()->GetBool(kDriverSettingsSection, key, &err);
  return err == vr::VRSettingsError_None ? v : fallback;
}

void log_line(const std::string& msg) {
  if (vr::VRDriverLog()) {
    vr::VRDriverLog()->Log(msg.c_str());
  }
}


const std::string& driver_provided_chaperone_json() {
  // SteamVR asks for Room Setup when the active universe has no chaperone
  // calibration. This minimal standing/seated universe is enough for our
  // runtime-local seated/standing profile and prevents the startup Room Setup
  // prompt. Coordinates are OpenVR tracking-space coordinates: +Y up, -Z
  // forward, meters.
  static const std::string kJson = R"json({
  "json_id": "chaperone_info",
  "version": 5,
  "time": "2026-06-16T00:00:00.000Z",
  "universes": [
    {
      "universeID": 42424242,
      "seated": {
        "translation": [0.0, 0.0, 0.0],
        "yaw": 0.0
      },
      "standing": {
        "translation": [0.0, 0.0, 0.0],
        "yaw": 0.0
      },
      "setup_standing2": {
        "translation": [0.0, 0.0, 0.0],
        "yaw": 0.0
      },
      "play_area": [2.0, 2.0],
      "collision_bounds": [
        [[-1.0, 0.0, -1.0], [ 1.0, 0.0, -1.0], [ 1.0, 2.0, -1.0], [-1.0, 2.0, -1.0]],
        [[ 1.0, 0.0, -1.0], [ 1.0, 0.0,  1.0], [ 1.0, 2.0,  1.0], [ 1.0, 2.0, -1.0]],
        [[ 1.0, 0.0,  1.0], [-1.0, 0.0,  1.0], [-1.0, 2.0,  1.0], [ 1.0, 2.0,  1.0]],
        [[-1.0, 0.0,  1.0], [-1.0, 0.0, -1.0], [-1.0, 2.0, -1.0], [-1.0, 2.0,  1.0]]
      ]
    }
  ]
})json";
  return kJson;
}

class XrTrackingHmdDriver final : public vr::ITrackedDeviceServerDriver,
                                     public vr::IVRDisplayComponent {
 public:
  XrTrackingHmdDriver() { load_settings(); }

  std::string serial() const { return serial_number_; }

  vr::EVRInitError Activate(uint32_t object_id) override {
    object_id_ = object_id;
    property_container_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);
    pose_smoothing_initialized_ = false;

    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_TrackingSystemName_String, "xr_tracking");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ManufacturerName_String, "XR Tracking Runtime");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ModelNumber_String, model_number_.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_RenderModelName_String, model_number_.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_SerialNumber_String, serial_number_.c_str());
    vr::VRProperties()->SetFloatProperty(property_container_, vr::Prop_UserIpdMeters_Float, ipd_meters_);
    vr::VRProperties()->SetFloatProperty(property_container_, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
    vr::VRProperties()->SetFloatProperty(property_container_, vr::Prop_DisplayFrequency_Float, display_frequency_);
    vr::VRProperties()->SetFloatProperty(property_container_, vr::Prop_SecondsFromVsyncToPhotons_Float,
                                         seconds_from_vsync_to_photons_);
    vr::VRProperties()->SetUint64Property(property_container_, vr::Prop_CurrentUniverseId_Uint64,
                                          kXrTrackingUniverseId);
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_DriverProvidedChaperoneJson_String,
                                          driver_provided_chaperone_json().c_str());
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DriverProvidedChaperoneVisibility_Bool,
                                        false);
    vr::VRProperties()->SetInt32Property(property_container_, vr::Prop_ExpectedTrackingReferenceCount_Int32, 0);
    vr::VRProperties()->SetInt32Property(property_container_, vr::Prop_ExpectedControllerCount_Int32, 2);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_IsOnDesktop_Bool,
                                          is_display_on_desktop_);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DisplayDebugMode_Bool, display_debug_mode_);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DeviceIsWireless_Bool, false);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_ContainsProximitySensor_Bool, true);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

    pose_reader_ = create_runtime_pose_reader(reader_config_);
    if (!pose_reader_) {
      log_line("[xr_tracking_openvr] no runtime pose reader for transport=" + reader_config_.transport);
    } else {
      log_line(std::string("[xr_tracking_openvr] runtime pose reader transport=") + pose_reader_->transport_name() +
               " stream=" + reader_config_.stream_id + " shm=" + reader_config_.shm_name +
               " udp=" + reader_config_.udp_bind_host + ":" + std::to_string(reader_config_.udp_port));
    }

    const vr::EVRInputError proximity_err =
        vr::VRDriverInput()->CreateBooleanComponent(property_container_, "/input/proximity/click", &proximity_component_);
    if (proximity_err != vr::VRInputError_None) {
      log_line("[xr_tracking_openvr] failed to create /input/proximity/click component err=" +
               std::to_string(static_cast<int>(proximity_err)));
      proximity_component_ = 0;
    } else {
      vr::VRDriverInput()->UpdateBooleanComponent(proximity_component_, true, 0.0);
      log_line("[xr_tracking_openvr] /input/proximity/click=true");
    }

    return vr::VRInitError_None;
  }

  void Deactivate() override {
    object_id_ = vr::k_unTrackedDeviceIndexInvalid;
    proximity_component_ = 0;
    pose_reader_.reset();
    pose_smoothing_initialized_ = false;
  }

  void EnterStandby() override {}

  void* GetComponent(const char* component_name_and_version) override {
    if (std::strcmp(component_name_and_version, vr::IVRDisplayComponent_Version) == 0) {
      return static_cast<vr::IVRDisplayComponent*>(this);
    }
    return nullptr;
  }

  void DebugRequest(const char*, char* response_buffer, uint32_t response_buffer_size) override {
    if (response_buffer_size > 0) response_buffer[0] = '\0';
  }

  vr::DriverPose_t GetPose() override {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation = quat(1.0, 0.0, 0.0, 0.0);
    pose.qDriverFromHeadRotation = quat(1.0, 0.0, 0.0, 0.0);
    pose.qRotation = quat(1.0, 0.0, 0.0, 0.0);
    pose.deviceIsConnected = true;
    pose.poseIsValid = false;
    pose.result = vr::TrackingResult_Uninitialized;

    RuntimePoseSample sample{};
    std::string error;
    const bool have_sample = pose_reader_ && pose_reader_->read_latest(sample, &error);
    if (!have_sample) {
      if (runtime_pose_optional_) {
        pose.poseIsValid = true;
        pose.result = vr::TrackingResult_Running_OK;
      }
      maybe_log_pose_error(error);
      return pose;
    }

    last_pose_error_.clear();
    const bool valid = runtime_pose_is_valid_for_openvr(sample.pose) &&
                       runtime_pose_is_fresh(sample, reader_config_.max_age_ms);
    if (!valid) {
      pose.result = vr::TrackingResult_Uninitialized;
      return pose;
    }

    pose.poseIsValid = true;
    pose.result = vr::TrackingResult_Running_OK;

    const std::array<double, 3> target_position =
        runtime_hmd_position_for_driver(sample.pose, coordinate_mode_);

    const vr::HmdQuaternion_t target_rotation =
        runtime_hmd_rotation_for_driver(sample.pose, coordinate_mode_);

    if (pose_smoothing_alpha_ >= 0.999f) {
      smoothed_position_ = target_position;
      smoothed_rotation_ = target_rotation;
      pose_smoothing_initialized_ = true;
    } else if (!pose_smoothing_initialized_) {
      smoothed_position_ = target_position;
      smoothed_rotation_ = target_rotation;
      pose_smoothing_initialized_ = true;
    } else {
      const double alpha = std::clamp(static_cast<double>(pose_smoothing_alpha_), 0.0, 1.0);
      for (int i = 0; i < 3; ++i) {
        smoothed_position_[i] += alpha * (target_position[i] - smoothed_position_[i]);
      }
      smoothed_rotation_ = quat_slerp_shortest(smoothed_rotation_, target_rotation, alpha);
    }

    pose.vecPosition[0] = smoothed_position_[0];
    pose.vecPosition[1] = smoothed_position_[1];
    pose.vecPosition[2] = smoothed_position_[2];
    pose.qRotation = smoothed_rotation_;

    if ((sample.pose.flags & xr_runtime::RUNTIME_HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
      const auto v = vec_for_coordinate_mode(coordinate_mode_, sample.pose.vx, sample.pose.vy, sample.pose.vz);
      pose.vecVelocity[0] = v[0];
      pose.vecVelocity[1] = v[1];
      pose.vecVelocity[2] = v[2];
    }
    if ((sample.pose.flags & xr_runtime::RUNTIME_HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) {
      const auto w = vec_for_coordinate_mode(coordinate_mode_, sample.pose.wx, sample.pose.wy, sample.pose.wz);
      pose.vecAngularVelocity[0] = w[0];
      pose.vecAngularVelocity[1] = w[1];
      pose.vecAngularVelocity[2] = w[2];
    }

    return pose;
  }

  void RunFrame() {
    if (object_id_ != vr::k_unTrackedDeviceIndexInvalid) {
      if (proximity_component_ != 0) {
        vr::VRDriverInput()->UpdateBooleanComponent(proximity_component_, true, 0.0);
      }
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, GetPose(), sizeof(vr::DriverPose_t));
    }
  }

  bool IsDisplayOnDesktop() override { return is_display_on_desktop_; }
  bool IsDisplayRealDisplay() override { return is_display_real_display_; }

  void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override {
    *width = static_cast<uint32_t>(render_width_);
    *height = static_cast<uint32_t>(render_height_);
  }

  void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) override {
    *y = 0;
    *width = static_cast<uint32_t>(window_width_ / 2);
    *height = static_cast<uint32_t>(window_height_);
    *x = eye == vr::Eye_Left ? 0u : static_cast<uint32_t>(window_width_ / 2);
  }

  void GetProjectionRaw(vr::EVREye, float* left, float* right, float* top, float* bottom) override {
    *left = projection_left_;
    *right = projection_right_;
    *top = projection_top_;
    *bottom = projection_bottom_;
  }

  vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
    vr::DistortionCoordinates_t coordinates{};
    coordinates.rfRed[0] = u;
    coordinates.rfRed[1] = v;
    coordinates.rfGreen[0] = u;
    coordinates.rfGreen[1] = v;
    coordinates.rfBlue[0] = u;
    coordinates.rfBlue[1] = v;
    return coordinates;
  }

  bool ComputeInverseDistortion(vr::HmdVector2_t* result,
                                vr::EVREye,
                                uint32_t,
                                float u,
                                float v) override {
    if (result == nullptr) {
      return false;
    }

    result->v[0] = u;
    result->v[1] = v;
    return true;
  }

  void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override {
    *x = window_x_;
    *y = window_y_;
    *width = static_cast<uint32_t>(window_width_);
    *height = static_cast<uint32_t>(window_height_);
  }

 private:
  void load_settings() {
    serial_number_ = get_string_setting(kSerialNumberKey, "xr-tracking-hmd-001");
    model_number_ = get_string_setting(kModelNumberKey, "XR Tracking Runtime HMD");

    reader_config_.transport = get_string_setting(kRuntimePoseTransportKey, "auto");
    reader_config_.registry_path = get_string_setting(kRuntimePoseRegistryPathKey, "");
    reader_config_.stream_id = get_string_setting(kRuntimePoseStreamKey, "runtime_hmd_pose");
    reader_config_.shm_name = get_string_setting(kRuntimePoseShmNameKey, "runtime_hmd_pose");
    reader_config_.max_age_ms = static_cast<uint32_t>(std::max(0, get_int_setting(kRuntimePoseMaxAgeMsKey, 250)));
    reader_config_.udp_bind_host = get_string_setting(kRuntimePoseUdpBindHostKey, "127.0.0.1");
    reader_config_.udp_port = static_cast<uint16_t>(std::clamp(get_int_setting(kRuntimePoseUdpPortKey, 45800), 1, 65535));
    runtime_pose_optional_ = get_bool_setting(kRuntimePoseOptionalKey, false);

    window_x_ = get_int_setting(kWindowXKey, 0);
    window_y_ = get_int_setting(kWindowYKey, 0);
    window_width_ = std::max(1, get_int_setting(kWindowWidthKey, 3840));
    window_height_ = std::max(1, get_int_setting(kWindowHeightKey, 1080));
    render_width_ = std::max(1, get_int_setting(kRenderWidthKey, 1920));
    render_height_ = std::max(1, get_int_setting(kRenderHeightKey, 1080));
    display_frequency_ = get_float_setting(kDisplayFrequencyKey, kDefaultDisplayFrequencyHz);
    seconds_from_vsync_to_photons_ = get_float_setting(kSecondsFromVsyncToPhotonsKey, 0.011f);
    ipd_meters_ = get_float_setting(kIpdMetersKey, 0.064f);
    display_debug_mode_ = get_bool_setting(kDisplayDebugModeKey, false);
    is_display_on_desktop_ = get_bool_setting(kIsDisplayOnDesktopKey, false);
    is_display_real_display_ = get_bool_setting(kIsDisplayRealDisplayKey, true);
    projection_left_ = get_float_setting(kProjectionLeftKey, -1.0f);
    projection_right_ = get_float_setting(kProjectionRightKey, 1.0f);
    projection_top_ = get_float_setting(kProjectionTopKey, -1.0f);
    projection_bottom_ = get_float_setting(kProjectionBottomKey, 1.0f);
    pose_smoothing_alpha_ = std::clamp(get_float_setting(kPoseSmoothingAlphaKey, 0.55f), 0.0f, 1.0f);
    coordinate_mode_ = parse_coordinate_mode(get_string_setting(kCoordinateModeKey, "runtime_ready"));
    log_line(std::string("[xr_tracking_openvr] coordinateMode=") + coordinate_mode_name(coordinate_mode_));
    log_line(std::string("[xr_tracking_openvr] display settings: window=") + std::to_string(window_x_) + "," +
             std::to_string(window_y_) + " " + std::to_string(window_width_) + "x" +
             std::to_string(window_height_) + " render=" + std::to_string(render_width_) + "x" +
             std::to_string(render_height_) + " frequency=" + std::to_string(display_frequency_) +
             " on_desktop=" + std::to_string(is_display_on_desktop_ ? 1 : 0) +
             " real_display=" + std::to_string(is_display_real_display_ ? 1 : 0) +
             " debug=" + std::to_string(display_debug_mode_ ? 1 : 0));
  }

  void maybe_log_pose_error(const std::string& error) {
    if (error.empty() || error == last_pose_error_) return;
    last_pose_error_ = error;
    log_line("[xr_tracking_openvr] runtime pose unavailable: " + error);
  }

  vr::TrackedDeviceIndex_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
  vr::PropertyContainerHandle_t property_container_ = vr::k_ulInvalidPropertyContainer;
  vr::VRInputComponentHandle_t proximity_component_ = 0;

  std::string serial_number_;
  std::string model_number_;
  RuntimePoseReaderConfig reader_config_;
  bool runtime_pose_optional_ = false;
  std::unique_ptr<IRuntimePoseReader> pose_reader_;
  std::string last_pose_error_;

  float pose_smoothing_alpha_ = 0.55f;  // 1.0 = raw/no smoothing; 0.35..0.70 = light smoothing.
  CoordinateMode coordinate_mode_ = CoordinateMode::RuntimeReady;
  bool pose_smoothing_initialized_ = false;
  std::array<double, 3> smoothed_position_{0.0, 0.0, 0.0};
  vr::HmdQuaternion_t smoothed_rotation_ = quat(1.0, 0.0, 0.0, 0.0);

  int32_t window_x_ = 0;
  int32_t window_y_ = 0;
  int32_t window_width_ = 3840;
  int32_t window_height_ = 1080;
  int32_t render_width_ = 1920;
  int32_t render_height_ = 1080;
  float display_frequency_ = kDefaultDisplayFrequencyHz;
  float seconds_from_vsync_to_photons_ = 0.011f;
  float ipd_meters_ = 0.064f;
  bool display_debug_mode_ = false;
  bool is_display_on_desktop_ = false;
  bool is_display_real_display_ = true;
  float projection_left_ = -1.0f;
  float projection_right_ = 1.0f;
  float projection_top_ = -1.0f;
  float projection_bottom_ = 1.0f;
};


struct HandControllerConfig {
  RuntimeHandReaderConfig reader;
  RuntimeControllerStateReaderConfig controller_reader;
  RuntimePoseReaderConfig hmd_reader;
  int runtime_controller_input_hold_ms = 3000;
  std::string mode = "none";
  std::string pose_mode = "runtime_absolute";
  std::string left_serial = "xr-tracking-left-hand-001";
  std::string right_serial = "xr-tracking-right-hand-001";
  std::string render_model;
  bool expose_system_button = false;
  float pose_y_offset = 1.60f;
  CoordinateMode coordinate_mode = CoordinateMode::RuntimeReady;
};

bool hand_pose_mode_is_hmd_relative(const std::string& pose_mode) {
  const std::string mode = lower_ascii(pose_mode);
  return mode == "hmd_relative" || mode == "relative_to_hmd";
}

class XrHandControllerDriver final : public vr::ITrackedDeviceServerDriver {
 public:
  XrHandControllerDriver(bool left, HandControllerConfig cfg)
      : left_(left), cfg_(std::move(cfg)) {}

  std::string serial() const { return left_ ? cfg_.left_serial : cfg_.right_serial; }

  vr::EVRInitError Activate(uint32_t object_id) override {
    object_id_ = object_id;
    property_container_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id_);

    const std::string model = left_ ? "XR Hand Controller Left" : "XR Hand Controller Right";

    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_TrackingSystemName_String, "xr_tracking");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ManufacturerName_String, "XR Tracking Runtime");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ModelNumber_String, model.c_str());
    // Let games draw their own hands without an extra SteamVR controller model
    // floating inside them. Do not publish an empty render model string: current
    // SteamVR builds may then fall back to a generic/Steam Controller model. Use
    // a project-owned non-existent model id as an explicit hidden sentinel.
    // If a visible debug model is needed, set handControllerRenderModel to a real
    // SteamVR render model, for example vr_controller_vive_1_5.
    std::string render_model = cfg_.render_model;
    const std::string render_model_l = lower_ascii(render_model);
    if (render_model.empty() || render_model_l == "none" ||
        render_model_l == "off" || render_model_l == "hidden") {
      render_model = kHiddenHandControllerRenderModel;
    }
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_RenderModelName_String, render_model.c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ModelNumber_String, "Vive Controller MV");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ManufacturerName_String, "HTC");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_SerialNumber_String, serial().c_str());
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_ControllerType_String, "xr_hand_controller");
    vr::VRProperties()->SetStringProperty(property_container_, vr::Prop_InputProfilePath_String,
        "{xr_tracking}/input/xr_hand_controller_profile.json");
    vr::VRProperties()->SetInt32Property(property_container_, vr::Prop_ControllerRoleHint_Int32,
                                         static_cast<int32_t>(left_ ? vr::TrackedControllerRole_LeftHand
                                                                    : vr::TrackedControllerRole_RightHand));
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DeviceIsWireless_Bool, true);
    vr::VRProperties()->SetBoolProperty(property_container_, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

    controller_reader_ = create_runtime_controller_state_reader(cfg_.controller_reader);
    if (!controller_reader_) {
      log_line(std::string("[xr_tracking_openvr] no runtime controller state reader for transport=") +
               cfg_.controller_reader.transport);
    } else {
      log_line(std::string("[xr_tracking_openvr] runtime controller state reader ") +
               (left_ ? "left" : "right") +
               " transport=" + controller_reader_->transport_name() +
               " stream=" + cfg_.controller_reader.stream_id +
               " shm=" + cfg_.controller_reader.shm_name +
               " udp=" + cfg_.controller_reader.udp_bind_host + ":" + std::to_string(cfg_.controller_reader.udp_port));
    }

    hand_reader_ = create_runtime_hand_reader(cfg_.reader);
    if (!hand_reader_) {
      log_line(std::string("[xr_tracking_openvr] no hand reader for transport=") + cfg_.reader.transport);
    } else {
      log_line(std::string("[xr_tracking_openvr] hand reader ") + (left_ ? "left" : "right") +
               " transport=" + hand_reader_->transport_name() +
               " stream=" + cfg_.reader.stream_id +
               " shm=" + cfg_.reader.shm_name +
               " udp=" + cfg_.reader.udp_bind_host + ":" + std::to_string(cfg_.reader.udp_port));
    }

    if (hand_pose_mode_is_hmd_relative(cfg_.pose_mode)) {
      pose_reader_ = create_runtime_pose_reader(cfg_.hmd_reader);
      if (!pose_reader_) {
        log_line("[xr_tracking_openvr] no runtime pose reader for hand-relative mode transport=" +
                 cfg_.hmd_reader.transport);
      } else {
        log_line(std::string("[xr_tracking_openvr] hand HMD-relative pose reader transport=") +
                 pose_reader_->transport_name() +
                 " stream=" + cfg_.hmd_reader.stream_id +
                 " shm=" + cfg_.hmd_reader.shm_name +
                 " udp=" + cfg_.hmd_reader.udp_bind_host + ":" + std::to_string(cfg_.hmd_reader.udp_port));
      }
    }

    create_input_components();
    cached_pose_ = make_invalid_pose();
    return vr::VRInitError_None;
  }

  void Deactivate() override {
    object_id_ = vr::k_unTrackedDeviceIndexInvalid;
    property_container_ = vr::k_ulInvalidPropertyContainer;
    trigger_value_component_ = 0;
    trigger_click_component_ = 0;
    grip_value_component_ = 0;
    grip_click_component_ = 0;
    thumbstick_x_component_ = 0;
    thumbstick_y_component_ = 0;
    thumbstick_click_component_ = 0;
    trackpad_x_component_ = 0;
    trackpad_y_component_ = 0;
    trackpad_touch_component_ = 0;
    trackpad_click_component_ = 0;
    menu_click_component_ = 0;
    a_click_component_ = 0;
    b_click_component_ = 0;
    x_click_component_ = 0;
    y_click_component_ = 0;
    system_click_component_ = 0;
    haptic_component_ = 0;
    controller_reader_.reset();
    hand_reader_.reset();
    pose_reader_.reset();
    cached_pose_ = make_invalid_pose();
    has_cached_runtime_controller_side_ = false;
    cached_runtime_controller_input_ns_ = 0;
    has_cached_runtime_controller_side_ = false;
    cached_runtime_controller_input_ns_ = 0;
  }

  void EnterStandby() override {}

  void* GetComponent(const char*) override { return nullptr; }

  void DebugRequest(const char*, char* response_buffer, uint32_t response_buffer_size) override {
    if (response_buffer_size > 0) response_buffer[0] = '\0';
  }

  vr::DriverPose_t GetPose() override { return cached_pose_; }

  void RunFrame() {
    if (object_id_ == vr::k_unTrackedDeviceIndexInvalid) return;

    RuntimeControllerStateSample controller_sample{};
    std::string controller_error;
    const bool have_controller_sample =
        controller_reader_ && controller_reader_->read_latest(controller_sample, &controller_error);
    if (have_controller_sample &&
        runtime_controller_state_frame_is_valid(controller_sample.frame) &&
        runtime_controller_state_sample_is_fresh(controller_sample, cfg_.controller_reader.max_age_ms)) {
      last_controller_state_error_.clear();
      const auto& controller_side = left_ ? controller_sample.frame.left : controller_sample.frame.right;
      cached_runtime_controller_side_ = controller_side;
      cached_runtime_controller_input_ns_ = steady_now_ns();
      has_cached_runtime_controller_side_ = true;
      maybe_log_runtime_controller_sample(controller_sample.frame.sequence, controller_side);
      if (runtime_controller_side_has_pose(controller_side, left_)) {
        cached_pose_ = make_pose_from_runtime_controller(controller_side);
      } else {
        cached_pose_ = make_invalid_pose();
      }
      update_inputs_from_runtime_controller(&controller_side);
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
      return;
    }
    if (controller_reader_ && !controller_error.empty()) {
      maybe_log_controller_state_error(controller_error);
    }

    if (hold_cached_runtime_controller_input_if_fresh()) {
      return;
    }

    RuntimeHandSample sample{};
    std::string error;
    const bool have_sample = hand_reader_ && hand_reader_->read_latest(sample, &error);
    if (!have_sample) {
      maybe_log_hand_error(error);
      cached_pose_ = make_invalid_pose();
      update_inputs(nullptr);
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
      return;
    }

    last_hand_error_.clear();
    if (!runtime_hand_sample_is_fresh(sample, cfg_.reader.max_age_ms)) {
      cached_pose_ = make_invalid_pose();
      update_inputs(nullptr);
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
      return;
    }

    const xr_tracking::HandSideF32V2& side = left_ ? sample.frame.left : sample.frame.right;
    if (!runtime_hand_side_is_valid(sample.frame, side, left_)) {
      cached_pose_ = make_invalid_pose();
      update_inputs(nullptr);
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
      return;
    }

    if (hand_pose_mode_is_hmd_relative(cfg_.pose_mode)) {
      RuntimePoseSample hmd_sample{};
      std::string hmd_error;
      const bool have_hmd = pose_reader_ && pose_reader_->read_latest(hmd_sample, &hmd_error);
      const bool hmd_valid = have_hmd &&
                             runtime_pose_is_valid_for_openvr(hmd_sample.pose) &&
                             runtime_pose_is_fresh(hmd_sample, cfg_.hmd_reader.max_age_ms);
      if (!hmd_valid) {
        maybe_log_runtime_pose_error(hmd_error.empty() ? "runtime_hmd_pose unavailable/stale/invalid" : hmd_error);
        cached_pose_ = make_invalid_pose();
        update_inputs(nullptr);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
        return;
      }

      last_runtime_pose_error_.clear();
      cached_pose_ = make_pose_from_hand_hmd_relative(side, hmd_sample.pose);
    } else {
      cached_pose_ = make_pose_from_hand_world(side);
    }

    update_inputs(&side);
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
  }

 private:
  vr::DriverPose_t make_invalid_pose() const {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation = quat(1.0, 0.0, 0.0, 0.0);
    pose.qDriverFromHeadRotation = quat(1.0, 0.0, 0.0, 0.0);
    pose.qRotation = quat(1.0, 0.0, 0.0, 0.0);
    pose.deviceIsConnected = true;
    pose.poseIsValid = false;
    pose.result = vr::TrackingResult_Uninitialized;
    return pose;
  }

  vr::DriverPose_t make_pose_from_hand_world(const xr_tracking::HandSideF32V2& side) const {
    vr::DriverPose_t pose = make_invalid_pose();
    pose.poseIsValid = true;
    pose.result = vr::TrackingResult_Running_OK;

    const auto p = vec_for_coordinate_mode(
        cfg_.coordinate_mode, side.controller_px, side.controller_py, side.controller_pz);
    pose.vecPosition[0] = p[0];
    pose.vecPosition[1] = p[1] + (cfg_.coordinate_mode == CoordinateMode::LegacyDriverTransform ? cfg_.pose_y_offset : 0.0f);
    pose.vecPosition[2] = p[2];

    pose.qRotation = quat_for_coordinate_mode(
        cfg_.coordinate_mode,
        quat(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz));

    if ((side.flags & xr_tracking::HAND_LINEAR_VELOCITY_VALID) != 0u) {
      const auto v = vec_for_coordinate_mode(cfg_.coordinate_mode, side.vx, side.vy, side.vz);
      pose.vecVelocity[0] = v[0];
      pose.vecVelocity[1] = v[1];
      pose.vecVelocity[2] = v[2];
    }

    if ((side.flags & xr_tracking::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
      const auto w = vec_for_coordinate_mode(cfg_.coordinate_mode, side.wx, side.wy, side.wz);
      pose.vecAngularVelocity[0] = w[0];
      pose.vecAngularVelocity[1] = w[1];
      pose.vecAngularVelocity[2] = w[2];
    }

    return pose;
  }

  vr::DriverPose_t make_pose_from_hand_hmd_relative(const xr_tracking::HandSideF32V2& side,
                                                     const xr_runtime::RuntimeHmdPoseF64V1& hmd) const {
    vr::DriverPose_t pose = make_invalid_pose();
    pose.poseIsValid = true;
    pose.result = vr::TrackingResult_Running_OK;

    const std::array<double, 3> hmd_pos = runtime_hmd_position_for_driver(hmd, cfg_.coordinate_mode);
    const vr::HmdQuaternion_t hmd_rot = runtime_hmd_rotation_for_driver(hmd, cfg_.coordinate_mode);

    auto hand_rel = vec_for_coordinate_mode(
        cfg_.coordinate_mode, side.controller_px, side.controller_py, side.controller_pz);

    if (cfg_.coordinate_mode == CoordinateMode::LegacyDriverTransform) {
      // Legacy diagnostic/controller comfort offset:
      // OpenVR forward relative to HMD is -Z.
      // In runtime_ready mode this belongs in xr_runtime_adapter profile config.
      hand_rel[2] -= 0.45;
    }

    const auto hand_rel_rotated = quat_rotate_vec(hmd_rot, hand_rel);

    pose.vecPosition[0] = hmd_pos[0] + hand_rel_rotated[0];
    pose.vecPosition[1] = hmd_pos[1] + hand_rel_rotated[1] +
                          (cfg_.coordinate_mode == CoordinateMode::LegacyDriverTransform ? cfg_.pose_y_offset : 0.0f);
    pose.vecPosition[2] = hmd_pos[2] + hand_rel_rotated[2];

    const vr::HmdQuaternion_t hand_rel_rot = quat_for_coordinate_mode(
        cfg_.coordinate_mode,
        quat(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz));

    if (cfg_.coordinate_mode == CoordinateMode::LegacyDriverTransform) {
      // Legacy local roll correction around controller/view forward axis.
      const vr::HmdQuaternion_t hand_local_roll_left_90 =
          quat(0.7071067811865476, 0.0, 0.0, -0.7071067811865475);
      pose.qRotation = quat_normalize(
          quat_multiply(hmd_rot, quat_multiply(hand_rel_rot, hand_local_roll_left_90)));
    } else {
      pose.qRotation = quat_normalize(quat_multiply(hmd_rot, hand_rel_rot));
    }

    if ((side.flags & xr_tracking::HAND_LINEAR_VELOCITY_VALID) != 0u) {
      const auto v_rel = vec_for_coordinate_mode(cfg_.coordinate_mode, side.vx, side.vy, side.vz);
      const auto v_abs = quat_rotate_vec(hmd_rot, v_rel);
      pose.vecVelocity[0] = v_abs[0];
      pose.vecVelocity[1] = v_abs[1];
      pose.vecVelocity[2] = v_abs[2];
    }

    if ((side.flags & xr_tracking::HAND_ANGULAR_VELOCITY_VALID) != 0u) {
      const auto w_rel = vec_for_coordinate_mode(cfg_.coordinate_mode, side.wx, side.wy, side.wz);
      const auto w_abs = quat_rotate_vec(hmd_rot, w_rel);
      pose.vecAngularVelocity[0] = w_abs[0];
      pose.vecAngularVelocity[1] = w_abs[1];
      pose.vecAngularVelocity[2] = w_abs[2];
    }

    return pose;
  }

  vr::DriverPose_t make_pose_from_runtime_controller(
      const xr_runtime::RuntimeControllerSideStateV1& side) const {
    vr::DriverPose_t pose = make_invalid_pose();
    if ((side.flags & xr_runtime::RUNTIME_CONTROLLER_POSE_VALID) == 0u) {
      return pose;
    }

    pose.poseIsValid = true;
    pose.result = vr::TrackingResult_Running_OK;

    const auto p = vec_for_coordinate_mode(
        cfg_.coordinate_mode, side.position[0], side.position[1], side.position[2]);
    pose.vecPosition[0] = p[0];
    pose.vecPosition[1] = p[1] + (cfg_.coordinate_mode == CoordinateMode::LegacyDriverTransform ? cfg_.pose_y_offset : 0.0f);
    pose.vecPosition[2] = p[2];

    // RuntimeControllerStateV1 stores xyzw, OpenVR helper expects wxyz.
    pose.qRotation = quat_for_coordinate_mode(
        cfg_.coordinate_mode,
        quat(side.orientation_xyzw[3],
             side.orientation_xyzw[0],
             side.orientation_xyzw[1],
             side.orientation_xyzw[2]));

    const auto v = vec_for_coordinate_mode(
        cfg_.coordinate_mode, side.linear_velocity[0], side.linear_velocity[1], side.linear_velocity[2]);
    pose.vecVelocity[0] = v[0];
    pose.vecVelocity[1] = v[1];
    pose.vecVelocity[2] = v[2];

    const auto w = vec_for_coordinate_mode(
        cfg_.coordinate_mode, side.angular_velocity[0], side.angular_velocity[1], side.angular_velocity[2]);
    pose.vecAngularVelocity[0] = w[0];
    pose.vecAngularVelocity[1] = w[1];
    pose.vecAngularVelocity[2] = w[2];

    return pose;
  }

  void create_input_components() {
    create_scalar_component("/input/trigger/value", &trigger_value_component_);
    create_boolean_component("/input/trigger/click", &trigger_click_component_);
    create_scalar_component("/input/grip/value", &grip_value_component_);
    create_boolean_component("/input/grip/click", &grip_click_component_);
    create_scalar_component("/input/joystick/x", &thumbstick_x_component_, vr::VRScalarUnits_NormalizedTwoSided);
    create_scalar_component("/input/joystick/y", &thumbstick_y_component_, vr::VRScalarUnits_NormalizedTwoSided);
    create_boolean_component("/input/joystick/click", &thumbstick_click_component_);
    // Vive-compatibility path: many SteamVR Input bindings, including HL Alyx,
    // have mature defaults for Vive Controller /input/trackpad. Mirror the same
    // runtime stick/D-pad state into a trackpad source so compatibility-mode
    // bindings can drive application actions instead of leaving them unbound.
    create_scalar_component("/input/trackpad/x", &trackpad_x_component_, vr::VRScalarUnits_NormalizedTwoSided);
    create_scalar_component("/input/trackpad/y", &trackpad_y_component_, vr::VRScalarUnits_NormalizedTwoSided);
    create_boolean_component("/input/trackpad/touch", &trackpad_touch_component_);
    create_boolean_component("/input/trackpad/click", &trackpad_click_component_);
    create_boolean_component("/input/application_menu/click", &menu_click_component_);
    create_boolean_component("/input/a/click", &a_click_component_);
    create_boolean_component("/input/b/click", &b_click_component_);
    create_boolean_component("/input/x/click", &x_click_component_);
    create_boolean_component("/input/y/click", &y_click_component_);
    if (cfg_.expose_system_button) {
      create_boolean_component("/input/system/click", &system_click_component_);
    } else {
      system_click_component_ = 0;
    }
    create_haptic_component("/output/haptic", &haptic_component_);
  }

  void create_scalar_component(const char* path,
                               vr::VRInputComponentHandle_t* handle,
                               vr::EVRScalarUnits units = vr::VRScalarUnits_NormalizedOneSided) {
    const vr::EVRInputError err = vr::VRDriverInput()->CreateScalarComponent(
        property_container_, path, handle, vr::VRScalarType_Absolute, units);
    if (err != vr::VRInputError_None) {
      log_line(std::string("[xr_tracking_openvr] failed to create ") + path +
               " err=" + std::to_string(static_cast<int>(err)));
      *handle = 0;
    }
  }

  void create_boolean_component(const char* path, vr::VRInputComponentHandle_t* handle) {
    const vr::EVRInputError err = vr::VRDriverInput()->CreateBooleanComponent(property_container_, path, handle);
    if (err != vr::VRInputError_None) {
      log_line(std::string("[xr_tracking_openvr] failed to create ") + path +
               " err=" + std::to_string(static_cast<int>(err)));
      *handle = 0;
    }
  }

  void create_haptic_component(const char* path, vr::VRInputComponentHandle_t* handle) {
    const vr::EVRInputError err = vr::VRDriverInput()->CreateHapticComponent(property_container_, path, handle);
    if (err != vr::VRInputError_None) {
      log_line(std::string("[xr_tracking_openvr] failed to create ") + path +
               " err=" + std::to_string(static_cast<int>(err)));
      *handle = 0;
    }
  }

  void update_inputs(const xr_tracking::HandSideF32V2* side) {
    const float pinch = side != nullptr && (side->flags & xr_tracking::HAND_PINCH_VALID) != 0u
                            ? std::clamp(side->pinch_strength, 0.0f, 1.0f)
                            : 0.0f;
    const float grab = side != nullptr && (side->flags & xr_tracking::HAND_GRAB_VALID) != 0u
                           ? std::clamp(side->grab_strength, 0.0f, 1.0f)
                           : 0.0f;
    const bool pinch_click = side != nullptr && side->pinch_active != 0;
    const bool grab_click = side != nullptr && side->grab_active != 0;

    if (trigger_value_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(trigger_value_component_, pinch, 0.0);
    }
    if (trigger_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(trigger_click_component_, pinch_click, 0.0);
    }
    if (grip_value_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(grip_value_component_, grab, 0.0);
    }
    if (grip_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(grip_click_component_, grab_click, 0.0);
    }
    // When no external controller state is active, keep the compatibility
    // stick/trackpad inputs neutral instead of leaving a stale value.
    if (thumbstick_x_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(thumbstick_x_component_, 0.0f, 0.0);
    }
    if (thumbstick_y_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(thumbstick_y_component_, 0.0f, 0.0);
    }
    if (thumbstick_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(thumbstick_click_component_, false, 0.0);
    }
    if (trackpad_x_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(trackpad_x_component_, 0.0f, 0.0);
    }
    if (trackpad_y_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(trackpad_y_component_, 0.0f, 0.0);
    }
    if (trackpad_touch_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(trackpad_touch_component_, false, 0.0);
    }
    if (trackpad_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(trackpad_click_component_, false, 0.0);
    }
  }

  void update_inputs_from_runtime_controller(const xr_runtime::RuntimeControllerSideStateV1* side) {
    const uint64_t buttons = side != nullptr ? side->buttons : 0ull;
    const float trigger = side != nullptr ? std::clamp(side->trigger, 0.0f, 1.0f) : 0.0f;
    const float grip = side != nullptr ? std::clamp(side->grip, 0.0f, 1.0f) : 0.0f;
    float stick_x = side != nullptr ? std::clamp(side->thumbstick_x, -1.0f, 1.0f) : 0.0f;
    float stick_y = side != nullptr ? std::clamp(side->thumbstick_y, -1.0f, 1.0f) : 0.0f;

    if (std::abs(stick_x) < 0.05f) {
      if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT) != 0ull) stick_x = -1.0f;
      if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT) != 0ull) stick_x = 1.0f;
    }
    if (std::abs(stick_y) < 0.05f) {
      if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN) != 0ull) stick_y = -1.0f;
      if ((buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_UP) != 0ull) stick_y = 1.0f;
    }

    const bool trigger_click = trigger >= 0.55f ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_TRIGGER) != 0ull;
    const bool grip_click = grip >= 0.55f ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_GRIP) != 0ull;
    const bool dpad_direction_pressed =
        (buttons & (xr_runtime::CONTROLLER_BUTTON_DPAD_LEFT |
                    xr_runtime::CONTROLLER_BUTTON_DPAD_RIGHT |
                    xr_runtime::CONTROLLER_BUTTON_DPAD_UP |
                    xr_runtime::CONTROLLER_BUTTON_DPAD_DOWN)) != 0ull;
    const bool thumbstick_click = (buttons & xr_runtime::CONTROLLER_BUTTON_THUMBSTICK) != 0ull ||
        (buttons & xr_runtime::CONTROLLER_BUTTON_DPAD_CENTER) != 0ull;
    // Vive Wand locomotion bindings commonly expect a trackpad click at the
    // touched direction, not only a touch/axis value. Physical D-pad directions
    // are discrete presses, so expose them as directional trackpad clicks while
    // keeping analog stick movement as touch-only.
    const bool trackpad_click = thumbstick_click || dpad_direction_pressed;
    const bool menu_click = (buttons & xr_runtime::CONTROLLER_BUTTON_MENU) != 0ull;
    const bool a_click = (buttons & xr_runtime::CONTROLLER_BUTTON_A) != 0ull;
    const bool b_click = (buttons & xr_runtime::CONTROLLER_BUTTON_B) != 0ull;
    const bool x_click = (buttons & xr_runtime::CONTROLLER_BUTTON_X) != 0ull;
    const bool y_click = (buttons & xr_runtime::CONTROLLER_BUTTON_Y) != 0ull;
    const bool system_click = cfg_.expose_system_button &&
        (buttons & xr_runtime::CONTROLLER_BUTTON_SYSTEM) != 0ull;

    if (trigger_value_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(trigger_value_component_, trigger, 0.0);
    }
    if (trigger_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(trigger_click_component_, trigger_click, 0.0);
    }
    if (grip_value_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(grip_value_component_, grip, 0.0);
    }
    if (grip_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(grip_click_component_, grip_click, 0.0);
    }
    if (thumbstick_x_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(thumbstick_x_component_, stick_x, 0.0);
    }
    if (thumbstick_y_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(thumbstick_y_component_, stick_y, 0.0);
    }
    if (thumbstick_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(thumbstick_click_component_, thumbstick_click, 0.0);
    }

    const bool trackpad_touch = std::abs(stick_x) >= 0.05f || std::abs(stick_y) >= 0.05f || trackpad_click;
    if (trackpad_x_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(trackpad_x_component_, stick_x, 0.0);
    }
    if (trackpad_y_component_ != 0) {
      vr::VRDriverInput()->UpdateScalarComponent(trackpad_y_component_, stick_y, 0.0);
    }
    if (trackpad_touch_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(trackpad_touch_component_, trackpad_touch, 0.0);
    }
    if (trackpad_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(trackpad_click_component_, trackpad_click, 0.0);
    }

    if (menu_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(menu_click_component_, menu_click, 0.0);
    }
    if (a_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(a_click_component_, a_click, 0.0);
    }
    if (b_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(b_click_component_, b_click, 0.0);
    }
    if (x_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(x_click_component_, x_click, 0.0);
    }
    if (y_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(y_click_component_, y_click, 0.0);
    }
    if (system_click_component_ != 0) {
      vr::VRDriverInput()->UpdateBooleanComponent(system_click_component_, system_click, 0.0);
    }
  }


  static uint64_t steady_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  bool hold_cached_runtime_controller_input_if_fresh() {
    if (!has_cached_runtime_controller_side_ || cfg_.runtime_controller_input_hold_ms <= 0) {
      return false;
    }

    const uint64_t now_ns = steady_now_ns();
    const uint64_t age_ns =
        now_ns >= cached_runtime_controller_input_ns_ ? now_ns - cached_runtime_controller_input_ns_ : 0ull;
    const uint64_t age_ms = age_ns / 1000000ull;
    if (age_ms > static_cast<uint64_t>(cfg_.runtime_controller_input_hold_ms)) {
      return false;
    }

    // Keep held buttons/axes alive through short runtime_controller_state gaps.
    // Without this, the no-hands fallback path clears input to neutral, so
    // SteamVR locomotion sees on/off/on/off pulses and games can queue movement.
    update_inputs_from_runtime_controller(&cached_runtime_controller_side_);

    if (object_id_ != vr::k_unTrackedDeviceIndexInvalid) {
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(object_id_, cached_pose_, sizeof(vr::DriverPose_t));
    }

    if (now_ns >= last_controller_hold_log_ns_ + 1000000000ull) {
      last_controller_hold_log_ns_ = now_ns;
      log_line(std::string("[xr_tracking_openvr] holding last runtime controller input ") +
               (left_ ? "left" : "right") +
               " age_ms=" + std::to_string(age_ms));
    }
    return true;
  }

  void maybe_log_runtime_controller_sample(
      uint64_t sequence,
      const xr_runtime::RuntimeControllerSideStateV1& side) {
    const uint64_t buttons = side.buttons;
    const bool interesting = buttons != 0ull ||
                             std::abs(side.trigger) > 0.01f ||
                             std::abs(side.grip) > 0.01f ||
                             std::abs(side.thumbstick_x) > 0.05f ||
                             std::abs(side.thumbstick_y) > 0.05f;

    if (!interesting && sequence < last_controller_debug_sequence_ + 180) {
      return;
    }
    if (sequence == last_controller_debug_sequence_ && buttons == last_controller_debug_buttons_) {
      return;
    }

    last_controller_debug_sequence_ = sequence;
    last_controller_debug_buttons_ = buttons;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "[xr_tracking_openvr] runtime controller %s seq=%llu flags=0x%x role=%u buttons=0x%llx trigger=%.2f grip=%.2f stick=(%.2f,%.2f)",
                  left_ ? "left" : "right",
                  static_cast<unsigned long long>(sequence),
                  side.flags,
                  side.role,
                  static_cast<unsigned long long>(buttons),
                  static_cast<double>(side.trigger),
                  static_cast<double>(side.grip),
                  static_cast<double>(side.thumbstick_x),
                  static_cast<double>(side.thumbstick_y));
    log_line(buf);
  }

  void maybe_log_controller_state_error(const std::string& error) {
    if (error.empty() || error == last_controller_state_error_) return;
    last_controller_state_error_ = error;
    log_line(std::string("[xr_tracking_openvr] runtime controller state unavailable ") +
             (left_ ? "left" : "right") + ": " + error);
  }

  void maybe_log_hand_error(const std::string& error) {
    if (error.empty() || error == last_hand_error_) return;
    last_hand_error_ = error;
    log_line(std::string("[xr_tracking_openvr] hand tracking unavailable ") +
             (left_ ? "left" : "right") + ": " + error);
  }

  void maybe_log_runtime_pose_error(const std::string& error) {
    if (error.empty() || error == last_runtime_pose_error_) return;
    last_runtime_pose_error_ = error;
    log_line(std::string("[xr_tracking_openvr] runtime HMD pose unavailable for hand ") +
             (left_ ? "left" : "right") + ": " + error);
  }

  bool left_ = true;
  HandControllerConfig cfg_;
  vr::TrackedDeviceIndex_t object_id_ = vr::k_unTrackedDeviceIndexInvalid;
  vr::PropertyContainerHandle_t property_container_ = vr::k_ulInvalidPropertyContainer;
  std::unique_ptr<IRuntimeControllerStateReader> controller_reader_;
  std::unique_ptr<IRuntimeHandReader> hand_reader_;
  std::unique_ptr<IRuntimePoseReader> pose_reader_;
  std::string last_controller_state_error_;
  std::string last_hand_error_;
  std::string last_runtime_pose_error_;
  vr::DriverPose_t cached_pose_{};

  vr::VRInputComponentHandle_t trigger_value_component_ = 0;
  vr::VRInputComponentHandle_t trigger_click_component_ = 0;
  vr::VRInputComponentHandle_t grip_value_component_ = 0;
  vr::VRInputComponentHandle_t grip_click_component_ = 0;
  vr::VRInputComponentHandle_t thumbstick_x_component_ = 0;
  vr::VRInputComponentHandle_t thumbstick_y_component_ = 0;
  vr::VRInputComponentHandle_t thumbstick_click_component_ = 0;
  vr::VRInputComponentHandle_t trackpad_x_component_ = 0;
  vr::VRInputComponentHandle_t trackpad_y_component_ = 0;
  vr::VRInputComponentHandle_t trackpad_touch_component_ = 0;
  vr::VRInputComponentHandle_t trackpad_click_component_ = 0;
  vr::VRInputComponentHandle_t menu_click_component_ = 0;
  vr::VRInputComponentHandle_t a_click_component_ = 0;
  vr::VRInputComponentHandle_t b_click_component_ = 0;
  vr::VRInputComponentHandle_t x_click_component_ = 0;
  vr::VRInputComponentHandle_t y_click_component_ = 0;
  vr::VRInputComponentHandle_t system_click_component_ = 0;
  vr::VRInputComponentHandle_t haptic_component_ = 0;
  bool has_cached_runtime_controller_side_ = false;
  xr_runtime::RuntimeControllerSideStateV1 cached_runtime_controller_side_{};
  uint64_t cached_runtime_controller_input_ns_ = 0;
  uint64_t last_controller_hold_log_ns_ = 0;

  uint64_t last_controller_debug_sequence_ = 0;
  uint64_t last_controller_debug_buttons_ = 0;
};

class ServerDriver final : public vr::IServerTrackedDeviceProvider {
 public:
  vr::EVRInitError Init(vr::IVRDriverContext* driver_context) override {
    vr::InitServerDriverContext(driver_context);
    log_line("[xr_tracking_openvr] server init");

    hmd_ = std::make_unique<XrTrackingHmdDriver>();
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
            hmd_->serial().c_str(), vr::TrackedDeviceClass_HMD, hmd_.get())) {
      hmd_.reset();
      return vr::VRInitError_Driver_Unknown;
    }

    load_hand_settings();
    maybe_add_hand_controllers();
    return vr::VRInitError_None;
  }

  void Cleanup() override {
    log_line("[xr_tracking_openvr] server cleanup");
    right_controller_.reset();
    left_controller_.reset();
    hmd_.reset();
    vr::CleanupDriverContext();
  }

  const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

  void RunFrame() override {
    if (hmd_) hmd_->RunFrame();
    if (left_controller_) left_controller_->RunFrame();
    if (right_controller_) right_controller_->RunFrame();
  }

  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

 private:
  void load_hand_settings() {
    hand_cfg_.mode = get_string_setting(kHandControllerModeKey, "none");
    hand_cfg_.reader.transport = get_string_setting(kRuntimeHandTransportKey, "auto");
    hand_cfg_.reader.registry_path = get_string_setting(kRuntimeHandRegistryPathKey, "");
    hand_cfg_.reader.stream_id = get_string_setting(kRuntimeHandStreamKey, "runtime_hand_tracking");
    hand_cfg_.reader.shm_name = get_string_setting(kRuntimeHandShmNameKey, "runtime_hand_tracking");
    hand_cfg_.reader.max_age_ms = static_cast<uint32_t>(std::max(0, get_int_setting(kRuntimeHandMaxAgeMsKey, 250)));
    hand_cfg_.reader.udp_bind_host = get_string_setting(kRuntimeHandUdpBindHostKey, "127.0.0.1");
    hand_cfg_.reader.udp_port = static_cast<uint16_t>(std::clamp(get_int_setting(kRuntimeHandUdpPortKey, 45801), 1, 65535));

    hand_cfg_.controller_reader.transport = get_string_setting(kRuntimeControllerStateTransportKey, "auto");
    hand_cfg_.controller_reader.registry_path = get_string_setting(kRuntimeControllerStateRegistryPathKey, "");
    hand_cfg_.controller_reader.stream_id = get_string_setting(kRuntimeControllerStateStreamKey, "runtime_controller_state");
    hand_cfg_.controller_reader.shm_name = get_string_setting(kRuntimeControllerStateShmNameKey, "runtime_controller_state");
    hand_cfg_.controller_reader.max_age_ms = static_cast<uint32_t>(std::max(0, get_int_setting(kRuntimeControllerStateMaxAgeMsKey, 1000)));
    hand_cfg_.controller_reader.udp_bind_host = get_string_setting(kRuntimeControllerStateUdpBindHostKey, "127.0.0.1");
    hand_cfg_.controller_reader.udp_port = static_cast<uint16_t>(std::clamp(get_int_setting(kRuntimeControllerStateUdpPortKey, 45802), 1, 65535));
    hand_cfg_.runtime_controller_input_hold_ms = static_cast<std::max(0, get_int_setting(kRuntimeControllerInputHoldMsKey, 3000)));

    hand_cfg_.hmd_reader.transport = get_string_setting(kRuntimePoseTransportKey, "auto");
    hand_cfg_.hmd_reader.registry_path = get_string_setting(kRuntimePoseRegistryPathKey, "");
    hand_cfg_.hmd_reader.stream_id = get_string_setting(kRuntimePoseStreamKey, "runtime_hmd_pose");
    hand_cfg_.hmd_reader.shm_name = get_string_setting(kRuntimePoseShmNameKey, "runtime_hmd_pose");
    hand_cfg_.hmd_reader.max_age_ms = static_cast<uint32_t>(std::max(0, get_int_setting(kRuntimePoseMaxAgeMsKey, 250)));
    hand_cfg_.hmd_reader.udp_bind_host = get_string_setting(kRuntimePoseUdpBindHostKey, "127.0.0.1");
    hand_cfg_.hmd_reader.udp_port = static_cast<uint16_t>(std::clamp(get_int_setting(kRuntimePoseUdpPortKey, 45800), 1, 65535));

    hand_cfg_.coordinate_mode = parse_coordinate_mode(get_string_setting(kCoordinateModeKey, "runtime_ready"));
    hand_cfg_.pose_mode = get_string_setting(kHandControllerPoseModeKey, "runtime_absolute");
    hand_cfg_.pose_y_offset = get_float_setting(kHandControllerPoseYOffsetKey, 0.0f);
    hand_cfg_.render_model = get_string_setting(kHandControllerRenderModelKey, kHiddenHandControllerRenderModel);
    hand_cfg_.expose_system_button = get_bool_setting(kHandControllerExposeSystemButtonKey, false);
    hand_cfg_.left_serial = get_string_setting(kLeftHandSerialKey, "xr-tracking-left-hand-001");
    hand_cfg_.right_serial = get_string_setting(kRightHandSerialKey, "xr-tracking-right-hand-001");

    log_line(std::string("[xr_tracking_openvr] hand coordinateMode=") +
             coordinate_mode_name(hand_cfg_.coordinate_mode) +
             " handControllerPoseMode=" + hand_cfg_.pose_mode);
  }

  void maybe_add_hand_controllers() {
    std::string mode = hand_cfg_.mode;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    if (mode.empty() || mode == "none" || mode == "off" || mode == "disabled") {
      log_line("[xr_tracking_openvr] hand controllers disabled");
      return;
    }

    if (mode == "skeleton") {
      log_line("[xr_tracking_openvr] handControllerMode=skeleton requested; skeletal input is TODO, registering controller sticks for now");
    } else if (mode != "sticks" && mode != "controllers") {
      log_line("[xr_tracking_openvr] unknown handControllerMode=" + hand_cfg_.mode + "; hand controllers disabled");
      return;
    }

    left_controller_ = std::make_unique<XrHandControllerDriver>(true, hand_cfg_);
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
            left_controller_->serial().c_str(), vr::TrackedDeviceClass_Controller, left_controller_.get())) {
      log_line("[xr_tracking_openvr] failed to add left hand controller");
      left_controller_.reset();
    }

    right_controller_ = std::make_unique<XrHandControllerDriver>(false, hand_cfg_);
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
            right_controller_->serial().c_str(), vr::TrackedDeviceClass_Controller, right_controller_.get())) {
      log_line("[xr_tracking_openvr] failed to add right hand controller");
      right_controller_.reset();
    }
  }

  std::unique_ptr<XrTrackingHmdDriver> hmd_;
  HandControllerConfig hand_cfg_;
  std::unique_ptr<XrHandControllerDriver> left_controller_;
  std::unique_ptr<XrHandControllerDriver> right_controller_;
};

ServerDriver g_server_driver;

}  // namespace
}  // namespace xr::openvr_driver

XR_DRIVER_EXPORT void* HmdDriverFactory(const char* interface_name, int* return_code) {
  if (std::strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0) {
    return &xr::openvr_driver::g_server_driver;
  }

  if (return_code != nullptr) {
    *return_code = vr::VRInitError_Init_InterfaceNotFound;
  }
  return nullptr;
}
