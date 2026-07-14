#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace xr_capture_cpp {

class SerialPort {
 public:
  SerialPort();
  ~SerialPort();

  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;

  void open(const std::string& port, int baud_rate);
  int read_timeout(uint8_t* data, size_t size, int timeout_ms);
  int write(const uint8_t* data, size_t size);
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xr_capture_cpp
