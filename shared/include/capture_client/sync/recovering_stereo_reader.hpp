#pragma once

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <capture_client/sync/latest_stereo_reader.hpp>

namespace capture_client {

// Reopening is necessary: a capture_service restart unlinks/recreates SHM,
// and existing mappings (or closed TCP receivers) cannot see its new frames.
class RecoveringStereoReader {
 public:
  using Factory = std::function<std::unique_ptr<ICaptureTransport>()>;
  RecoveringStereoReader(Factory factory, int64_t max_delta_ns, bool latest_only,
                         uint64_t max_scan_back)
      : factory_(std::move(factory)), max_delta_ns_(max_delta_ns),
        latest_only_(latest_only), max_scan_back_(max_scan_back) {
    reopen();
  }

  std::optional<StereoPair> read_next(double timeout_s = 1.0) {
    auto pair = reader_->read_next(timeout_s);
    const auto now = std::chrono::steady_clock::now();
    if (pair) {
      last_frame_ = now;
    } else if (now - last_frame_ >= std::chrono::seconds(1) &&
               now - last_attempt_ >= std::chrono::seconds(1)) {
      last_attempt_ = now;
      try {
        reopen();
        ++generation_;
        std::cerr << "[capture] reopened stereo input, generation=" << generation_ << '\n';
      } catch (const std::exception& e) {
        std::cerr << "[capture] cannot reopen stereo input: " << e.what() << '\n';
      }
    }
    return pair;
  }

  uint64_t generation() const { return generation_; }
  const std::string& transport_type() const { return transport_->type(); }

 private:
  void reopen() {
    auto transport = factory_();
    auto reader = std::make_unique<LatestStereoReader>(
        *transport, max_delta_ns_, latest_only_, max_scan_back_);
    reader_.reset();
    transport_ = std::move(transport);
    reader_ = std::move(reader);
  }
  Factory factory_;
  int64_t max_delta_ns_;
  bool latest_only_;
  uint64_t max_scan_back_;
  std::unique_ptr<ICaptureTransport> transport_;
  std::unique_ptr<LatestStereoReader> reader_;
  uint64_t generation_ = 0;
  std::chrono::steady_clock::time_point last_frame_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_attempt_ = last_frame_;
};
}  // namespace capture_client
