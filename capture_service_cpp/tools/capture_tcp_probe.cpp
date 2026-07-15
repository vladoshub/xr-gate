#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t socket) {
  if (socket != INVALID_SOCKET) closesocket(socket);
}
static int socket_error() { return WSAGetLastError(); }
static bool connect_in_progress(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
}
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t socket) {
  if (socket >= 0) ::close(socket);
}
static int socket_error() { return errno; }
static bool connect_in_progress(int error) { return error == EINPROGRESS || error == EWOULDBLOCK; }
#endif

namespace {

constexpr const char* kHelloPrefix = "CAPHELLO ";
constexpr std::size_t kMaxHelloSize = 1024U * 1024U;

struct SocketRuntime {
  SocketRuntime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
#endif
  }
  ~SocketRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

class SocketHandle {
 public:
  explicit SocketHandle(socket_t socket = kInvalidSocket) : socket_(socket) {}
  ~SocketHandle() { close_socket(socket_); }

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  SocketHandle(SocketHandle&& other) noexcept : socket_(other.release()) {}
  SocketHandle& operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
      close_socket(socket_);
      socket_ = other.release();
    }
    return *this;
  }

  socket_t get() const { return socket_; }
  bool valid() const { return socket_ != kInvalidSocket; }
  socket_t release() {
    const socket_t value = socket_;
    socket_ = kInvalidSocket;
    return value;
  }

 private:
  socket_t socket_ = kInvalidSocket;
};

using Clock = std::chrono::steady_clock;

int remaining_ms(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) return 0;
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
  if (value <= 0) return 1;
  return static_cast<int>(std::min<int64_t>(value, std::numeric_limits<int>::max()));
}

void set_nonblocking(const socket_t socket, const bool enabled) {
#ifdef _WIN32
  u_long mode = enabled ? 1UL : 0UL;
  if (ioctlsocket(socket, FIONBIO, &mode) != 0) {
    throw std::runtime_error("ioctlsocket(FIONBIO) failed");
  }
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0) throw std::runtime_error("fcntl(F_GETFL) failed");
  const int next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (fcntl(socket, F_SETFL, next) != 0) {
    throw std::runtime_error("fcntl(F_SETFL) failed");
  }
#endif
}

bool wait_socket(const socket_t socket, const bool readable, const int timeout_ms) {
  fd_set read_set;
  fd_set write_set;
  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  if (readable) {
    FD_SET(socket, &read_set);
  } else {
    FD_SET(socket, &write_set);
  }

  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
  const int result = select(0, readable ? &read_set : nullptr,
                            readable ? nullptr : &write_set, nullptr, &timeout);
#else
  const int result = select(socket + 1, readable ? &read_set : nullptr,
                            readable ? nullptr : &write_set, nullptr, &timeout);
#endif
  if (result < 0) throw std::runtime_error("select failed");
  return result > 0;
}

SocketHandle connect_tcp(const std::string& host,
                         const int port,
                         const Clock::time_point deadline) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(port);
  const int lookup = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
  if (lookup != 0) {
#ifdef _WIN32
    throw std::runtime_error("getaddrinfo failed: " + std::to_string(lookup));
#else
    throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(lookup));
#endif
  }

  struct AddrInfoGuard {
    addrinfo* value = nullptr;
    ~AddrInfoGuard() { if (value) freeaddrinfo(value); }
  } guard{results};

  int last_error = 0;
  for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
    SocketHandle socket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (!socket.valid()) {
      last_error = socket_error();
      continue;
    }

    set_nonblocking(socket.get(), true);
    const int result = ::connect(socket.get(), address->ai_addr,
#ifdef _WIN32
                                 static_cast<int>(address->ai_addrlen)
#else
                                 address->ai_addrlen
#endif
    );
    if (result == 0) {
      set_nonblocking(socket.get(), false);
      return socket;
    }

    last_error = socket_error();
    if (!connect_in_progress(last_error)) continue;

    const int timeout_ms = remaining_ms(deadline);
    if (timeout_ms <= 0 || !wait_socket(socket.get(), false, timeout_ms)) {
      last_error = 0;
      continue;
    }

    int pending_error = 0;
#ifdef _WIN32
    int error_size = sizeof(pending_error);
#else
    socklen_t error_size = sizeof(pending_error);
#endif
    if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&pending_error), &error_size) != 0 ||
        pending_error != 0) {
      last_error = pending_error != 0 ? pending_error : socket_error();
      continue;
    }

    set_nonblocking(socket.get(), false);
    return socket;
  }

  if (remaining_ms(deadline) <= 0) {
    throw std::runtime_error("TCP connect timed out");
  }
  throw std::runtime_error("TCP connect failed, socket error=" + std::to_string(last_error));
}

std::string read_line(const socket_t socket,
                      const Clock::time_point deadline,
                      const std::size_t max_size) {
  std::string output;
  while (output.size() < max_size) {
    const int timeout_ms = remaining_ms(deadline);
    if (timeout_ms <= 0 || !wait_socket(socket, true, timeout_ms)) {
      throw std::runtime_error("timed out while reading CAPHELLO header");
    }
    char value = 0;
    const int received = ::recv(socket, &value, 1, 0);
    if (received <= 0) throw std::runtime_error("connection closed while reading CAPHELLO header");
    if (value == '\n') return output;
    if (value != '\r') output.push_back(value);
  }
  throw std::runtime_error("CAPHELLO header is too long");
}

