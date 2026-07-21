#include <xr_override_controller/backend_control.hpp>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace xr_override_controller {
namespace {

std::atomic<float> g_gravity_magnitude{kDefaultGravityMagnitudeMps2};
std::atomic<uint64_t> g_reset_counter{0};

bool is_json_value_terminator(char c) {
  return c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
         c == '\r' || c == '\n';
}

size_t value_start_after_key(const std::string& text,
                             const std::string& key,
                             const std::filesystem::path& path) {
  const std::string quoted_key = "\"" + key + "\"";
  const size_t key_pos = text.find(quoted_key);
  if (key_pos == std::string::npos) {
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

float parse_positive_float(const std::string& text,
                           const std::string& key,
                           const std::filesystem::path& path) {
  const size_t start = value_start_after_key(text, key, path);
  if (start == std::string::npos) {
    throw std::runtime_error(
        "backend control file '" + path.string() + "' has empty " + key);
  }
  errno = 0;
  char* end = nullptr;
  const float value = std::strtof(text.c_str() + start, &end);
  if (end == text.c_str() + start || errno == ERANGE ||
      (end != text.c_str() + text.size() && !is_json_value_terminator(*end)) ||
      !std::isfinite(value) || value <= 0.0f) {
    throw std::runtime_error(
        "backend control file '" + path.string() +
        "' has invalid " + key + "; expected a positive finite number");
  }
  return std::abs(value);
}

uint64_t parse_non_negative_uint(const std::string& text,
                                 const std::string& key,
                                 const std::filesystem::path& path) {
  const size_t start = value_start_after_key(text, key, path);
  if (start == std::string::npos || text[start] == '-') {
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
  if (path.empty()) {
    throw std::runtime_error("backend control file path is empty");
  }

  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open backend control file: " + path.string());
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  BackendControlSnapshot snapshot;
  snapshot.gravity_magnitude = parse_positive_float(
      text, "gravity_magnitude", path);

  // reset_counter is part of the shared BackendControlSnapshot contract. It is
  // tracked and logged here, but changing it does not reset provider filters.
  // xr_runtime_adapter remains the owner of runtime prediction/yaw resets.
  snapshot.reset_counter = parse_non_negative_uint(
      text, "reset_counter", path);
  return snapshot;
}

BackendControlSnapshot current_backend_control_snapshot() {
  BackendControlSnapshot snapshot;
  snapshot.gravity_magnitude = g_gravity_magnitude.load(std::memory_order_relaxed);
  snapshot.reset_counter = g_reset_counter.load(std::memory_order_relaxed);
  return snapshot;
}

void update_backend_control_snapshot(const BackendControlSnapshot& snapshot) {
  const float gravity =
      std::isfinite(snapshot.gravity_magnitude) && snapshot.gravity_magnitude > 0.0f
          ? std::abs(snapshot.gravity_magnitude)
          : kDefaultGravityMagnitudeMps2;
  g_gravity_magnitude.store(gravity, std::memory_order_relaxed);
  g_reset_counter.store(snapshot.reset_counter, std::memory_order_relaxed);
}

}  // namespace xr_override_controller
