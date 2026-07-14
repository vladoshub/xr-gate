#include "capture_service_cpp/platform/serial_port.hpp"

#define NOMINMAX
#include <windows.h>

#include <stdexcept>
#include <string>

namespace xr_capture_cpp {

struct SerialPort::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
};

SerialPort::SerialPort() : impl_(new Impl()) {}
SerialPort::~SerialPort() { close(); }

void SerialPort::open(const std::string& port, int baud_rate) {
  close();
  std::string path = port;
  if (path.rfind("COM", 0) == 0 && path.size() > 4) path = "\\\\.\\" + path;
  impl_->handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (impl_->handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("failed to open serial port " + port + ", error=" + std::to_string(GetLastError()));
  }

  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(impl_->handle, &dcb)) {
    const DWORD error = GetLastError();
    close();
    throw std::runtime_error("GetCommState failed, error=" + std::to_string(error));
  }
  dcb.BaudRate = static_cast<DWORD>(baud_rate);
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fRtsControl = RTS_CONTROL_DISABLE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  if (!SetCommState(impl_->handle, &dcb)) {
    const DWORD error = GetLastError();
    close();
    throw std::runtime_error("SetCommState failed, error=" + std::to_string(error));
  }
  PurgeComm(impl_->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

int SerialPort::read_timeout(uint8_t* data, size_t size, int timeout_ms) {
  if (impl_->handle == INVALID_HANDLE_VALUE) throw std::runtime_error("serial port is not open");
  COMMTIMEOUTS timeouts{};
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(timeout_ms);
  timeouts.ReadTotalTimeoutMultiplier = 0;
  SetCommTimeouts(impl_->handle, &timeouts);
  DWORD count = 0;
  if (!ReadFile(impl_->handle, data, static_cast<DWORD>(size), &count, nullptr)) return -1;
  return static_cast<int>(count);
}

int SerialPort::write(const uint8_t* data, size_t size) {
  if (impl_->handle == INVALID_HANDLE_VALUE) throw std::runtime_error("serial port is not open");
  DWORD count = 0;
  if (!WriteFile(impl_->handle, data, static_cast<DWORD>(size), &count, nullptr)) return -1;
  return static_cast<int>(count);
}

void SerialPort::close() {
  if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->handle);
    impl_->handle = INVALID_HANDLE_VALUE;
  }
}

}  // namespace xr_capture_cpp
