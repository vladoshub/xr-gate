#include "gearvr_input_provider.hpp"

// Gear VR packet layout and command sequence are based on the public
// reverse-engineering work in uutzinger/gearVRC (MIT, Copyright 2023 Urs
// Utzinger) and earlier community protocol documentation. OS-specific BLE
// access is isolated behind BleTransport.

#include "gearvr_ble_transport.hpp"
#include "gearvr_config_migration.hpp"
#include "gearvr_input_codes.hpp"
#include "gearvr_options.hpp"
#include "gearvr_protocol.hpp"
#include "gearvr_imu_stream.hpp"
#include "gearvr_touchpad.hpp"
#include <xr_override_controller/imu/controller_imu_calibration.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace xr_override_controller::gearvr {
namespace codes = input_codes;
namespace {

constexpr uint64_t kStaleNs = 250'000'000ull;

int64_t monotonic_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string stable_material(const std::string& platform,
                            const std::string& stable_id,
                            const std::string& name) {
  return platform + "|gearvr_ble|" + stable_id + "|" + name;
}

std::string key_name(uint16_t code) {
  switch (code) {
    case codes::kBtnTrigger: return "BTN_TRIGGER";
    case codes::kBtnLeft: return "BTN_LEFT";
    case codes::kBtnTouch: return "BTN_TOUCH";
    case codes::kKeyBack: return "KEY_BACK";
    case codes::kKeyHomepage: return "KEY_HOMEPAGE";
    case codes::kKeyVolumeUp: return "KEY_VOLUMEUP";
    case codes::kKeyVolumeDown: return "KEY_VOLUMEDOWN";
    case codes::kKeyUp: return "KEY_UP";
    case codes::kKeyDown: return "KEY_DOWN";
    case codes::kKeyLeft: return "KEY_LEFT";
    case codes::kKeyRight: return "KEY_RIGHT";
    default: return "KEY_" + std::to_string(code);
  }
}

std::string abs_name(uint16_t code) {
  if (code == codes::kAbsX) return "ABS_X";
  if (code == codes::kAbsY) return "ABS_Y";
  return "ABS_" + std::to_string(code);
}

}  // namespace

struct GearVrInputProvider::Impl {
  struct PendingEvent {
    std::string stable_id;
    InputEvent event;
  };

  struct Session {
    std::string stable_id;
    std::string address;
    std::string name = "Gear VR Controller";
    std::string platform;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    bool notifications_active = false;
    bool ever_connected = false;
    bool seen = false;
    std::string error;
    DeviceFingerprint fingerprint;
    GearVrImuStream imu_stream;
    TouchpadProcessor touchpad;
    uint64_t last_packet_ns = 0;
    uint64_t last_imu_drop_warning_ns = 0;
    uint8_t previous_buttons = 0;
    bool previous_touch = false;

    Session(float beta, TouchpadOptions touchpad_options)
        : imu_stream(beta), touchpad(std::move(touchpad_options)) {}
  };

  explicit Impl(ProviderOptionValues input_options)
      : options(load_gearvr_options(input_options)), transport(make_platform_ble_transport(options)) {
    if (!transport) throw std::runtime_error("Gear VR BLE transport is unavailable on this platform");
  }

  TouchpadOptions touchpad_options() const {
    TouchpadOptions out;
    out.mode = options.touchpad_mode;
    out.deadzone = options.touchpad_deadzone;
    out.radius = options.touchpad_radius;
    out.invert_x = options.touchpad_invert_x;
    out.invert_y = options.touchpad_invert_y;
    return out;
  }

  void update_fingerprint(Session& session) {
    session.fingerprint.platform = session.platform.empty() ? transport->platform_name() : session.platform;
    session.fingerprint.backend = "gearvr_ble";
    session.fingerprint.name = session.name.empty() ? "Gear VR Controller" : session.name;
    session.fingerprint.phys = session.address;
    session.fingerprint.uniq = session.stable_id;
    session.fingerprint.bustype = codes::kBusBluetooth;
    session.fingerprint.stable_hash = stable_hash64(stable_material(
        session.fingerprint.platform, session.stable_id, session.fingerprint.name));
  }

  void queue_event(Session& session,
                   uint16_t type,
                   uint16_t code,
                   int32_t value,
                   uint64_t timestamp_ns) {
    InputEvent event;
    event.device_index = std::numeric_limits<size_t>::max();
    event.type = type;
    event.code = code;
    event.value = value;
    event.timestamp_ns = static_cast<int64_t>(timestamp_ns);
    if (type == codes::kEvAbs) {
      auto existing = std::find_if(pending_events.begin(), pending_events.end(),
                                   [&](const PendingEvent& queued) {
        return queued.stable_id == session.stable_id && queued.event.type == type &&
               queued.event.code == code;
      });
      if (existing != pending_events.end()) existing->event = event;
      else pending_events.push_back({session.stable_id, event});
    } else {
      pending_events.push_back({session.stable_id, event});
    }
  }

