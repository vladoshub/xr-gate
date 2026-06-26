#pragma once

#include "capture_service_cpp/common.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xr_capture_cpp {

class TcpFanoutPublisher {
 public:
  struct Impl;
  TcpFanoutPublisher(std::unordered_map<std::string, StreamSpec> specs,
                     std::string bind_host,
                     int port,
                     std::string namespace_name,
                     int client_queue_size);
  ~TcpFanoutPublisher();

  TcpFanoutPublisher(const TcpFanoutPublisher&) = delete;
  TcpFanoutPublisher& operator=(const TcpFanoutPublisher&) = delete;

  void start();
  void stop();
  void publish(const std::string& stream_id,
               const uint8_t* payload,
               size_t payload_len,
               uint64_t sequence,
               uint64_t timestamp_ns,
               uint32_t width,
               uint32_t height,
               uint32_t format_code,
               uint32_t flags,
               const std::string& frame_id);

  std::string registry_entry(const StreamSpec& spec) const;
  std::string advertise_host() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace xr_capture_cpp
