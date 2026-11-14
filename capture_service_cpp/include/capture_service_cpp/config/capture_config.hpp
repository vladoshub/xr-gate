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

struct ExtraCameraConfig {
  std::string name;
  CameraSourceConfig camera;
  std::string stream_id;
  std::string frame_id;
  std::string format = "gray8";
};

struct ExtraCaptureConfig {
  RuntimeConfig service;
  std::vector<ExtraCameraConfig> cameras;
};

ConfigSelection resolve_config_selection(int argc, char** argv);
std::string selected_config_path(const ConfigSelection& selection);
bool load_runtime_config_file(const std::string& path, RuntimeConfig& cfg);
void validate_runtime_config(RuntimeConfig& cfg);
ExtraCaptureConfig load_extra_capture_config(const std::string& path);

}  // namespace xr_capture_cpp
