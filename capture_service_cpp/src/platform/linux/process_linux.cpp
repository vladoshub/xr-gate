#include "capture_service_cpp/platform/process.hpp"

#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace xr_capture_cpp {

long long current_process_id() {
  return static_cast<long long>(::getpid());
}

void ExtraCaptureProcess::start(std::vector<std::string> arguments) {
  std::string executable = "/proc/self/exe";
  std::vector<char*> argv{executable.data()};
  for (auto& arg : arguments) argv.push_back(arg.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  int error = ::posix_spawn_file_actions_init(&actions);
  if (error) throw std::system_error(error, std::generic_category(), "initializing extra capture process");
  // The worker must never retain a primary camera or listening socket.
  error = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  pid_t child = -1;
  if (!error) error = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (error) throw std::system_error(error, std::generic_category(), "starting extra capture process");
  pid_ = child;
}

bool ExtraCaptureProcess::running() {
  if (pid_ < 0) return false;
  const pid_t result = ::waitpid(pid_, nullptr, WNOHANG);
  if (result == 0 || (result < 0 && errno == EINTR)) return true;
  pid_ = -1;
  return false;
}

ExtraCaptureProcess::~ExtraCaptureProcess() {
  if (!running()) return;
  ::kill(pid_, SIGTERM);
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (!running()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!running()) return;
  ::kill(pid_, SIGKILL);
  for (int attempt = 0; attempt < 10 && running(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void bind_extra_capture_to_parent(long long parent_pid) {
  if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
    throw std::system_error(errno, std::generic_category(), "setting extra capture parent-death signal");
  }
  if (::getppid() != parent_pid) throw std::runtime_error("primary capture process already exited");
  ::signal(SIGPIPE, SIG_IGN);
}

}  // namespace xr_capture_cpp
