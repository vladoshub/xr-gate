#include "runtime_hand_reader.hpp"

#ifdef _WIN32

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <xr_runtime/net/runtime_output_udp.hpp>

#include "runtime_pose_reader.hpp"

namespace xr::openvr_driver {
namespace {

class WindowsRuntimeHandUdpReader final : public IRuntimeHandReader {
 public:
  explicit WindowsRuntimeHandUdpReader(RuntimeHandReaderConfig cfg)
      : cfg_(std::move(cfg)), receiver_(cfg_.udp_bind_host, cfg_.udp_port,
                                        xr_runtime::platform::UdpReceiveMode::NonBlocking) {}

  bool read_latest(RuntimeHandSample& out, std::string* error) override {
    bool received = false;
    for (int i = 0; i < 64; ++i) {
      auto n = receiver_.receive(buffer_.data(), buffer_.size());
      if (!n) break;
      try {
        const auto h = xr_runtime::net::runtime_output_udp::decode_header(buffer_.data(), *n);
        if (h.packet_type != static_cast<uint16_t>(xr_runtime::net::runtime_output_udp::PacketType::RuntimeHandTracking)) {
          continue;
        }
        xr_tracking::HandTrackingFrameF32V2 frame{};
        if (!xr_runtime::net::runtime_output_udp::decode_payload(
                h, buffer_.data(), *n,
                xr_runtime::net::runtime_output_udp::PacketType::RuntimeHandTracking, frame)) {
          continue;
        }
        latest_.frame = frame;
        latest_.read_time_ns = monotonic_now_ns();
        const int64_t ts = frame.timestamp_ns != 0 ? static_cast<int64_t>(frame.timestamp_ns)
                                                   : static_cast<int64_t>(frame.source_timestamp_ns);
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
        const int64_t ts = out.frame.timestamp_ns != 0 ? static_cast<int64_t>(out.frame.timestamp_ns)
                                                       : static_cast<int64_t>(out.frame.source_timestamp_ns);
        out.age_ns = ts > 0 ? out.read_time_ns - ts : 0;
      }
      return true;
    }
    if (error) *error = "no runtime hand UDP packet yet";
    return false;
  }

  const char* transport_name() const override { return "windows_udp"; }

 private:
  RuntimeHandReaderConfig cfg_;
  xr_runtime::platform::UdpReceiver receiver_;
  std::array<uint8_t, 4096> buffer_{};
  RuntimeHandSample latest_{};
  bool has_latest_ = false;
};

}  // namespace

std::unique_ptr<IRuntimeHandReader> create_windows_runtime_hand_reader(const RuntimeHandReaderConfig& cfg) {
  return std::make_unique<WindowsRuntimeHandUdpReader>(cfg);
}

}  // namespace xr::openvr_driver

#endif  // _WIN32
