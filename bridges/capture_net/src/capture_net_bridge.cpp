#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include <capture_client/net/capture_net_protocol.hpp>
#include <capture_client/transports/shm_transport.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }

class TcpClient {
 public:
  explicit TcpClient(int fd) : fd_(fd) {}
  ~TcpClient() { capture_client::capture_net_v1::close_fd(fd_); }

  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

  bool send(const capture_client::RawMessage& msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) return false;
    if (!capture_client::capture_net_v1::write_raw_message(fd_, msg)) {
      capture_client::capture_net_v1::close_fd(fd_);
      return false;
    }
    ++sent_;
    return true;
  }

  uint64_t sent() const { return sent_; }

 private:
  int fd_ = -1;
  mutable std::mutex mu_;
  uint64_t sent_ = 0;
};

class ClientSet {
 public:
  void add(std::shared_ptr<TcpClient> c) {
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(std::move(c));
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return clients_.size();
  }

  size_t broadcast(const capture_client::RawMessage& msg) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t ok = 0;
    auto it = clients_.begin();
    while (it != clients_.end()) {
      if ((*it)->send(msg)) {
        ++ok;
        ++it;
      } else {
        it = clients_.erase(it);
      }
    }
    return ok;
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<TcpClient>> clients_;
};

void accept_loop(int listen_fd, ClientSet& clients) {
  while (!g_stop) {
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (g_stop) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    capture_client::capture_net_v1::set_tcp_nodelay(fd);
    capture_client::capture_net_v1::set_socket_send_buffer(fd, 4 * 1024 * 1024);
    capture_client::capture_net_v1::set_socket_recv_buffer(fd, 4 * 1024 * 1024);
    clients.add(std::make_shared<TcpClient>(fd));
    std::cout << "[capture_net_bridge] client connected; clients=" << clients.size() << "\n";
  }
}

uint64_t send_new_from_reader(capture_client::IStreamReader& reader,
                              uint64_t last_seq,
                              uint64_t max_backfill,
                              ClientSet& clients,
                              uint64_t& sent_messages) {
  const uint64_t latest = reader.latest_sequence();
  if (latest <= last_seq) return last_seq;

  if (clients.size() == 0) {
    return latest;
  }

  uint64_t start = last_seq + 1;
  if (max_backfill > 0 && latest > last_seq + max_backfill) {
    start = latest - max_backfill + 1;
  }

  capture_client::RawMessage msg;
  uint64_t new_last = last_seq;
  for (uint64_t seq = start; seq <= latest; ++seq) {
    if (!reader.read_sequence(seq, msg)) {
      new_last = seq;
      continue;
    }
    clients.broadcast(msg);
    ++sent_messages;
    new_last = seq;
  }
  return std::max(new_last, latest);
}

}  // namespace

int main(int argc, char** argv) {
  std::string registry_path = "/tmp/capture_service_streams.json";
  std::string listen_host = "127.0.0.1";
  int listen_port = 45555;
  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string imu_stream = "imu0";
  double duration_s = 0.0;
  double poll_ms = 1.0;
  int print_every = 30;
  uint64_t max_backfill = 256;

  CLI::App app{"capture_net_v1 bridge: capture_service SHM -> TCP"};
  app.add_option("--registry", registry_path, "capture_service SHM registry path");
  app.add_option("--listen-host", listen_host, "TCP listen host");
  app.add_option("--listen-port", listen_port, "TCP listen port");
  app.add_option("--cam0-stream", cam0_stream, "Stream id for cam0");
  app.add_option("--cam1-stream", cam1_stream, "Stream id for cam1");
  app.add_option("--imu-stream", imu_stream, "Stream id for IMU");
  app.add_option("--duration", duration_s, "Run duration in seconds. 0 means until Ctrl+C");
  app.add_option("--poll-ms", poll_ms, "Poll period for SHM streams");
  app.add_option("--print-every", print_every, "Print every N stereo-ish camera frames");
  app.add_option("--max-backfill", max_backfill, "Max packets to backfill per stream after lag. 0 means no limit");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    std::cout << "== capture_net_bridge ==\n";
    std::cout << "registry: " << registry_path << "\n";
    std::cout << "listen: " << listen_host << ":" << listen_port << "\n";
    std::cout << "streams: " << cam0_stream << ", " << cam1_stream << ", " << imu_stream << "\n";

    capture_client::ShmCaptureTransport transport(registry_path, cam0_stream, cam1_stream, imu_stream);

    int listen_fd = capture_client::capture_net_v1::create_server_socket(listen_host, listen_port);
    ClientSet clients;
    std::thread accept_thread([&] { accept_loop(listen_fd, clients); });

    uint64_t last_cam0 = transport.cam0().latest_sequence();
    uint64_t last_cam1 = transport.cam1().latest_sequence();
    uint64_t last_imu = transport.imu().latest_sequence();
    uint64_t sent_messages = 0;
    uint64_t sent_camera0 = 0;
    uint64_t tick = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto last_print = t0;

    while (!g_stop) {
      const auto now = std::chrono::steady_clock::now();
      if (duration_s > 0.0 && std::chrono::duration<double>(now - t0).count() >= duration_s) {
        break;
      }

      const uint64_t before_cam0 = sent_messages;
      last_cam0 = send_new_from_reader(transport.cam0(), last_cam0, max_backfill, clients, sent_messages);
      if (sent_messages > before_cam0) sent_camera0 += (sent_messages - before_cam0);
      last_cam1 = send_new_from_reader(transport.cam1(), last_cam1, max_backfill, clients, sent_messages);
      last_imu = send_new_from_reader(transport.imu(), last_imu, max_backfill, clients, sent_messages);

      ++tick;
      if (print_every > 0 && tick % static_cast<uint64_t>(print_every) == 0) {
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        const double dt = std::chrono::duration<double>(now - last_print).count();
        last_print = now;
        std::cout << "[capture_net_bridge] clients=" << clients.size()
                  << " sent_messages=" << sent_messages
                  << " cam0_seq=" << last_cam0
                  << " cam1_seq=" << last_cam1
                  << " imu_seq=" << last_imu
                  << " elapsed=" << elapsed
                  << " dt=" << dt << "\n";
      }

      std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(poll_ms));
    }

    g_stop = true;
    ::shutdown(listen_fd, SHUT_RDWR);
    capture_client::capture_net_v1::close_fd(listen_fd);
    if (accept_thread.joinable()) accept_thread.join();

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "== capture_net_bridge summary ==\n";
    std::cout << "sent_messages: " << sent_messages << "\n";
    std::cout << "elapsed_s: " << elapsed << "\n";
    std::cout << "message_rate_hz: " << (elapsed > 0.0 ? sent_messages / elapsed : 0.0) << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
