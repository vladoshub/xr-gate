#pragma once

#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/sources/camera_source.hpp"
#include "capture_service_cpp/stream_publishers.hpp"

#include <atomic>
#include <memory>

namespace xr_capture_cpp {

void camera_thread(const RuntimeConfig& cfg, std::unique_ptr<ICameraSource> source, StreamPublishers* publishers,
                   CameraViewSink* view_sink, std::atomic<int>& view_capture_exit);

}  // namespace xr_capture_cpp
