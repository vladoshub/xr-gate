#pragma once

#include <cstdlib>
#include <string>

namespace xr_runtime {

inline std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

inline bool path_has_trailing_separator(const std::string& path) {
  return !path.empty() && (path.back() == '/' || path.back() == '\\');
}

inline char preferred_path_separator_for(const std::string& path) {
  return path.find('\\') != std::string::npos ? '\\' : '/';
}

inline std::string join_runtime_path(const std::string& dir, const std::string& leaf) {
  if (dir.empty()) return leaf;
  if (path_has_trailing_separator(dir)) return dir + leaf;
  return dir + preferred_path_separator_for(dir) + leaf;
}

// One place for runtime state/log defaults. Linux/WSL keeps historical /tmp behavior.
// Native Windows can later point XR_RUNTIME_DIR to %LOCALAPPDATA%\\XrTracking
// without touching runtime adapter/core code.
inline std::string runtime_state_dir() {
  const std::string explicit_dir = env_or_empty("XR_RUNTIME_DIR");
  if (!explicit_dir.empty()) return explicit_dir;

#ifndef _WIN32
  const std::string tmpdir = env_or_empty("TMPDIR");
  if (!tmpdir.empty()) return tmpdir;
  return "/tmp";
#else
  const std::string local_app_data = env_or_empty("LOCALAPPDATA");
  if (!local_app_data.empty()) return join_runtime_path(local_app_data, "XrTracking");
  return ".";
#endif
}

inline std::string default_tracking_registry_path() {
  return join_runtime_path(runtime_state_dir(), "tracking_streams.json");
}

// Runtime-normalized output streams produced by xr_runtime_adapter for
// runtime-specific consumers such as Monado/OpenXR and SteamVR/OpenVR.
inline std::string default_runtime_tracking_registry_path() {
  const std::string explicit_file = env_or_empty("XR_RUNTIME_TRACKING_REGISTRY");
  if (!explicit_file.empty()) return explicit_file;
  return join_runtime_path(runtime_state_dir(), "runtime_tracking_streams.json");
}

inline std::string default_capture_service_registry_path() {
  return join_runtime_path(runtime_state_dir(), "capture_service_streams.json");
}

// Backend control belongs to the Linux/container VIO backend, not to the
// cross-platform runtime adapter state. Keep the historical default so existing
// gravity/reset workflows continue to work unless callers override it explicitly.
inline std::string default_backend_control_file_path() {
  const std::string explicit_file = env_or_empty("XR_BACKEND_CONTROL_FILE");
  if (!explicit_file.empty()) return explicit_file;
#ifndef _WIN32
  return "/tmp/xr_backend_control.json";
#else
  return join_runtime_path(runtime_state_dir(), "xr_backend_control.json");
#endif
}

}  // namespace xr_runtime
