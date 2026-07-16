#pragma once

#include <xr_override_controller/input_provider.hpp>

#include <memory>
#include <vector>

namespace xr_override_controller {

class CompositeInputProvider final : public InputProvider {
 public:
  explicit CompositeInputProvider(std::vector<std::unique_ptr<InputProvider>> providers);
  ~CompositeInputProvider() override = default;

  std::string provider_name() const override { return "composite"; }
  bool requires_polling() const override;
  std::vector<DeviceInfo> scan_devices(bool open_readable) override;
  void flush_events(std::vector<DeviceInfo>& devices) override;
  std::optional<InputEvent> wait_event(std::vector<DeviceInfo>& devices,
                                       int timeout_ms,
                                       bool include_stdin) override;
  std::string input_name(const DeviceInfo& device,
                         uint16_t type,
                         uint16_t code) const override;
  InputBindingSpec make_input_spec(const DeviceInfo& device,
                                   uint16_t type,
                                   uint16_t code) const override;
  ConfigMigrationResult migrate_config(AppConfig& cfg) const override;
  xr_runtime::ControllerImuStateV1 imu_state(const DeviceInfo& device) const override;
  void close_devices(std::vector<DeviceInfo>& devices) override;
  bool set_device_grab(std::vector<DeviceInfo>& devices,
                       const std::set<size_t>& device_indices,
                       bool enabled,
                       std::ostream* log) override;

 private:
  struct LocalDeviceView {
    std::vector<DeviceInfo> devices;
    std::vector<size_t> local_to_global;
  };

  LocalDeviceView make_local_view(const std::vector<DeviceInfo>& devices,
                                  size_t provider_slot) const;
  static void sync_local_view(std::vector<DeviceInfo>& global,
                              const LocalDeviceView& local);
  std::optional<InputEvent> poll_children(std::vector<DeviceInfo>& devices,
                                          bool include_stdin);

  std::vector<std::unique_ptr<InputProvider>> providers_;
  size_t next_wait_provider_ = 0;
};

}  // namespace xr_override_controller
