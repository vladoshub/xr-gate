#include "capture_service_cpp/tcp_fanout.hpp"

#include <condition_variable>
#include <deque>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { if (s != INVALID_SOCKET) closesocket(s); }
static int last_socket_error() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { if (s >= 0) ::close(s); }
static int last_socket_error() { return errno; }
#endif

namespace xr_capture_cpp {
namespace {

constexpr const char* kHelloPrefix = "CAPHELLO ";
constexpr const char* kMsgPrefix = "CAPMSG1 ";

struct SocketRuntime {
  SocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
#endif
  }
  ~SocketRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

void send_all(socket_t sock, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const int n = ::send(sock, data + off, static_cast<int>(len - off), 0);
    if (n <= 0) throw std::runtime_error("socket send failed");
    off += static_cast<size_t>(n);
  }
}

void send_all(socket_t sock, const std::string& data) { send_all(sock, data.data(), data.size()); }
void send_all(socket_t sock, const std::vector<uint8_t>& data) { if (!data.empty()) send_all(sock, reinterpret_cast<const char*>(data.data()), data.size()); }

bool wait_readable(socket_t sock, int timeout_ms) {
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(sock, &rfds);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
  const int r = select(0, &rfds, nullptr, nullptr, &tv);
#else
  const int r = select(sock + 1, &rfds, nullptr, nullptr, &tv);
#endif
  return r > 0 && FD_ISSET(sock, &rfds);
}

std::string try_read_line(socket_t sock, int timeout_ms, size_t limit = 4096) {
  if (!wait_readable(sock, timeout_ms)) return {};
  std::string out;
  while (out.size() < limit) {
    char c = 0;
    const int n = ::recv(sock, &c, 1, 0);
    if (n <= 0) break;
    if (c == '\n') break;
    out.push_back(c);
    if (!wait_readable(sock, 50)) break;
  }
  return out;
}

std::string stream_info_json(const StreamSpec& spec) {
  std::ostringstream os;
  os << "{"
     << "\"stream_id\":\"" << json_escape(spec.stream_id) << "\","
     << "\"kind\":\"" << json_escape(spec.kind_name) << "\","
     << "\"frame_id\":\"" << json_escape(spec.frame_id) << "\","
     << "\"width\":" << spec.width << ","
     << "\"height\":" << spec.height << ","
     << "\"format_code\":" << spec.format_code << ","
     << "\"format_name\":\"" << json_escape(spec.format_name) << "\","
     << "\"payload_size\":" << spec.payload_size << ","
     << "\"slot_count\":" << spec.slot_count << ","
     << "\"description\":\"" << json_escape(spec.description) << "\","
     << "\"transport\":\"tcp\","
     << "\"protocol\":\"" << kTcpProtocolName << "\""
     << "}";
  return os.str();
}

struct TcpMessage {
  std::string header_json;
  std::vector<uint8_t> payload;
};

class Client {
 public:
  Client(TcpFanoutPublisher::Impl* server, socket_t sock, int id);
  ~Client();
  void start();
  void stop();
  void enqueue(TcpMessage msg);
  bool alive() const { return alive_.load(); }

 private:
  void run();
  void send_hello();
  void read_subscribe();
  bool wants(const std::string& stream_id) const;

  TcpFanoutPublisher::Impl* server_ = nullptr;
  socket_t sock_ = kInvalidSocket;
  int id_ = 0;
  std::atomic<bool> alive_{true};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<TcpMessage> queue_;
  std::unordered_set<std::string> subscriptions_;
  bool subscribe_all_ = true;
  size_t dropped_ = 0;
};

}  // namespace

struct TcpFanoutPublisher::Impl {
  std::unordered_map<std::string, StreamSpec> specs;
  std::string bind_host;
  int port = 45660;
  std::string namespace_name;
  int client_queue_size = 256;
  SocketRuntime socket_runtime;
  socket_t server_sock = kInvalidSocket;
  std::atomic<bool> stop{false};
  std::thread accept_thread;
  std::mutex clients_mutex;
  std::vector<std::shared_ptr<Client>> clients;
  int next_client_id = 1;

  Impl(std::unordered_map<std::string, StreamSpec> specs_, std::string bind_host_, int port_, std::string namespace_name_, int client_queue_size_)
      : specs(std::move(specs_)), bind_host(std::move(bind_host_)), port(port_), namespace_name(std::move(namespace_name_)), client_queue_size(client_queue_size_) {}