  void release_all_inputs(Session& session) {
    const uint64_t now_ns = static_cast<uint64_t>(monotonic_now_ns());
    static const std::array<uint16_t, 7> keys{
        codes::kBtnTrigger, codes::kKeyHomepage, codes::kKeyBack,
        codes::kBtnLeft, codes::kBtnTouch, codes::kKeyVolumeUp, codes::kKeyVolumeDown,
    };
    for (uint16_t key : keys) queue_event(session, codes::kEvKey, key, 0, now_ns);
    session.touchpad.release([&](uint16_t type, uint16_t code, int32_t value) {
      queue_event(session, type, code, value, now_ns);
    });
    // Always force neutral axes even when the touchpad state was already idle.
    queue_event(session, codes::kEvAbs, codes::kAbsX, 0, now_ns);
    queue_event(session, codes::kEvAbs, codes::kAbsY, 0, now_ns);
    session.previous_buttons = 0;
    session.previous_touch = false;
    session.touchpad.reset();
  }

  void update_connected(Session& session, bool connected) {
    if (session.connected == connected) return;
    session.connected = connected;
    session.ever_connected = session.ever_connected || connected;
    if (connected) {
      session.error.clear();
      if (session.imu_stream.latest.status != xr_runtime::CONTROLLER_IMU_ACTIVE) {
        session.imu_stream.latest.status = xr_runtime::CONTROLLER_IMU_CONNECTED;
      }
    } else {
      release_all_inputs(session);
      session.notifications_active = false;
      if (session.imu_stream.latest.sequence != 0) {
        std::cerr << "[override_controller][WARN] Gear VR IMU epoch reset for "
                  << session.stable_id << ": reason=disconnect epoch="
                  << session.imu_stream.epoch() << "->" << session.imu_stream.epoch() + 1
                  << "; AHRS=reset\n";
      }
      session.imu_stream.reset();
      session.imu_stream.latest.data_flags = 0;
      session.imu_stream.latest.sample_count = 0;
      session.imu_stream.latest.status = session.ever_connected ? xr_runtime::CONTROLLER_IMU_LOST
                                                  : xr_runtime::CONTROLLER_IMU_CONFIGURED;
      session.last_packet_ns = 0;
    }
  }

  void sync_transport_devices() {
    for (auto& [id, session] : sessions) session->seen = false;

    for (const BleDeviceSnapshot& snapshot : transport->devices()) {
      if (snapshot.stable_id.empty()) continue;
      auto& session_ptr = sessions[snapshot.stable_id];
      if (!session_ptr) {
        session_ptr = std::make_unique<Session>(
            static_cast<float>(options.madgwick_beta), touchpad_options());
        session_ptr->stable_id = snapshot.stable_id;
      }
      Session& session = *session_ptr;
      session.seen = true;
      session.address = snapshot.address;
      session.name = snapshot.name.empty() ? "Gear VR Controller" : snapshot.name;
      session.platform = snapshot.platform.empty() ? transport->platform_name() : snapshot.platform;
      session.paired = snapshot.paired;
      session.trusted = snapshot.trusted;
      session.notifications_active = snapshot.notifications_active;
      session.error = snapshot.error;
      update_fingerprint(session);
      update_connected(session, snapshot.connected);
      if (!session.connected) {
        session.imu_stream.latest.status = session.ever_connected ? xr_runtime::CONTROLLER_IMU_LOST
                                                    : xr_runtime::CONTROLLER_IMU_CONFIGURED;
      } else if (session.last_packet_ns == 0) {
        session.imu_stream.latest.status = xr_runtime::CONTROLLER_IMU_CONNECTED;
      }
    }

    for (auto& [id, session] : sessions) {
      if (!session->seen && session->connected) update_connected(*session, false);
    }
  }

  void emit_button_changes(Session& session, uint8_t buttons, uint64_t timestamp_ns) {
    static const std::array<std::pair<uint8_t, uint16_t>, 6> mapping{{
        {0, codes::kBtnTrigger},
        {1, codes::kKeyHomepage},
        {2, codes::kKeyBack},
        {3, codes::kBtnLeft},
        {4, codes::kKeyVolumeUp},
        {5, codes::kKeyVolumeDown},
    }};
    for (const auto& [bit, code] : mapping) {
      const bool now = (buttons & (1u << bit)) != 0;
      const bool before = (session.previous_buttons & (1u << bit)) != 0;
      if (now != before) queue_event(session, codes::kEvKey, code, now ? 1 : 0, timestamp_ns);
    }
    session.previous_buttons = buttons;
  }

