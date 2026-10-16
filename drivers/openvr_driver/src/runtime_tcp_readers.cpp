#include "runtime_tcp_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace xr::openvr_driver {
namespace {
int64_t local_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t age(int64_t received, int64_t initial, int64_t now) {
  const int64_t local_age = std::max(int64_t{0}, now - received);
  return local_age > INT64_MAX - initial ? INT64_MAX : initial + local_age;
}

class TcpPoseReader final : public IRuntimePoseReader {
 public:
  explicit TcpPoseReader(const RuntimeTcpConfig& config) : client_(RuntimeTcpClient::acquire(config)) {}
  bool read_latest(RuntimePoseSample& out, std::string* error) override {
    RuntimeTcpSnapshot snapshot;
    out = {};
    if (!client_->read_latest(snapshot, error)) return false;
    if (!snapshot.pose) { if (error) *error = "TCP HMD stream unavailable"; return false; }
    out.pose = *snapshot.pose; out.read_time_ns = local_now_ns();
    out.age_ns = age(snapshot.received_ns, snapshot.pose_age_ns, out.read_time_ns);
    return true;
  }
  const char* transport_name() const override { return "app_tcp"; }
 private:
  std::shared_ptr<RuntimeTcpClient> client_;
};

class TcpHandReader final : public IRuntimeHandReader {
 public:
  explicit TcpHandReader(const RuntimeTcpConfig& config) : client_(RuntimeTcpClient::acquire(config)) {}
  bool read_latest(RuntimeHandSample& out, std::string* error) override {
    RuntimeTcpSnapshot snapshot;
    out = {};
    if (!client_->read_latest(snapshot, error)) return false;
    if (!snapshot.hands) { if (error) *error = "TCP hand stream unavailable"; return false; }
    out.frame = *snapshot.hands; out.read_time_ns = local_now_ns();
    out.age_ns = age(snapshot.received_ns, snapshot.hand_age_ns, out.read_time_ns);
    return true;
  }
  const char* transport_name() const override { return "app_tcp"; }
 private:
  std::shared_ptr<RuntimeTcpClient> client_;
};

class TcpControllerReader final : public IRuntimeControllerStateReader {
 public:
  explicit TcpControllerReader(const RuntimeControllerStateReaderConfig& config)
      : client_(RuntimeTcpClient::acquire(config.tcp)), max_age_ms_(config.max_age_ms) {}
  bool read_latest(RuntimeControllerStateSample& out, std::string* error) override {
    RuntimeTcpSnapshot snapshot;
    out = {};
    if (!client_->read_latest(snapshot, error)) { reset_buttons(); return false; }
    if (!snapshot.controllers) { reset_buttons(); if (error) *error = "TCP controller stream unavailable"; return false; }
    out.frame = *snapshot.controllers; out.read_time_ns = local_now_ns();
    out.age_ns = age(snapshot.received_ns, snapshot.controller_age_ns, out.read_time_ns);
    const int64_t max_age_ns = int64_t(max_age_ms_) * 1000000;
    const bool read_gap = max_age_ms_ && last_read_ns_ && out.read_time_ns - last_read_ns_ > max_age_ns;
    last_read_ns_ = out.read_time_ns;
    if (epoch_ != snapshot.connection_epoch || read_gap || (max_age_ms_ && out.age_ns > max_age_ns)) {
      reset_buttons(); epoch_ = snapshot.connection_epoch;
    }
    if ((!max_age_ms_ || out.age_ns <= int64_t(max_age_ms_) * 1000000) && sequence_ != snapshot.sequence) {
      recover_short_taps(out.frame.left, left_);
      recover_short_taps(out.frame.right, right_);
      sequence_ = snapshot.sequence;
    }
    return true;
  }
  const char* transport_name() const override { return "app_tcp"; }
 private:
  struct ButtonBaseline {
    bool valid = false;
    uint64_t device = 0;
    uint32_t source = 0;
    std::array<uint32_t, 32> press{}, release{};
  };
  void reset_buttons() { left_ = {}; right_ = {}; sequence_ = 0; }
  static void recover_short_taps(xr_runtime::RuntimeControllerSideStateV1& side, ButtonBaseline& previous) {
    if ((side.flags & xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID) == 0) { previous.valid = false; return; }
    const bool comparable = previous.valid && previous.device == side.source_device_hash && previous.source == side.source_mask;
    for (size_t i = 0; i < 32; ++i) {
      const uint64_t bit = uint64_t{1} << i;
      const uint32_t pressed = uint32_t(side.press_counters[i] - previous.press[i]);
      const uint32_t released = uint32_t(side.release_counters[i] - previous.release[i]);
      // Unsigned subtraction handles wrap. Establish a baseline on first data,
      // reconnect and source change; historical counters are never replayed.
      if (comparable && (side.buttons & bit) == 0 &&
          pressed != 0 && pressed < 0x80000000u && released != 0 && released < 0x80000000u) {
        side.buttons |= bit;  // One reader tick; next read copies the actual released state.
        side.changed_buttons |= bit;
        if (bit == xr_runtime::CONTROLLER_BUTTON_TRIGGER) side.trigger = 1.0f;
        if (bit == xr_runtime::CONTROLLER_BUTTON_GRIP) side.grip = 1.0f;
      }
      previous.press[i] = side.press_counters[i]; previous.release[i] = side.release_counters[i];
    }
    previous.valid = true; previous.device = side.source_device_hash; previous.source = side.source_mask;
  }
  std::shared_ptr<RuntimeTcpClient> client_;
  uint32_t max_age_ms_;
  uint64_t epoch_ = 0, sequence_ = 0;
  int64_t last_read_ns_ = 0;
  ButtonBaseline left_, right_;
};
}  // namespace

std::unique_ptr<IRuntimePoseReader> create_tcp_runtime_pose_reader(const RuntimePoseReaderConfig& config) {
  return std::make_unique<TcpPoseReader>(config.tcp);
}
std::unique_ptr<IRuntimeHandReader> create_tcp_runtime_hand_reader(const RuntimeHandReaderConfig& config) {
  return std::make_unique<TcpHandReader>(config.tcp);
}
std::unique_ptr<IRuntimeControllerStateReader> create_tcp_runtime_controller_state_reader(
    const RuntimeControllerStateReaderConfig& config) {
  return std::make_unique<TcpControllerReader>(config);
}

}  // namespace xr::openvr_driver
