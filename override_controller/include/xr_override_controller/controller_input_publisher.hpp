#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <xr_override_controller/types.hpp>
#include <xr_runtime/contracts/controller_input_contract.hpp>

namespace xr_override_controller {

class ControllerInputShmPublisher {
 public:
  explicit ControllerInputShmPublisher(PublishConfig cfg);
  ~ControllerInputShmPublisher();

  ControllerInputShmPublisher(const ControllerInputShmPublisher&) = delete;
  ControllerInputShmPublisher& operator=(const ControllerInputShmPublisher&) = delete;

  void publish(const OutputState& state);
  uint64_t sequence() const { return sequence_; }

 private:
  void write_metadata();
  void update_registry() const;

  PublishConfig cfg_;
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t header_size_ = 4096;
  size_t slot_header_size_ = 128;
  size_t payload_size_ = sizeof(xr_runtime::ControllerInputV2);
  size_t slot_stride_ = 0;
  uint64_t sequence_ = 0;
};

class ControllerInputTcpPublisher {
 public:
  explicit ControllerInputTcpPublisher(PublishConfig cfg);
  ~ControllerInputTcpPublisher();

  ControllerInputTcpPublisher(const ControllerInputTcpPublisher&) = delete;
  ControllerInputTcpPublisher& operator=(const ControllerInputTcpPublisher&) = delete;

  void publish(const OutputState& state);
  uint64_t sequence() const { return sequence_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  PublishConfig cfg_;
  uint64_t sequence_ = 0;
};

class ControllerInputPublisher {
 public:
  explicit ControllerInputPublisher(PublishConfig cfg);
  ~ControllerInputPublisher();

  ControllerInputPublisher(const ControllerInputPublisher&) = delete;
  ControllerInputPublisher& operator=(const ControllerInputPublisher&) = delete;

  void publish(const OutputState& state);
  uint64_t sequence() const;
  const std::string& transport() const { return transport_; }

 private:
  std::string transport_;
  std::unique_ptr<ControllerInputShmPublisher> shm_;
  std::unique_ptr<ControllerInputTcpPublisher> tcp_;
};

}  // namespace xr_override_controller