  void handle_packet(const BlePacket& packet) {
    const auto session_it = sessions.find(packet.stable_id);
    if (session_it == sessions.end()) return;
    Session& session = *session_it->second;
    // Transport callbacks can be drained in the same pump that reports a
    // disconnect. Ignore any trailing notification once the final transport
    // snapshot is no longer readable, matching the old in-order monolith.
    if (!session.connected || !session.notifications_active) return;
    const auto decoded = decode_packet(packet.bytes.data(), packet.bytes.size());
    if (!decoded) return;

    const uint64_t packet_host_ns = packet.timestamp_ns != 0
                                        ? packet.timestamp_ns
                                        : static_cast<uint64_t>(monotonic_now_ns());
    const uint64_t host_gap_ns = session.last_packet_ns != 0 &&
        packet_host_ns > session.last_packet_ns ? packet_host_ns - session.last_packet_ns : 0;
    session.last_packet_ns = packet_host_ns;
    emit_button_changes(session, decoded->buttons, packet_host_ns);
    const bool touched = !(decoded->touch_x == 0 && decoded->touch_y == 0);
    if (touched != session.previous_touch) {
      queue_event(session, codes::kEvKey, codes::kBtnTouch, touched ? 1 : 0, packet_host_ns);
      session.previous_touch = touched;
    }
    session.touchpad.process(decoded->touch_x, decoded->touch_y,
                             [&](uint16_t type, uint16_t code, int32_t value) {
      queue_event(session, type, code, value, packet_host_ns);
    });

    const auto previous = session.imu_stream.latest;
    const uint32_t epoch_before = session.imu_stream.epoch();
    const uint32_t dropped_before = session.imu_stream.dropped_batches();
    const uint32_t wraps_before = session.imu_stream.clock_wraps();
    const bool accepted = session.imu_stream.push(*decoded, packet_host_ns);
    if (accepted && options.madgwick_beta > 0 &&
        (session.imu_stream.latest.data_flags & xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID) != 0u &&
        ((previous.data_flags & xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID) == 0u ||
         session.imu_stream.epoch() != epoch_before)) {
      const auto& q = session.imu_stream.latest.orientation_xyzw;
      std::cerr << "[override_controller] Gear VR gravity reference initialized for "
                << session.stable_id << " epoch=" << session.imu_stream.epoch()
                << " orientation_ns=" << session.imu_stream.latest.orientation_timestamp_ns
                << " orientation_xyzw=(" << q[0] << "," << q[1] << "," << q[2] << "," << q[3] << ")\n";
    }
    if (session.imu_stream.epoch() != epoch_before) {
      std::cerr << "[override_controller][WARN] Gear VR IMU epoch reset for "
                << session.stable_id << ": reason=device_clock_restart epoch="
                << epoch_before << "->" << session.imu_stream.epoch()
                << " previous_ticks=" << previous.latest_device_timestamp_ticks
                << " newest_raw_us=" << decoded->imu_samples.back().device_timestamp_us
                << " host_gap_ms=" << double(host_gap_ns) / 1.0e6 << "; AHRS=reset\n";
    } else if (accepted && session.imu_stream.clock_wraps() != wraps_before) {
      std::cerr << "[override_controller] Gear VR IMU clock wrap for "
                << session.stable_id << ": reason=clock_wrap period_us=" << kTimestampWrapUs
                << " wraps=" << session.imu_stream.clock_wraps()
                << " epoch=" << epoch_before
                << " extended_ticks=" << session.imu_stream.latest.latest_device_timestamp_ticks
                << "; AHRS=retained yaw=retained\n";
    } else if (accepted && host_gap_ns > kStaleNs) {
      std::cerr << "[override_controller] Gear VR IMU delivery resumed for "
                << session.stable_id << ": reason=host_receive_gap host_gap_ms="
                << double(host_gap_ns) / 1.0e6 << " device_gap_us="
                << (session.imu_stream.latest.latest_device_timestamp_ticks -
                    previous.latest_device_timestamp_ticks)
                << " epoch=" << epoch_before << "; AHRS=retained\n";
    }
    if (session.imu_stream.dropped_batches() != dropped_before &&
        (session.last_imu_drop_warning_ns == 0 ||
         packet_host_ns - session.last_imu_drop_warning_ns >= 1'000'000'000ull)) {
      session.last_imu_drop_warning_ns = packet_host_ns;
      const auto& current = session.imu_stream.latest;
      const uint64_t first_gap_us = accepted && current.sample_count != 0 &&
          current.samples[0].device_timestamp_ticks > previous.latest_device_timestamp_ticks
              ? current.samples[0].device_timestamp_ticks - previous.latest_device_timestamp_ticks : 0;
      const bool missing_interval = accepted && previous.sequence != 0 &&
          session.imu_stream.epoch() == epoch_before &&
          first_gap_us > 250'000u && first_gap_us < 0x80000000u;
      std::cerr << "[override_controller][WARN] Gear VR IMU batches dropped for "
                << session.stable_id << ": " << session.imu_stream.dropped_batches()
                << " total; reason=" << (missing_interval ? "device_sample_gap" :
                                        accepted ? "queue_overflow" : "invalid_mapped_time")
                << " first_gap_us=" << first_gap_us
                << " epoch=" << session.imu_stream.epoch() << "\n";
    }
  }

  void drain_transport_packets() {
    BlePacket packet;
    while (transport->pop_packet(packet)) handle_packet(packet);
  }

  void update_stale_states(uint64_t now_ns) {
    for (auto& [id, session] : sessions) {
      if (session->last_packet_ns != 0 && now_ns > session->last_packet_ns + kStaleNs &&
          session->imu_stream.latest.status == xr_runtime::CONTROLLER_IMU_ACTIVE) {
        // Delivery can pause during device scans or synchronous native calls.
        // AHRS intervals follow the IMU clock, so a host timeout does not
        // invalidate its reference or require another optical yaw alignment.
        session->imu_stream.mark_stale();
        std::cerr << "[override_controller][WARN] Gear VR IMU stale for "
                  << session->stable_id << ": reason=host_receive_timeout host_gap_ms="
                  << double(now_ns - session->last_packet_ns) / 1.0e6
                  << " epoch=" << session->imu_stream.epoch() << "; AHRS=retained\n";
      }
    }
  }

  void pump(int timeout_ms, bool include_stdin, bool* stdin_ready = nullptr) {
    transport->pump(timeout_ms, include_stdin, stdin_ready);
    sync_transport_devices();
    drain_transport_packets();
    update_stale_states(static_cast<uint64_t>(monotonic_now_ns()));
  }

  size_t device_index_for_stable_id(const std::vector<DeviceInfo>& devices,
                                    const std::string& stable_id) const {
    for (size_t index = 0; index < devices.size(); ++index) {
      if (devices[index].fingerprint.uniq == stable_id) return index;
    }
    return std::numeric_limits<size_t>::max();
  }

  std::optional<InputEvent> pop_pending_event(const std::vector<DeviceInfo>& devices) {
    while (!pending_events.empty()) {
      PendingEvent pending = std::move(pending_events.front());
      pending_events.pop_front();
      const size_t index = device_index_for_stable_id(devices, pending.stable_id);
      if (index == std::numeric_limits<size_t>::max()) continue;
      pending.event.device_index = index;
      return pending.event;
    }
    return std::nullopt;
  }

  std::vector<DeviceInfo> make_device_views() const {
    std::vector<DeviceInfo> result;
    result.reserve(sessions.size());
    for (const auto& [id, session] : sessions) {
      DeviceInfo device;
      device.fingerprint = session->fingerprint;
      device.provider_device_index = result.size();
      device.identity_known = session->paired;
      device.readable = session->connected && session->notifications_active;
      device.open_error = session->error;
      result.push_back(std::move(device));
    }
    return result;
  }

  void update_device_views(std::vector<DeviceInfo>& devices) const {
    for (auto& device : devices) {
      const auto it = sessions.find(device.fingerprint.uniq);
      if (it == sessions.end()) {
        device.readable = false;
        continue;
      }
      const Session& session = *it->second;
      device.fingerprint = session.fingerprint;
      device.identity_known = session.paired;
      device.readable = session.connected && session.notifications_active;
      device.open_error = session.error;
    }
  }

  GearVrOptions options;
  std::unique_ptr<BleTransport> transport;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::deque<PendingEvent> pending_events;
};

GearVrInputProvider::GearVrInputProvider(ProviderOptionValues options)
    : impl_(std::make_unique<Impl>(std::move(options))) {
  std::cerr << "[override_controller][gearvr_ble] madgwick_beta=" << impl_->options.madgwick_beta
            << " gravity_initialization=" << (impl_->options.madgwick_beta > 0 ? "stationary_force" : "gyro_only")
            << " imu_continuity_revision=2\n";
}

GearVrInputProvider::~GearVrInputProvider() = default;

std::vector<DeviceInfo> GearVrInputProvider::scan_devices(bool open_readable) {
  (void)open_readable;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(impl_->options.initial_scan_ms);
  do {
    impl_->pump(50, false);
  } while (impl_->sessions.empty() && std::chrono::steady_clock::now() < deadline);
  return impl_->make_device_views();
}

void GearVrInputProvider::flush_events(std::vector<DeviceInfo>& devices) {
  impl_->pending_events.clear();
  impl_->pump(0, false);
  impl_->pending_events.clear();
  for (auto& [id, session] : impl_->sessions) session->imu_stream.clear_pending();
  impl_->update_device_views(devices);
}

std::optional<InputEvent> GearVrInputProvider::wait_event(std::vector<DeviceInfo>& devices,
                                                          int timeout_ms,
                                                          bool include_stdin) {
  if (auto event = impl_->pop_pending_event(devices)) return event;
  const auto deadline = timeout_ms < 0
      ? std::chrono::steady_clock::time_point::max()
      : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    int remaining = timeout_ms;
    if (timeout_ms >= 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return std::nullopt;
      remaining = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    }
    bool stdin_ready = false;
    impl_->pump(remaining, include_stdin, &stdin_ready);
    impl_->update_device_views(devices);
    if (stdin_ready) {
      std::string ignored;
      std::getline(std::cin, ignored);
      InputEvent event;
      event.device_index = std::numeric_limits<size_t>::max();
      event.timestamp_ns = monotonic_now_ns();
      return event;
    }
    if (auto event = impl_->pop_pending_event(devices)) return event;
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) return std::nullopt;
  }
}

