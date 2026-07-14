#include "../gearvr_ble_transport.hpp"

#include "../gearvr_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace xr_override_controller::gearvr {
namespace {

// Minimal sd-bus ABI declarations. Loading libsystemd dynamically keeps the
// evdev-only runtime free from a hard libsystemd development dependency.
struct sd_bus;
struct sd_bus_slot;
struct sd_bus_message;
struct sd_bus_error {
  const char* name;
  const char* message;
  int need_free;
};
using sd_bus_message_handler_t = int (*)(sd_bus_message*, void*, sd_bus_error*);

class SdBusApi {
 public:
  using OpenSystemFn = int (*)(sd_bus**);
  using BusUnrefFn = sd_bus* (*)(sd_bus*);
  using SlotUnrefFn = sd_bus_slot* (*)(sd_bus_slot*);
  using MessageUnrefFn = sd_bus_message* (*)(sd_bus_message*);
  using AddMatchFn = int (*)(sd_bus*, sd_bus_slot**, const char*, sd_bus_message_handler_t, void*);
  using CallMethodFn = int (*)(sd_bus*, const char*, const char*, const char*, const char*, sd_bus_error*, sd_bus_message**, const char*, ...);
  using CallMethodAsyncFn = int (*)(sd_bus*, sd_bus_slot**, const char*, const char*, const char*, const char*, sd_bus_message_handler_t, void*, uint64_t, const char*, ...);
  using ProcessFn = int (*)(sd_bus*, sd_bus_message**);
  using GetFdFn = int (*)(sd_bus*);
  using GetEventsFn = int (*)(sd_bus*);
  using GetTimeoutFn = int (*)(sd_bus*, uint64_t*);
  using SetMethodTimeoutFn = int (*)(sd_bus*, uint64_t);
  using MessageGetPathFn = const char* (*)(sd_bus_message*);
  using MessageGetErrnoFn = int (*)(sd_bus_message*);
  using MessageReadFn = int (*)(sd_bus_message*, const char*, ...);
  using MessageReadArrayFn = int (*)(sd_bus_message*, char, const void**, size_t*);
  using MessageEnterContainerFn = int (*)(sd_bus_message*, char, const char*);
  using MessageExitContainerFn = int (*)(sd_bus_message*);
  using MessagePeekTypeFn = int (*)(sd_bus_message*, char*, const char**);
  using MessageSkipFn = int (*)(sd_bus_message*, const char*);
  using MessageNewMethodCallFn = int (*)(sd_bus*, sd_bus_message**, const char*, const char*, const char*, const char*);
  using MessageAppendFn = int (*)(sd_bus_message*, const char*, ...);
  using MessageAppendArrayFn = int (*)(sd_bus_message*, char, const void*, size_t);
  using MessageOpenContainerFn = int (*)(sd_bus_message*, char, const char*);
  using MessageCloseContainerFn = int (*)(sd_bus_message*);
  using BusCallFn = int (*)(sd_bus*, sd_bus_message*, uint64_t, sd_bus_error*, sd_bus_message**);
  using ErrorFreeFn = void (*)(sd_bus_error*);

  SdBusApi() {
    handle_ = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!handle_) throw std::runtime_error(std::string("cannot load libsystemd.so.0: ") + dlerror());
    open_system = load<OpenSystemFn>("sd_bus_open_system");
    bus_unref = load<BusUnrefFn>("sd_bus_unref");
    slot_unref = load<SlotUnrefFn>("sd_bus_slot_unref");
    message_unref = load<MessageUnrefFn>("sd_bus_message_unref");
    add_match = load<AddMatchFn>("sd_bus_add_match");
    call_method = load<CallMethodFn>("sd_bus_call_method");
    call_method_async = load<CallMethodAsyncFn>("sd_bus_call_method_async");
    process = load<ProcessFn>("sd_bus_process");
    get_fd = load<GetFdFn>("sd_bus_get_fd");
    get_events = load<GetEventsFn>("sd_bus_get_events");
    get_timeout = load<GetTimeoutFn>("sd_bus_get_timeout");
    set_method_timeout = load<SetMethodTimeoutFn>("sd_bus_set_method_call_timeout");
    message_get_path = load<MessageGetPathFn>("sd_bus_message_get_path");
    message_get_errno = load<MessageGetErrnoFn>("sd_bus_message_get_errno");
    message_read = load<MessageReadFn>("sd_bus_message_read");
    message_read_array = load<MessageReadArrayFn>("sd_bus_message_read_array");
    message_enter_container = load<MessageEnterContainerFn>("sd_bus_message_enter_container");
    message_exit_container = load<MessageExitContainerFn>("sd_bus_message_exit_container");
    message_peek_type = load<MessagePeekTypeFn>("sd_bus_message_peek_type");
    message_skip = load<MessageSkipFn>("sd_bus_message_skip");
    message_new_method_call = load<MessageNewMethodCallFn>("sd_bus_message_new_method_call");
    message_append = load<MessageAppendFn>("sd_bus_message_append");
    message_append_array = load<MessageAppendArrayFn>("sd_bus_message_append_array");
    message_open_container = load<MessageOpenContainerFn>("sd_bus_message_open_container");
    message_close_container = load<MessageCloseContainerFn>("sd_bus_message_close_container");
    bus_call = load<BusCallFn>("sd_bus_call");
    error_free = load<ErrorFreeFn>("sd_bus_error_free");
  }

