#include "xiao_nrf54l15_input_provider.hpp"

#include "xiao_nrf54l15_input_codes.hpp"
#include "xiao_nrf54l15_options.hpp"
#include "xiao_nrf54l15_serial_transport.hpp"
#include "xr_controller_v1_protocol.hpp"

#include <xr_override_controller/imu/controller_imu_processor.hpp>

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

namespace xr_override_controller::xiao_nrf54l15 {
namespace codes = input_codes;
namespace {

int64_t monotonic_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string stable_material(const DeviceFingerprint& fp) {
  return fp.platform + "|" + fp.backend + "|" + fp.uniq + "|" +
         fp.by_id_path + "|" + fp.by_path + "|" + fp.name;
}

std::string key_name(uint16_t code) {
  switch (code) {
    case codes::kBtnSouth: return "XCTL_BUTTON_A";
    case codes::kBtnEast: return "XCTL_BUTTON_B";
    case codes::kBtnNorth: return "XCTL_BUTTON_C";
    case codes::kBtnTrigger: return "XCTL_TRIGGER";
    case codes::kBtnTl2: return "XCTL_GRIP";
    case codes::kBtnStart: return "XCTL_MENU";
    case codes::kBtnThumbL: return "XCTL_STICK_CLICK";
    case codes::kKeyUp: return "XCTL_DPAD_UP";
    case codes::kKeyDown: return "XCTL_DPAD_DOWN";
    case codes::kKeyLeft: return "XCTL_DPAD_LEFT";
    case codes::kKeyRight: return "XCTL_DPAD_RIGHT";
    default: return "XCTL_KEY_" + std::to_string(code);
  }
}

std::string abs_name(uint16_t code) {
  switch (code) {
    case codes::kAbsX: return "XCTL_THUMBSTICK_X";
    case codes::kAbsY: return "XCTL_THUMBSTICK_Y";
    case codes::kAbsZ: return "XCTL_TRIGGER_AXIS";
    case codes::kAbsRz: return "XCTL_GRIP_AXIS";
    default: return "XCTL_ABS_" + std::to_string(code);
  }
}

struct DeviceClockSync {
  bool initialized = false;
  uint64_t offset_ns = 0;

  void reset() {
    initialized = false;
    offset_ns = 0;
  }

  static uint64_t candidate_offset(uint64_t device_timestamp_us,
                                   uint64_t receive_timestamp_ns,
                                   uint64_t estimated_wire_ns) {
    const uint64_t device_ns = device_timestamp_us * 1000ull;
    const uint64_t adjusted_receive_ns =
        receive_timestamp_ns > estimated_wire_ns
            ? receive_timestamp_ns - estimated_wire_ns
            : receive_timestamp_ns;
    return adjusted_receive_ns > device_ns ? adjusted_receive_ns - device_ns : 0;
  }

  void prime(uint64_t newest_device_timestamp_us,
             uint64_t receive_timestamp_ns,
             uint64_t estimated_wire_ns) {
    if (initialized) return;
    initialized = true;
    offset_ns = candidate_offset(newest_device_timestamp_us,
                                 receive_timestamp_ns,
                                 estimated_wire_ns);
  }

  uint64_t map(uint64_t device_timestamp_us,
               uint64_t receive_timestamp_ns,
               uint64_t estimated_wire_ns) {
    const uint64_t device_ns = device_timestamp_us * 1000ull;
    const uint64_t candidate = candidate_offset(
        device_timestamp_us, receive_timestamp_ns, estimated_wire_ns);

    if (!initialized) {
      initialized = true;
      offset_ns = candidate;
    } else if (candidate < offset_ns) {
      // Lower observed latency is safe to accept immediately.
      offset_ns = candidate;
    } else {
      // Track normal MCU/host clock drift without following transient serial or
      // scheduler latency spikes. At 208 Hz this permits about 0.4 ms/s of
      // upward correction, far above crystal drift but below transport jitter.
      constexpr uint64_t kMaxUpwardSlewPerSampleNs = 2000;
      offset_ns += std::min(candidate - offset_ns,
                            kMaxUpwardSlewPerSampleNs);
    }

    const uint64_t mapped = device_ns + offset_ns;
    return std::min(mapped, receive_timestamp_ns);
  }
};

}  // namespace

struct XiaoNrf54l15InputProvider::Impl {
  struct PendingEvent {
    std::string stable_id;
    InputEvent event;
  };

