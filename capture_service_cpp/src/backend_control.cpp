#include "capture_service_cpp/backend_control.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace xr_capture_cpp {
namespace {

bool is_json_value_terminator(char c) {
  return c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
         c == '\r' || c == '\n';
}

size_t value_start_after_key(const std::string& text,
                             const std::string& key,
                             const std::filesystem::path& path,
                             bool required) {
  const std::string quoted_key = "\"" + key + "\"";
  const size_t key_pos = text.find(quoted_key);
  if (key_pos == std::string::npos) {
    if (!required) return std::string::npos;
    throw std::runtime_error(
        "backend control file '" + path.string() + "' is missing " + key);
  }
  const size_t colon = text.find(':', key_pos + quoted_key.size());
  if (colon == std::string::npos) {
    throw std::runtime_error(
        "backend control file '" + path.string() + "' has invalid " + key);
  }
  return text.find_first_not_of(" \t\r\n", colon + 1);
}

double parse_positive_double(const std::string& text,
                             const std::string& key,
                             const std::filesystem::path& path) {
  const size_t start = value_start_after_key(text, key, path, true);
  if (start == std::string::npos) {
    throw std::runtime_error(
        "backend control file '" + path.string() + "' has empty " + key);
  }
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str() + start, &end);
  if (end == text.c_str() + start || errno == ERANGE ||
      (end != text.c_str() + text.size() && !is_json_value_terminator(*end)) ||
      !std::isfinite(value) || value <= 0.0) {
    throw std::runtime_error(
        "backend control file '" + path.string() +
        "' has invalid " + key + "; expected a positive finite number");
  }
  return std::abs(value);
}

uint64_t parse_optional_non_negative_uint(const std::string& text,
                                          const std::string& key,
                                          const std::filesystem::path& path,
                                          uint64_t fallback) {
  const size_t start = value_start_after_key(text, key, path, false);
  if (start == std::string::npos) return fallback;
  if (text[start] == '-') {
    throw std::runtime_error(
        "backend control file '" + path.string() +
        "' has invalid " + key + "; expected a non-negative integer");
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str() + start, &end, 10);
  if (end == text.c_str() + start || errno == ERANGE ||
      (end != text.c_str() + text.size() && !is_json_value_terminator(*end))) {
    throw std::runtime_error(
        "backend control file '" + path.string() +
        "' has invalid " + key + "; expected a non-negative integer");
  }
  return static_cast<uint64_t>(value);
}

}  // namespace

BackendControlSnapshot load_backend_control_snapshot(const std::filesystem::path& path) {
  if (path.empty()) throw std::runtime_error("backend control file path is empty");

  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open backend control file: " + path.string());
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  BackendControlSnapshot snapshot;
  snapshot.gravity_magnitude =
      parse_positive_double(text, "gravity_magnitude", path);
  snapshot.reset_counter = parse_optional_non_negative_uint(
      text, "reset_counter", path, 0);
  return snapshot;
}

BackendControlReader::BackendControlReader(std::filesystem::path path,
                                           int poll_interval_ms)
    : path_(std::move(path)),
      poll_interval_ns_(poll_interval_ms > 0
                            ? static_cast<uint64_t>(poll_interval_ms) * 1000000ULL
                            : 0) {}

void BackendControlReader::poll_if_due(uint64_t now_ns) {
  if (path_.empty()) return;
  if (polled_once_) {
    if (poll_interval_ns_ == 0) return;
    if (now_ns < next_poll_ns_) return;
  }

  polled_once_ = true;
  next_poll_ns_ = now_ns + poll_interval_ns_;

  try {
    const BackendControlSnapshot loaded = load_backend_control_snapshot(path_);
    const bool changed = !loaded_once_ ||
                         std::abs(loaded.gravity_magnitude - snapshot_.gravity_magnitude) > 1.0e-9 ||
                         loaded.reset_counter != snapshot_.reset_counter;
    snapshot_ = loaded;
    if (changed) {
      std::cerr << "[capture_service_cpp] backend control gravity_magnitude="
                << snapshot_.gravity_magnitude
                << " reset_counter=" << snapshot_.reset_counter
                << " file=" << path_.string() << std::endl;
    }
    loaded_once_ = true;
    failure_logged_ = false;
  } catch (const std::exception& e) {
    if (!failure_logged_) {
      std::cerr << "[capture_service_cpp][WARN] " << e.what()
                << "; using last gravity_magnitude=" << snapshot_.gravity_magnitude
                << " and retrying after the polling interval" << std::endl;
      failure_logged_ = true;
    }
  }
}

}  // namespace xr_capture_cpp
