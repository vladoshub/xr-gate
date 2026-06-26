#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_tracking/contracts/body_tracker_set_contract.hpp>

namespace xr_runtime_adapter::coordinate_util {

struct Qd {
  double w = 1.0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct V3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

bool finite_q(const Qd& q);
bool finite_v3(const V3d& v);
Qd normalize_q(Qd q);
Qd q_conj(const Qd& q);
Qd q_mul_raw(const Qd& a, const Qd& b);
Qd q_mul(const Qd& a, const Qd& b);
V3d q_rotate(const Qd& q_raw, const V3d& v);
double yaw_from_q_z_up(const Qd& q_raw);
Qd q_from_yaw_z_up(double yaw);
Qd q_from_axis_angle(double ax, double ay, double az, double radians);
Qd q_from_euler_basis_xyz(const V3d& radians);
Qd apply_basis_quat_transform(const Qd& basis_q, const Qd& in);

struct CoordinateTransformConfig {
  bool enabled = true;
  std::array<int, 3> axis_map{{0, 1, 2}};
  std::array<double, 3> sign{{1.0, 1.0, 1.0}};
  V3d rotation_deg{};
  V3d rotation_rad{};
  double scale = 1.0;
  V3d offset_m{};
};

struct OrientationTransformConfig {
  bool enabled = false;
  V3d basis_rotation_deg{};
  V3d basis_rotation_rad{};
  Qd basis_q{};
};

struct HmdRelativeConfig {
  bool enabled = false;
  V3d offset_m{};
  bool rotate_with_hmd_orientation = false;
};

struct HandSideOrientationOffsetConfig {
  bool enabled = false;
  V3d rotation_deg{};
  V3d rotation_rad{};
  Qd q{};
};

struct HandOrientationOffsetConfig {
  bool enabled = false;
  HandSideOrientationOffsetConfig left{};
  HandSideOrientationOffsetConfig right{};
  bool apply_to_controller = true;
  bool apply_to_palm = true;
  bool apply_to_wrist = true;
  bool apply_to_joints = false;
  bool pre_multiply = false;
};

struct SpatialMeshRuntimeConfig {
  struct CameraRelativeRuntimeConfig {
    bool enabled = false;
    bool require_hmd = true;
    bool apply_hmd_position = true;
    bool apply_hmd_orientation = true;
    double max_hmd_age_ms = 250.0;
    V3d offset_m{};
  };

  // Mesh-only policy for streams.spatial_proxy_mesh.mesh_runtime in the
  // tracking transform JSON.  Position transform still uses the regular
  // coordinate_transform block above.
  std::string triangle_winding = "auto"; // auto, keep, swap
  V3d extra_rotation_deg{};
  V3d extra_offset_m{};

  // Runtime binding policy for world/tracking-frame spatial meshes.  These
  // fields are intentionally mesh-only: HMD pose recenter/origin stays owned by
  // xr_runtime_adapter, while spatial_proxy_mesh decides whether to apply the
  // same origin to room geometry.
  bool apply_runtime_origin_position = true;
  bool apply_runtime_origin_orientation = true;

  // Camera-relative mode for pose_input=none xr_spatial output.  In this
  // mode source vertices are treated as camera/HMD-local vectors and attached to
  // the latest runtime HMD pose instead of being interpreted as tracking_world.
  CameraRelativeRuntimeConfig camera_relative_runtime{};
};

struct StreamTransformConfig {
  CoordinateTransformConfig coordinate_transform{};
  OrientationTransformConfig orientation_transform{};
  HmdRelativeConfig hmd_relative{};
  HandOrientationOffsetConfig hand_orientation_offset{};
  SpatialMeshRuntimeConfig spatial_mesh{};
};

struct TrackingTransformConfig {
  bool enabled = false;
  std::string path;
  StreamTransformConfig hmd{};
  StreamTransformConfig hmd_3dof{};
  StreamTransformConfig hand_tracking_21_joint{};
  StreamTransformConfig hand_skeleton26{};
  StreamTransformConfig body_trackers{};
  StreamTransformConfig spatial_proxy_mesh{};
};

TrackingTransformConfig load_tracking_transform_config(const std::string& path);

V3d apply_stream_position_transform(const StreamTransformConfig& cfg,
                                    const V3d& in,
                                    const V3d* hmd_position = nullptr,
                                    const Qd* hmd_orientation = nullptr);
V3d apply_stream_vector_transform(const StreamTransformConfig& cfg, const V3d& in);
Qd apply_stream_orientation_transform(const StreamTransformConfig& cfg,
                                      const Qd& in,
                                      const Qd* hmd_orientation = nullptr);

void apply_hmd_pose_transform(xr_runtime::HmdPoseF64V1& hmd,
                              const StreamTransformConfig& cfg);
void apply_hand_frame_transform(xr_runtime::HandTrackingFrameF32V2& hand,
                                const StreamTransformConfig& cfg,
                                const V3d* hmd_position,
                                const Qd* hmd_orientation);
void apply_hand_frame_transform(xr_runtime::HandTrackingFrameF64V1& hand,
                                const StreamTransformConfig& cfg,
                                const V3d* hmd_position,
                                const Qd* hmd_orientation);
void apply_body_tracker_frame_transform(xr_tracking::BodyTrackerSetFrameF32V1& frame,
                                        const StreamTransformConfig& cfg,
                                        const V3d* hmd_position = nullptr);

std::string axis_map_string(const CoordinateTransformConfig& cfg);
void log_stream_transform(const char* name, const StreamTransformConfig& cfg, bool include_mesh_runtime = false);

enum class RuntimeOriginMode {
  NONE,
  START_POSE,
  YAW_ONLY,
};

RuntimeOriginMode parse_runtime_origin_mode(const std::string& s);

struct RuntimeOriginSnapshot {
  bool enabled = false;
  bool ready = false;
  V3d origin_p{};
  Qd origin_q{};
  uint64_t origin_reset_counter = 0;
  std::string output_frame = "runtime_local";
};

class RuntimeOriginState {
 public:
  RuntimeOriginState(std::string mode_name,
                     std::string output_frame,
                     bool recenter_on_reset_counter);

  bool enabled() const;
  bool ready() const;
  const std::string& mode_name() const;
  const std::string& output_frame() const;
  uint64_t origin_reset_counter() const;
  RuntimeOriginSnapshot snapshot() const;
  bool apply(xr_runtime::HmdPoseF64V1& hmd);

 private:
  void capture_origin(const V3d& p_raw, const Qd& q_raw, uint64_t reset_counter);

  std::string mode_name_;
  std::string output_frame_;
  RuntimeOriginMode mode_ = RuntimeOriginMode::NONE;
  bool recenter_on_reset_counter_ = true;
  bool ready_ = false;
  uint64_t origin_reset_counter_ = 0;
  V3d origin_p_;
  Qd origin_q_;
};

void apply_body_tracker_origin_transform(xr_tracking::BodyTrackerSetFrameF32V1& frame,
                                         const RuntimeOriginSnapshot& origin);

}  // namespace xr_runtime_adapter::coordinate_util
