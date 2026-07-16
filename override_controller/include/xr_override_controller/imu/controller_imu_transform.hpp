#pragma once

#include <xr_override_controller/types.hpp>

namespace xr_override_controller::imu {

xr_runtime::ControllerImuStateV1 apply_orientation_transform(
    xr_runtime::ControllerImuStateV1 state,
    const OrientationTransformConfig& config);

}  // namespace xr_override_controller::imu
