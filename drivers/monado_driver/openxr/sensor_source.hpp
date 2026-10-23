#pragma once
#include "xr_monado_driver/runtime_readers.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>
#include <xr_video/contracts/stereo_video_contract.hpp>
namespace xr::monado_driver {
bool option(const char *name, bool fallback = false);
std::string setting(const char *name, const char *fallback);
uint32_t number(const char *name, uint32_t fallback, uint32_t min = 1,
                uint32_t max = 60000);
struct SensorState {
  std::shared_ptr<const xr_spatial::RuntimeSpatialProxyMeshF32V1> mesh;
  std::shared_ptr<const xr_video::StereoVideoFrame> video;
  uint64_t tracking_epoch = 0;
  bool tracking_valid = false;
  bool mesh_world_valid = false;
};
// One optional worker per process, never any I/O/decoding on xrWaitFrame or
// pose callbacks. The HMD validity transition is recorded even if no app polls
// during an outage.
class SensorSource {
public:
  SensorSource();
  ~SensorSource();
  SensorState latest();
  bool spatial_enabled() const { return spatial_; }
  bool video_enabled() const { return video_; }

private:
  void run();
  void run_video_tcp();
  bool spatial_, video_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  SensorState state_;
  int64_t mesh_local_ns_ = 0, video_local_ns_ = 0;
  uint64_t mesh_epoch_ = 0;
  std::thread worker_, video_worker_;
};
} // namespace xr::monado_driver
