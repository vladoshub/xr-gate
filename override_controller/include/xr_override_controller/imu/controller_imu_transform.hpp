#pragma once

#include <xr_override_controller/types.hpp>

namespace xr_override_controller::imu {

xr_runtime::ControllerImuStateV1 apply_orientation_transform(
    xr_runtime::ControllerImuStateV1 state,
    const OrientationTransformConfig& config);

xr_runtime::ControllerImuStateV1 apply_orientation_offset(
    xr_runtime::ControllerImuStateV1 state,
    const OrientationOffsetConfig& config);

xr_runtime::ControllerImuStateV1 apply_orientation_calibration(
    xr_runtime::ControllerImuStateV1 state,
    const OrientationTransformConfig& transform,
    const OrientationOffsetConfig& offset);

}  // namespace xr_override_controller::imu