  struct Session {
    explicit Session(float beta) : imu_processor(beta) {}

    std::string stable_id;
    SerialDeviceSnapshot snapshot;
    DeviceFingerprint fingerprint;
    bool seen = false;
    bool connected = false;
    bool ever_connected = false;
    bool protocol_seen = false;
    std::string protocol_device_uid;
    bool controls_valid = false;
    bool have_sequence = false;
    uint32_t previous_sequence = 0;
    uint32_t previous_buttons = 0;
    std::array<int16_t, kXrControllerV1AxisCount> previous_axes{};
    uint64_t last_packet_ns = 0;
    uint64_t last_sample_ns = 0;
    XrControllerV1StreamDecoder decoder;
    DeviceClockSync clock_sync;
    imu::ControllerImuProcessor imu_processor;
    xr_runtime::ControllerImuStateV1 imu;
  };

  explicit Impl(ProviderOptionValues values)
      : options(load_xiao_nrf54l15_options(values)),
        transport(make_platform_serial_transport(options)) {
    if (!transport) {
      throw std::runtime_error(
          "XIAO nRF54L15 serial transport is unavailable on this platform");
    }
  }

  void update_fingerprint(Session& session) {
    const auto& snapshot = session.snapshot;
    auto& fp = session.fingerprint;
    fp.platform = transport->platform_name();
    fp.backend = "xiao_nrf54l15";
    fp.event_path = snapshot.port_path;
    fp.by_id_path = snapshot.by_id_path;
    fp.by_path = snapshot.by_path;
    fp.name = snapshot.name.empty() ? "XIAO nRF54L15 Controller" : snapshot.name;
    fp.phys = snapshot.phys;
    fp.uniq = session.protocol_device_uid.empty()
                  ? snapshot.stable_id
                  : "xiao_nrf54l15:uid:" + session.protocol_device_uid;
    fp.bustype = codes::kBusUsb;
    fp.vendor = snapshot.vendor;
    fp.product = snapshot.product;
    fp.version = snapshot.version;
    fp.stable_hash = stable_hash64(stable_material(fp));
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
      auto existing = std::find_if(
          pending_events.begin(), pending_events.end(),
          [&](const PendingEvent& queued) {
            return queued.stable_id == session.stable_id &&
                   queued.event.type == type && queued.event.code == code;
          });
      if (existing != pending_events.end()) {
        existing->event = event;
      } else {
        pending_events.push_back({session.stable_id, event});
      }
    } else {
      pending_events.push_back({session.stable_id, event});
    }
  }

  void release_all_inputs(Session& session, uint64_t timestamp_ns) {
    static constexpr std::array<uint16_t, 11> kKeys{{
        codes::kBtnSouth,
        codes::kBtnEast,
        codes::kBtnNorth,
        codes::kBtnTrigger,
        codes::kBtnTl2,
        codes::kBtnStart,
        codes::kBtnThumbL,
        codes::kKeyUp,
        codes::kKeyDown,
        codes::kKeyLeft,
        codes::kKeyRight,
    }};
    for (uint16_t code : kKeys) {
      queue_event(session, codes::kEvKey, code, 0, timestamp_ns);
    }
    queue_event(session, codes::kEvAbs, codes::kAbsX, 0, timestamp_ns);
    queue_event(session, codes::kEvAbs, codes::kAbsY, 0, timestamp_ns);
    queue_event(session, codes::kEvAbs, codes::kAbsZ, 0, timestamp_ns);
    queue_event(session, codes::kEvAbs, codes::kAbsRz, 0, timestamp_ns);
    session.previous_buttons = 0;
    session.previous_axes.fill(0);
    session.controls_valid = false;
  }

