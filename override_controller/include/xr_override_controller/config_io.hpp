#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <xr_override_controller/types.hpp>

namespace xr_override_controller {

std::filesystem::path default_config_dir();
std::vector<std::filesystem::path> list_config_files(const std::filesystem::path& dir);
AppConfig load_config_file(const std::filesystem::path& path);
void save_config_file(const AppConfig& cfg, const std::filesystem::path& path);
std::filesystem::path choose_or_create_config_path(const std::filesystem::path& dir, const std::string& preferred_name);

}  // namespace xr_override_controller
