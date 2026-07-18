#include "../xiao_nrf54l15_serial_transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace xr_override_controller::xiao_nrf54l15 {
namespace {

uint64_t monotonic_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string trim_copy(std::string value) {
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
          value.back() == '\t')) {
    value.pop_back();
  }
  size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t')) {
    ++first;
  }
  return value.substr(first);
}

std::string read_first_line(const fs::path& path) {
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) return {};
  return trim_copy(line);
}

uint16_t parse_hex_u16(const std::string& value) {
  if (value.empty()) return 0;
  try {
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed, 16);
    if (consumed != value.size()) return 0;
    return static_cast<uint16_t>(parsed & 0xffffu);
  } catch (...) {
    return 0;
  }
}

fs::path weak_canonical_or_self(const fs::path& path) {
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(path, ec);
  return ec ? path : canonical;
}

std::string find_stable_symlink(const fs::path& directory,
                                const fs::path& target) {
  std::error_code ec;
  if (!fs::exists(directory, ec)) return {};
  const fs::path wanted = weak_canonical_or_self(target);
  for (const auto& entry : fs::directory_iterator(directory, ec)) {
    if (ec) break;
    if (!entry.is_symlink(ec)) continue;
    const fs::path resolved = weak_canonical_or_self(entry.path());
    if (resolved == wanted) return entry.path().string();
  }
  return {};
}

struct UsbAttributes {
  std::string serial;
  std::string manufacturer;
  std::string product_name;
  std::string phys;
  uint16_t vendor = 0;
  uint16_t product = 0;
  uint16_t version = 0;
};

UsbAttributes usb_attributes_for_tty(const fs::path& canonical_port) {
  UsbAttributes out;
  const std::string tty_name = canonical_port.filename().string();
  if (tty_name.empty()) return out;

  std::error_code ec;
  fs::path current = fs::weakly_canonical(
      fs::path("/sys/class/tty") / tty_name / "device", ec);
  if (ec) return out;

  for (int depth = 0; depth < 10 && !current.empty(); ++depth) {
    const std::string vendor = read_first_line(current / "idVendor");
    const std::string product = read_first_line(current / "idProduct");
    if (!vendor.empty() || !product.empty()) {
      out.vendor = parse_hex_u16(vendor);
      out.product = parse_hex_u16(product);
      out.version = parse_hex_u16(read_first_line(current / "bcdDevice"));
      out.serial = read_first_line(current / "serial");
      out.manufacturer = read_first_line(current / "manufacturer");
      out.product_name = read_first_line(current / "product");
      out.phys = current.string();
      break;
    }
    const fs::path parent = current.parent_path();
    if (parent == current) break;
    current = parent;
  }
  return out;
}

std::string stable_id_for(const std::string& serial,
                          const std::string& by_id,
                          const std::string& by_path,
                          const fs::path& canonical_port,
                          const fs::path& requested_path,
                          bool explicit_candidate) {
  if (explicit_candidate) {
    return "xiao_nrf54l15:explicit:" + requested_path.string();
  }
  if (!serial.empty()) return "xiao_nrf54l15:serial:" + serial;
  if (!by_id.empty()) {
    return "xiao_nrf54l15:by-id:" + fs::path(by_id).filename().string();
  }
  if (!by_path.empty()) {
    return "xiao_nrf54l15:by-path:" + fs::path(by_path).filename().string();
  }
  const std::string fallback = !canonical_port.empty()
                                   ? canonical_port.string()
                                   : requested_path.string();
  return "xiao_nrf54l15:path:" + fallback;
}

speed_t baud_constant(uint32_t baud_rate) {
  switch (baud_rate) {
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default:
      throw std::runtime_error("requested UART baud is not supported by this Linux libc");
  }
}

bool configure_serial(int fd, uint32_t baud_rate, std::string& error) {
  termios tty{};
  if (tcgetattr(fd, &tty) != 0) {
    error = std::string("tcgetattr: ") + std::strerror(errno);
    return false;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= CLOCAL | CREAD;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
#ifdef CRTSCTS
  tty.c_cflag &= ~CRTSCTS;
#endif
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  const speed_t speed = baud_constant(baud_rate);
  if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0) {
    error = std::string("cfset speed: ") + std::strerror(errno);
    return false;
  }
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    error = std::string("tcsetattr: ") + std::strerror(errno);
    return false;
  }
  (void)tcflush(fd, TCIFLUSH);
  return true;
}

struct Candidate {
  fs::path requested_path;
  fs::path canonical_port;
  SerialDeviceSnapshot snapshot;
};