  void update_connected(Session& session, bool connected) {
    if (session.connected == connected) return;
    session.connected = connected;
    session.ever_connected = session.ever_connected || connected;
    if (connected) {
      if (session.protocol_seen &&
          session.imu.status != xr_runtime::CONTROLLER_IMU_ACTIVE) {
        session.imu.status = xr_runtime::CONTROLLER_IMU_CONNECTED;
      }
      return;
    }

    if (session.controls_valid || session.previous_buttons != 0 ||
        std::any_of(session.previous_axes.begin(), session.previous_axes.end(),
                    [](int16_t value) { return value != 0; })) {
      release_all_inputs(session, static_cast<uint64_t>(monotonic_now_ns()));
    }
    session.decoder.reset();
    session.clock_sync.reset();
    session.imu_processor.reset();
    session.have_sequence = false;
    session.last_packet_ns = 0;
    session.last_sample_ns = 0;
    session.imu.data_flags = 0;
    session.imu.sample_count = 0;
    if (session.protocol_seen) {
      session.imu.status = session.ever_connected
                               ? xr_runtime::CONTROLLER_IMU_LOST
                               : xr_runtime::CONTROLLER_IMU_CONFIGURED;
    }
  }

  void sync_transport_devices() {
    for (auto& [id, session] : sessions) session->seen = false;

    for (const auto& snapshot : transport->devices()) {
      if (snapshot.stable_id.empty()) continue;
      auto& session_ptr = sessions[snapshot.stable_id];
      if (!session_ptr) {
        session_ptr = std::make_unique<Session>(
            static_cast<float>(options.madgwick_beta));
        session_ptr->stable_id = snapshot.stable_id;
      }
      Session& session = *session_ptr;
      session.seen = true;
      session.snapshot = snapshot;
      update_fingerprint(session);
      update_connected(session, snapshot.connected);
      if (!snapshot.connected && session.protocol_seen) {
        session.imu.status = session.ever_connected
                                 ? xr_runtime::CONTROLLER_IMU_LOST
                                 : xr_runtime::CONTROLLER_IMU_CONFIGURED;
      } else if (snapshot.connected && session.protocol_seen &&
                 session.last_packet_ns == 0) {
        session.imu.status = xr_runtime::CONTROLLER_IMU_CONNECTED;
      }
    }

    for (auto& [id, session] : sessions) {
      if (!session->seen && session->connected) update_connected(*session, false);
    }
  }

  void emit_button_changes(Session& session,
                           uint32_t buttons,
                           uint64_t timestamp_ns) {
    static constexpr std::array<std::pair<uint32_t, uint16_t>, 11> kMapping{{
        {kButtonA, codes::kBtnSouth},
        {kButtonB, codes::kBtnEast},
        {kButtonC, codes::kBtnNorth},
        {kButtonTrigger, codes::kBtnTrigger},
        {kButtonGrip, codes::kBtnTl2},
        {kButtonMenu, codes::kBtnStart},
        {kButtonStickClick, codes::kBtnThumbL},
        {kButtonDpadUp, codes::kKeyUp},
        {kButtonDpadDown, codes::kKeyDown},
        {kButtonDpadLeft, codes::kKeyLeft},
        {kButtonDpadRight, codes::kKeyRight},
    }};

    buttons &= kKnownButtonMask;
    for (const auto& [bit, code] : kMapping) {
      const bool now = (buttons & bit) != 0;
      const bool before = (session.previous_buttons & bit) != 0;
      if (now != before) {
        queue_event(session, codes::kEvKey, code, now ? 1 : 0, timestamp_ns);
      }
    }
    session.previous_buttons = buttons;
  }

  void emit_axis_changes(Session& session,
                         const std::array<int16_t, kXrControllerV1AxisCount>& axes,
                         uint64_t timestamp_ns) {
    static constexpr std::array<uint16_t, kXrControllerV1AxisCount> kCodes{{
        codes::kAbsX, codes::kAbsY, codes::kAbsZ, codes::kAbsRz,
    }};
    for (size_t i = 0; i < axes.size(); ++i) {
      if (axes[i] != session.previous_axes[i]) {
        queue_event(session, codes::kEvAbs, kCodes[i], axes[i], timestamp_ns);
      }
    }
    session.previous_axes = axes;
  }