  void accept_loop() {
    while (!stop.load()) {
      if (!wait_readable(server_sock, 500)) continue;
      sockaddr_storage addr{};
#ifdef _WIN32
      int addr_len = sizeof(addr);
#else
      socklen_t addr_len = sizeof(addr);
#endif
      socket_t client_sock = ::accept(server_sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
      if (client_sock == kInvalidSocket) {
        if (!stop.load()) std::cerr << "[capture_service_cpp][tcp] accept failed err=" << last_socket_error() << std::endl;
        continue;
      }
      int one = 1;
      setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
      std::shared_ptr<Client> client;
      {
        std::lock_guard<std::mutex> lock(clients_mutex);
        client = std::make_shared<Client>(this, client_sock, next_client_id++);
        clients.push_back(client);
      }
      client->start();
    }
  }
};

namespace {

Client::Client(TcpFanoutPublisher::Impl* server, socket_t sock, int id) : server_(server), sock_(sock), id_(id) {}
Client::~Client() { stop(); }

void Client::start() { thread_ = std::thread(&Client::run, this); }

void Client::stop() {
  bool expected = true;
  if (alive_.compare_exchange_strong(expected, false)) {
#ifdef _WIN32
    shutdown(sock_, SD_BOTH);
#else
    shutdown(sock_, SHUT_RDWR);
#endif
    close_socket(sock_);
    cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
}

void Client::enqueue(TcpMessage msg) {
  if (!alive_.load()) return;
  // Stream id is visible in header JSON; cheap enough for this experimental path.
  if (!subscribe_all_) {
    bool ok = false;
    for (const auto& sid : subscriptions_) {
      const std::string needle = "\"stream_id\":\"" + sid + "\"";
      if (msg.header_json.find(needle) != std::string::npos) { ok = true; break; }
    }
    if (!ok) return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= static_cast<size_t>(server_->client_queue_size)) {
    queue_.pop_front();
    ++dropped_;
  }
  queue_.push_back(std::move(msg));
  cv_.notify_one();
}

void Client::send_hello() {
  std::ostringstream streams;
  bool first = true;
  for (const auto& kv : server_->specs) {
    if (!first) streams << ",";
    first = false;
    streams << "\"" << json_escape(kv.first) << "\":" << stream_info_json(kv.second);
  }
  std::ostringstream hello;
  hello << "{"
        << "\"protocol\":\"" << kTcpProtocolName << "\","
        << "\"server_time_ns\":" << wall_ns() << ","
        << "\"namespace\":\"" << json_escape(server_->namespace_name) << "\","
        << "\"subscribe\":\"send line: SUBSCRIBE stream0,stream1 or SUBSCRIBE *\","
        << "\"streams\":{" << streams.str() << "}"
        << "}";
  const std::string raw = hello.str();
  std::ostringstream line;
  line << kHelloPrefix << raw.size() << "\n";
  send_all(sock_, line.str());
  send_all(sock_, raw);
}

void Client::read_subscribe() {
  const std::string line = try_read_line(sock_, 250);
  if (line.empty() || line == "SUBSCRIBE *") {
    subscribe_all_ = true;
    return;
  }
  const std::string prefix = "SUBSCRIBE ";
  if (line.rfind(prefix, 0) != 0) {
    subscribe_all_ = true;
    return;
  }
  subscribe_all_ = false;
  std::string value = line.substr(prefix.size());
  std::string cur;
  for (char c : value) {
    if (c == ',') {
      if (!cur.empty()) subscriptions_.insert(cur);
      cur.clear();
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) subscriptions_.insert(cur);
  if (subscriptions_.empty()) subscribe_all_ = true;
}

void Client::run() {
  try {
    send_hello();
    read_subscribe();
    std::cerr << "[capture_service_cpp][tcp] client " << id_ << " subscribed=" << (subscribe_all_ ? "*" : "filtered") << std::endl;
    while (alive_.load()) {
      TcpMessage msg;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return !alive_.load() || !queue_.empty(); });
        if (!alive_.load()) break;
        msg = std::move(queue_.front());
        queue_.pop_front();
      }
      std::ostringstream line;
      line << kMsgPrefix << msg.header_json.size() << " " << msg.payload.size() << "\n";
      send_all(sock_, line.str());
      send_all(sock_, msg.header_json);
      send_all(sock_, msg.payload);
    }
  } catch (const std::exception& e) {
    std::cerr << "[capture_service_cpp][tcp] client " << id_ << " disconnected: " << e.what() << std::endl;
  }
  alive_.store(false);
}

}  // namespace

TcpFanoutPublisher::TcpFanoutPublisher(std::unordered_map<std::string, StreamSpec> specs,
                                       std::string bind_host,
                                       int port,
                                       std::string namespace_name,
                                       int client_queue_size)
    : impl_(new Impl(std::move(specs), std::move(bind_host), port, std::move(namespace_name), client_queue_size)) {}

TcpFanoutPublisher::~TcpFanoutPublisher() { stop(); }