std::string read_exact(const socket_t socket,
                       const Clock::time_point deadline,
                       const std::size_t size) {
  std::string output(size, '\0');
  std::size_t offset = 0;
  while (offset < size) {
    const int timeout_ms = remaining_ms(deadline);
    if (timeout_ms <= 0 || !wait_socket(socket, true, timeout_ms)) {
      throw std::runtime_error("timed out while reading CAPHELLO JSON");
    }
    const std::size_t remaining = size - offset;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 64U * 1024U));
    const int received = ::recv(socket, output.data() + offset, chunk, 0);
    if (received <= 0) throw std::runtime_error("connection closed while reading CAPHELLO JSON");
    offset += static_cast<std::size_t>(received);
  }
  return output;
}

void send_close(const socket_t socket) {
  constexpr char request[] = "CLOSE\n";
#ifdef _WIN32
  (void)::send(socket, request, static_cast<int>(sizeof(request) - 1U), 0);
#else
  (void)::send(socket, request, sizeof(request) - 1U, MSG_NOSIGNAL);
#endif
}

std::size_t skip_space(const std::string& text, std::size_t index) {
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
  return index;
}

std::string extract_json_string(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  std::size_t position = 0;
  while ((position = json.find(needle, position)) != std::string::npos) {
    std::size_t index = skip_space(json, position + needle.size());
    if (index >= json.size() || json[index] != ':') {
      position += needle.size();
      continue;
    }
    index = skip_space(json, index + 1U);
    if (index >= json.size() || json[index] != '"') return {};
    ++index;

    std::string output;
    while (index < json.size()) {
      const char value = json[index++];
      if (value == '"') return output;
      if (value != '\\') {
        output.push_back(value);
        continue;
      }
      if (index >= json.size()) return {};
      const char escaped = json[index++];
      switch (escaped) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        default: return {};
      }
    }
    return {};
  }
  return {};
}

bool valid_profile_name(const std::string& profile) {
  return !profile.empty() &&
         std::all_of(profile.begin(), profile.end(), [](const unsigned char value) {
           return std::isalnum(value) || value == '_' || value == '-' || value == '.';
         });
}

struct Options {
  std::string host = "127.0.0.1";
  int port = 45660;
  int timeout_ms = 1500;
  enum class Output { Profile, Namespace, Json } output = Output::Profile;
};

void print_usage(const char* executable) {
  std::cout
      << "Usage: " << executable << " [OPTIONS]\n"
      << "Read capture_service_cpp TCP CAPHELLO metadata and exit.\n\n"
      << "Options:\n"
      << "  --host HOST          Capture TCP host (default: 127.0.0.1)\n"
      << "  --port PORT          Capture TCP port (default: 45660)\n"
      << "  --timeout-ms N       Total connect/read timeout (default: 1500)\n"
      << "  --print-profile      Print profile metadata (default)\n"
      << "  --print-namespace    Print namespace metadata\n"
      << "  --print-json         Print the complete CAPHELLO JSON\n"
      << "  -h, --help           Show this help\n";
}

int parse_int(const std::string& value, const char* option) {
  std::size_t consumed = 0;
  const long parsed = std::stol(value, &consumed, 10);
  if (consumed != value.size() || parsed < 1 || parsed > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("invalid value for ") + option + ": " + value);
  }
  return static_cast<int>(parsed);
}

Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto require_value = [&](const char* option) -> std::string {
      if (index + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + option);
      return argv[++index];
    };

    if (argument == "--host") options.host = require_value("--host");
    else if (argument == "--port") options.port = parse_int(require_value("--port"), "--port");
    else if (argument == "--timeout-ms") options.timeout_ms = parse_int(require_value("--timeout-ms"), "--timeout-ms");
    else if (argument == "--print-profile") options.output = Options::Output::Profile;
    else if (argument == "--print-namespace") options.output = Options::Output::Namespace;
    else if (argument == "--print-json") options.output = Options::Output::Json;
    else if (argument == "-h" || argument == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + argument);
    }
  }
  if (options.host.empty()) throw std::runtime_error("--host must not be empty");
  if (options.port > 65535) throw std::runtime_error("--port is out of range");
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    SocketRuntime socket_runtime;
    const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
    SocketHandle socket = connect_tcp(options.host, options.port, deadline);

    const std::string header = read_line(socket.get(), deadline, 256);
    if (header.rfind(kHelloPrefix, 0) != 0) {
      throw std::runtime_error("unexpected TCP greeting: " + header);
    }

    const std::string length_text = header.substr(std::char_traits<char>::length(kHelloPrefix));
    std::size_t consumed = 0;
    const unsigned long long parsed_length = std::stoull(length_text, &consumed, 10);
    if (consumed != length_text.size() || parsed_length == 0 || parsed_length > kMaxHelloSize) {
      throw std::runtime_error("invalid CAPHELLO JSON length: " + length_text);
    }

    const std::string hello = read_exact(socket.get(), deadline, static_cast<std::size_t>(parsed_length));
    send_close(socket.get());

    if (options.output == Options::Output::Json) {
      std::cout << hello << '\n';
      return 0;
    }

    const char* key = options.output == Options::Output::Profile ? "profile" : "namespace";
    const std::string value = extract_json_string(hello, key);
    if (value.empty()) {
      std::cerr << "[capture_tcp_probe][ERROR] CAPHELLO does not contain a non-empty " << key << "\n";
      return 3;
    }
    if (options.output == Options::Output::Profile && !valid_profile_name(value)) {
      std::cerr << "[capture_tcp_probe][ERROR] CAPHELLO contains an invalid profile name: " << value << "\n";
      return 3;
    }

    std::cout << value << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "[capture_tcp_probe][ERROR] " << error.what() << '\n';
    return 4;
  }
}
