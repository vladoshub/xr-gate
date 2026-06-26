#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <xr_runtime/contracts/runtime_pose_stream.hpp>
#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>
#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>

namespace xr::monado_driver {

struct RuntimePoseReaderConfig {
  std::string transport = "auto";
  std::string registry_path;
  std::string stream_id = "runtime_hmd_pose";
  std::string shm_name = "runtime_hmd_pose";
  uint32_t max_age_ms = 250;
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

struct RuntimeControllerStateReaderConfig {
  std::string transport = "auto";
  std::string registry_path;
  std::string stream_id = "runtime_controller_state";
  std::string shm_name = "runtime_controller_state";
  uint32_t max_age_ms = 250;
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

struct RuntimeHandReaderConfig {
  std::string transport = "auto";
  std::string registry_path;
  std::string stream_id = "runtime_hand_tracking";
  std::string shm_name = "runtime_hand_tracking";
  uint32_t max_age_ms = 250;
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

int64_t monotonic_now_ns();

bool runtime_pose_is_valid(const xr_runtime::RuntimeHmdPoseF64V1& pose);
bool runtime_pose_sample_is_fresh(const RuntimePoseSample& sample, uint32_t max_age_ms);

bool runtime_controller_state_frame_is_valid(const xr_runtime::RuntimeControllerStateFrameV1& frame);
bool runtime_controller_state_sample_is_fresh(const RuntimeControllerStateSample& sample, uint32_t max_age_ms);
bool runtime_controller_side_has_pose(const xr_runtime::RuntimeControllerSideStateV1& side, bool left);

bool runtime_hand_frame_is_valid(const xr_tracking::HandTrackingFrameF32V2& frame);
bool runtime_hand_sample_is_fresh(const RuntimeHandSample& sample, uint32_t max_age_ms);
bool runtime_hand_side_is_valid(const xr_tracking::HandTrackingFrameF32V2& frame,
                                const xr_tracking::HandSideF32V2& side,
                                bool left);

std::unique_ptr<IRuntimePoseReader> create_runtime_pose_reader(const RuntimePoseReaderConfig& cfg);
std::unique_ptr<IRuntimeControllerStateReader> create_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& cfg);
std::unique_ptr<IRuntimeHandReader> create_runtime_hand_reader(const RuntimeHandReaderConfig& cfg);

}  // namespace xr::monado_driver
