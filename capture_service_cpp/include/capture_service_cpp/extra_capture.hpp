#pragma once

#include "capture_service_cpp/config/capture_config.hpp"
#include "capture_service_cpp/platform/process.hpp"
#include "capture_service_cpp/stream_publishers.hpp"

namespace xr_capture_cpp {

constexpr const char* kExtraCaptureWorkerArg = "--extra-capture-worker";

void start_extra_capture(ExtraCaptureProcess& process, const RuntimeConfig& primary,
                         const StreamPublishers& publishers, const StreamPublishers* additional = nullptr);
void check_extra_capture_resources(const ExtraCaptureConfig& extra,
                                   const std::vector<std::string>& protected_files,
                                   const std::vector<std::string>& protected_shm,
                                   int reserved_tcp_port);
int run_extra_capture(int argc, char** argv);

}  // namespace xr_capture_cpp
