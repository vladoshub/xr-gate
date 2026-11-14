#include "capture_service_cpp/platform/process.hpp"

#include <stdexcept>
#include <system_error>
#include <process.h>
#include <windows.h>

namespace xr_capture_cpp {
namespace {

std::string quote_argument(const std::string& argument) {
  std::string quoted = "\"";
  size_t slashes = 0;
  for (char c : argument) {
    if (c == '\\') {
      ++slashes;
      continue;
    }
    quoted.append(c == '"' ? 2 * slashes + 1 : slashes, '\\');
    quoted += c;
    slashes = 0;
  }
  quoted.append(2 * slashes, '\\');
  quoted += '"';
  return quoted;
}

std::system_error process_error(const char* message) {
  return std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

}  // namespace

long long current_process_id() {
  return static_cast<long long>(::_getpid());
}

void ExtraCaptureProcess::start(std::vector<std::string> arguments) {
  std::string executable(32768, '\0');
  const DWORD length = GetModuleFileNameA(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (!length || length >= executable.size()) throw process_error("resolving the capture executable");
  executable.resize(length);
  std::string command_line = quote_argument(executable);
  for (const auto& argument : arguments) command_line += " " + quote_argument(argument);

  job_ = CreateJobObjectA(nullptr, nullptr);
  if (!job_) throw process_error("creating the extra capture job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    throw process_error("setting the extra capture job lifetime");
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  // Inherit standard streams before the primary opens any camera handles.
  if (!CreateProcessA(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                      nullptr, nullptr, &startup, &child)) {
    throw process_error("starting extra capture process");
  }
  const bool started = AssignProcessToJobObject(job_, child.hProcess) && ResumeThread(child.hThread) != DWORD(-1);
  const DWORD error = started ? ERROR_SUCCESS : GetLastError();
  CloseHandle(child.hThread);
  if (!started) {
    TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hProcess);
    throw std::system_error(static_cast<int>(error), std::system_category(), "starting the extra capture job");
  }
  process_ = child.hProcess;
}

bool ExtraCaptureProcess::running() {
  return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

ExtraCaptureProcess::~ExtraCaptureProcess() {
  if (job_) CloseHandle(job_);
  if (process_) {
    WaitForSingleObject(process_, 500);
    CloseHandle(process_);
  }
}

void bind_extra_capture_to_parent(long long) {
  // The parent assigns the worker to its kill-on-close job before resuming it.
}

}  // namespace xr_capture_cpp