  void handle_controls(Session& session,
                       const XrControllerV1Packet& packet,
                       uint64_t timestamp_ns) {
    const bool valid = (packet.flags & kFlagControlsValid) != 0;
    if (!valid) {
      if (session.controls_valid) release_all_inputs(session, timestamp_ns);
      return;
    }
    emit_button_changes(session, packet.buttons, timestamp_ns);
    emit_axis_changes(session, packet.axes, timestamp_ns);
    session.controls_valid = true;
  }

  uint64_t estimated_wire_ns() const {
    return (kXrControllerV1PacketSize * 10ull * 1'000'000'000ull) /
           std::max<uint32_t>(1u, options.baud_rate);
  }

  void reset_packet_timing(Session& session) {
    session.clock_sync.reset();
    session.imu_processor.reset();
    session.last_sample_ns = 0;
    session.have_sequence = false;
  }

  void handle_identity(Session& session,
                       const XrControllerIdentityV1Packet& identity) {
    const std::string uid = xr_controller_device_uid_hex(identity);
    if (uid.empty()) return;
    if (!session.protocol_device_uid.empty() &&
        session.protocol_device_uid != uid) {
      std::cerr << "[xiao_nrf54l15] warning: protocol device UID changed on "
                << session.snapshot.port_path << " from "
                << session.protocol_device_uid << " to " << uid << std::endl;
    }
    if (session.protocol_device_uid != uid) {
      session.protocol_device_uid = uid;
      update_fingerprint(session);
      std::cerr << "[xiao_nrf54l15] device_uid=" << uid
                << " port=" << session.snapshot.port_path << std::endl;
    }
  }

  void handle_packet(Session& session,
                     const XrControllerV1Packet& packet,
                     uint64_t receive_timestamp_ns) {
    session.protocol_seen = true;
    session.last_packet_ns = receive_timestamp_ns;

    if (session.have_sequence) {
      const uint32_t delta = packet.sequence - session.previous_sequence;
      if (delta == 0) return;
      // Unsigned wrap naturally produces a small positive delta. A large delta
      // means a device reboot/backward sequence and must reset the AHRS clock.
      if (delta > 0x80000000u) {
        reset_packet_timing(session);
      }
    }
    session.have_sequence = true;
    session.previous_sequence = packet.sequence;

    const uint64_t wire_ns = estimated_wire_ns();
    uint64_t sample_timestamp_ns = receive_timestamp_ns;
    if ((packet.flags & kFlagTimestampValid) != 0 && packet.timestamp_us != 0) {
      sample_timestamp_ns = session.clock_sync.map(
          packet.timestamp_us, receive_timestamp_ns, wire_ns);
    }
    if (session.last_sample_ns != 0 &&
        sample_timestamp_ns <= session.last_sample_ns) {
      sample_timestamp_ns = session.last_sample_ns + 1;
    }
    session.last_sample_ns = sample_timestamp_ns;

    imu::RawControllerImuSample raw;
    raw.host_timestamp_ns = sample_timestamp_ns;
    raw.device_timestamp_ticks = packet.timestamp_us;
    raw.angular_velocity_rad_s = {
        packet.gyro_rad_s[0], packet.gyro_rad_s[1], packet.gyro_rad_s[2]};
    raw.specific_force_m_s2 = {
        packet.accel_m_s2[0], packet.accel_m_s2[1], packet.accel_m_s2[2]};
    raw.gyroscope_valid = true;
    raw.accelerometer_valid = true;

    const imu::QuaternionXyzw orientation = session.imu_processor.process(raw);
    const imu::RawControllerImuSample corrected =
        session.imu_processor.corrected_sample();

    auto& state = session.imu;
    state = {};
    state.status = xr_runtime::CONTROLLER_IMU_ACTIVE;
    state.capability_flags = xr_runtime::CONTROLLER_IMU_CAP_GYROSCOPE |
                             xr_runtime::CONTROLLER_IMU_CAP_ACCELEROMETER |
                             xr_runtime::CONTROLLER_IMU_CAP_ORIENTATION;
    state.data_flags = xr_runtime::CONTROLLER_IMU_GYROSCOPE_VALID |
                       xr_runtime::CONTROLLER_IMU_ACCELEROMETER_VALID |
                       xr_runtime::CONTROLLER_IMU_ORIENTATION_VALID |
                       xr_runtime::CONTROLLER_IMU_HOST_TIME_SYNCED;
    if (session.imu_processor.gyro_calibrated()) {
      state.data_flags |= xr_runtime::CONTROLLER_IMU_GYROSCOPE_CALIBRATED;
    }
    state.sequence = packet.sequence;
    state.latest_sample_timestamp_ns = sample_timestamp_ns;
    state.orientation_timestamp_ns = sample_timestamp_ns;
    state.orientation_xyzw[0] = orientation.x;
    state.orientation_xyzw[1] = orientation.y;
    state.orientation_xyzw[2] = orientation.z;
    state.orientation_xyzw[3] = orientation.w;
    state.sample_count = 1;
    auto& sample = state.samples[0];
    sample.timestamp_ns = sample_timestamp_ns;
    sample.angular_velocity_rad_s[0] = corrected.angular_velocity_rad_s.x;
    sample.angular_velocity_rad_s[1] = corrected.angular_velocity_rad_s.y;
    sample.angular_velocity_rad_s[2] = corrected.angular_velocity_rad_s.z;
    sample.specific_force_m_s2[0] = corrected.specific_force_m_s2.x;
    sample.specific_force_m_s2[1] = corrected.specific_force_m_s2.y;
    sample.specific_force_m_s2[2] = corrected.specific_force_m_s2.z;
    sample.flags = xr_runtime::CONTROLLER_IMU_GYROSCOPE_VALID |
                   xr_runtime::CONTROLLER_IMU_ACCELEROMETER_VALID |
                   xr_runtime::CONTROLLER_IMU_HOST_TIME_SYNCED;
    if (session.imu_processor.gyro_calibrated()) {
      sample.flags |= xr_runtime::CONTROLLER_IMU_GYROSCOPE_CALIBRATED;
    }

    handle_controls(session, packet, sample_timestamp_ns);
  }

  void drain_transport_bytes() {
    SerialBytes bytes;
    while (transport->pop_bytes(bytes)) {
      const auto it = sessions.find(bytes.stable_id);
      if (it == sessions.end()) continue;
      Session& session = *it->second;
      if (!session.connected || bytes.bytes.empty()) continue;
      session.decoder.append(bytes.bytes.data(), bytes.bytes.size());

      std::vector<XrControllerV1Packet> packets;
      while (true) {
        auto packet = session.decoder.pop();
        while (auto identity = session.decoder.pop_identity()) {
          handle_identity(session, *identity);
        }
        if (!packet) break;
        packets.push_back(*packet);
      }
      if (packets.empty()) continue;

      if (session.have_sequence) {
        const uint32_t delta = packets.front().sequence - session.previous_sequence;
        if (delta > 0x80000000u) reset_packet_timing(session);
      }

      // A serial read may contain several complete packets. Anchor the newest
      // device timestamp to the end of the read before processing the batch in
      // wire order, preserving the real 208 Hz spacing even on the first read.
      const auto newest_timestamp = std::find_if(
          packets.rbegin(), packets.rend(), [](const XrControllerV1Packet& packet) {
            return (packet.flags & kFlagTimestampValid) != 0 && packet.timestamp_us != 0;
          });
      if (newest_timestamp != packets.rend()) {
        session.clock_sync.prime(newest_timestamp->timestamp_us,
                                 bytes.host_timestamp_ns,
                                 estimated_wire_ns());
      }

      for (const auto& packet : packets) {
        handle_packet(session, packet, bytes.host_timestamp_ns);
      }
    }
  }

  void update_stale_states(uint64_t now_ns) {
    const uint64_t stale_ns = static_cast<uint64_t>(options.stale_ms) * 1'000'000ull;
    for (auto& [id, session] : sessions) {
      if (session->last_packet_ns != 0 && now_ns > session->last_packet_ns + stale_ns) {
        if (session->imu.status == xr_runtime::CONTROLLER_IMU_ACTIVE) {
          session->imu.status = xr_runtime::CONTROLLER_IMU_STALE;
          session->imu.data_flags = 0;
          session->imu.sample_count = 0;
        }
        if (session->controls_valid) {
          release_all_inputs(*session, now_ns);
        }
      }
    }
  }

  void pump(int timeout_ms, bool include_stdin, bool* stdin_ready = nullptr) {
    transport->pump(timeout_ms, include_stdin, stdin_ready);
    sync_transport_devices();
    drain_transport_bytes();
    update_stale_states(static_cast<uint64_t>(monotonic_now_ns()));
  }

  size_t device_index_for_stable_id(const std::vector<DeviceInfo>& devices,
                                    const std::string& stable_id) const {
    const auto session_it = sessions.find(stable_id);
    if (session_it == sessions.end()) return std::numeric_limits<size_t>::max();
    const std::string& uniq = session_it->second->fingerprint.uniq;
    for (size_t index = 0; index < devices.size(); ++index) {
      if (devices[index].fingerprint.uniq == uniq) return index;
    }
    return std::numeric_limits<size_t>::max();
  }

  std::optional<InputEvent> pop_pending_event(
      const std::vector<DeviceInfo>& devices) {
    while (!pending_events.empty()) {
      PendingEvent pending = std::move(pending_events.front());
      pending_events.pop_front();
      const size_t index =
          device_index_for_stable_id(devices, pending.stable_id);
      if (index == std::numeric_limits<size_t>::max()) continue;
      pending.event.device_index = index;
      return pending.event;
    }
    return std::nullopt;
  }

  bool visible(const Session& session) const {
    return session.protocol_seen || session.snapshot.explicit_candidate;
  }

  bool has_protocol_device() const {
    return std::any_of(sessions.begin(), sessions.end(), [](const auto& entry) {
      return entry.second->protocol_seen;
    });
  }

  bool all_protocol_devices_have_uid() const {
    bool found = false;
    for (const auto& [transport_id, session] : sessions) {
      (void)transport_id;
      if (!session->protocol_seen) continue;
      found = true;
      if (session->protocol_device_uid.empty()) return false;
    }
    return found;
  }


  const Session* session_for_device_uniq(const std::string& uniq) const {
    const auto direct = sessions.find(uniq);
    if (direct != sessions.end()) return direct->second.get();
    for (const auto& [transport_id, session] : sessions) {
      (void)transport_id;
      if (session->fingerprint.uniq == uniq) return session.get();
    }
    return nullptr;
  }

  std::vector<DeviceInfo> make_device_views() const {
    std::vector<DeviceInfo> result;
    for (const auto& [id, session] : sessions) {
      if (!visible(*session)) continue;
      DeviceInfo device;
      device.fingerprint = session->fingerprint;
      device.provider_device_index = result.size();
      device.identity_known = session->protocol_seen ||
                              session->snapshot.explicit_candidate;
      device.readable = session->connected && session->protocol_seen;
      device.open_error = session->snapshot.error;
      if (session->connected && !session->protocol_seen &&
          device.open_error.empty()) {
        device.open_error = "waiting for xr_controller_v1 XCTL packets";
      }
      result.push_back(std::move(device));
    }
    return result;
  }

  void update_device_views(std::vector<DeviceInfo>& devices) const {
    for (auto& device : devices) {
      const Session* session_ptr = session_for_device_uniq(device.fingerprint.uniq);
      if (!session_ptr || !visible(*session_ptr)) {
        device.readable = false;
        continue;
      }
      const Session& session = *session_ptr;
      const size_t provider_slot = device.provider_slot;
      const size_t provider_device_index = device.provider_device_index;
      device.fingerprint = session.fingerprint;
      device.provider_slot = provider_slot;
      device.provider_device_index = provider_device_index;
      device.identity_known = session.protocol_seen ||
                              session.snapshot.explicit_candidate;
      device.readable = session.connected && session.protocol_seen;
      device.open_error = session.snapshot.error;
      if (session.connected && !session.protocol_seen &&
          device.open_error.empty()) {
        device.open_error = "waiting for xr_controller_v1 XCTL packets";
      }
    }
  }

  XiaoNrf54l15Options options;
  std::unique_ptr<SerialTransport> transport;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::deque<PendingEvent> pending_events;
};

XiaoNrf54l15InputProvider::XiaoNrf54l15InputProvider(
    ProviderOptionValues options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

XiaoNrf54l15InputProvider::~XiaoNrf54l15InputProvider() = default;

std::vector<DeviceInfo> XiaoNrf54l15InputProvider::scan_devices(
    bool open_readable) {
  (void)open_readable;
  impl_->pump(0, false);
  auto current = impl_->make_device_views();

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(impl_->options.initial_scan_ms);
  std::optional<std::chrono::steady_clock::time_point> first_protocol_seen;
  if (impl_->has_protocol_device()) {
    first_protocol_seen = std::chrono::steady_clock::now();
  }
  do {
    impl_->pump(25, false);
    current = impl_->make_device_views();
    const auto now = std::chrono::steady_clock::now();
    if (impl_->has_protocol_device()) {
      if (!first_protocol_seen) first_protocol_seen = now;
      // Allow all serial devices to publish both XCTL and the periodic XCID
      // identity before the trainer freezes fingerprints. Older firmware
      // without XCID simply uses the full initial_scan_ms window and retains
      // the existing port-based fallback.
      if (impl_->all_protocol_devices_have_uid() &&
          now >= *first_protocol_seen + std::chrono::milliseconds(200)) {
        break;
      }
    }
  } while (std::chrono::steady_clock::now() < deadline);
  return current;
}

void XiaoNrf54l15InputProvider::flush_events(
    std::vector<DeviceInfo>& devices) {
  impl_->pending_events.clear();
  impl_->pump(0, false);
  impl_->pending_events.clear();
  impl_->update_device_views(devices);
}

std::optional<InputEvent> XiaoNrf54l15InputProvider::wait_event(
    std::vector<DeviceInfo>& devices,
    int timeout_ms,
    bool include_stdin) {
  if (auto event = impl_->pop_pending_event(devices)) return event;
  const auto deadline = timeout_ms < 0
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
  while (true) {
    int remaining = timeout_ms;
    if (timeout_ms >= 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return std::nullopt;
      remaining = static_cast<int>(std::chrono::duration_cast<
          std::chrono::milliseconds>(deadline - now).count());
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
    if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
  }
}

std::string XiaoNrf54l15InputProvider::input_name(
    const DeviceInfo& device, uint16_t type, uint16_t code) const {
  (void)device;
  if (type == codes::kEvKey) return key_name(code);
  if (type == codes::kEvAbs) return abs_name(code);
  return "XCTL_EV" + std::to_string(type) + ":" + std::to_string(code);
}

InputBindingSpec XiaoNrf54l15InputProvider::make_input_spec(
    const DeviceInfo& device, uint16_t type, uint16_t code) const {
  (void)device;
  InputBindingSpec spec;
  spec.type = type;
  spec.code = code;
  spec.name = input_name(device, type, code);
  if (type == codes::kEvAbs) {
    spec.kind = InputKind::AbsAxis;
    spec.abs_min = -32768;
    spec.abs_max = 32767;
    spec.abs_flat = impl_->options.axis_flat;
  } else {
    spec.kind = InputKind::Key;
  }
  return spec;
}

xr_runtime::ControllerImuStateV1 XiaoNrf54l15InputProvider::imu_state(
    const DeviceInfo& device) const {
  for (const auto& [transport_id, session] : impl_->sessions) {
    if (session->fingerprint.uniq == device.fingerprint.uniq) {
      return session->imu;
    }
  }
  return xr_runtime::ControllerImuStateV1{};
}

void XiaoNrf54l15InputProvider::close_devices(
    std::vector<DeviceInfo>& devices) {
  // Keep serial sessions alive across the core's periodic reattach pass. The
  // transport itself closes dead descriptors and reconnects them by stable ID.
  impl_->update_device_views(devices);
}

bool XiaoNrf54l15InputProvider::set_device_grab(
    std::vector<DeviceInfo>& devices,
    const std::set<size_t>& device_indices,
    bool enabled,
    std::ostream* log) {
  (void)devices;
  (void)device_indices;
  (void)enabled;
  (void)log;
  return false;
}

}  // namespace xr_override_controller::xiao_nrf54l15
