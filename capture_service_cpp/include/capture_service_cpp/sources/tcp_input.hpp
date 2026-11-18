#pragma once

#include "capture_service_cpp/common.hpp"

#include <functional>
#include <memory>
#include <mutex>

namespace xr_capture_cpp {

class TcpInputClock;

// The primary connection owns clock synchronization. The matching IMU
// connection projects its timestamps through the same clock.
class TcpCaptureSession {
 public:
  explicit TcpCaptureSession(std::function<void(bool)> on_imu_capability = {});
  std::shared_ptr<TcpInputClock> clock_for(const std::string& id, bool imu) const;
  bool imu_matches(const std::shared_ptr<TcpInputClock>& clock) const;
  void set_primary(const std::string& id, bool has_imu, const std::shared_ptr<TcpInputClock>& clock);

 private:
  mutable std::mutex mutex_;
  std::function<void(bool)> on_imu_capability_;
  std::string id_;
  bool has_imu_ = false;
  std::shared_ptr<TcpInputClock> clock_;
};

enum class TcpInputRole { Primary, External, Imu };

struct TcpInputView {
  std::string id;
  int width;
  int height;
  int channels = 1;
};

struct TcpInputSample {
  uint64_t sequence = 0;
  uint64_t timestamp_ns = 0;
  std::array<float, 6> imu{};
};

class TcpSensorInput {
 public:
  TcpSensorInput(TcpInputConfig cfg, TcpInputRole role, int stall_ms, std::vector<TcpInputView> views,
                 std::shared_ptr<TcpCaptureSession> session = {});
  ~TcpSensorInput();
  void open();
  void close();
  bool read(TcpInputSample& sample);
  // Borrowed pixels from the most recent successful read, in configured view
  // order. The next read may reuse the storage.
  uint8_t* pixels(size_t view_index);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xr_capture_cpp
