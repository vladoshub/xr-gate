#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <CLI/CLI.hpp>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_runtime/net/tracking_net_v1.hpp>
#include <xr_runtime/platform/udp_socket.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }


}  // namespace

int main(int argc, char** argv) {
  std::string bind_host = "0.0.0.0";
  uint16_t bind_port = 45670;
  double duration_s = 0.0;
  int print_every = 30;

  CLI::App app{"tracking_net_v1 UDP debug receiver for hmd_pose + hand summary"};
  app.add_option("--bind-host", bind_host, "UDP bind host");
  app.add_option("--bind-port", bind_port, "UDP bind port");
  app.add_option("--duration", duration_s, "run duration seconds; 0 means until Ctrl+C");
  app.add_option("--print-every", print_every, "print every N valid packets");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  try {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "== tracking_udp_debug_receiver / tracking_net_v1 ==\n";
    std::cout << "bind: " << bind_host << ":" << bind_port << "\n";

    xr_runtime::platform::UdpReceiver receiver(
        bind_host, bind_port, xr_runtime::platform::UdpReceiveMode::TimedBlocking, 100);

    const auto start = std::chrono::steady_clock::now();

    uint64_t received = 0;
    uint64_t valid = 0;
    uint64_t invalid = 0;
    uint64_t valid_hmd = 0;
    uint64_t valid_hand = 0;
    uint64_t valid_hand_v2_summary = 0;
    uint64_t valid_hand_v2_joints = 0;

    uint64_t last_transport_seq = 0;
    uint64_t last_hmd_stream_seq = 0;
    uint64_t last_hand_stream_seq = 0;
    uint64_t estimated_transport_loss = 0;
    uint64_t duplicate_or_reordered = 0;

    alignas(8) uint8_t buffer[4096];

    while (!g_stop) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(now - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;

      const auto n_opt = receiver.receive(buffer, sizeof(buffer));
      if (!n_opt) continue;
      const size_t n = *n_opt;

      ++received;

      try {
        if (n < sizeof(xr_runtime::tracking_net_v1::PacketHeader)) {
          throw std::runtime_error("short packet");
        }

        const auto header = xr_runtime::tracking_net_v1::decode_header(buffer, n);
        xr_runtime::tracking_net_v1::validate_common_header(header, n);

        if (last_transport_seq == 0 || header.transport_sequence > last_transport_seq) {
          if (last_transport_seq != 0 && header.transport_sequence > last_transport_seq + 1) {
            estimated_transport_loss += header.transport_sequence - last_transport_seq - 1;
          }
          last_transport_seq = header.transport_sequence;
        } else {
          ++duplicate_or_reordered;
        }

        if (header.packet_type == static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HmdPoseF64)) {
          xr_runtime::tracking_net_v1::validate_hmd_pose_packet_header(header, n);

          const auto hmd = xr_runtime::tracking_net_v1::decode_hmd_pose_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));

          ++valid;
          ++valid_hmd;
          last_hmd_stream_seq = header.stream_sequence;

          if (print_every > 0 && valid % static_cast<uint64_t>(print_every) == 0) {
            const double rate = double(valid) / std::max(1e-9, elapsed_s);
            const double same_clock_sender_age_ms =
                xr_runtime::ns_to_ms(xr_runtime::now_ns() -
                                     static_cast<int64_t>(header.send_timestamp_ns));
            const double same_clock_pose_age_ms =
                xr_runtime::ns_to_ms(xr_runtime::now_ns() -
                                     static_cast<int64_t>(hmd.timestamp_ns));

            std::cout << "[tracking_udp_rx] valid=" << valid
                      << " type=HMD"
                      << " total_rate=" << rate << "Hz"
                      << " transport_seq=" << header.transport_sequence
                      << " stream_seq=" << header.stream_sequence
                      << " hmd_status=" << xr_runtime::hmd_status_name(hmd.tracking_status)
                      << " same_clock_sender_age_ms=" << same_clock_sender_age_ms
                      << " same_clock_pose_age_ms=" << same_clock_pose_age_ms
                      << " p=(" << hmd.px << "," << hmd.py << "," << hmd.pz << ")"
                      << " estimated_transport_loss=" << estimated_transport_loss
                      << " dup_or_reordered=" << duplicate_or_reordered
                      << "\n";
          }
        } else if (header.packet_type == static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandSummaryF64)) {
          xr_runtime::tracking_net_v1::validate_hand_summary_packet_header(header, n);

          const auto hands = xr_runtime::tracking_net_v1::decode_hand_summary_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));

          ++valid;
          ++valid_hand;
          last_hand_stream_seq = header.stream_sequence;

          if (print_every > 0 && valid % static_cast<uint64_t>(print_every) == 0) {
            const double rate = double(valid) / std::max(1e-9, elapsed_s);
            const double same_clock_sender_age_ms =
                xr_runtime::ns_to_ms(xr_runtime::now_ns() -
                                     static_cast<int64_t>(header.send_timestamp_ns));
            const double same_clock_pose_age_ms =
                xr_runtime::ns_to_ms(xr_runtime::now_ns() -
                                     static_cast<int64_t>(hands.timestamp_ns));

            std::cout << "[tracking_udp_rx] valid=" << valid
                      << " type=HAND_SUMMARY"
                      << " total_rate=" << rate << "Hz"
                      << " transport_seq=" << header.transport_sequence
                      << " stream_seq=" << header.stream_sequence
                      << " hand_status=" << xr_runtime::hand_status_name(hands.tracking_status)
                      << " hand_count=" << hands.hand_count
                      << " same_clock_sender_age_ms=" << same_clock_sender_age_ms
                      << " same_clock_pose_age_ms=" << same_clock_pose_age_ms
                      << " L(status=" << xr_runtime::hand_status_name(hands.left.status)
                      << ",pinch=" << hands.left.pinch_strength
                      << ",grab=" << hands.left.grab_strength
                      << ")"
                      << " R(status=" << xr_runtime::hand_status_name(hands.right.status)
                      << ",pinch=" << hands.right.pinch_strength
                      << ",grab=" << hands.right.grab_strength
                      << ")"
                      << " estimated_transport_loss=" << estimated_transport_loss
                      << " dup_or_reordered=" << duplicate_or_reordered
                      << "\n";
          }
        } else if (header.packet_type == static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandSummaryF32V2)) {
          xr_runtime::tracking_net_v1::validate_hand_summary_v2_packet_header(header, n);

          const auto hands = xr_runtime::tracking_net_v1::decode_hand_summary_f32_v2_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));

          ++valid;
          ++valid_hand;
          ++valid_hand_v2_summary;
          last_hand_stream_seq = header.stream_sequence;

          if (print_every > 0 && valid % static_cast<uint64_t>(print_every) == 0) {
            const double rate = double(valid) / std::max(1e-9, elapsed_s);
            const double same_clock_sender_age_ms =
                xr_runtime::ns_to_ms(xr_runtime::now_ns() -
                                     static_cast<int64_t>(header.send_timestamp_ns));
            const double same_clock_pose_age_ms =
                xr_runtime::ns_to_ms(xr_runtime::now_ns() -
                                     static_cast<int64_t>(hands.timestamp_ns));

            std::cout << "[tracking_udp_rx] valid=" << valid
                      << " type=HAND_SUMMARY_F32_V2"
                      << " total_rate=" << rate << "Hz"
                      << " bytes=" << n
                      << " transport_seq=" << header.transport_sequence
                      << " stream_seq=" << header.stream_sequence
                      << " hand_status=" << xr_runtime::hand_status_name(hands.tracking_status)
                      << " hand_count=" << hands.hand_count
                      << " same_clock_sender_age_ms=" << same_clock_sender_age_ms
                      << " same_clock_pose_age_ms=" << same_clock_pose_age_ms
                      << " L(status=" << xr_runtime::hand_status_name(hands.left.status)
                      << ",joints=" << hands.left.joint_count
                      << ",pinch=" << hands.left.pinch_strength
                      << ",grab=" << hands.left.grab_strength
                      << ")"
                      << " R(status=" << xr_runtime::hand_status_name(hands.right.status)
                      << ",joints=" << hands.right.joint_count
                      << ",pinch=" << hands.right.pinch_strength
                      << ",grab=" << hands.right.grab_strength
                      << ")"
                      << " estimated_transport_loss=" << estimated_transport_loss
                      << " dup_or_reordered=" << duplicate_or_reordered
                      << "\n";
          }
        } else if (header.packet_type == static_cast<uint16_t>(xr_runtime::tracking_net_v1::PacketType::HandJointsF32V2)) {
          xr_runtime::tracking_net_v1::validate_hand_joints_v2_packet_header(header, n);

          const auto joints = xr_runtime::tracking_net_v1::decode_hand_joints_f32_v2_payload(
              buffer + sizeof(xr_runtime::tracking_net_v1::PacketHeader),
              n - sizeof(xr_runtime::tracking_net_v1::PacketHeader));

          ++valid;
          ++valid_hand;
          ++valid_hand_v2_joints;
          last_hand_stream_seq = header.stream_sequence;

          if (print_every > 0 && valid % static_cast<uint64_t>(print_every) == 0) {
            const double rate = double(valid) / std::max(1e-9, elapsed_s);
            std::cout << "[tracking_udp_rx] valid=" << valid
                      << " type=HAND_JOINTS_F32_V2"
                      << " total_rate=" << rate << "Hz"
                      << " bytes=" << n
                      << " transport_seq=" << header.transport_sequence
                      << " stream_seq=" << header.stream_sequence
                      << " handedness=" << joints.handedness
                      << " hand_status=" << xr_runtime::hand_status_name(joints.status)
                      << " joint_count=" << joints.joint_count
                      << " confidence=" << joints.confidence
                      << " estimated_transport_loss=" << estimated_transport_loss
                      << " dup_or_reordered=" << duplicate_or_reordered
                      << "\n";
          }
        } else {
          throw std::runtime_error("unknown packet_type " + std::to_string(header.packet_type));
        }
      } catch (const std::exception& e) {
        ++invalid;
        if (invalid <= 5) {
          std::cout << "[tracking_udp_rx] invalid packet: " << e.what() << "\n";
        }
      }
    }

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "=== tracking_udp_debug_receiver summary ===\n";
    std::cout << "runtime_s: " << elapsed_s << "\n";
    std::cout << "received_packets: " << received << "\n";
    std::cout << "valid_packets: " << valid << "\n";
    std::cout << "valid_hmd_packets: " << valid_hmd << "\n";
    std::cout << "valid_hand_packets: " << valid_hand << "\n";
    std::cout << "invalid_packets: " << invalid << "\n";
    std::cout << "valid_rate_hz: " << (double(valid) / std::max(1e-9, elapsed_s)) << "\n";
    std::cout << "last_transport_seq: " << last_transport_seq << "\n";
    std::cout << "last_hmd_stream_seq: " << last_hmd_stream_seq << "\n";
    std::cout << "last_hand_stream_seq: " << last_hand_stream_seq << "\n";
    std::cout << "estimated_transport_loss: " << estimated_transport_loss << "\n";
    std::cout << "duplicate_or_reordered: " << duplicate_or_reordered << "\n";

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
