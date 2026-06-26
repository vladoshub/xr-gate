#pragma once

#include <atomic>
#include <chrono>
#include <cctype>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <xr_video/net/stereo_video_tcp.hpp>

namespace xr_video {

struct RecordControlCallbacks {
  std::function<std::string(const std::string& dir)> start;
  std::function<std::string()> stop;
  std::function<std::string()> status;
  std::function<std::string()> quit;
};

class RecordControlTcpServer {
 public:
  RecordControlTcpServer(std::string bind_host,
                         int port,
                         RecordControlCallbacks callbacks)
      : bind_host_(std::move(bind_host)), port_(port), callbacks_(std::move(callbacks)) {
    listen_fd_ = tcp_detail::listen_tcp(bind_host_, port_);
    running_ = true;
    thread_ = std::thread([this] { loop(); });
  }

  ~RecordControlTcpServer() { stop(); }

  RecordControlTcpServer(const RecordControlTcpServer&) = delete;
  RecordControlTcpServer& operator=(const RecordControlTcpServer&) = delete;

  void stop() {
    if (!running_.exchange(false)) return;

    // Wake blocking accept() before joining the control thread.
    const auto fd = listen_fd_;
    listen_fd_ = tcp_detail::kInvalidSocket;
    if (fd != tcp_detail::kInvalidSocket) {
      tcp_detail::shutdown_socket(fd);
      tcp_detail::close_socket(fd);
    }

    if (thread_.joinable()) thread_.join();
  }

 private:
  static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
  }

  static bool starts_with(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
  }

  static bool read_line(tcp_detail::socket_t fd, std::string& line) {
    line.clear();
    char c = 0;
    while (line.size() < 4096) {
#ifdef _WIN32
      const int n = ::recv(fd, &c, 1, 0);
#else
      const ssize_t n = ::recv(fd, &c, 1, 0);
#endif
      if (n <= 0) return false;
      if (c == '\n') return true;
      if (c != '\r') line.push_back(c);
    }
    return true;
  }

  static void write_string(tcp_detail::socket_t fd, const std::string& s) {
    tcp_detail::write_all(fd,
                          reinterpret_cast<const uint8_t*>(s.data()),
                          static_cast<size_t>(s.size()));
  }

  std::string handle_command(const std::string& raw) {
    const std::string cmd = trim(raw);
    if (cmd == "help") {
      return "commands: record start [dir] | record stop | record status | quit | help\nOK\n";
    }
    if (cmd == "record stop" || cmd == "stop") {
      return callbacks_.stop ? callbacks_.stop() + "\n" : "ERR stop unsupported\n";
    }
    if (cmd == "record status" || cmd == "status") {
      return callbacks_.status ? callbacks_.status() + "\n" : "ERR status unsupported\n";
    }
    if (cmd == "quit") {
      return callbacks_.quit ? callbacks_.quit() + "\n" : "ERR quit unsupported\n";
    }
    if (cmd == "record start" || cmd == "start") {
      return callbacks_.start ? callbacks_.start("") + "\n" : "ERR start unsupported\n";
    }
    if (starts_with(cmd, "record start ")) {
      return callbacks_.start ? callbacks_.start(trim(cmd.substr(std::string("record start ").size()))) + "\n"
                              : "ERR start unsupported\n";
    }
    if (starts_with(cmd, "start ")) {
      return callbacks_.start ? callbacks_.start(trim(cmd.substr(std::string("start ").size()))) + "\n"
                              : "ERR start unsupported\n";
    }
    return "ERR unknown command; try help\n";
  }

  void loop() {
    while (running_) {
      tcp_detail::socket_t fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd == tcp_detail::kInvalidSocket) {
        if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      try {
        std::string line;
        while (running_ && read_line(fd, line)) {
          const std::string response = handle_command(line);
          write_string(fd, response);
          if (trim(line) == "quit") break;
        }
      } catch (const std::exception& e) {
        if (running_) std::cerr << "[xr_video_record_control] client error: " << e.what() << "\n";
      }
      tcp_detail::close_socket(fd);
    }
  }

  std::string bind_host_;
  int port_ = 0;
  RecordControlCallbacks callbacks_;
  tcp_detail::socket_t listen_fd_ = tcp_detail::kInvalidSocket;
  std::atomic_bool running_{false};
  std::thread thread_;
};

}  // namespace xr_video
