#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>

namespace xr::openvr_driver {

struct RuntimeControllerStateReaderConfig {
  std::string transport = "auto";
  std::string registry_path;
  std::string stream_id = "runtime_controller_state";
  std::string shm_name = "runtime_controller_state";
  uint32_t max_age_ms = 250;
  std::string udp_bind_host = "127.0.0.1";
  uint16_t udp_port = 45802;
};

struct RuntimeControllerStateSample {
  xr_runtime::RuntimeControllerStateFrameV1 frame{};
  int64_t read_time_ns = 0;
  int64_t age_ns = 0;
};

class IRuntimeControllerStateReader {
 public:
  virtual ~IRuntimeControllerStateReader() = default;
  virtual bool read_latest(RuntimeControllerStateSample& out, std::string* error) = 0;
  virtual const char* transport_name() const = 0;
};

bool runtime_controller_state_frame_is_valid(const xr_runtime::RuntimeControllerStateFrameV1& frame);
bool runtime_controller_state_sample_is_fresh(const RuntimeControllerStateSample& sample,
                                              uint32_t max_age_ms);
bool runtime_controller_side_has_pose(const xr_runtime::RuntimeControllerSideStateV1& side,
                                      bool left);

std::unique_ptr<IRuntimeControllerStateReader> create_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& cfg);

}  // namespace xr::openvr_driver
