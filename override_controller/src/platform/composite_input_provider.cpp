#include "composite_input_provider.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace xr_override_controller {

CompositeInputProvider::CompositeInputProvider(std::vector<std::unique_ptr<InputProvider>> providers)
    : providers_(std::move(providers)) {
  if (providers_.empty()) throw std::runtime_error("at least one input provider must be enabled");
}

CompositeInputProvider::LocalDeviceView CompositeInputProvider::make_local_view(
    const std::vector<DeviceInfo>& devices,
    size_t provider_slot) const {
  std::vector<std::pair<size_t, size_t>> ordered;  // provider-local index, global index
  for (size_t global = 0; global < devices.size(); ++global) {
    if (devices[global].provider_slot == provider_slot) {
      ordered.emplace_back(devices[global].provider_device_index, global);
    }
  }
  std::sort(ordered.begin(), ordered.end());

  LocalDeviceView view;
  view.devices.reserve(ordered.size());
  view.local_to_global.reserve(ordered.size());
  for (const auto& [provider_index, global] : ordered) {
    (void)provider_index;
    DeviceInfo local = devices[global];
    // Child providers address the vector passed to wait_event by contiguous
    // local index. Preserve the original provider index separately in the
    // global DeviceInfo; do not expose sparse indices to the child.
    local.provider_slot = 0;
    local.provider_device_index = view.devices.size();
    view.local_to_global.push_back(global);
    view.devices.push_back(std::move(local));
  }
  return view;
}

void CompositeInputProvider::sync_local_view(std::vector<DeviceInfo>& global,
                                              const LocalDeviceView& local) {
  for (size_t i = 0; i < local.devices.size() && i < local.local_to_global.size(); ++i) {
    const size_t global_index = local.local_to_global[i];
    if (global_index >= global.size()) continue;
    const size_t provider_slot = global[global_index].provider_slot;
    const size_t provider_device_index = global[global_index].provider_device_index;
    global[global_index] = local.devices[i];
    global[global_index].provider_slot = provider_slot;
    global[global_index].provider_device_index = provider_device_index;
  }
}


bool CompositeInputProvider::requires_polling() const {
  return std::any_of(providers_.begin(), providers_.end(), [](const auto& provider) {
    return provider->requires_polling();
  });
}

std::vector<DeviceInfo> CompositeInputProvider::scan_devices(bool open_readable) {
  std::vector<DeviceInfo> out;
  for (size_t provider_slot = 0; provider_slot < providers_.size(); ++provider_slot) {
    auto local = providers_[provider_slot]->scan_devices(open_readable);
    for (size_t local_index = 0; local_index < local.size(); ++local_index) {
      local[local_index].provider_slot = provider_slot;
      local[local_index].provider_device_index = local_index;
      out.push_back(std::move(local[local_index]));
    }
  }
  return out;
}

void CompositeInputProvider::flush_events(std::vector<DeviceInfo>& devices) {
  for (size_t provider_slot = 0; provider_slot < providers_.size(); ++provider_slot) {
    auto view = make_local_view(devices, provider_slot);
    providers_[provider_slot]->flush_events(view.devices);
    sync_local_view(devices, view);
  }
}

std::optional<InputEvent> CompositeInputProvider::poll_children(std::vector<DeviceInfo>& devices,
                                                                 bool include_stdin) {
  for (size_t provider_slot = 0; provider_slot < providers_.size(); ++provider_slot) {
    auto view = make_local_view(devices, provider_slot);
    auto event = providers_[provider_slot]->wait_event(view.devices, 0,
                                                        include_stdin && provider_slot == 0);
    sync_local_view(devices, view);
    if (!event) continue;
    if (event->device_index != std::numeric_limits<size_t>::max()) {
      if (event->device_index >= view.local_to_global.size()) continue;
      event->device_index = view.local_to_global[event->device_index];
    }
    return event;
  }
  return std::nullopt;
}

std::optional<InputEvent> CompositeInputProvider::wait_event(std::vector<DeviceInfo>& devices,
                                                              int timeout_ms,
                                                              bool include_stdin) {
  const auto deadline = timeout_ms < 0
      ? std::chrono::steady_clock::time_point::max()
      : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    if (auto event = poll_children(devices, include_stdin)) return event;
    if (timeout_ms == 0 || std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

std::string CompositeInputProvider::input_name(uint16_t type, uint16_t code) const {
  return providers_.front()->input_name(type, code);
}

InputBindingSpec CompositeInputProvider::make_input_spec(const DeviceInfo& device,
                                                          uint16_t type,
                                                          uint16_t code) const {
  if (device.provider_slot >= providers_.size()) throw std::runtime_error("invalid provider slot");
  return providers_[device.provider_slot]->make_input_spec(device, type, code);
}

xr_runtime::ControllerImuStateV1 CompositeInputProvider::imu_state(const DeviceInfo& device) const {
  if (device.provider_slot >= providers_.size()) return {};
  return providers_[device.provider_slot]->imu_state(device);
}

void CompositeInputProvider::close_devices(std::vector<DeviceInfo>& devices) {
  for (size_t provider_slot = 0; provider_slot < providers_.size(); ++provider_slot) {
    auto view = make_local_view(devices, provider_slot);
    providers_[provider_slot]->close_devices(view.devices);
    sync_local_view(devices, view);
  }
}

bool CompositeInputProvider::set_device_grab(std::vector<DeviceInfo>& devices,
                                              const std::set<size_t>& device_indices,
                                              bool enabled,
                                              std::ostream* log) {
  bool any_ok = false;
  for (size_t provider_slot = 0; provider_slot < providers_.size(); ++provider_slot) {
    auto view = make_local_view(devices, provider_slot);
    std::set<size_t> local_indices;
    for (size_t local = 0; local < view.local_to_global.size(); ++local) {
      if (device_indices.count(view.local_to_global[local]) != 0) local_indices.insert(local);
    }
    if (!local_indices.empty()) {
      any_ok = providers_[provider_slot]->set_device_grab(view.devices,
                                                           local_indices,
                                                           enabled,
                                                           log) || any_ok;
    }
    sync_local_view(devices, view);
  }
  return any_ok;
}

}  // namespace xr_override_controller
