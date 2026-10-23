#pragma once

#include <cstdint>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>
#include <memory>
#include <optional>
#include <string>

#include <xr_runtime/contracts/runtime_controller_state_contract.hpp>
#include <xr_runtime/contracts/runtime_pose_stream.hpp>
#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>

namespace xr_runtime_adapter {

struct AppTcpServerConfig {
  std::string host = "127.0.0.1";
  int port = 45810;
  double rate_hz = 60.0;
  int max_clients = 8;
  std::string runtime_frame = "runtime_local";
  bool spatial_mesh = false; // Optional v1 spatial_mesh messages; off for older clients.
};

// A copy of the outputs from one runtime tick, after transforms and prediction.
// No SHM output needs to be enabled to use the application server.
struct AppTcpFrame {
  std::shared_ptr<const xr_spatial::RuntimeSpatialProxyMeshF32V1> spatial;
  int64_t read_timestamp_ns = 0;
  std::optional<xr_runtime::RuntimeHmdPoseF64V1> hmd;
  std::optional<xr_tracking::HandTrackingFrameF32V2> hands;
  std::optional<xr_runtime::RuntimeControllerStateFrameV1> controllers;
};

// Construct only when explicitly enabled. All socket operations and JSON
// serialization run on a worker; publish() never waits for a socket or queue.
class AppTcpServer {
 public:
  explicit AppTcpServer(AppTcpServerConfig config);
  ~AppTcpServer();
  AppTcpServer(const AppTcpServer&) = delete;
  AppTcpServer& operator=(const AppTcpServer&) = delete;

  bool has_clients() const;
  void publish(const AppTcpFrame& frame);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xr_runtime_adapter
