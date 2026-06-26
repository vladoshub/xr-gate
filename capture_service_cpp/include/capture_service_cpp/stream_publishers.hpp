#pragma once

#include "capture_service_cpp/common.hpp"
#include "capture_service_cpp/shm_publisher.hpp"
#include "capture_service_cpp/tcp_fanout.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xr_capture_cpp {

class StreamPublishers {
 public:
  explicit StreamPublishers(RuntimeConfig cfg);
  ~StreamPublishers();

  const RuntimeConfig& config() const { return cfg_; }
  void add_stream(const StreamSpec& spec);
  void start();
  void write_registry();
  uint64_t publish(const std::string& stream_id,
                   const uint8_t* payload,
                   size_t payload_len,
                   uint64_t timestamp_ns,
                   uint32_t width,
                   uint32_t height,
                   uint32_t format_code,
                   uint32_t flags,
                   const std::string& frame_id);

 private:
  RuntimeConfig cfg_;
  bool enable_shm_ = false;
  bool enable_tcp_ = false;
  std::unordered_map<std::string, StreamSpec> specs_;
  std::unordered_map<std::string, std::unique_ptr<ShmPublisher>> shm_publishers_;
  std::unordered_map<std::string, uint64_t> sequences_;
  std::unique_ptr<TcpFanoutPublisher> tcp_;
};

}  // namespace xr_capture_cpp
