#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <xr_runtime/platform/udp_socket.hpp>
#include <xr_spatial/contracts/runtime_spatial_proxy_mesh_contract.hpp>

namespace {
std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }

struct Assembly {
  uint64_t sequence = 0;
  xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1 header{};
  std::vector<uint8_t> payload;
  std::vector<uint8_t> received;
  uint16_t received_count = 0;

  void reset(const xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1& h) {
    sequence = h.mesh_sequence;
    header = h;
    payload.assign(h.full_payload_size_bytes, 0);
    received.assign(h.chunk_count, 0);
    received_count = 0;
  }

  bool add(const xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1& h, const uint8_t* data, size_t n) {
    if (sequence != h.mesh_sequence || received.empty()) reset(h);
    if (h.chunk_index >= received.size()) return false;
    if (h.payload_offset_bytes + h.payload_size > payload.size()) return false;
    if (n < h.payload_size) return false;
    std::memcpy(payload.data() + h.payload_offset_bytes, data, h.payload_size);
    if (!received[h.chunk_index]) { received[h.chunk_index] = 1; ++received_count; }
    header = h;
    return received_count == received.size();
  }
};

void save_ply(const std::filesystem::path& path, const xr_spatial::RuntimeSpatialProxyMeshF32V1& mesh) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::ofstream os(path);
  if (!os) throw std::runtime_error("failed to write PLY: " + path.string());
  os << "ply\nformat ascii 1.0\n";
  os << "element vertex " << mesh.vertex_count << "\n";
  os << "property float x\nproperty float y\nproperty float z\n";
  os << "element face " << mesh.triangle_count << "\n";
  os << "property list uchar int vertex_indices\n";
  os << "end_header\n";
  for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
    const auto& v = mesh.vertices[i];
    os << v.x << ' ' << v.y << ' ' << v.z << '\n';
  }
  for (uint32_t i = 0; i < mesh.triangle_count; ++i) {
    const auto& t = mesh.triangles[i];
    os << "3 " << t.i0 << ' ' << t.i1 << ' ' << t.i2 << '\n';
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::string bind_host = "0.0.0.0";
  uint16_t bind_port = 45740;
  double duration_s = 0.0;
  int print_every = 1;
  std::string output_dir = "/tmp/xr_spatial_proxy_udp";

  CLI::App app{"UDP debug receiver for XR spatial proxy mesh chunks"};
  app.add_option("--bind-host", bind_host, "UDP bind host");
  app.add_option("--bind-port", bind_port, "UDP bind port");
  app.add_option("--duration", duration_s, "run duration seconds; 0 means until Ctrl+C");
  app.add_option("--print-every", print_every, "print every N complete meshes");
  app.add_option("--output-dir", output_dir, "directory for received proxy mesh PLY snapshots");
  try { app.parse(argc, argv); } catch (const CLI::ParseError& e) { return app.exit(e); }

  try {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    xr_runtime::platform::UdpReceiver rx(bind_host, bind_port, xr_runtime::platform::UdpReceiveMode::TimedBlocking, 100);
    std::cout << "== spatial_proxy_mesh_udp_debug_receiver ==\n";
    std::cout << "bind: " << bind_host << ":" << bind_port << " output_dir=" << output_dir << "\n";
    const auto start = std::chrono::steady_clock::now();
    alignas(8) uint8_t buf[2048];
    uint64_t packets = 0, complete = 0, invalid = 0;
    Assembly asmbl;
    while (!g_stop) {
      const double elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (duration_s > 0.0 && elapsed_s >= duration_s) break;
      auto n_opt = rx.receive(buf, sizeof(buf));
      if (!n_opt) continue;
      ++packets;
      const size_t n = *n_opt;
      if (n < sizeof(xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1)) { ++invalid; continue; }
      xr_spatial::RuntimeSpatialProxyMeshUdpChunkHeaderV1 h{};
      std::memcpy(&h, buf, sizeof(h));
      if (!xr_spatial::proxy_mesh_udp_magic_ok(h) || h.version != 2 || h.header_size != sizeof(h) ||
          n < sizeof(h) + h.payload_size) { ++invalid; continue; }
      if (asmbl.sequence != h.mesh_sequence || asmbl.received.empty()) asmbl.reset(h);
      if (asmbl.add(h, buf + sizeof(h), n - sizeof(h))) {
        const auto mesh = xr_spatial::proxy_mesh_from_payload(asmbl.header, asmbl.payload);
        ++complete;
        const std::filesystem::path p = std::filesystem::path(output_dir) /
            ("proxy_mesh_" + std::to_string(mesh.sequence) + ".ply");
        save_ply(p, mesh);
        if (print_every > 0 && complete % static_cast<uint64_t>(print_every) == 0) {
          std::cout << "[spatial_proxy_udp_rx] complete=" << complete
                    << " seq=" << mesh.sequence
                    << " kind=" << mesh.mesh_kind
                    << " grid=" << mesh.grid_width << "x" << mesh.grid_height
                    << " valid=" << mesh.valid_point_count
                    << " vertices=" << mesh.vertex_count
                    << " triangles=" << mesh.triangle_count
                    << " chunks=" << h.chunk_count
                    << " packets=" << packets
                    << " invalid=" << invalid
                    << " wrote=" << p << "\n";
        }
        asmbl.received.clear();
      }
    }
    std::cout << "=== spatial_proxy_mesh_udp_debug_receiver summary ===\n";
    std::cout << "packets=" << packets << " complete=" << complete << " invalid=" << invalid << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