  ~SdBusApi() {
    if (handle_) dlclose(handle_);
  }

  SdBusApi(const SdBusApi&) = delete;
  SdBusApi& operator=(const SdBusApi&) = delete;

  OpenSystemFn open_system = nullptr;
  BusUnrefFn bus_unref = nullptr;
  SlotUnrefFn slot_unref = nullptr;
  MessageUnrefFn message_unref = nullptr;
  AddMatchFn add_match = nullptr;
  CallMethodFn call_method = nullptr;
  CallMethodAsyncFn call_method_async = nullptr;
  ProcessFn process = nullptr;
  GetFdFn get_fd = nullptr;
  GetEventsFn get_events = nullptr;
  GetTimeoutFn get_timeout = nullptr;
  SetMethodTimeoutFn set_method_timeout = nullptr;
  MessageGetPathFn message_get_path = nullptr;
  MessageGetErrnoFn message_get_errno = nullptr;
  MessageReadFn message_read = nullptr;
  MessageReadArrayFn message_read_array = nullptr;
  MessageEnterContainerFn message_enter_container = nullptr;
  MessageExitContainerFn message_exit_container = nullptr;
  MessagePeekTypeFn message_peek_type = nullptr;
  MessageSkipFn message_skip = nullptr;
  MessageNewMethodCallFn message_new_method_call = nullptr;
  MessageAppendFn message_append = nullptr;
  MessageAppendArrayFn message_append_array = nullptr;
  MessageOpenContainerFn message_open_container = nullptr;
  MessageCloseContainerFn message_close_container = nullptr;
  BusCallFn bus_call = nullptr;
  ErrorFreeFn error_free = nullptr;

 private:
  template <typename T>
  T load(const char* name) {
    dlerror();
    void* symbol = dlsym(handle_, name);
    const char* error = dlerror();
    if (error || !symbol) throw std::runtime_error(std::string("libsystemd missing ") + name);
    return reinterpret_cast<T>(symbol);
  }

  void* handle_ = nullptr;
};

struct MessageGuard {
  SdBusApi* api = nullptr;
  sd_bus_message* value = nullptr;
  ~MessageGuard() {
    if (api && value) api->message_unref(value);
  }
};

struct ErrorGuard {
  SdBusApi* api = nullptr;
  sd_bus_error value{nullptr, nullptr, 0};
  ~ErrorGuard() {
    if (api) api->error_free(&value);
  }
};

constexpr const char* kBluezService = "org.bluez";
constexpr const char* kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char* kDeviceInterface = "org.bluez.Device1";
constexpr const char* kGattServiceInterface = "org.bluez.GattService1";
constexpr const char* kGattCharacteristicInterface = "org.bluez.GattCharacteristic1";
constexpr uint64_t kManagedObjectsRefreshNs = 2'000'000'000ull;
constexpr uint64_t kKeepAliveNs = 10'000'000'000ull;
constexpr uint64_t kInitCommandGapNs = 20'000'000ull;
constexpr uint64_t kDbusCallTimeoutUsec = 5'000'000ull;