std::string GearVrInputProvider::input_name(const DeviceInfo& device,
                                                uint16_t type,
                                                uint16_t code) const {
  (void)device;
  if (type == codes::kEvKey) return key_name(code);
  if (type == codes::kEvAbs) return abs_name(code);
  return "EV" + std::to_string(type) + ":" + std::to_string(code);
}

InputBindingSpec GearVrInputProvider::make_input_spec(const DeviceInfo& device,
                                                       uint16_t type,
                                                       uint16_t code) const {
  (void)device;
  InputBindingSpec spec;
  spec.type = type;
  spec.code = code;
  spec.name = input_name(device, type, code);
  if (type == codes::kEvAbs) {
    spec.kind = InputKind::AbsAxis;
    spec.abs_min = -32767;
    spec.abs_max = 32767;
    spec.abs_flat = 2048;
  } else {
    spec.kind = InputKind::Key;
  }
  return spec;
}

ConfigMigrationResult GearVrInputProvider::migrate_config(AppConfig& cfg) const {
  return migrate_legacy_config(cfg);
}

xr_runtime::ControllerImuStateV1 GearVrInputProvider::imu_state(const DeviceInfo& device) const {
  const auto it = impl_->sessions.find(device.fingerprint.uniq);
  return it == impl_->sessions.end() ? xr_runtime::ControllerImuStateV1{} : it->second->imu_stream.latest;
}