void TcpFanoutPublisher::start() {
  if (impl_->server_sock != kInvalidSocket) return;
  impl_->server_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (impl_->server_sock == kInvalidSocket) throw std::runtime_error("TCP socket() failed");
  int one = 1;
  setsockopt(impl_->server_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(impl_->port));
  std::string bind = impl_->bind_host.empty() ? "0.0.0.0" : impl_->bind_host;
  if (bind == "0.0.0.0" || bind == "*") addr.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1) throw std::runtime_error("invalid TCP bind host: " + bind);
  if (::bind(impl_->server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) throw std::runtime_error("TCP bind failed err=" + std::to_string(last_socket_error()));
  if (::listen(impl_->server_sock, 16) != 0) throw std::runtime_error("TCP listen failed");
  impl_->accept_thread = std::thread(&TcpFanoutPublisher::Impl::accept_loop, impl_.get());
  std::cerr << "[capture_service_cpp][tcp] listening on " << impl_->bind_host << ":" << impl_->port << std::endl;
}

void TcpFanoutPublisher::stop() {
  if (!impl_) return;
  impl_->stop.store(true);
  if (impl_->server_sock != kInvalidSocket) {
#ifdef _WIN32
    shutdown(impl_->server_sock, SD_BOTH);
#else
    shutdown(impl_->server_sock, SHUT_RDWR);
#endif
    close_socket(impl_->server_sock);
    impl_->server_sock = kInvalidSocket;
  }
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
  std::vector<std::shared_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    clients.swap(impl_->clients);
  }
  for (auto& client : clients) client->stop();
}

void TcpFanoutPublisher::publish(const std::string& stream_id,
                                 const uint8_t* payload,
                                 size_t payload_len,
                                 uint64_t sequence,
                                 uint64_t timestamp_ns,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t format_code,
                                 uint32_t flags,
                                 const std::string& frame_id) {
  auto it = impl_->specs.find(stream_id);
  if (it == impl_->specs.end()) return;
  const auto& spec = it->second;
  std::ostringstream header;
  header << "{"
         << "\"protocol\":\"" << kTcpProtocolName << "\","
         << "\"stream_id\":\"" << json_escape(stream_id) << "\","
         << "\"sequence\":" << sequence << ","
         << "\"timestamp_ns\":" << timestamp_ns << ","
         << "\"monotonic_ns\":" << steady_ns() << ","
         << "\"payload_size\":" << payload_len << ","
         << "\"width\":" << (width ? width : spec.width) << ","
         << "\"height\":" << (height ? height : spec.height) << ","
         << "\"format_code\":" << (format_code ? format_code : spec.format_code) << ","
         << "\"format_name\":\"" << json_escape(spec.format_name) << "\","
         << "\"flags\":" << flags << ","
         << "\"frame_id\":\"" << json_escape(frame_id.empty() ? spec.frame_id : frame_id) << "\""
         << "}";
  TcpMessage msg;
  msg.header_json = header.str();
  msg.payload.assign(payload, payload + payload_len);
  std::vector<std::shared_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    clients = impl_->clients;
  }
  for (auto& client : clients) if (client->alive()) client->enqueue(msg);
}

std::string TcpFanoutPublisher::registry_entry(const StreamSpec& spec) const {
  std::ostringstream os;
  os << "{\n"
     << "      \"description\": \"" << json_escape(spec.description) << "\",\n"
     << "      \"format_code\": " << spec.format_code << ",\n"
     << "      \"format_name\": \"" << spec.format_name << "\",\n"
     << "      \"frame_id\": \"" << spec.frame_id << "\",\n"
     << "      \"height\": " << spec.height << ",\n"
     << "      \"kind\": \"" << spec.kind_name << "\",\n"
     << "      \"namespace\": \"" << json_escape(impl_->namespace_name) << "\",\n"
     << "      \"payload_size\": " << spec.payload_size << ",\n"
     << "      \"protocol\": \"" << kTcpProtocolName << "\",\n"
     << "      \"slot_count\": " << spec.slot_count << ",\n"
     << "      \"stream_id\": \"" << spec.stream_id << "\",\n"
     << "      \"tcp_host\": \"" << advertise_host() << "\",\n"
     << "      \"tcp_port\": " << impl_->port << ",\n"
     << "      \"transport\": \"tcp\",\n"
     << "      \"width\": " << spec.width << "\n"
     << "    }";
  return os.str();
}

std::string TcpFanoutPublisher::advertise_host() const {
  if (impl_->bind_host.empty() || impl_->bind_host == "0.0.0.0" || impl_->bind_host == "::" || impl_->bind_host == "*") return "127.0.0.1";
  return impl_->bind_host;
}

}  // namespace xr_capture_cpp
