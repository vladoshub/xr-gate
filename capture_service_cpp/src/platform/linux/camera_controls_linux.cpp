#include "capture_service_cpp/platform/camera_controls.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <linux/videodev2.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <unordered_map>
#include <unistd.h>

namespace xr_capture_cpp {
namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) ::close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_ = -1;
};

int xioctl(int fd, unsigned long request, void* arg) {
  int rc = 0;
  do {
    rc = ::ioctl(fd, request, arg);
  } while (rc < 0 && errno == EINTR);
  return rc;
}

std::string device_path_for_controls(const CameraDeviceConfig& cfg) {
  if (!cfg.device_path.empty()) return cfg.device_path;
  return "/dev/video" + std::to_string(cfg.index);
}

using NativeControls = std::unordered_map<std::string, v4l2_query_ext_ctrl>;

NativeControls enumerate_controls(int fd) {
  NativeControls controls;
  v4l2_query_ext_ctrl query{};
  query.id = V4L2_CTRL_FLAG_NEXT_CTRL;

  while (xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &query) == 0) {
    if ((query.flags & V4L2_CTRL_FLAG_DISABLED) == 0 &&
        query.type != V4L2_CTRL_TYPE_CTRL_CLASS) {
      const std::string name = normalize_camera_control_name(
          reinterpret_cast<const char*>(query.name));
      if (!name.empty()) controls.emplace(name, query);
    }
    query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
  }

  if (errno != EINVAL) {
    throw std::runtime_error(std::string("VIDIOC_QUERY_EXT_CTRL failed: ") +
                             std::strerror(errno));
  }
  return controls;
}

bool is_scalar_control_type(uint32_t type) {
  switch (type) {
    case V4L2_CTRL_TYPE_INTEGER:
    case V4L2_CTRL_TYPE_BOOLEAN:
    case V4L2_CTRL_TYPE_MENU:
    case V4L2_CTRL_TYPE_INTEGER_MENU:
    case V4L2_CTRL_TYPE_BITMASK:
    case V4L2_CTRL_TYPE_BUTTON:
    case V4L2_CTRL_TYPE_INTEGER64:
      return true;
    default:
      return false;
  }
}

int64_t set_control(int fd, const v4l2_query_ext_ctrl& query, int64_t requested) {
  if ((query.flags & V4L2_CTRL_FLAG_READ_ONLY) != 0) {
    throw std::runtime_error("control is read-only");
  }
  if (!is_scalar_control_type(query.type)) {
    throw std::runtime_error("control type is not an integer scalar");
  }
  if (query.type != V4L2_CTRL_TYPE_BUTTON &&
      (requested < query.minimum || requested > query.maximum)) {
    throw std::runtime_error("value " + std::to_string(requested) +
                             " is outside [" + std::to_string(query.minimum) + ", " +
                             std::to_string(query.maximum) + "]");
  }
  if (query.type != V4L2_CTRL_TYPE_BUTTON && query.step > 1 &&
      ((requested - query.minimum) % static_cast<int64_t>(query.step)) != 0) {
    throw std::runtime_error("value " + std::to_string(requested) +
                             " does not match step " + std::to_string(query.step));
  }

  if (query.type == V4L2_CTRL_TYPE_INTEGER64) {
    v4l2_ext_control control{};
    control.id = query.id;
    control.value64 = requested;
    v4l2_ext_controls controls{};
    controls.which = V4L2_CTRL_WHICH_CUR_VAL;
    controls.count = 1;
    controls.controls = &control;
    if (xioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
      throw std::runtime_error(std::string("VIDIOC_S_EXT_CTRLS failed: ") +
                               std::strerror(errno));
    }
    control.value64 = 0;
    if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) < 0) {
      throw std::runtime_error(std::string("VIDIOC_G_EXT_CTRLS failed: ") +
                               std::strerror(errno));
    }
    return control.value64;
  }

  if (requested < std::numeric_limits<int32_t>::min() ||
      requested > std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error("value does not fit V4L2 32-bit control");
  }

  v4l2_control control{};
  control.id = query.id;
  control.value = static_cast<int32_t>(requested);
  if (xioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
    throw std::runtime_error(std::string("VIDIOC_S_CTRL failed: ") +
                             std::strerror(errno));
  }
  control.value = 0;
  if (xioctl(fd, VIDIOC_G_CTRL, &control) < 0) {
    throw std::runtime_error(std::string("VIDIOC_G_CTRL failed: ") +
                             std::strerror(errno));
  }
  return control.value;
}

void handle_error(const CameraDeviceConfig& cfg,
                  const std::string& label,
                  const std::string& message) {
  const std::string full = "camera controls for " + label + ": " + message;
  if (cfg.controls.strict) throw std::runtime_error(full);
  std::cerr << "[capture_service_cpp][WARN] " << full << std::endl;
}

}  // namespace

void apply_camera_controls(const CameraDeviceConfig& cfg, const std::string& label) {
  if (cfg.controls.values.empty()) return;

  const std::string path = device_path_for_controls(cfg);
  const int raw_fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (raw_fd < 0) {
    handle_error(cfg, label,
                 "failed to open " + path + ": " + std::strerror(errno));
    return;
  }
  ScopedFd fd(raw_fd);

  NativeControls available;
  try {
    available = enumerate_controls(fd.get());
  } catch (const std::exception& e) {
    handle_error(cfg, label, e.what());
    return;
  }

  for (const auto& [configured_name, value] : cfg.controls.values) {
    const std::string name = normalize_camera_control_name(configured_name);
    const auto it = available.find(name);
    if (it == available.end()) {
      handle_error(cfg, label, "control '" + configured_name + "' was not found on " + path);
      continue;
    }

    try {
      const int64_t applied = set_control(fd.get(), it->second, value);
      std::cerr << "[capture_service_cpp] camera control label=" << label
                << " name=" << name << " requested=" << value
                << " applied=" << applied << std::endl;
    } catch (const std::exception& e) {
      handle_error(cfg, label, "failed to apply '" + configured_name + "': " + e.what());
    }
  }
}

}  // namespace xr_capture_cpp
