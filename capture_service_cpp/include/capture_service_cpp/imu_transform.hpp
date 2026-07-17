#pragma once

#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/sources/imu_source.hpp"

#include <array>
#include <string>

namespace xr_capture_cpp {

std::array<float, 3> rotate_imu_vector(const ImuTransformConfig& transform,
                                       const std::array<float, 3>& value);
void apply_imu_transform(const ImuTransformConfig& transform, ImuSample& sample);
std::string imu_transform_description(const ImuTransformConfig& transform);

}  // namespace xr_capture_cpp
