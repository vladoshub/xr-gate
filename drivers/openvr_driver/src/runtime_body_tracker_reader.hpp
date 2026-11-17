#pragma once

#include <memory>
#include <string>

#include <xr_tracking/contracts/body_tracker_set_wire.hpp>

#include "runtime_tcp_config.hpp"

namespace xr::openvr_driver {

struct RuntimeBodyTrackerReaderConfig {
  std::string shm_name = "runtime_body_trackers";
  RuntimeTcpConfig tcp;
};

struct RuntimeBodyTrackerSample {
  xr_tracking::BodyTrackerSetFrameF32V1 frame{};
  int64_t age_ns = 0;
};

class IRuntimeBodyTrackerReader {
 public:
  virtual ~IRuntimeBodyTrackerReader() = default;
  virtual bool read_latest(RuntimeBodyTrackerSample& out, std::string* error) = 0;
};

std::unique_ptr<IRuntimeBodyTrackerReader> create_runtime_body_tracker_reader(
    const RuntimeBodyTrackerReaderConfig& cfg);
std::unique_ptr<IRuntimeBodyTrackerReader> create_tcp_runtime_body_tracker_reader(
    const RuntimeTcpConfig& cfg);

}  // namespace xr::openvr_driver
