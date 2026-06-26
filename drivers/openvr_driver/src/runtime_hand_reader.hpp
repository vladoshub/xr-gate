#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "runtime_hand_contract.hpp"

namespace xr::openvr_driver {

struct RuntimeHandReaderConfig {
  // Transport selector is platform-neutral. Current implementation supports
  // Linux/POSIX SHM. Windows is intentionally a stub until the runtime adapter
  // publishes HAND_TRACKING_21_JOINT_F32_V2 through a Windows-friendly transport.
  std::string transport = "auto";
  std::string registry_path;
  std::string stream_id = "hand_tracking";
  std::string shm_name = "hand_tracking";
  uint32_t max_age_ms = 250;
  std::string udp_bind_host = "127.0.0.1";
  uint16_t udp_port = 45801;
};

struct RuntimeHandSample {
  xr_tracking::HandTrackingFrameF32V2 frame{};
  int64_t read_time_ns = 0;
  int64_t age_ns = 0;
};

class IRuntimeHandReader {
 public:
  virtual ~IRuntimeHandReader() = default;
  virtual bool read_latest(RuntimeHandSample& out, std::string* error) = 0;
  virtual const char* transport_name() const = 0;
};

bool runtime_hand_frame_is_valid(const xr_tracking::HandTrackingFrameF32V2& frame);
bool runtime_hand_sample_is_fresh(const RuntimeHandSample& sample, uint32_t max_age_ms);
bool runtime_hand_side_is_valid(const xr_tracking::HandTrackingFrameF32V2& frame,
                                const xr_tracking::HandSideF32V2& side,
                                bool left);

std::unique_ptr<IRuntimeHandReader> create_runtime_hand_reader(const RuntimeHandReaderConfig& cfg);

}  // namespace xr::openvr_driver