Candidate make_candidate(const fs::path& requested_path, bool explicit_candidate) {
  Candidate out;
  out.requested_path = requested_path;
  out.canonical_port = weak_canonical_or_self(requested_path);

  std::error_code ec;
  if (!fs::exists(requested_path, ec)) {
    out.canonical_port = requested_path;
  }

  out.snapshot.port_path = requested_path.string();
  out.snapshot.explicit_candidate = explicit_candidate;
  out.snapshot.by_id_path = find_stable_symlink("/dev/serial/by-id", out.canonical_port);
  out.snapshot.by_path = find_stable_symlink("/dev/serial/by-path", out.canonical_port);
  if (requested_path.string().rfind("/dev/serial/by-id/", 0) == 0) {
    out.snapshot.by_id_path = requested_path.string();
  }
  if (requested_path.string().rfind("/dev/serial/by-path/", 0) == 0) {
    out.snapshot.by_path = requested_path.string();
  }

  const UsbAttributes usb = usb_attributes_for_tty(out.canonical_port);
  out.snapshot.serial = usb.serial;
  out.snapshot.vendor = usb.vendor;
  out.snapshot.product = usb.product;
  out.snapshot.version = usb.version;
  out.snapshot.phys = !usb.phys.empty() ? usb.phys : out.canonical_port.string();
  out.snapshot.name = !usb.product_name.empty()
                          ? usb.product_name
                          : "XIAO nRF54L15 Controller";
  out.snapshot.stable_id = stable_id_for(
      usb.serial, out.snapshot.by_id_path, out.snapshot.by_path,
      out.canonical_port, requested_path, explicit_candidate);
  return out;
}

