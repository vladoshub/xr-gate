#pragma once

#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/stream_publishers.hpp"

namespace xr_capture_cpp {

void imu_thread(const RuntimeConfig& cfg, StreamPublishers* publishers);

}  // namespace xr_capture_cpp
