#pragma once

#include <optional>
#include <string>
#include <utility>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_tracking/types/tracking_types.hpp>
#include <xr_spatial_backend/pose_math.hpp>

namespace xr_spatial_backend {

class HmdPoseStreamReader {
 public:
  HmdPoseStreamReader(std::string registry_path, std::string stream_id);

  std::optional<xr_runtime::HmdPoseF64V1> latest() const;
  Transform3d latest_transform_or_throw(int64_t frame_timestamp_ns,
                                        double max_pose_age_ms,
                                        bool* stale_pose_out,
                                        double* pose_age_ms_out,
                                        uint64_t* pose_timestamp_ns_out) const;

 private:
  std::string registry_path_;
  std::string stream_id_;
  xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1> reader_;
};

}  // namespace xr_spatial_backend