std::vector<fs::path> directory_entries(const fs::path& directory) {
  std::vector<fs::path> paths;
  std::error_code ec;
  if (!fs::exists(directory, ec)) return paths;
  for (const auto& entry : fs::directory_iterator(directory, ec)) {
    if (ec) break;
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::vector<Candidate> discover_candidates(const XiaoNrf54l15Options& options) {
  std::vector<std::pair<fs::path, bool>> requested;
  if (!options.ports.empty()) {
    for (const auto& port : options.ports) requested.emplace_back(port, true);
  } else {
    for (const auto& path : directory_entries("/dev/serial/by-id")) {
      requested.emplace_back(path, false);
    }
    for (const auto& path : directory_entries("/dev")) {
      const std::string name = path.filename().string();
      if (name.rfind("ttyACM", 0) == 0) requested.emplace_back(path, false);
    }
  }

  std::vector<Candidate> candidates;
  std::set<std::string> canonical_seen;
  for (const auto& [path, explicit_candidate] : requested) {
    Candidate candidate = make_candidate(path, explicit_candidate);
    const std::string key = candidate.canonical_port.string();
    if (!canonical_seen.insert(key).second) continue;
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

}  // namespace

class LinuxSerialTransport final : public SerialTransport {
 public:
  explicit LinuxSerialTransport(XiaoNrf54l15Options options)
      : options_(std::move(options)), next_scan_(std::chrono::steady_clock::now()) {}

  ~LinuxSerialTransport() override {
    for (auto& [id, session] : sessions_) close_session(session, {});
  }

  std::string platform_name() const override { return "linux"; }

  std::vector<SerialDeviceSnapshot> devices() const override {
    std::vector<SerialDeviceSnapshot> out;
    out.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) out.push_back(session.snapshot);
    return out;
  }

  void pump(int timeout_ms, bool include_stdin,
            bool* stdin_ready = nullptr) override {
    if (stdin_ready) *stdin_ready = false;
    scan_if_due();

    std::vector<pollfd> poll_fds;
    std::vector<Session*> fd_sessions;
    poll_fds.reserve(sessions_.size() + (include_stdin ? 1u : 0u));
    fd_sessions.reserve(sessions_.size());

    for (auto& [id, session] : sessions_) {
      if (session.fd < 0) continue;
      pollfd pfd{};
      pfd.fd = session.fd;
      pfd.events = POLLIN | POLLERR | POLLHUP | POLLNVAL;
      poll_fds.push_back(pfd);
      fd_sessions.push_back(&session);
    }

    size_t stdin_index = std::numeric_limits<size_t>::max();
    if (include_stdin) {
      stdin_index = poll_fds.size();
      pollfd pfd{};
      pfd.fd = STDIN_FILENO;
      pfd.events = POLLIN;
      poll_fds.push_back(pfd);
    }

    int effective_timeout = timeout_ms;
    if (timeout_ms < 0) effective_timeout = 1000;
    const auto now = std::chrono::steady_clock::now();
    if (next_scan_ > now) {
      const auto until_scan = std::chrono::duration_cast<std::chrono::milliseconds>(
          next_scan_ - now).count();
      const int scan_timeout = static_cast<int>(std::max<int64_t>(0, until_scan));
      if (effective_timeout < 0 || scan_timeout < effective_timeout) {
        effective_timeout = scan_timeout;
      }
    } else {
      effective_timeout = 0;
    }

    int rc = 0;
    if (!poll_fds.empty()) {
      do {
        rc = ::poll(poll_fds.data(), poll_fds.size(), effective_timeout);
      } while (rc < 0 && errno == EINTR);
    } else if (effective_timeout > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(effective_timeout));
    }

    if (rc > 0) {
      for (size_t i = 0; i < fd_sessions.size(); ++i) {
        Session& session = *fd_sessions[i];
        const short revents = poll_fds[i].revents;
        if ((revents & POLLIN) != 0) read_session(session);
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          close_session(session, "serial device disconnected");
        }
      }
      if (stdin_index != std::numeric_limits<size_t>::max() &&
          (poll_fds[stdin_index].revents & POLLIN) != 0 && stdin_ready) {
        *stdin_ready = true;
      }
    }

    scan_if_due();
  }

  bool pop_bytes(SerialBytes& bytes) override {
    if (pending_bytes_.empty()) return false;
    bytes = std::move(pending_bytes_.front());
    pending_bytes_.pop_front();
    return true;
  }

 private:
  struct Session {
    SerialDeviceSnapshot snapshot;
    fs::path open_path;
    int fd = -1;
    bool seen = false;
  };

  void open_session(Session& session) {
    if (session.fd >= 0) return;
    errno = 0;
    const int fd = ::open(session.open_path.c_str(),
                          O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      session.snapshot.connected = false;
      session.snapshot.error = std::strerror(errno);
      return;
    }

    std::string error;
    try {
      if (!configure_serial(fd, options_.baud_rate, error)) {
        ::close(fd);
        session.snapshot.connected = false;
        session.snapshot.error = error;
        return;
      }
    } catch (const std::exception& e) {
      ::close(fd);
      session.snapshot.connected = false;
      session.snapshot.error = e.what();
      return;
    }

    session.fd = fd;
    session.snapshot.connected = true;
    session.snapshot.error.clear();
  }

  void close_session(Session& session, std::string error) {
    if (session.fd >= 0) {
      ::close(session.fd);
      session.fd = -1;
    }
    session.snapshot.connected = false;
    if (!error.empty()) session.snapshot.error = std::move(error);
  }

  void read_session(Session& session) {
    std::array<uint8_t, 4096> buffer{};
    while (session.fd >= 0) {
      const ssize_t read_size = ::read(session.fd, buffer.data(), buffer.size());
      if (read_size > 0) {
        SerialBytes bytes;
        bytes.stable_id = session.snapshot.stable_id;
        bytes.bytes.assign(buffer.begin(), buffer.begin() + read_size);
        bytes.host_timestamp_ns = monotonic_now_ns();
        pending_bytes_.push_back(std::move(bytes));
        continue;
      }
      if (read_size == 0) {
        close_session(session, "serial device returned EOF");
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      close_session(session, std::strerror(errno));
      return;
    }
  }

  void scan_if_due() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_scan_) return;
    next_scan_ = now + std::chrono::milliseconds(options_.reconnect_ms);

    for (auto& [id, session] : sessions_) session.seen = false;
    for (Candidate& candidate : discover_candidates(options_)) {
      auto& session = sessions_[candidate.snapshot.stable_id];
      session.seen = true;
      const bool was_connected = session.fd >= 0;
      session.snapshot = candidate.snapshot;
      session.open_path = candidate.requested_path;
      session.snapshot.connected = was_connected;
      if (session.fd < 0) open_session(session);
    }

    for (auto& [id, session] : sessions_) {
      if (!session.seen && session.fd >= 0) {
        close_session(session, "serial path disappeared");
      }
    }
  }

  XiaoNrf54l15Options options_;
  std::map<std::string, Session> sessions_;
  std::deque<SerialBytes> pending_bytes_;
  std::chrono::steady_clock::time_point next_scan_;
};

std::unique_ptr<SerialTransport> make_platform_serial_transport(
    const XiaoNrf54l15Options& options) {
  return std::make_unique<LinuxSerialTransport>(options);
}

}  // namespace xr_override_controller::xiao_nrf54l15