xr_runtime::ControllerImuStateV1 GearVrInputProvider::consume_imu_state(const DeviceInfo& device) {
  const auto it = impl_->sessions.find(device.fingerprint.uniq);
  return it == impl_->sessions.end() ? xr_runtime::ControllerImuStateV1{} : it->second->imu_stream.consume();
}

bool GearVrInputProvider::has_pending_imu(const DeviceInfo& device) const {
  const auto it = impl_->sessions.find(device.fingerprint.uniq);
  return it != impl_->sessions.end() && it->second->imu_stream.pending();
}

bool GearVrInputProvider::set_accelerometer_calibration(
    const DeviceInfo& device, const imu::AccelerometerCalibration& calibration) {
  const auto it = impl_->sessions.find(device.fingerprint.uniq);
  if (it == impl_->sessions.end()) return false;
  auto& stream = it->second->imu_stream;
  if (imu::same_accelerometer_calibration(stream.accelerometer_calibration(), calibration)) return false;
  stream.set_accelerometer_calibration(calibration);
  return true;
}

void GearVrInputProvider::close_devices(std::vector<DeviceInfo>& devices) {
  // Keep transport reconnect state alive across core reattach scans.
  impl_->update_device_views(devices);
}

bool GearVrInputProvider::set_device_grab(std::vector<DeviceInfo>& devices,
                                          const std::set<size_t>& device_indices,
                                          bool enabled,
                                          std::ostream* log) {
  (void)devices;
  (void)device_indices;
  (void)enabled;
  (void)log;
  return false;
}

}  // namespace xr_override_controller::gearvr
