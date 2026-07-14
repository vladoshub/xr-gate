#pragma once

#include "capture_service_cpp/common.hpp"

#include <string>

namespace xr_capture_cpp {

struct ConfigSelection {
  std::string exact_path;
  std::string directory;
  std::string name = "config.yaml";
  bool explicit_selection = false;
};

ConfigSelection resolve_config_selection(int argc, char** argv);
std::string selected_config_path(const ConfigSelection& selection);
bool load_runtime_config_file(const std::string& path, RuntimeConfig& cfg);
void validate_runtime_config(RuntimeConfig& cfg);

}  // namespace xr_capture_cpp
