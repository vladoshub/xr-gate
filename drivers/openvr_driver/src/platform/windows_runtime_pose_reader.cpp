#include "runtime_pose_reader.hpp"

#if defined(_WIN32)

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <xr_runtime/net/runtime_output_udp.hpp>

namespace xr::openvr_driver {
namespace {

class WindowsRuntimePoseUdpReader final : public IRuntimePoseReader {
 public:
  explicit WindowsRuntimePoseUdpReader(RuntimePoseReaderConfig cfg)
      : cfg_(std::move(cfg)), receiver_(cfg_.udp_bind_host, cfg_.udp_port,
                                        xr_runtime::platform::UdpReceiveMode::NonBlocking) {}

  bool read_latest(RuntimePoseSample& out, std::string* error) override {
    bool received = false;
    for (int i = 0; i < 64; ++i) {
      auto n = receiver_.receive(buffer_.data(), buffer_.size());
      if (!n) break;
      try {
        const auto h = xr_runtime::net::runtime_output_udp::decode_header(buffer_.data(), *n);
        if (h.packet_type != static_cast<uint16_t>(xr_runtime::net::runtime_output_udp::PacketType::RuntimeHmdPose)) {
          continue;
        }
        xr_runtime::RuntimeHmdPoseF64V1 pose{};
        if (!xr_runtime::net::runtime_output_udp::decode_payload(
                h, buffer_.data(), *n,
                xr_runtime::net::runtime_output_udp::PacketType::RuntimeHmdPose, pose)) {
          continue;
        }
        latest_.pose = pose;
        latest_.read_time_ns = monotonic_now_ns();
        const int64_t ts = pose.timestamp_ns != 0 ? static_cast<int64_t>(pose.timestamp_ns)
                                                  : static_cast<int64_t>(pose.target_timestamp_ns);
        latest_.age_ns = ts > 0 ? latest_.read_time_ns - ts : 0;
        has_latest_ = true;
        received = true;
      } catch (const std::exception& e) {
        if (error) *error = e.what();
      }
    }
    if (has_latest_) {
      out = latest_;
      if (!received) {
        out.read_time_ns = monotonic_now_ns();
        const int64_t ts = out.pose.timestamp_ns != 0 ? static_cast<int64_t>(out.pose.timestamp_ns)
                                                      : static_cast<int64_t>(out.pose.target_timestamp_ns);
        out.age_ns = ts > 0 ? out.read_time_ns - ts : 0;
      }
      return true;
    }
    if (error) *error = "no runtime pose UDP packet yet";
    return false;
  }

  const char* transport_name() const override { return "windows_udp"; }

 private:
  RuntimePoseReaderConfig cfg_;
  xr_runtime::platform::UdpReceiver receiver_;
  std::array<uint8_t, 4096> buffer_{};
  RuntimePoseSample latest_{};
  bool has_latest_ = false;
};

}  // namespace

std::unique_ptr<IRuntimePoseReader> create_windows_runtime_pose_reader(const RuntimePoseReaderConfig& cfg) {
  return std::make_unique<WindowsRuntimePoseUdpReader>(cfg);
}

}  // namespace xr::openvr_driver

#else

namespace xr::openvr_driver {
std::unique_ptr<IRuntimePoseReader> create_windows_runtime_pose_reader(const RuntimePoseReaderConfig&) {
  return nullptr;
}
}  // namespace xr::openvr_driver

#endif