int64_t monotonic_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint64_t monotonic_now_us() {
  return static_cast<uint64_t>(monotonic_now_ns() / 1000);
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string uppercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

bool is_gearvr_name(const std::string& name) {
  const std::string lower = lower_copy(name);
  return lower.rfind("gear vr controller", 0) == 0;
}

class LinuxGearVrBleTransport final : public BleTransport {
 public:
  explicit LinuxGearVrBleTransport(InputProviderOptions options)
      : options_(std::move(options)), api_(std::make_unique<SdBusApi>()) {
    const int rc = api_->open_system(&bus_);
    if (rc < 0 || !bus_) {
      throw std::runtime_error("cannot open system D-Bus for BlueZ: " + std::to_string(rc));
    }
    (void)api_->set_method_timeout(bus_, kDbusCallTimeoutUsec);
    add_matches();
    refresh_requested_ = true;
  }

  ~LinuxGearVrBleTransport() override {
    for (auto& [id, session_ptr] : sessions_) {
      Session& session = *session_ptr;
      if (session.notifications_started && !session.notify_path.empty()) {
        MessageGuard reply{api_.get(), nullptr};
        (void)api_->call_method(bus_, kBluezService, session.notify_path.c_str(),
                                kGattCharacteristicInterface, "StopNotify", nullptr,
                                &reply.value, nullptr);
      }
      if (!session.command_path.empty()) (void)write_command(session, kCommandOff, false);
      if (session.connect_slot) {
        api_->slot_unref(session.connect_slot);
        session.connect_slot = nullptr;
      }
    }
    if (properties_slot_) api_->slot_unref(properties_slot_);
    if (interfaces_added_slot_) api_->slot_unref(interfaces_added_slot_);
    if (interfaces_removed_slot_) api_->slot_unref(interfaces_removed_slot_);
    if (bus_) api_->bus_unref(bus_);
  }

  std::string platform_name() const override { return "linux"; }

  void pump(int timeout_ms, bool include_stdin, bool* stdin_ready) override {
    if (stdin_ready) *stdin_ready = false;
    process_bus_messages();
    uint64_t now_ns = static_cast<uint64_t>(monotonic_now_ns());
    advance_sessions(now_ns);
    process_bus_messages();

    const int bus_fd = api_->get_fd(bus_);
    const int bus_events = api_->get_events(bus_);
    std::array<pollfd, 2> fds{};
    nfds_t count = 0;
    if (bus_fd >= 0) {
      fds[count].fd = bus_fd;
      fds[count].events = static_cast<short>(bus_events);
      ++count;
    }
    if (include_stdin) {
      fds[count].fd = STDIN_FILENO;
      fds[count].events = POLLIN;
      ++count;
    }

    const int poll_timeout = compute_poll_timeout_ms(timeout_ms, now_ns);
    const int rc = poll(fds.data(), count, poll_timeout);
    if (rc < 0 && errno != EINTR) {
      throw std::runtime_error(std::string("Gear VR BlueZ poll failed: ") + std::strerror(errno));
    }
    if (include_stdin && count > 0) {
      const size_t index = bus_fd >= 0 ? 1 : 0;
      if (stdin_ready) *stdin_ready = index < count && (fds[index].revents & POLLIN) != 0;
    }
    process_bus_messages();
    now_ns = static_cast<uint64_t>(monotonic_now_ns());
    advance_sessions(now_ns);
    process_bus_messages();
  }

  std::vector<BleDeviceSnapshot> devices() const override {
    std::vector<BleDeviceSnapshot> out;
    out.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
      BleDeviceSnapshot snapshot;
      snapshot.stable_id = session->stable_id;
      snapshot.address = session->address;
      snapshot.name = session->name;
      snapshot.platform = "linux";
      snapshot.paired = session->paired;
      snapshot.trusted = session->trusted;
      snapshot.connected = session->connected;
      snapshot.notifications_active = session->connected && session->notifications_started;
      snapshot.error = session->error;
      out.push_back(std::move(snapshot));
    }
    return out;
  }

  bool pop_packet(BlePacket& packet) override {
    if (pending_packets_.empty()) return false;
    packet = std::move(pending_packets_.front());
    pending_packets_.pop_front();
    return true;
  }

 private:
  struct Session {
    LinuxGearVrBleTransport* owner = nullptr;
    std::string stable_id;
    std::string device_path;
    std::string address;
    std::string name = "Gear VR Controller";
    std::string notify_path;
    std::string command_path;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    bool services_resolved = false;
    bool notifications_started = false;
    bool connect_pending = false;
    bool seen_in_refresh = false;
    std::string error;
    uint64_t next_connect_ns = 0;
    uint64_t next_keepalive_ns = 0;
    uint64_t next_notify_attempt_ns = 0;
    uint64_t next_init_command_ns = 0;
    size_t init_command_index = 0;
    sd_bus_slot* connect_slot = nullptr;
  };

  struct ManagedDevice {
    std::string path;
    std::string address;
    std::string name;
    std::set<std::string> uuids;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    bool services_resolved = false;
  };

  struct ManagedService {
    std::string path;
    std::string uuid;
    std::string device_path;
  };

  struct ManagedCharacteristic {
    std::string path;
    std::string uuid;
    std::string service_path;
  };

  void add_matches() {
    const char* properties_match =
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path_namespace='/org/bluez'";
    const char* added_match =
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesAdded'";
    const char* removed_match =
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesRemoved'";
    if (api_->add_match(bus_, &properties_slot_, properties_match,
                        &LinuxGearVrBleTransport::on_properties_changed, this) < 0 ||
        api_->add_match(bus_, &interfaces_added_slot_, added_match,
                        &LinuxGearVrBleTransport::on_interfaces_changed, this) < 0 ||
        api_->add_match(bus_, &interfaces_removed_slot_, removed_match,
                        &LinuxGearVrBleTransport::on_interfaces_changed, this) < 0) {
      throw std::runtime_error("cannot subscribe to BlueZ D-Bus signals");
    }
  }

  static int on_interfaces_changed(sd_bus_message*, void* userdata, sd_bus_error*) {
    auto* self = static_cast<LinuxGearVrBleTransport*>(userdata);
    self->refresh_requested_ = true;
    return 0;
  }

  static int on_properties_changed(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto* self = static_cast<LinuxGearVrBleTransport*>(userdata);
    self->handle_properties_changed(message);
    return 0;
  }

  static int on_connect_reply(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto* session = static_cast<Session*>(userdata);
    LinuxGearVrBleTransport& self = *session->owner;
    const int message_errno = self.api_->message_get_errno(message);
    if (session->connect_slot) {
      self.api_->slot_unref(session->connect_slot);
      session->connect_slot = nullptr;
    }
    session->connect_pending = false;
    if (message_errno != 0 && message_errno != EISCONN &&
        message_errno != EALREADY && message_errno != EINPROGRESS) {
      session->error = "BlueZ Connect failed: errno=" + std::to_string(message_errno);
      session->next_connect_ns = static_cast<uint64_t>(monotonic_now_ns()) +
                                 static_cast<uint64_t>(self.options_.gearvr_reconnect_ms) * 1'000'000ull;
      std::cerr << "[override_controller][gearvr_ble][WARN] " << session->address
                << ": " << session->error << "\n";
    } else {
      session->error.clear();
      self.refresh_requested_ = true;
    }
    return 0;
  }

  void handle_properties_changed(sd_bus_message* message) {
    const char* interface = nullptr;
    if (api_->message_read(message, "s", &interface) <= 0 || !interface) return;
    const std::string path = api_->message_get_path(message) ? api_->message_get_path(message) : "";

    if (api_->message_enter_container(message, 'a', "{sv}") <= 0) return;
    while (api_->message_enter_container(message, 'e', "sv") > 0) {
      const char* property = nullptr;
      if (api_->message_read(message, "s", &property) <= 0 || !property) {
        api_->message_exit_container(message);
        continue;
      }
      char type = 0;
      const char* contents = nullptr;
      if (api_->message_peek_type(message, &type, &contents) <= 0 || type != 'v' || !contents ||
          api_->message_enter_container(message, 'v', contents) <= 0) {
        api_->message_exit_container(message);
        continue;
      }

      if (std::strcmp(interface, kGattCharacteristicInterface) == 0 &&
          std::strcmp(property, "Value") == 0 && contents[0] == 'a') {
        const void* bytes = nullptr;
        size_t size = 0;
        if (api_->message_read_array(message, 'y', &bytes, &size) > 0 && bytes) {
          if (Session* session = session_for_notify_path(path)) {
            BlePacket packet;
            packet.stable_id = session->stable_id;
            packet.timestamp_ns = static_cast<uint64_t>(monotonic_now_ns());
            const auto* begin = static_cast<const uint8_t*>(bytes);
            packet.bytes.assign(begin, begin + size);
            pending_packets_.push_back(std::move(packet));
          }
        }
      } else if (std::strcmp(interface, kDeviceInterface) == 0) {
        if (Session* session = session_for_device_path(path)) {
          if (std::strcmp(property, "Connected") == 0 && contents[0] == 'b') {
            int value = 0;
            if (api_->message_read(message, "b", &value) > 0) update_connected(*session, value != 0);
          } else if (std::strcmp(property, "ServicesResolved") == 0 && contents[0] == 'b') {
            int value = 0;
            if (api_->message_read(message, "b", &value) > 0) {
              session->services_resolved = value != 0;
              refresh_requested_ = true;
            }
          } else if ((std::strcmp(property, "Alias") == 0 ||
                      std::strcmp(property, "Name") == 0) && contents[0] == 's') {
            const char* value = nullptr;
            if (api_->message_read(message, "s", &value) > 0 && value) session->name = value;
          } else {
            (void)api_->message_skip(message, contents);
          }
        } else {
          (void)api_->message_skip(message, contents);
        }
      } else {
        (void)api_->message_skip(message, contents);
      }
      api_->message_exit_container(message);
      api_->message_exit_container(message);
    }
    api_->message_exit_container(message);
    (void)api_->message_skip(message, "as");
  }

  Session* session_for_device_path(const std::string& path) {
    for (auto& [id, session] : sessions_) {
      if (session->device_path == path) return session.get();
    }
    return nullptr;
  }

  Session* session_for_notify_path(const std::string& path) {
    for (auto& [id, session] : sessions_) {
      if (session->notify_path == path) return session.get();
    }
    return nullptr;
  }

  void update_connected(Session& session, bool connected) {
    if (session.connected == connected) return;
    session.connected = connected;
    if (connected) {
      session.error.clear();
      session.next_connect_ns = 0;
      session.next_keepalive_ns = static_cast<uint64_t>(monotonic_now_ns()) + kKeepAliveNs;
      session.next_notify_attempt_ns = static_cast<uint64_t>(monotonic_now_ns());
      refresh_requested_ = true;
      std::cerr << "[override_controller][gearvr_ble] connected " << session.name
                << " [" << session.address << "]\n";
    } else {
      session.notifications_started = false;
      session.services_resolved = false;
      session.notify_path.clear();
      session.command_path.clear();
      session.init_command_index = 0;
      session.next_notify_attempt_ns = 0;
      session.next_connect_ns = static_cast<uint64_t>(monotonic_now_ns()) +
                                static_cast<uint64_t>(options_.gearvr_reconnect_ms) * 1'000'000ull;
      std::cerr << "[override_controller][gearvr_ble] disconnected " << session.address
                << "; reconnect scheduled\n";
    }
  }

  template <typename Callback>
  void parse_property_dict(sd_bus_message* message, Callback&& callback) {
    if (api_->message_enter_container(message, 'a', "{sv}") <= 0) return;
    while (api_->message_enter_container(message, 'e', "sv") > 0) {
      const char* name = nullptr;
      if (api_->message_read(message, "s", &name) <= 0 || !name) {
        api_->message_exit_container(message);
        continue;
      }
      char variant_type = 0;
      const char* variant_contents = nullptr;
      if (api_->message_peek_type(message, &variant_type, &variant_contents) > 0 &&
          variant_type == 'v' && variant_contents &&
          api_->message_enter_container(message, 'v', variant_contents) > 0) {
        callback(std::string(name), std::string(variant_contents), message);
        api_->message_exit_container(message);
      }
      api_->message_exit_container(message);
    }
    api_->message_exit_container(message);
  }

  bool read_bool(sd_bus_message* message, bool& out) {
    int value = 0;
    if (api_->message_read(message, "b", &value) <= 0) return false;
    out = value != 0;
    return true;
  }

  bool read_string(sd_bus_message* message, std::string& out, char type = 's') {
    const char* value = nullptr;
    char signature[2] = {type, '\0'};
    if (api_->message_read(message, signature, &value) <= 0 || !value) return false;
    out = value;
    return true;
  }

  void read_string_array(sd_bus_message* message, std::set<std::string>& out) {
    if (api_->message_enter_container(message, 'a', "s") <= 0) return;
    const char* value = nullptr;
    while (api_->message_read(message, "s", &value) > 0) {
      if (value) out.insert(lower_copy(value));
    }
    api_->message_exit_container(message);
  }

  bool refresh_managed_objects() {
    MessageGuard reply{api_.get(), nullptr};
    ErrorGuard error{api_.get()};
    const int rc = api_->call_method(bus_, kBluezService, "/", kObjectManagerInterface,
                                     "GetManagedObjects", &error.value, &reply.value, nullptr);
    if (rc < 0 || !reply.value) {
      std::cerr << "[override_controller][gearvr_ble][WARN] BlueZ GetManagedObjects failed: "
                << (error.value.message ? error.value.message : std::to_string(rc)) << "\n";
      return false;
    }

    std::map<std::string, ManagedDevice> devices;
    std::map<std::string, ManagedService> services;
    std::map<std::string, ManagedCharacteristic> characteristics;

    if (api_->message_enter_container(reply.value, 'a', "{oa{sa{sv}}}") <= 0) return false;
    while (api_->message_enter_container(reply.value, 'e', "oa{sa{sv}}") > 0) {
      const char* object_path_c = nullptr;
      if (api_->message_read(reply.value, "o", &object_path_c) <= 0 || !object_path_c) {
        api_->message_exit_container(reply.value);
        continue;
      }
      const std::string object_path = object_path_c;
      if (api_->message_enter_container(reply.value, 'a', "{sa{sv}}") <= 0) {
        api_->message_exit_container(reply.value);
        continue;
      }
      while (api_->message_enter_container(reply.value, 'e', "sa{sv}") > 0) {
        const char* interface_c = nullptr;
        if (api_->message_read(reply.value, "s", &interface_c) <= 0 || !interface_c) {
          api_->message_exit_container(reply.value);
          continue;
        }
        const std::string interface = interface_c;
        if (interface == kDeviceInterface) {
          ManagedDevice& device = devices[object_path];
          device.path = object_path;
          parse_property_dict(reply.value, [&](const std::string& name,
                                               const std::string& type,
                                               sd_bus_message* value) {
            if (name == "Address" && type == "s") read_string(value, device.address);
            else if ((name == "Alias" || name == "Name") && type == "s") {
              std::string parsed;
              if (read_string(value, parsed) && (device.name.empty() || name == "Alias")) {
                device.name = parsed;
              }
            } else if (name == "Paired" && type == "b") read_bool(value, device.paired);
            else if (name == "Trusted" && type == "b") read_bool(value, device.trusted);
            else if (name == "Connected" && type == "b") read_bool(value, device.connected);
            else if (name == "ServicesResolved" && type == "b") {
              read_bool(value, device.services_resolved);
            } else if (name == "UUIDs" && type == "as") {
              read_string_array(value, device.uuids);
            } else {
              (void)api_->message_skip(value, type.c_str());
            }
          });
        } else if (interface == kGattServiceInterface) {
          ManagedService& service = services[object_path];
          service.path = object_path;
          parse_property_dict(reply.value, [&](const std::string& name,
                                               const std::string& type,
                                               sd_bus_message* value) {
            if (name == "UUID" && type == "s") read_string(value, service.uuid);
            else if (name == "Device" && type == "o") read_string(value, service.device_path, 'o');
            else (void)api_->message_skip(value, type.c_str());
          });
          service.uuid = lower_copy(service.uuid);
        } else if (interface == kGattCharacteristicInterface) {
          ManagedCharacteristic& characteristic = characteristics[object_path];
          characteristic.path = object_path;
          parse_property_dict(reply.value, [&](const std::string& name,
                                               const std::string& type,
                                               sd_bus_message* value) {
            if (name == "UUID" && type == "s") read_string(value, characteristic.uuid);
            else if (name == "Service" && type == "o") {
              read_string(value, characteristic.service_path, 'o');
            } else {
              (void)api_->message_skip(value, type.c_str());
            }
          });
          characteristic.uuid = lower_copy(characteristic.uuid);
        } else {
          (void)api_->message_skip(reply.value, "a{sv}");
        }
        api_->message_exit_container(reply.value);
      }
      api_->message_exit_container(reply.value);
      api_->message_exit_container(reply.value);
    }
    api_->message_exit_container(reply.value);

    for (auto& [id, session] : sessions_) session->seen_in_refresh = false;

    for (const auto& [path, managed] : devices) {
      if (!managed.paired || managed.address.empty()) continue;
      const bool uuid_match = managed.uuids.count(std::string(kServiceUuid)) != 0;
      if (!is_gearvr_name(managed.name) && !uuid_match) continue;

      const std::string address = uppercase_copy(managed.address);
      const std::string stable_id = "gearvr_ble:" + address;
      auto& session_ptr = sessions_[stable_id];
      if (!session_ptr) {
        session_ptr = std::make_unique<Session>();
        session_ptr->owner = this;
        session_ptr->stable_id = stable_id;
        session_ptr->address = address;
        session_ptr->next_connect_ns = static_cast<uint64_t>(monotonic_now_ns());
        std::cerr << "[override_controller][gearvr_ble] discovered paired controller "
                  << managed.name << " [" << address << "]\n";
      }
      Session& session = *session_ptr;
      session.seen_in_refresh = true;
      session.device_path = managed.path;
      session.address = address;
      session.name = managed.name.empty() ? "Gear VR Controller" : managed.name;
      session.paired = managed.paired;
      session.trusted = managed.trusted;
      session.services_resolved = managed.services_resolved;
      update_connected(session, managed.connected);

      session.notify_path.clear();
      session.command_path.clear();
      for (const auto& [service_path, service] : services) {
        if (service.device_path != managed.path || service.uuid != kServiceUuid) continue;
        for (const auto& [characteristic_path, characteristic] : characteristics) {
          if (characteristic.service_path != service_path) continue;
          if (characteristic.uuid == kNotifyUuid) session.notify_path = characteristic_path;
          else if (characteristic.uuid == kCommandUuid) session.command_path = characteristic_path;
        }
      }
    }

    for (auto& [id, session] : sessions_) {
      if (!session->seen_in_refresh && session->connected) update_connected(*session, false);
    }

    next_refresh_ns_ = static_cast<uint64_t>(monotonic_now_ns()) + kManagedObjectsRefreshNs;
    refresh_requested_ = false;
    return true;
  }

  void request_connect(Session& session, uint64_t now_ns) {
    if (session.connect_pending || session.connected || !session.paired ||
        session.device_path.empty() || now_ns < session.next_connect_ns) {
      return;
    }
    session.connect_pending = true;
    const int rc = api_->call_method_async(bus_, &session.connect_slot, kBluezService,
                                           session.device_path.c_str(), kDeviceInterface,
                                           "Connect", &LinuxGearVrBleTransport::on_connect_reply,
                                           &session, 10'000'000ull, nullptr);
    if (rc < 0) {
      session.connect_pending = false;
      session.connect_slot = nullptr;
      session.error = "cannot queue BlueZ Connect: " + std::to_string(rc);
      session.next_connect_ns = now_ns +
          static_cast<uint64_t>(options_.gearvr_reconnect_ms) * 1'000'000ull;
    } else {
      std::cerr << "[override_controller][gearvr_ble] connecting " << session.address
                << (session.trusted ? "" : " (paired but not trusted)") << "\n";
    }
  }

  bool start_notifications(Session& session) {
    if (session.notify_path.empty()) return false;
    MessageGuard reply{api_.get(), nullptr};
    ErrorGuard error{api_.get()};
    const int rc = api_->call_method(bus_, kBluezService, session.notify_path.c_str(),
                                     kGattCharacteristicInterface, "StartNotify",
                                     &error.value, &reply.value, nullptr);
    if (rc < 0) {
      session.error = std::string("StartNotify failed: ") +
                      (error.value.message ? error.value.message : std::to_string(rc));
      return false;
    }
    session.notifications_started = true;
    session.init_command_index = 0;
    session.next_init_command_ns = static_cast<uint64_t>(monotonic_now_ns());
    return true;
  }

  bool write_command(Session& session, const Command& command, bool log_error = true) {
    if (session.command_path.empty()) return false;
    MessageGuard call{api_.get(), nullptr};
    MessageGuard reply{api_.get(), nullptr};
    ErrorGuard error{api_.get()};
    int rc = api_->message_new_method_call(bus_, &call.value, kBluezService,
                                           session.command_path.c_str(),
                                           kGattCharacteristicInterface, "WriteValue");
    if (rc < 0) return false;
    rc = api_->message_append_array(call.value, 'y', command.data(), command.size());
    if (rc >= 0) rc = api_->message_open_container(call.value, 'a', "{sv}");
    if (rc >= 0) rc = api_->message_open_container(call.value, 'e', "sv");
    if (rc >= 0) rc = api_->message_append(call.value, "s", "type");
    if (rc >= 0) rc = api_->message_open_container(call.value, 'v', "s");
    if (rc >= 0) rc = api_->message_append(call.value, "s", "command");
    if (rc >= 0) rc = api_->message_close_container(call.value);
    if (rc >= 0) rc = api_->message_close_container(call.value);
    if (rc >= 0) rc = api_->message_close_container(call.value);
    if (rc >= 0) {
      rc = api_->bus_call(bus_, call.value, kDbusCallTimeoutUsec, &error.value, &reply.value);
    }
    if (rc < 0 && log_error) {
      session.error = std::string("WriteValue failed: ") +
                      (error.value.message ? error.value.message : std::to_string(rc));
      std::cerr << "[override_controller][gearvr_ble][WARN] " << session.address
                << ": " << session.error << "\n";
    }
    return rc >= 0;
  }

  void advance_sessions(uint64_t now_ns) {
    if (refresh_requested_ || now_ns >= next_refresh_ns_) (void)refresh_managed_objects();
    for (auto& [id, session_ptr] : sessions_) {
      Session& session = *session_ptr;
      if (!session.connected) {
        request_connect(session, now_ns);
        continue;
      }
      if (session.services_resolved && !session.notify_path.empty() &&
          !session.command_path.empty()) {
        if (!session.notifications_started) {
          if (now_ns < session.next_notify_attempt_ns) continue;
          if (!start_notifications(session)) {
            session.next_notify_attempt_ns = now_ns +
                static_cast<uint64_t>(options_.gearvr_reconnect_ms) * 1'000'000ull;
            continue;
          }
        }
        const auto& commands = initialization_commands();
        if (session.init_command_index < commands.size() &&
            now_ns >= session.next_init_command_ns) {
          if (write_command(session, commands[session.init_command_index])) {
            ++session.init_command_index;
            session.next_init_command_ns = now_ns + kInitCommandGapNs;
            if (session.init_command_index == commands.size()) {
              std::cerr << "[override_controller][gearvr_ble] sensor mode initialized "
                        << session.address << "\n";
            }
          } else {
            session.next_init_command_ns = now_ns +
                static_cast<uint64_t>(options_.gearvr_reconnect_ms) * 1'000'000ull;
          }
        }
        if (session.init_command_index == commands.size() &&
            now_ns >= session.next_keepalive_ns) {
          (void)write_command(session, kCommandKeepAlive);
          session.next_keepalive_ns = now_ns + kKeepAliveNs;
        }
      }
    }
  }

  int compute_poll_timeout_ms(int requested_timeout_ms, uint64_t now_ns) {
    int timeout = requested_timeout_ms < 0 ? 50 : std::min(requested_timeout_ms, 50);
    auto shorten = [&](uint64_t deadline_ns) {
      if (deadline_ns == 0) return;
      if (deadline_ns <= now_ns) {
        timeout = 0;
        return;
      }
      const int ms = static_cast<int>((deadline_ns - now_ns + 999'999ull) / 1'000'000ull);
      timeout = std::min(timeout, ms);
    };
    shorten(next_refresh_ns_);
    for (const auto& [id, session] : sessions_) {
      shorten(session->next_connect_ns);
      if (session->connected) {
        shorten(session->next_notify_attempt_ns);
        shorten(session->next_init_command_ns);
        shorten(session->next_keepalive_ns);
      }
    }
    uint64_t bus_timeout_us = std::numeric_limits<uint64_t>::max();
    if (api_->get_timeout(bus_, &bus_timeout_us) >= 0 &&
        bus_timeout_us != std::numeric_limits<uint64_t>::max()) {
      const uint64_t now_us = monotonic_now_us();
      if (bus_timeout_us <= now_us) timeout = 0;
      else timeout = std::min(timeout,
                              static_cast<int>((bus_timeout_us - now_us + 999) / 1000));
    }
    return std::max(0, timeout);
  }

  void process_bus_messages() {
    while (true) {
      const int rc = api_->process(bus_, nullptr);
      if (rc <= 0) break;
    }
  }

  InputProviderOptions options_;
  std::unique_ptr<SdBusApi> api_;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* properties_slot_ = nullptr;
  sd_bus_slot* interfaces_added_slot_ = nullptr;
  sd_bus_slot* interfaces_removed_slot_ = nullptr;
  std::map<std::string, std::unique_ptr<Session>> sessions_;
  std::deque<BlePacket> pending_packets_;
  bool refresh_requested_ = false;
  uint64_t next_refresh_ns_ = 0;
};

}  // namespace

std::unique_ptr<BleTransport> make_platform_ble_transport(const InputProviderOptions& options) {
  return std::make_unique<LinuxGearVrBleTransport>(options);
}

}  // namespace xr_override_controller::gearvr
