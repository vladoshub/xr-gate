#pragma once

#include <memory>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>
#include <optional>
#include <string>

#include "runtime_pose_reader.hpp"
#include "runtime_hand_reader.hpp"
#include "runtime_controller_state_reader.hpp"

namespace xr::openvr_driver {

struct RuntimeTcpSnapshot {
  std::shared_ptr<const xr_spatial::RuntimeSpatialProxyMeshF32V1> spatial;
  int64_t spatial_received_ns = 0;
  int64_t spatial_age_ns = 0;
  uint64_t sequence = 0;
  uint64_t connection_epoch = 0;
  int64_t received_ns = 0;
  int64_t pose_age_ns = 0;
  int64_t hand_age_ns = 0;
  int64_t controller_age_ns = 0;
  std::optional<xr_runtime::RuntimeHmdPoseF64V1> pose;
  std::optional<xr_tracking::HandTrackingFrameF32V2> hands;
  std::optional<xr_runtime::RuntimeControllerStateFrameV1> controllers;
};

// All readers for one endpoint share one connection and worker. Reading only
// copies the latest fixed-size snapshot; it never reads or parses a socket.
class RuntimeTcpClient {
 public:
  static std::shared_ptr<RuntimeTcpClient> acquire(const RuntimeTcpConfig& config);
  ~RuntimeTcpClient();
  bool read_latest(RuntimeTcpSnapshot& out, std::string* error);
  RuntimeTcpClient(const RuntimeTcpClient&) = delete;
  RuntimeTcpClient& operator=(const RuntimeTcpClient&) = delete;

 private:
  explicit RuntimeTcpClient(RuntimeTcpConfig config);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<IRuntimePoseReader> create_tcp_runtime_pose_reader(const RuntimePoseReaderConfig& config);
std::unique_ptr<IRuntimeHandReader> create_tcp_runtime_hand_reader(const RuntimeHandReaderConfig& config);
std::unique_ptr<IRuntimeControllerStateReader> create_tcp_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& config);

}  // namespace xr::openvr_driver
