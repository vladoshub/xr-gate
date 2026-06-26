#include <xr_spatial_backend/pose_reader.hpp>

#include <xr_tracking/publishers/hmd_pose_shm_publisher.hpp>

#include <cmath>
#include <stdexcept>

namespace xr_spatial_backend {

HmdPoseStreamReader::HmdPoseStreamReader(std::string registry_path, std::string stream_id)
    : registry_path_(std::move(registry_path)),
      stream_id_(std::move(stream_id)),
      reader_(xr_runtime::stream_info_from_registry(registry_path_, stream_id_),
              xr_runtime::HMD_POSE_FORMAT_NAME) {}

std::optional<xr_runtime::HmdPoseF64V1> HmdPoseStreamReader::latest() const {
  return reader_.latest();
}

Transform3d HmdPoseStreamReader::latest_transform_or_throw(int64_t frame_timestamp_ns,
                                                           double max_pose_age_ms,
                                                           bool* stale_pose_out,
                                                           double* pose_age_ms_out,
                                                           uint64_t* pose_timestamp_ns_out) const {
  auto pose = latest();
  if (!pose) throw std::runtime_error("no HMD pose sample available from pose stream");
  if ((pose->flags & xr_runtime::HMD_FLAG_POSE_VALID) == 0u) {
    throw std::runtime_error("latest HMD pose is not valid");
  }
  const double age_ms = std::abs(double(frame_timestamp_ns - static_cast<int64_t>(pose->timestamp_ns))) / 1e6;
  if (stale_pose_out) *stale_pose_out = age_ms > max_pose_age_ms;
  if (pose_age_ms_out) *pose_age_ms_out = age_ms;
  if (pose_timestamp_ns_out) *pose_timestamp_ns_out = pose->timestamp_ns;
  if (max_pose_age_ms > 0.0 && age_ms > max_pose_age_ms) {
    // Do not throw here. Spatial mapping is non-critical; caller can drop if desired.
  }
  return pose_to_transform(pose->px, pose->py, pose->pz,
                           pose->qw, pose->qx, pose->qy, pose->qz);
}

}  // namespace xr_spatial_backend
