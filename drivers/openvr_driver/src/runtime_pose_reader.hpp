#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <xr_runtime/contracts/runtime_pose_stream.hpp>

namespace xr::openvr_driver {

struct RuntimePoseReaderConfig {
  // Transport selector is intentionally platform-neutral. Current Stage 8B
  // implements Linux/POSIX SHM. Windows can later add named shared memory,
  // UDP, or named-pipe readers without changing the OpenVR driver core.
  std::string transport = "auto";
  std::string registry_path;
  std::string stream_id = "runtime_hmd_pose";
  std::string shm_name = "runtime_hmd_pose";
  uint32_t max_age_ms = 250;
  std::string udp_bind_host = "127.0.0.1";
  uint16_t udp_port = 45800;
};

struct RuntimePoseSample {
  xr_runtime::RuntimeHmdPoseF64V1 pose{};
  int64_t read_time_ns = 0;
  int64_t age_ns = 0;
};

class IRuntimePoseReader {
 public:
  virtual ~IRuntimePoseReader() = default;
  virtual bool read_latest(RuntimePoseSample& out, std::string* error) = 0;
  virtual const char* transport_name() const = 0;
};

int64_t monotonic_now_ns();
bool runtime_pose_is_valid_for_openvr(const xr_runtime::RuntimeHmdPoseF64V1& pose);
bool runtime_pose_is_fresh(const RuntimePoseSample& sample, uint32_t max_age_ms);

std::unique_ptr<IRuntimePoseReader> create_runtime_pose_reader(const RuntimePoseReaderConfig& cfg);

}  // namespace xr::openvr_driver
