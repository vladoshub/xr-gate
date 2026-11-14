#pragma once

#include <string>
#include <vector>

namespace xr_capture_cpp {

long long current_process_id();

// Starts a second instance of this executable for optional camera capture.
class ExtraCaptureProcess {
 public:
  ExtraCaptureProcess() = default;
  ~ExtraCaptureProcess();
  ExtraCaptureProcess(const ExtraCaptureProcess&) = delete;
  ExtraCaptureProcess& operator=(const ExtraCaptureProcess&) = delete;

  void start(std::vector<std::string> arguments);
  bool running();

 private:
#ifdef _WIN32
  void* process_ = nullptr;
  void* job_ = nullptr;
#else
  int pid_ = -1;
#endif
};

void bind_extra_capture_to_parent(long long parent_pid);

}  // namespace xr_capture_cpp
