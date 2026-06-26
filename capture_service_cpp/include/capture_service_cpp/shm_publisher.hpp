#pragma once

#include "capture_service_cpp/common.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace xr_capture_cpp {

class ShmPublisher {
 public:
  ShmPublisher(const StreamSpec& spec, const std::string& namespace_name);
  ~ShmPublisher();

  ShmPublisher(const ShmPublisher&) = delete;
  ShmPublisher& operator=(const ShmPublisher&) = delete;

  const StreamSpec& spec() const { return spec_; }
  const std::string& shm_name() const { return shm_name_; }
  size_t slot_stride() const { return slot_stride_; }

  uint64_t publish(const uint8_t* payload, size_t payload_len, uint64_t timestamp_ns, uint32_t width, uint32_t height);
  std::string registry_entry() const;

 private:
  void write_global_header(uint64_t latest_seq);
  void write_metadata();

  StreamSpec spec_;
  std::string shm_name_;
  size_t slot_stride_ = 0;
  size_t size_ = 0;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  uint64_t seq_ = 0;
};

}  // namespace xr_capture_cpp
