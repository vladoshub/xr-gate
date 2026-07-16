#pragma once

#include <xr_override_controller/input_provider.hpp>

namespace xr_override_controller::gearvr {

ConfigMigrationResult migrate_legacy_config(AppConfig& cfg);

}  // namespace xr_override_controller::gearvr
