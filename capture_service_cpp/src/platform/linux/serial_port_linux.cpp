#include "capture_service_cpp/platform/serial_port.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>

namespace xr_capture_cpp {
namespace {

speed_t baud_to_termios(int baud) {
  switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B576000
    case 576000: return B576000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
    default: throw std::runtime_error("unsupported serial baud rate on this Linux platform: " + std::to_string(baud));
  }
}

}  // namespace

struct SerialPort::Impl {
  int fd = -1;
};

SerialPort::SerialPort() : impl_(new Impl()) {}
SerialPort::~SerialPort() { close(); }

void SerialPort::open(const std::string& port, int baud_rate) {
  close();
  impl_->fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (impl_->fd < 0) throw std::runtime_error("failed to open serial port " + port + ": " + std::strerror(errno));

  termios tty{};
  if (tcgetattr(impl_->fd, &tty) != 0) {
    const std::string error = std::strerror(errno);
    close();
    throw std::runtime_error("tcgetattr failed for " + port + ": " + error);
  }
  cfmakeraw(&tty);
  const speed_t speed = baud_to_termios(baud_rate);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag |= CLOCAL | CREAD;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;
  if (tcsetattr(impl_->fd, TCSANOW, &tty) != 0) {
    const std::string error = std::strerror(errno);
    close();
    throw std::runtime_error("tcsetattr failed for " + port + ": " + error);
  }
  tcflush(impl_->fd, TCIOFLUSH);
}

int SerialPort::read_timeout(uint8_t* data, size_t size, int timeout_ms) {
  if (impl_->fd < 0) throw std::runtime_error("serial port is not open");
  pollfd pfd{};
  pfd.fd = impl_->fd;
  pfd.events = POLLIN;
  const int ready = ::poll(&pfd, 1, timeout_ms);
  if (ready < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  if (ready == 0) return 0;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
  const ssize_t count = ::read(impl_->fd, data, size);
  if (count < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
  }
  return static_cast<int>(count);
}

int SerialPort::write(const uint8_t* data, size_t size) {
  if (impl_->fd < 0) throw std::runtime_error("serial port is not open");
  const ssize_t count = ::write(impl_->fd, data, size);
  return count < 0 ? -1 : static_cast<int>(count);
}

void SerialPort::close() {
  if (impl_ && impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
}

}  // namespace xr_capture_cpp
