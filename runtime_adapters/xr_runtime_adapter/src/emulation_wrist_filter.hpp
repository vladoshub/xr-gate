#pragma once

#include "emulation_controller.hpp"

namespace xr_runtime_adapter::emulation {
// Apply only to output copies, after recognition, synthesis and all other offsets/filters.
// Runtime is +Y up, -Z forward: positive yaw turns the left hand outward, negative the right.
void apply_pose_yaw_offset(xr_runtime::HandTrackingFrameF32V2& frame, const Config& config,
                           bool left_optical = true, bool right_optical = true);
void apply_pose_yaw_offset(xr_runtime::HandTrackingFrameF64V1& frame, const Config& config,
                           bool left_optical = true, bool right_optical = true);
void apply_pose_yaw_offset(xr_runtime::RuntimeControllerStateFrameV1& frame, const Config& config);

// One shared rigid correction for the whole hand; finger articulation is untouched.
class WristPoseFilter {
 public:
  void apply(xr_runtime::HandTrackingFrameF32V2& frame, const Config& config, uint64_t now_ns,
             uint64_t origin_reset_counter, bool filter_left = true, bool filter_right = true);

 private:
  struct State {
    bool active = false;
    uint64_t stamp = 0, sequence = 0, reset = 0, origin = 0;
    std::array<double, 4> orientation{{1, 0, 0, 0}};
    Vec3 angular_velocity{};
  };
  State left_, right_;
  void side(xr_runtime::HandSideF32V2& hand, State& state,
            const xr_runtime::HandTrackingFrameF32V2& frame, const Config& config, uint64_t now,
            uint64_t origin, bool enabled);
};
}  // namespace xr_runtime_adapter::emulation
