#include <xr_override_controller/config_io.hpp>
#include <xr_override_controller/controller_input_publisher.hpp>
#include <xr_runtime/config/controller_orientation_offset.hpp>
#include <xr_runtime/contracts/runtime_adapter.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace xr_override_controller;
using namespace xr_runtime;
using namespace std::chrono_literals;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

void config_roundtrip(const std::filesystem::path& dir) {
  const auto path = dir / "profile.json";
  nlohmann::json document = {
      {"name", "test"}, {"devices", nlohmann::json::array()},
      {"provider_options", {{"gearvr_ble", {{"gyro_bias.max_bias_rad_s", .12}}}}},
      {"optic_runtime_head_offset", {
          {"enabled", true}, {"multiply_order", "post"},
          {"left", {{"rz_deg", -40.0}, {"aim_offset", {{"pitch_deg", -30.0}, {"yaw_deg", -30.0}}}}},
          {"right", {{"enabled", false}, {"ry_deg", 80.0}}}}}};
  std::ofstream(path) << document;
  auto config = load_config_file(path);
  require(config.provider_options.at("gearvr_ble").at("gyro_bias.max_bias_rad_s") == "0.12", "provider config lost");
  require(config.runtime_profile.left.flags == (CONTROLLER_RUNTIME_OFFSET_PROVIDED | CONTROLLER_RUNTIME_OFFSET_ENABLED), "left profile flags");
  require(config.runtime_profile.right.flags == CONTROLLER_RUNTIME_OFFSET_PROVIDED, "explicit disabled offset must replace fallback");
  const float expected[] = {-.149418119f, -.320428191f, -.382056608f, .853834146f};
  for (int i = 0; i < 4; ++i)
    require(std::abs(config.runtime_profile.left.rotation_xyzw[i] - expected[i]) < 1e-6, "aim composition changed");
  save_config_file(config, path);
  auto saved = nlohmann::json::parse(std::ifstream(path));
  require(saved.at("optic_runtime_head_offset") == document.at("optic_runtime_head_offset"), "human-readable offset changed during save");
  require(load_config_file(path).provider_options == config.provider_options, "provider save/load changed values");
  document["optic_runtime_head_offset"] = {{"enabled", false}};
  std::ofstream(path) << document;
  config = load_config_file(path);
  require(config.runtime_profile.left.flags == CONTROLLER_RUNTIME_OFFSET_PROVIDED &&
          config.runtime_profile.right.flags == CONTROLLER_RUNTIME_OFFSET_PROVIDED, "root disabled should suppress both fallbacks");
  document.erase("optic_runtime_head_offset");
  document.erase("provider_options");
  std::ofstream(path) << document;
  config = load_config_file(path);
  require(config.runtime_profile.left.flags == 0 && config.runtime_profile.right.flags == 0, "legacy profile must use runtime defaults");
  document["optic_runtime_head_offset"] = {{"left", {{"multiply_order", "invalid"}}}};
  std::ofstream(path) << document;
  bool rejected = false;
  try { load_config_file(path); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid runtime offset accepted");
}

#ifndef _WIN32
struct Socket {
  int fd = -1;
  Socket() : fd(socket(AF_INET, SOCK_STREAM, 0)) { require(fd >= 0, "socket failed"); }
  ~Socket() { if (fd >= 0) close(fd); }
  Socket(const Socket&) = delete;
};

int free_port() {
  Socket socket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "port bind failed");
  socklen_t size = sizeof(address);
  require(getsockname(socket.fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname failed");
  return ntohs(address.sin_port);
}

void connect(Socket& socket, int port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  require(::connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect failed");
  const timeval timeout{2, 0};
  setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

void read_exact(int socket, void* output, size_t size) {
  auto* data = static_cast<char*>(output);
  while (size) {
    const auto count = recv(socket, data, size, 0);
    require(count > 0, "TCP read failed");
    data += count;
    size -= static_cast<size_t>(count);
  }
}

struct Source {
  PublishConfig config;
  std::unique_ptr<ControllerInputTcpPublisher> publisher;
  std::mutex mutex;
  OutputState state;
  std::atomic<bool> stop{false};
  std::thread thread;
  Source() {
    config.transport = "tcp";
    config.tcp_port = free_port();
    state.left.configured = state.left.connected = true;
    state.runtime_profile.left = parse_controller_orientation_offset({{"ry_deg", 35}}, "left");
    publisher = std::make_unique<ControllerInputTcpPublisher>(config);
    thread = std::thread([this] {
      while (!stop) {
        { std::lock_guard<std::mutex> lock(mutex); publisher->publish(state); }
        std::this_thread::sleep_for(5ms);
      }
    });
  }
  ~Source() { stop = true; thread.join(); }
};

void tcp_profiles() {
  Source source;
  // Old clients never negotiate, and must keep receiving plain V3 samples.
  Socket legacy;
  connect(legacy, source.config.tcp_port);
  for (int i = 0; i < 3; ++i) {
    ControllerInputTcpHeader header;
    ControllerInputV3 frame;
    read_exact(legacy.fd, &header, sizeof(header));
    require(header.magic == CONTROLLER_INPUT_TCP_MAGIC_V3 && header.payload_size == sizeof(frame), "legacy client received metadata");
    read_exact(legacy.fd, &frame, sizeof(frame));
  }
  // A split hello exercises partial nonblocking negotiation reads.
  Socket negotiated;
  connect(negotiated, source.config.tcp_port);
  const uint64_t hello = CONTROLLER_INPUT_TCP_PROFILE_HELLO;
  require(send(negotiated.fd, &hello, 3, MSG_NOSIGNAL) == 3, "hello send");
  std::this_thread::sleep_for(15ms);
  require(send(negotiated.fd, reinterpret_cast<const char*>(&hello) + 3, 5, MSG_NOSIGNAL) == 5, "hello suffix send");
  int profiles = 0;
  uint64_t revision = 0;
  for (int frames = 0; frames < 10;) {
    ControllerInputTcpHeader header;
    read_exact(negotiated.fd, &header, sizeof(header));
    if (header.magic == CONTROLLER_INPUT_TCP_PROFILE_MAGIC) {
      ControllerRuntimeProfileV1 profile;
      read_exact(negotiated.fd, &profile, sizeof(profile));
      ++profiles;
      revision = profile.revision;
    } else {
      ControllerInputV3 frame;
      read_exact(negotiated.fd, &frame, sizeof(frame));
      ++frames;
    }
  }
  require(profiles == 1 && revision == 1, "profile must be sent once per connection");
  {
    std::lock_guard<std::mutex> lock(source.mutex);
    source.state.runtime_profile.left = parse_controller_orientation_offset({{"enabled", false}}, "left");
  }
  for (;;) {
    ControllerInputTcpHeader header;
    read_exact(negotiated.fd, &header, sizeof(header));
    if (header.magic == CONTROLLER_INPUT_TCP_PROFILE_MAGIC) {
      ControllerRuntimeProfileV1 profile;
      read_exact(negotiated.fd, &profile, sizeof(profile));
      require(profile.revision == 2 && profile.left.flags == CONTROLLER_RUNTIME_OFFSET_PROVIDED, "updated profile lost");
      break;
    }
    ControllerInputV3 frame;
    read_exact(negotiated.fd, &frame, sizeof(frame));
  }
  // A fresh/reconnected reader obtains the latest complete snapshot immediately.
  for (int reconnect = 0; reconnect < 2; ++reconnect) {
    ControllerInputTcpClient client("127.0.0.1", source.config.tcp_port);
    client.read_next();
    require(client.runtime_profile().revision == 2 &&
            client.runtime_profile().left.flags == CONTROLLER_RUNTIME_OFFSET_PROVIDED, "reconnect snapshot lost");
  }
}

void legacy_publisher() {
  Socket listener;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener.fd, 1) == 0, "legacy listen failed");
  socklen_t size = sizeof(address);
  getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &size);
  std::thread server([&] {
    const int peer = accept(listener.fd, nullptr, nullptr);
    uint64_t hello;
    read_exact(peer, &hello, sizeof(hello));
    ControllerInputTcpHeader header;
    ControllerInputV3 frame;
    frame.sequence = 1;
    send(peer, &header, sizeof(header), MSG_NOSIGNAL);
    send(peer, &frame, sizeof(frame), MSG_NOSIGNAL);
    close(peer);
  });
  ControllerInputTcpClient client("127.0.0.1", ntohs(address.sin_port));
  const auto frame = client.read_next();
  server.join();
  require(frame.sequence == 1 && client.runtime_profile().revision == 0, "legacy publisher fallback broken");
}

void shm_profiles(const std::filesystem::path& dir) {
  PublishConfig config;
  config.registry_path = (dir / "streams.json").string();
  config.shm_name = "xr_profile_test_" + std::to_string(getpid());
  config.slot_count = 4;
  {
    ControllerInputShmPublisher publisher(config);
    OutputState state;
    state.runtime_profile.left = parse_controller_orientation_offset({{"rx_deg", -30}}, "left");
    publisher.publish(state);
    // Attach after publication: the profile is persistent, not an event slot.
    TrackingRingReader<ControllerInputV3> reader(stream_info_from_registry(config.registry_path, config.stream_id), CONTROLLER_INPUT_V3_FORMAT_NAME);
    const auto* bytes = reader.header_extension(CONTROLLER_RUNTIME_PROFILE_SHM_OFFSET, sizeof(ControllerRuntimeProfileShmV1));
    require(bytes != nullptr, "missing SHM extension");
    const auto& extension = *reinterpret_cast<const ControllerRuntimeProfileShmV1*>(bytes);
    ControllerRuntimeProfileV1 profile;
    require(read_controller_runtime_profile(extension, profile) && profile.revision == 1, "late reader missed SHM profile");
    for (int i = 0; i < 10; ++i) publisher.publish(state);
    require(read_controller_runtime_profile(extension, profile) && profile.revision == 1, "unchanged SHM profile rewritten");
    state.runtime_profile = {};
    publisher.publish(state);
    require(read_controller_runtime_profile(extension, profile) && profile.revision == 2 && profile.left.flags == 0, "SHM profile removal lost");
  }
  shm_unlink(("/" + config.shm_name).c_str());
}
#endif

int main() {
  try {
    const auto dir = std::filesystem::temp_directory_path() / ("xr_profile_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    config_roundtrip(dir);
#ifndef _WIN32
    tcp_profiles();
    legacy_publisher();
    shm_profiles(dir);
#endif
    std::filesystem::remove_all(dir);
    std::cout << "controller runtime profile tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
