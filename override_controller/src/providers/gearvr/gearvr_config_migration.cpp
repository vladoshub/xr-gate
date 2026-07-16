#include "gearvr_config_migration.hpp"

#include "gearvr_input_codes.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace xr_override_controller::gearvr {
namespace {

bool is_gearvr_device(const ConfigDevice& device) {
  return device.fingerprint.backend == "gearvr_ble" ||
         device.fingerprint.backend == "gearvr";
}

void collect_binding_sides(int device_id,
                           std::set<ControllerSide>& sides,
                           const std::vector<BindingConfig>& bindings) {
  for (const auto& binding : bindings) {
    if (binding.device_id == device_id) sides.insert(binding.side);
  }
}

std::optional<ControllerSide> unique_binding_side(const AppConfig& cfg,
                                                  const ConfigDevice& device) {
  if (device.imu_side) return device.imu_side;
  std::set<ControllerSide> sides;
  collect_binding_sides(device.id, sides, cfg.bindings);
  collect_binding_sides(device.id, sides, cfg.hold_toggle_bindings);
  collect_binding_sides(device.id, sides, cfg.alternative_bindings);
  collect_binding_sides(device.id, sides, cfg.alternative_hold_toggle_bindings);
  if (sides.size() == 1) return *sides.begin();
  return std::nullopt;
}

bool migrate_imu_sides(AppConfig& cfg) {
  bool changed = false;
  for (auto& device : cfg.devices) {
    if (device.imu_side_explicit || !is_gearvr_device(device)) continue;
    const auto side = unique_binding_side(cfg, device);
    if (!side) continue;
    device.imu_side = *side;
    device.imu_side_explicit = true;
    changed = true;
  }
  return changed;
}

bool has_touch_binding(const AppConfig& cfg, int device_id) {
  const auto contains = [&](const std::vector<BindingConfig>& bindings) {
    return std::any_of(bindings.begin(), bindings.end(), [&](const BindingConfig& binding) {
      return binding.device_id == device_id &&
             binding.action == ControllerAction::ThumbstickTouch;
    });
  };
  return contains(cfg.bindings) || contains(cfg.hold_toggle_bindings) ||
         contains(cfg.alternative_bindings) ||
         contains(cfg.alternative_hold_toggle_bindings);
}

bool migrate_touch_bindings(AppConfig& cfg) {
  bool changed = false;
  for (const auto& device : cfg.devices) {
    if (!is_gearvr_device(device) || has_touch_binding(cfg, device.id)) continue;
    const auto side = unique_binding_side(cfg, device);
    if (!side) continue;

    BindingConfig binding;
    binding.side = *side;
    binding.action = ControllerAction::ThumbstickTouch;
    binding.device_id = device.id;
    binding.device = device.fingerprint;
    binding.input.kind = InputKind::Key;
    binding.input.type = input_codes::kEvKey;
    binding.input.code = input_codes::kBtnTouch;
    binding.input.name = "BTN_TOUCH";
    cfg.bindings.push_back(std::move(binding));
    changed = true;
  }
  return changed;
}

}  // namespace

ConfigMigrationResult migrate_legacy_config(AppConfig& cfg) {
  ConfigMigrationResult result;
  if (migrate_imu_sides(cfg)) {
    result.changed = true;
    result.notes.push_back("Gear VR devices[].imu_side");
  }
  if (migrate_touch_bindings(cfg)) {
    result.changed = true;
    result.notes.push_back("Gear VR capacitive thumbstick_touch binding");
  }
  return result;
}

}  // namespace xr_override_controller::gearvr
