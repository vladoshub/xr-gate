#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>

#include <xr_runtime/contracts/runtime_adapter.hpp>
#include <xr_runtime/net/tracking_net_v1.hpp>
#include <xr_runtime/platform/udp_socket.hpp>
#include <xr_runtime/registry/runtime_paths.hpp>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }


bool is_source_stale(int64_t now_ns_value, uint64_t source_timestamp_ns, double max_source_age_ms) {
  if (max_source_age_ms <= 0.0) return false;
  if (source_timestamp_ns == 0) return true;
  const double age_ms = xr_runtime::ns_to_ms(now_ns_value - static_cast<int64_t>(source_timestamp_ns));
  return !std::isfinite(age_ms) || age_ms > max_source_age_ms;
}

}  // namespace

int main(int argc, char** argv) {
  std::string registry_path = xr_runtime::default_tracking_registry_path();
  std::string hmd_stream = "hmd_pose";
  std::string hand_stream = "hand_tracking";
  std::string target_host = "127.0.0.1";
  uint16_t target_port = 45670;
  std::string mode = "event";  // event or rate
  double send_rate_hz = 90.0;
  double duration_s = 0.0;
  double poll_ms = 1.0;
  double max_source_age_ms = 250.0;
  int print_every = 30;
  bool send_hmd = true;
  bool send_hands = true;
  bool require_tracking = false;
  std::string spatial_proxy_mesh_input = "none"; // none, shm
  std::string spatial_proxy_mesh_registry = xr_runtime::default_runtime_tracking_registry_path();
  std::string spatial_proxy_mesh_stream = "spatial_proxy_mesh";
  std::string spatial_proxy_mesh_udp_host = "127.0.0.1";
  uint16_t spatial_proxy_mesh_udp_port = 45740;
  double spatial_proxy_mesh_send_rate_hz = 2.0;
  size_t spatial_proxy_mesh_udp_mtu_bytes = 1200;
  double spatial_proxy_mesh_reattach_interval_ms = 500.0;
  std::string hand_net_mode = "auto";  // auto, summary-v1, summary-v2, pose-plus-joints-v2

  CLI::App app{"tracking_net_v1 UDP bridge for hmd_pose + hand summary"};
  app.add_option("--registry", registry_path, "tracking stream registry path");
  app.add_option("--hmd-stream", hmd_stream, "HMD pose stream id");
  app.add_option("--hand-stream", hand_stream, "hand tracking stream id");
  app.add_option("--target-host", target_host, "UDP target IPv4/hostname");
  app.add_option("--target-port", target_port, "UDP target port");
  app.add_option("--mode", mode, "send mode: event or rate");
  app.add_option("--send-rate", send_rate_hz, "rate mode send rate in Hz");
  app.add_option("--duration", duration_s, "run duration seconds; 0 means until Ctrl+C");
  app.add_option("--poll-ms", poll_ms, "event mode poll interval ms");
  app.add_option("--max-source-age-ms", max_source_age_ms,
                 "maximum source frame age before skipping packet; <=0 disables");
  app.add_option("--print-every", print_every, "print every N sent packets");
  app.add_flag("--send-hmd,!--no-send-hmd", send_hmd, "enable/disable HMD pose packets");
  app.add_flag("--send-hands,!--no-send-hands", send_hands, "enable/disable hand summary packets");
  app.add_option("--hand-net-mode", hand_net_mode,
                 "hand UDP mode: auto, summary-v1, summary-v2, pose-plus-joints-v2");
  app.add_flag("--require-tracking", require_tracking,
               "send only HMD tracking_status=2 frames; hand summary is still sent");
  app.add_option("--spatial-proxy-mesh-input", spatial_proxy_mesh_input,
                 "Optional source spatial live-depth-grid/proxy-mesh input to forward over UDP: none or shm");
  app.add_option("--spatial-proxy-mesh-registry", spatial_proxy_mesh_registry,
                 "Registry path used with --spatial-proxy-mesh-input shm");
  app.add_option("--spatial-proxy-mesh-stream", spatial_proxy_mesh_stream,
                 "Spatial proxy mesh stream id used with --spatial-proxy-mesh-input shm");
  app.add_option("--spatial-proxy-mesh-udp-host", spatial_proxy_mesh_udp_host,
                 "UDP target host for spatial proxy mesh chunks");
  app.add_option("--spatial-proxy-mesh-udp-port", spatial_proxy_mesh_udp_port,
                 "UDP target port for spatial proxy mesh chunks");
  app.add_option("--spatial-proxy-mesh-send-rate-hz", spatial_proxy_mesh_send_rate_hz,
                 "Max spatial proxy mesh UDP snapshot rate");
  app.add_option("--spatial-proxy-mesh-udp-mtu-bytes", spatial_proxy_mesh_udp_mtu_bytes,
                 "Target UDP payload size for spatial proxy mesh chunks");
  app.add_option("--spatial-proxy-mesh-reattach-interval-ms", spatial_proxy_mesh_reattach_interval_ms,
                 "Retry interval while waiting for spatial proxy mesh SHM stream; <=0 disables retry");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  try {
    if (mode != "event" && mode != "rate") {
      throw std::runtime_error("--mode must be event or rate");
    }
    if (send_rate_hz <= 0.0) {
      throw std::runtime_error("--send-rate must be > 0");
    }
    if (!send_hmd && !send_hands && spatial_proxy_mesh_input == "none") {
      throw std::runtime_error("nothing to send: HMD, hands, and spatial proxy mesh are all disabled");
    }
    if (spatial_proxy_mesh_input != "none" && spatial_proxy_mesh_input != "shm") {
      throw std::runtime_error("--spatial-proxy-mesh-input must be one of: none, shm");
    }
    if (hand_net_mode != "auto" && hand_net_mode != "summary-v1" &&
        hand_net_mode != "summary-v2" && hand_net_mode != "pose-plus-joints-v2") {
      throw std::runtime_error("--hand-net-mode must be one of: auto, summary-v1, summary-v2, pose-plus-joints-v2");
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "== tracking_udp_bridge / tracking_net_v1 ==\n";
    std::cout << "registry: " << registry_path << "\n";
    std::cout << "hmd_stream: " << hmd_stream << "\n";
    std::cout << "hand_stream: " << hand_stream << "\n";
    std::cout << "target: " << target_host << ":" << target_port << "\n";
    std::cout << "mode: " << mode << "\n";
    std::cout << "send_hmd: " << (send_hmd ? "true" : "false") << "\n";
    std::cout << "send_hands: " << (send_hands ? "true" : "false") << "\n";
    std::cout << "max_source_age_ms: " << max_source_age_ms << "\n";

    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>> hmd_reader;
    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF64V1>> hand_reader;
    std::unique_ptr<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF32V2>> hand_reader_v2;
    std::unique_ptr<xr_runtime::TrackingRingReader<xr_spatial::RuntimeSpatialProxyMeshF32V1>> spatial_proxy_mesh_reader;

    if (send_hmd) {
      auto hmd_info = xr_runtime::stream_info_from_registry(registry_path, hmd_stream);
      hmd_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HmdPoseF64V1>>(
          std::move(hmd_info), "HMD_POSE_F64_LE");
      std::cout << "[tracking_udp_bridge] attached hmd stream: " << hmd_stream
                << " frame=" << hmd_reader->info().frame_id
                << " shm=" << hmd_reader->info().shm_name << "\n";
    }

    if (send_hands) {

      auto hand_info = xr_runtime::stream_info_from_registry(registry_path, hand_stream);
      if (hand_info.format_name == "HAND_TRACKING_21_JOINT_F32_V2" ||
          hand_info.format_name == "HAND_TRACKING_F32_V2" ||
          hand_info.format_name == "HAND_TRACKING_V2") {
        const std::string hand_format_name = hand_info.format_name;
        hand_reader_v2 = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF32V2>>(
            std::move(hand_info), hand_format_name);
        std::cout << "[tracking_udp_bridge] attached 21-joint hand stream: " << hand_stream
                  << " format=" << hand_format_name
                  << " frame=" << hand_reader_v2->info().frame_id
                  << " shm=" << hand_reader_v2->info().shm_name
                  << " payload_size=" << hand_reader_v2->info().payload_size << "\n";
      } else {
        hand_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_runtime::HandTrackingFrameF64V1>>(
            std::move(hand_info), "HAND_TRACKING_V1");
        std::cout << "[tracking_udp_bridge] attached hand stream: " << hand_stream
                  << " format=HAND_TRACKING_V1"
                  << " frame=" << hand_reader->info().frame_id
                  << " shm=" << hand_reader->info().shm_name << "\n";
      }
    }

    auto spatial_proxy_mesh_next_attach_attempt = std::chrono::steady_clock::now();
    const auto spatial_proxy_mesh_reattach_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(std::max(50.0, spatial_proxy_mesh_reattach_interval_ms)));
    uint64_t spatial_proxy_mesh_attach_attempts = 0;
    auto try_attach_spatial_proxy_mesh = [&]() -> bool {
      if (spatial_proxy_mesh_input != "shm") return false;
      ++spatial_proxy_mesh_attach_attempts;
      try {
        auto spatial_info = xr_runtime::stream_info_from_registry(spatial_proxy_mesh_registry, spatial_proxy_mesh_stream);
        spatial_proxy_mesh_reader = std::make_unique<xr_runtime::TrackingRingReader<xr_spatial::RuntimeSpatialProxyMeshF32V1>>(
            std::move(spatial_info), xr_spatial::RUNTIME_SPATIAL_PROXY_MESH_FORMAT_NAME);
        std::cout << "[tracking_udp_bridge] attached spatial proxy mesh stream: " << spatial_proxy_mesh_stream
                  << " frame=" << spatial_proxy_mesh_reader->info().frame_id
                  << " shm=" << spatial_proxy_mesh_reader->info().shm_name
                  << " target=" << spatial_proxy_mesh_udp_host << ":" << spatial_proxy_mesh_udp_port << "\n";
        return true;
      } catch (const std::exception& e) {
        if (spatial_proxy_mesh_attach_attempts == 1 || (spatial_proxy_mesh_attach_attempts % 10) == 0) {
          std::cout << "[tracking_udp_bridge] waiting for spatial proxy mesh stream "
                    << spatial_proxy_mesh_registry << ":" << spatial_proxy_mesh_stream
                    << ": " << e.what() << "\n";
        }
        spatial_proxy_mesh_reader.reset();
        return false;
      }
    };
    if (spatial_proxy_mesh_input == "shm") {
      try_attach_spatial_proxy_mesh();
    }

    xr_runtime::platform::UdpSender sender(target_host, target_port);
    std::unique_ptr<xr_runtime::platform::UdpSender> spatial_proxy_sender;
    if (spatial_proxy_mesh_input == "shm") {
      spatial_proxy_sender = std::make_unique<xr_runtime::platform::UdpSender>(spatial_proxy_mesh_udp_host, spatial_proxy_mesh_udp_port);
    }

    const auto start = std::chrono::steady_clock::now();
    auto next_send = start;
    const auto send_period = std::chrono::duration<double>(1.0 / send_rate_hz);

    uint64_t transport_seq = 0;
    uint64_t sent_hmd_packets = 0;
    uint64_t sent_hand_packets = 0;
    uint64_t sent_hand_v2_summary_packets = 0;
    uint64_t sent_hand_v2_joints_packets = 0;
    uint64_t sent_spatial_proxy_mesh_snapshots = 0;
    uint64_t sent_spatial_proxy_mesh_chunks = 0;
    uint64_t last_sent_spatial_proxy_mesh_seq = 0;
    auto next_spatial_proxy_mesh_send = start;
    const auto spatial_proxy_mesh_send_period = std::chrono::duration<double>(1.0 / std::max(0.1, spatial_proxy_mesh_send_rate_hz));

    uint64_t last_sent_hmd_source_seq = 0;
    uint64_t last_sent_hand_source_seq = 0;

    uint64_t no_hmd_frame_count = 0;
    uint64_t no_hand_frame_count = 0;
    uint64_t skipped_hmd_not_tracking = 0;
    uint64_t skipped_hmd_stale = 0;
    uint64_t skipped_hand_stale = 0;

    while (!g_stop) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed_s = std::chrono::duration<double>(now - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;
      if (spatial_proxy_mesh_input == "shm" && !spatial_proxy_mesh_reader &&
          spatial_proxy_mesh_reattach_interval_ms > 0.0 &&
          now >= spatial_proxy_mesh_next_attach_attempt) {
        try_attach_spatial_proxy_mesh();
        spatial_proxy_mesh_next_attach_attempt = now + spatial_proxy_mesh_reattach_period;
      }

      bool rate_tick = false;
      if (mode == "rate") {
        if (now >= next_send) {
          rate_tick = true;
          do {
            next_send += std::chrono::duration_cast<std::chrono::steady_clock::duration>(send_period);
          } while (next_send <= now);
        }
      }

      if (hmd_reader) {
        const auto hmd = hmd_reader->latest();
        bool should_send_hmd = false;
        if (!hmd || hmd->sequence == 0) {
          ++no_hmd_frame_count;
        } else if (mode == "event") {
          should_send_hmd = hmd->sequence != last_sent_hmd_source_seq;
        } else {
          should_send_hmd = rate_tick;
        }

        if (should_send_hmd && hmd) {
          const int64_t send_now_ns = xr_runtime::now_ns();
          const bool hmd_stale = is_source_stale(send_now_ns, hmd->timestamp_ns, max_source_age_ms);
          if (hmd_stale) {
            ++skipped_hmd_stale;
            last_sent_hmd_source_seq = hmd->sequence;
          } else if (require_tracking && hmd->tracking_status != 2) {
            ++skipped_hmd_not_tracking;
            last_sent_hmd_source_seq = hmd->sequence;
          } else {
            xr_runtime::tracking_net_v1::HmdPosePacket packet {};
            packet.header = xr_runtime::tracking_net_v1::make_header(
                xr_runtime::tracking_net_v1::PacketType::HmdPoseF64,
                ++transport_seq,
                hmd->sequence,
                static_cast<uint64_t>(send_now_ns),
                hmd->source_timestamp_ns,
                hmd->reset_counter,
                sizeof(xr_runtime::HmdPoseF64V1));
            packet.payload = *hmd;

            const auto wire = xr_runtime::tracking_net_v1::encode_hmd_pose_packet(packet);
            sender.send_packet(wire.data(), wire.size());
            last_sent_hmd_source_seq = hmd->sequence;
            ++sent_hmd_packets;

            if (print_every > 0 && transport_seq % static_cast<uint64_t>(print_every) == 0) {
              const double rate = double(transport_seq) / std::max(1e-9, elapsed_s);
              const double source_age_ms =
                  xr_runtime::ns_to_ms(static_cast<int64_t>(packet.header.send_timestamp_ns) -
                                       static_cast<int64_t>(hmd->timestamp_ns));

              std::cout << "[tracking_udp_bridge] sent=" << transport_seq
                        << " type=HMD"
                        << " total_rate=" << rate << "Hz"
                        << " src_seq=" << hmd->sequence
                        << " hmd_status=" << xr_runtime::hmd_status_name(hmd->tracking_status)
                        << " source_age_ms=" << source_age_ms
                        << " p=(" << hmd->px << "," << hmd->py << "," << hmd->pz << ")"
                        << "\n";
            }
          }
        }
      }

      if (hand_reader) {
        const auto hand = hand_reader->latest();
        bool should_send_hand = false;
        if (!hand || hand->sequence == 0) {
          ++no_hand_frame_count;
        } else if (mode == "event") {
          should_send_hand = hand->sequence != last_sent_hand_source_seq;
        } else {
          should_send_hand = rate_tick;
        }

        if (should_send_hand && hand) {
          const int64_t send_now_ns = xr_runtime::now_ns();
          const bool hand_stale = is_source_stale(send_now_ns, hand->timestamp_ns, max_source_age_ms);
          if (hand_stale) {
            ++skipped_hand_stale;
            last_sent_hand_source_seq = hand->sequence;
          } else {
            const auto summary = xr_runtime::tracking_net_v1::make_hand_summary(*hand);

            xr_runtime::tracking_net_v1::HandSummaryPacket packet {};
            packet.header = xr_runtime::tracking_net_v1::make_header(
                xr_runtime::tracking_net_v1::PacketType::HandSummaryF64,
                ++transport_seq,
                summary.sequence,
                static_cast<uint64_t>(send_now_ns),
                summary.source_timestamp_ns,
                summary.reset_counter,
                sizeof(xr_runtime::tracking_net_v1::HandSummaryF64V1));
            packet.payload = summary;

            const auto wire = xr_runtime::tracking_net_v1::encode_hand_summary_packet(packet);
            sender.send_packet(wire.data(), wire.size());
            last_sent_hand_source_seq = hand->sequence;
            ++sent_hand_packets;

            if (print_every > 0 && transport_seq % static_cast<uint64_t>(print_every) == 0) {
              const double rate = double(transport_seq) / std::max(1e-9, elapsed_s);
              const double source_age_ms =
                  xr_runtime::ns_to_ms(static_cast<int64_t>(packet.header.send_timestamp_ns) -
                                       static_cast<int64_t>(summary.timestamp_ns));

              std::cout << "[tracking_udp_bridge] sent=" << transport_seq
                        << " type=HAND_SUMMARY_F64"
                        << " total_rate=" << rate << "Hz"
                        << " bytes=" << wire.size()
                        << " src_seq=" << summary.sequence
                        << " hand_status=" << xr_runtime::hand_status_name(summary.tracking_status)
                        << " hand_count=" << summary.hand_count
                        << " source_age_ms=" << source_age_ms
                        << " L(pinch=" << summary.left.pinch_strength
                        << ",grab=" << summary.left.grab_strength << ")"
                        << " R(pinch=" << summary.right.pinch_strength
                        << ",grab=" << summary.right.grab_strength << ")"
                        << "\n";
            }
          }
        }
      }

      if (hand_reader_v2) {
        const auto hand = hand_reader_v2->latest();
        bool should_send_hand = false;
        if (!hand || hand->sequence == 0) {
          ++no_hand_frame_count;
        } else if (mode == "event") {
          should_send_hand = hand->sequence != last_sent_hand_source_seq;
        } else {
          should_send_hand = rate_tick;
        }

        if (should_send_hand && hand) {
          const int64_t send_now_ns = xr_runtime::now_ns();
          const bool hand_stale = is_source_stale(send_now_ns, hand->timestamp_ns, max_source_age_ms);
          if (hand_stale) {
            ++skipped_hand_stale;
            last_sent_hand_source_seq = hand->sequence;
          } else {
            const bool send_summary = hand_net_mode == "auto" || hand_net_mode == "summary-v2" ||
                                      hand_net_mode == "pose-plus-joints-v2";
            const bool send_joints = hand_net_mode == "auto" || hand_net_mode == "pose-plus-joints-v2";

            if (send_summary) {
              const auto summary = xr_runtime::tracking_net_v1::make_hand_summary_v2(*hand);
              xr_runtime::tracking_net_v1::HandSummaryV2Packet packet {};
              packet.header = xr_runtime::tracking_net_v1::make_header(
                  xr_runtime::tracking_net_v1::PacketType::HandSummaryF32V2,
                  ++transport_seq,
                  summary.sequence,
                  static_cast<uint64_t>(send_now_ns),
                  summary.source_timestamp_ns,
                  summary.reset_counter,
                  sizeof(xr_runtime::tracking_net_v1::HandSummaryF32V2));
              packet.payload = summary;
              const auto wire = xr_runtime::tracking_net_v1::encode_hand_summary_v2_packet(packet);
              sender.send_packet(wire.data(), wire.size());
              ++sent_hand_packets;
              ++sent_hand_v2_summary_packets;

              if (print_every > 0 && transport_seq % static_cast<uint64_t>(print_every) == 0) {
                const double rate = double(transport_seq) / std::max(1e-9, elapsed_s);
                const double source_age_ms =
                    xr_runtime::ns_to_ms(static_cast<int64_t>(packet.header.send_timestamp_ns) -
                                         static_cast<int64_t>(summary.timestamp_ns));

                std::cout << "[tracking_udp_bridge] sent=" << transport_seq
                          << " type=HAND_SUMMARY_F32_V2"
                          << " total_rate=" << rate << "Hz"
                          << " bytes=" << wire.size()
                          << " src_seq=" << summary.sequence
                          << " hand_status=" << xr_runtime::hand_status_name(summary.tracking_status)
                          << " hand_count=" << summary.hand_count
                          << " source_age_ms=" << source_age_ms
                          << " L(joints=" << summary.left.joint_count << ",pinch=" << summary.left.pinch_strength
                          << ",grab=" << summary.left.grab_strength << ")"
                          << " R(joints=" << summary.right.joint_count << ",pinch=" << summary.right.pinch_strength
                          << ",grab=" << summary.right.grab_strength << ")"
                          << "\n";
              }
            }

            if (send_joints) {
              for (int side_idx = 0; side_idx < 2; ++side_idx) {
                const bool right = side_idx == 1;
                const auto joints = xr_runtime::tracking_net_v1::make_hand_joints_v2(*hand, right);
                xr_runtime::tracking_net_v1::HandJointsV2Packet packet {};
                packet.header = xr_runtime::tracking_net_v1::make_header(
                    xr_runtime::tracking_net_v1::PacketType::HandJointsF32V2,
                    ++transport_seq,
                    joints.sequence,
                    static_cast<uint64_t>(send_now_ns),
                    joints.source_timestamp_ns,
                    joints.reset_counter,
                    sizeof(xr_runtime::tracking_net_v1::HandJointsF32V2));
                packet.payload = joints;
                const auto wire = xr_runtime::tracking_net_v1::encode_hand_joints_v2_packet(packet);
                sender.send_packet(wire.data(), wire.size());
                ++sent_hand_packets;
                ++sent_hand_v2_joints_packets;
              }
            }

            last_sent_hand_source_seq = hand->sequence;
          }
        }
      }


      if (spatial_proxy_mesh_reader && spatial_proxy_sender) {
        const auto mesh = spatial_proxy_mesh_reader->latest();
        bool should_send_mesh = false;
        if (mesh && mesh->sequence != 0) {
          if (mode == "event") {
            should_send_mesh = mesh->sequence != last_sent_spatial_proxy_mesh_seq;
          } else if (now >= next_spatial_proxy_mesh_send) {
            should_send_mesh = true;
            do { next_spatial_proxy_mesh_send += std::chrono::duration_cast<std::chrono::steady_clock::duration>(spatial_proxy_mesh_send_period); }
            while (next_spatial_proxy_mesh_send <= now);
          }
        }
        if (should_send_mesh && mesh) {
          const auto chunks = xr_spatial::encode_proxy_mesh_udp_chunks(*mesh, spatial_proxy_mesh_udp_mtu_bytes);
          for (const auto& c : chunks) spatial_proxy_sender->send_packet(c.data(), c.size());
          last_sent_spatial_proxy_mesh_seq = mesh->sequence;
          ++sent_spatial_proxy_mesh_snapshots;
          sent_spatial_proxy_mesh_chunks += chunks.size();
          if (print_every > 0 && sent_spatial_proxy_mesh_snapshots % static_cast<uint64_t>(print_every) == 0) {
            std::cout << "[tracking_udp_bridge] sent spatial_proxy_mesh seq=" << mesh->sequence
                      << " kind=" << mesh->mesh_kind
                      << " grid=" << mesh->grid_width << "x" << mesh->grid_height
                      << " valid=" << mesh->valid_point_count
                      << " vertices=" << mesh->vertex_count
                      << " triangles=" << mesh->triangle_count
                      << " chunks=" << chunks.size()
                      << " target=" << spatial_proxy_mesh_udp_host << ":" << spatial_proxy_mesh_udp_port << "\n";
          }
        }
      }

      if (mode == "event") {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(poll_ms));
      } else {
        std::this_thread::sleep_until(next_send);
      }
    }

    const double elapsed_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "=== tracking_udp_bridge summary ===\n";
    std::cout << "runtime_s: " << elapsed_s << "\n";
    std::cout << "sent_packets: " << transport_seq << "\n";
    std::cout << "send_rate_hz: " << (double(transport_seq) / std::max(1e-9, elapsed_s)) << "\n";
    std::cout << "sent_hmd_packets: " << sent_hmd_packets << "\n";
    std::cout << "sent_hand_packets: " << sent_hand_packets << "\n";
    std::cout << "sent_spatial_proxy_mesh_snapshots: " << sent_spatial_proxy_mesh_snapshots << "\n";
    std::cout << "sent_spatial_proxy_mesh_chunks: " << sent_spatial_proxy_mesh_chunks << "\n";
    std::cout << "last_hmd_source_seq: " << last_sent_hmd_source_seq << "\n";
    std::cout << "last_hand_source_seq: " << last_sent_hand_source_seq << "\n";
    std::cout << "no_hmd_frame_count: " << no_hmd_frame_count << "\n";
    std::cout << "no_hand_frame_count: " << no_hand_frame_count << "\n";
    std::cout << "skipped_hmd_not_tracking: " << skipped_hmd_not_tracking << "\n";
    std::cout << "skipped_hmd_stale: " << skipped_hmd_stale << "\n";
    std::cout << "skipped_hand_stale: " << skipped_hand_stale << "\n";

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
