#pragma once

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <capture_client/net/capture_net_protocol.hpp>
#include <capture_client/transports/transport.hpp>

namespace capture_client {

struct TcpCaptureTransportConfig {
  std::string host = "127.0.0.1";
  int port = 45555;
  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string imu_stream = "imu0";
  size_t camera_slots = 256;
  size_t imu_slots = 8192;
  double first_packet_timeout_s = 3.0;
};

class TcpStreamReader final : public IStreamReader {
 public:
  TcpStreamReader(std::string stream_id, size_t max_slots) : max_slots_(max_slots) {
    info_.stream_id = std::move(stream_id);
    info_.frame_id = info_.stream_id;
    info_.kind = "UNKNOWN";
    info_.format_name = "UNKNOWN";
    info_.slot_count = max_slots_;
  }

  const StreamInfo& info() const override { return info_; }

  uint64_t latest_sequence() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return latest_sequence_;
  }

  bool read_latest(RawMessage& out) const override {
    std::lock_guard<std::mutex> lock(mu_);
    if (latest_sequence_ == 0) return false;
    auto it = ring_.find(latest_sequence_);
    if (it == ring_.end()) return false;
    out = it->second;
    return true;
  }

  bool read_sequence(uint64_t sequence, RawMessage& out) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = ring_.find(sequence);
    if (it == ring_.end()) return false;
    out = it->second;
    return true;
  }

  void push(RawMessage msg) {
    std::lock_guard<std::mutex> lock(mu_);
    info_.stream_id = msg.stream_id;
    info_.frame_id = msg.frame_id.empty() ? msg.stream_id : msg.frame_id;
    info_.width = static_cast<int>(msg.width);
    info_.height = static_cast<int>(msg.height);
    info_.format_code = static_cast<int>(msg.format_code);
    info_.payload_size = msg.payload_size;
    info_.slot_count = max_slots_;
    switch (static_cast<FormatCode>(msg.format_code)) {
      case FormatCode::GRAY8:
        info_.kind = "IMAGE";
        info_.format_name = "GRAY8";
        break;
      case FormatCode::IMU_F32_LE:
        info_.kind = "IMU";
        info_.format_name = "IMU_F32_LE";
        break;
      default:
        info_.kind = "BYTES";
        info_.format_name = "UNKNOWN";
        break;
    }

    const uint64_t seq = msg.sequence;
    ring_[seq] = std::move(msg);
    order_.push_back(seq);
    latest_sequence_ = std::max(latest_sequence_, seq);
    while (order_.size() > max_slots_) {
      const uint64_t old = order_.front();
      order_.pop_front();
      ring_.erase(old);
    }
    cv_.notify_all();
  }

  bool wait_for_first(double timeout_s) const {
    std::unique_lock<std::mutex> lock(mu_);
    if (latest_sequence_ != 0) return true;
    return cv_.wait_for(lock, std::chrono::duration<double>(timeout_s), [&] { return latest_sequence_ != 0; });
  }

 private:
  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  StreamInfo info_;
  size_t max_slots_ = 256;
  uint64_t latest_sequence_ = 0;
  std::unordered_map<uint64_t, RawMessage> ring_;
  std::deque<uint64_t> order_;
};

class TcpCaptureTransport final : public ICaptureTransport {
 public:
  explicit TcpCaptureTransport(TcpCaptureTransportConfig cfg)
      : cfg_(std::move(cfg)),
        cam0_(std::make_unique<TcpStreamReader>(cfg_.cam0_stream, cfg_.camera_slots)),
        cam1_(std::make_unique<TcpStreamReader>(cfg_.cam1_stream, cfg_.camera_slots)),
        imu_(std::make_unique<TcpStreamReader>(cfg_.imu_stream, cfg_.imu_slots)) {
    fd_ = capture_net_v1::connect_tcp(cfg_.host, cfg_.port);
    running_ = true;
    recv_thread_ = std::thread([this] { recv_loop(); });

    const bool got_all = wait_for_all_first_packets(cfg_.first_packet_timeout_s);
    if (!got_all) {
      throw std::runtime_error(
          "TcpCaptureTransport connected, but did not receive initial packets for all streams "
          "within timeout. Need camera0, camera1 and imu0 before starting synchronizer.");
    }
  }

  ~TcpCaptureTransport() override {
    running_ = false;
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
    }
    if (recv_thread_.joinable()) recv_thread_.join();
    capture_net_v1::close_fd(fd_);
  }

  TcpCaptureTransport(const TcpCaptureTransport&) = delete;
  TcpCaptureTransport& operator=(const TcpCaptureTransport&) = delete;

  const std::string& type() const override {
    static const std::string kType = "tcp";
    return kType;
  }

  IStreamReader& cam0() override { return *cam0_; }
  IStreamReader& cam1() override { return *cam1_; }
  IStreamReader& imu() override { return *imu_; }

 private:
  bool wait_for_all_first_packets(double timeout_s) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cam0_->latest_sequence() != 0 &&
          cam1_->latest_sequence() != 0 &&
          imu_->latest_sequence() != 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cerr << "[TcpCaptureTransport] initial stream state: "
              << "cam0_seq=" << cam0_->latest_sequence()
              << " cam1_seq=" << cam1_->latest_sequence()
              << " imu_seq=" << imu_->latest_sequence()
              << "\n";

    return cam0_->latest_sequence() != 0 &&
           cam1_->latest_sequence() != 0 &&
           imu_->latest_sequence() != 0;
  }

  void recv_loop() {
    try {
      while (running_) {
        RawMessage msg;
        if (!capture_net_v1::read_raw_message(fd_, msg)) break;
        if (msg.stream_id == cfg_.cam0_stream) {
          cam0_->push(std::move(msg));
        } else if (msg.stream_id == cfg_.cam1_stream) {
          cam1_->push(std::move(msg));
        } else if (msg.stream_id == cfg_.imu_stream) {
          imu_->push(std::move(msg));
        }
      }
    } catch (const std::exception& e) {
      if (running_) {
        std::cerr << "[TcpCaptureTransport] recv loop error: " << e.what() << "\n";
      }
    }
    running_ = false;
  }

  TcpCaptureTransportConfig cfg_;
  int fd_ = -1;
  std::atomic_bool running_{false};
  std::thread recv_thread_;
  std::unique_ptr<TcpStreamReader> cam0_;
  std::unique_ptr<TcpStreamReader> cam1_;
  std::unique_ptr<TcpStreamReader> imu_;
};

}  // namespace capture_client
