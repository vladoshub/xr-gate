// Run the real BlueZ session lifecycle against a small in-memory ObjectManager.
// No system bus or Bluetooth hardware is used.
#include "../src/providers/gearvr/transport/linux_gearvr_ble_transport.cpp"

#include <cstdarg>
#include <iterator>
#include <sstream>

using namespace xr_override_controller::gearvr;

namespace {
const std::string left_path = "/org/bluez/hci2/dev_AA_BB_CC_DD_EE_01";
const std::string right_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02";
std::string cleanup_path;
std::vector<std::string> operations;
std::vector<uint64_t> timeouts;
bool fail_left_disconnect = false;
bool refresh_pending = false;
sd_bus_message_handler_t refresh_callback = nullptr;
void* refresh_userdata = nullptr;

void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}

struct Node {
  char type;
  std::string text;
  std::vector<Node> children;
};
Node string_node(const char* value) { return {'s', value, {}}; }
Node property(const char* key, char type, const char* value) {
  return {'e', "sv", {string_node(key), {'v', std::string(1, type), {{type, value, {}}}}}};
}
Node device(const std::string& path, const char* name, const char* address,
            bool connected, bool paired = true) {
  return {'e', "oa{sa{sv}}", {{'o', path, {}}, {'a', "{sa{sv}}", {
    {'e', "sa{sv}", {string_node("org.bluez.Device1"), {'a', "{sv}", {
      property("Address", 's', address), property("Name", 's', name),
      property("Adapter", 'o', path.substr(0, path.find_last_of('/')).c_str()),
      property("Paired", 'b', paired ? "1" : "0"),
      property("Connected", 'b', connected ? "1" : "0"),
      property("ServicesResolved", 'b', "0")
    }}}}
  }}}};
}
Node adapter(const std::string& path, const char* address, bool powered) {
  return {'e', "oa{sa{sv}}", {{'o', path, {}}, {'a', "{sa{sv}}", {
    {'e', "sa{sv}", {string_node("org.bluez.Adapter1"), {'a', "{sv}", {
      property("Address", 's', address), property("Alias", 's', "Test adapter"),
      property("Powered", 'b', powered ? "1" : "0")
    }}}}
  }}}};
}
Node root{'r', "", {{'a', "{oa{sa{sv}}}", {
  adapter("/org/bluez/hci0", "00:00:00:00:00:01", true),
  adapter("/org/bluez/hci2", "00:00:00:00:00:02", true),
  device(left_path, "Gear VR Controller(Left)", "AA:BB:CC:DD:EE:01", true),
  device(right_path, "Gear VR Controller(Right)", "AA:BB:CC:DD:EE:02", false),
  device("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03", "Keyboard", "AA:BB:CC:DD:EE:03", true),
  device("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_04", "Gear VR Controller", "AA:BB:CC:DD:EE:04", true, false)
}}}};

struct Frame { const Node* node; size_t next; };
std::vector<Frame> frames;
Node* next_node() {
  if (frames.empty() || frames.back().next >= frames.back().node->children.size()) return nullptr;
  return const_cast<Node*>(&frames.back().node->children[frames.back().next]);
}
std::string records() {
  std::ifstream input(cleanup_path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int fake_open(sd_bus** bus) {
  static char token;
  *bus = reinterpret_cast<sd_bus*>(&token);
  return 0;
}
sd_bus* fake_unref(sd_bus*) { return nullptr; }
sd_bus_message* fake_message_unref(sd_bus_message*) { return nullptr; }
sd_bus_slot* fake_slot_unref(sd_bus_slot*) {
  operations.push_back("cancel-connect");
  return nullptr;
}
int fake_match(sd_bus*, sd_bus_slot**, const char* match,
               sd_bus_message_handler_t callback, void* userdata) {
  if (std::strstr(match, "InterfacesAdded")) {
    refresh_callback = callback;
    refresh_userdata = userdata;
  }
  return 0;
}
int fake_set_timeout(sd_bus*, uint64_t value) { timeouts.push_back(value); return 0; }
int fake_process(sd_bus*, sd_bus_message**) {
  if (!refresh_pending) return 0;
  refresh_pending = false;
  refresh_callback(nullptr, refresh_userdata, nullptr);
  return 1;
}
int fake_fd(sd_bus*) { return -1; }
int fake_events(sd_bus*) { return 0; }
int fake_timeout(sd_bus*, uint64_t* timeout) {
  *timeout = std::numeric_limits<uint64_t>::max();
  return 0;
}
int fake_enter(sd_bus_message*, char type, const char* contents) {
  const Node* node = next_node();
  if (!node || node->type != type || node->text != contents) return 0;
  ++frames.back().next;
  frames.push_back({node, 0});
  return 1;
}
int fake_exit(sd_bus_message*) { frames.pop_back(); return 1; }
int fake_peek(sd_bus_message*, char* type, const char** contents) {
  const Node* node = next_node();
  if (!node) return 0;
  *type = node->type;
  *contents = node->text.c_str();
  return 1;
}
int fake_read(sd_bus_message*, const char* signature, ...) {
  const Node* node = next_node();
  if (!node || node->type != signature[0]) return 0;
  va_list args;
  va_start(args, signature);
  if (node->type == 'b') *va_arg(args, int*) = node->text == "1";
  else *va_arg(args, const char**) = node->text.c_str();
  va_end(args);
  ++frames.back().next;
  return 1;
}
int fake_skip(sd_bus_message*, const char*) { ++frames.back().next; return 1; }
int fake_call(sd_bus*, const char*, const char* path, const char*, const char* method,
              sd_bus_error* error, sd_bus_message** reply, const char*, ...) {
  if (std::strcmp(method, "GetManagedObjects") == 0) {
    frames = {{&root, 0}};
    *reply = reinterpret_cast<sd_bus_message*>(&root);
    return 0;
  }
  require(std::strcmp(method, "Disconnect") == 0, "unexpected BlueZ method");
  operations.push_back("Disconnect:" + std::string(path));
  if (fail_left_disconnect && path == left_path) {
    error->name = "org.bluez.Error.Failed";
    error->message = "simulated Bluetooth failure";
    return -EIO;
  }
  return 0;
}
int fake_connect(sd_bus*, sd_bus_slot** slot, const char*, const char* path,
                 const char*, const char* method, sd_bus_message_handler_t,
                 void*, const char*, ...) {
  require(std::strcmp(method, "Connect") == 0, "unexpected async method");
  if (std::getenv("OVERRIDE_CONTROLLER_BLE_CLEANUP_FILE")) {
    require(records().find("+" + std::string(path) + "\n") != std::string::npos,
            "connection was not registered before Connect");
  }
  static char token;
  *slot = reinterpret_cast<sd_bus_slot*>(&token);
  operations.push_back("Connect:" + std::string(path));
  return 0;
}
void fake_error_free(sd_bus_error*) {}

void refresh(BleTransport& transport) {
  refresh_pending = true;
  transport.pump(0, false, nullptr);
}

Node duplicate_controller_objects(bool first_powered, bool second_powered,
                                  bool first_connected, bool second_connected) {
  return {'r', "", {{'a', "{oa{sa{sv}}}", {
    adapter("/org/bluez/hci0", "00:00:00:00:00:01", first_powered),
    adapter("/org/bluez/hci2", "00:00:00:00:00:02", second_powered),
    device("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01", "Gear VR Controller",
           "AA:BB:CC:DD:EE:01", first_connected),
    device("/org/bluez/hci2/dev_AA_BB_CC_DD_EE_01", "Gear VR Controller",
           "AA:BB:CC:DD:EE:01", second_connected)
  }}}};
}

void duplicate_pairings_keep_one_adapter_route() {
  const Node saved_root = root;
  fail_left_disconnect = false;
  unsetenv("OVERRIDE_CONTROLLER_BLE_CLEANUP_FILE");
  unsetenv("XR_BLUETOOTH_ADAPTER");
  // Exercise both BlueZ object orders and both-powered/offline old pairings.
  for (bool first_connected : {false, true}) {
    for (bool other_powered : {false, true}) {
      root = duplicate_controller_objects(first_connected || other_powered,
                                          !first_connected || other_powered,
                                          first_connected, !first_connected);
      operations.clear();
      auto transport = make_platform_ble_transport(GearVrOptions{});
      transport->pump(0, false, nullptr);
      const auto snapshots = transport->devices();
      require(snapshots.size() == 1 && snapshots[0].connected,
              "duplicate pairing hid the connected controller");
      std::ostringstream progress;
      auto* previous = std::cerr.rdbuf(progress.rdbuf());
      for (unsigned i = 0; i < 6; ++i) refresh(*transport);
      std::cerr.rdbuf(previous);
      require(progress.str().find("] disconnected ") == std::string::npos,
              "refresh fabricated a disconnect from the other adapter's pairing");
      require(progress.str().find("] connected ") == std::string::npos,
              "refresh reinitialized an unchanged Bluetooth connection");
      require(operations.empty(), "refresh connected to the other adapter");
    }
  }
  root = saved_root;
}

void adapter_selection_and_power_changes() {
  const Node saved_root = root;
  unsetenv("OVERRIDE_CONTROLLER_BLE_CLEANUP_FILE");
  for (const char* selector : {"hci0", "/org/bluez/hci0", "00:00:00:00:00:01"}) {
    root = duplicate_controller_objects(true, true, false, true);
    operations.clear();
    setenv("XR_BLUETOOTH_ADAPTER", selector, 1);
    {
      auto transport = make_platform_ble_transport(GearVrOptions{});
      transport->pump(0, false, nullptr);
      require(transport->devices().size() == 1 && !transport->devices()[0].connected,
              "explicit adapter selector was ignored");
      require(operations == std::vector<std::string>{
                  "Connect:/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01"},
              "provider connected outside the selected adapter");
    }
    require(std::find(operations.begin(), operations.end(),
                "Disconnect:/org/bluez/hci2/dev_AA_BB_CC_DD_EE_01") == operations.end(),
            "shutdown disconnected another adapter's controller");
  }
  unsetenv("XR_BLUETOOTH_ADAPTER");
  root = duplicate_controller_objects(false, false, false, false);
  operations.clear();
  {
    auto transport = make_platform_ble_transport(GearVrOptions{});
    transport->pump(0, false, nullptr);
    require(transport->devices().empty() && operations.empty(),
            "provider attempted to connect through an unpowered adapter");
  }
  root = duplicate_controller_objects(true, true, false, true);
  operations.clear();
  {
    auto transport = make_platform_ble_transport(GearVrOptions{});
    transport->pump(0, false, nullptr);
    // Once selected, the route stays pinned for this provider lifetime.
    root = duplicate_controller_objects(true, false, false, false);
    refresh(*transport);
    require(transport->devices().size() == 1 && !transport->devices()[0].connected,
            "unpowered adapter left a connected snapshot");
    require(operations.empty(), "provider silently switched to another adapter");
    for (unsigned i = 0; i < 3; ++i) refresh(*transport);
    require(operations.empty(), "provider kept retrying an unavailable adapter");
    root = duplicate_controller_objects(true, true, false, true);
    refresh(*transport);
    require(transport->devices()[0].connected,
            "original adapter could not resume after powering on");
  }
  root = saved_root;
}
}

extern "C" void* __real_dlsym(void*, const char*);
extern "C" void* __wrap_dlsym(void* handle, const char* name) {
#define FAKE_SYMBOL(symbol, fn) if (std::strcmp(name, symbol) == 0) return reinterpret_cast<void*>(&fn)
  FAKE_SYMBOL("sd_bus_open_system", fake_open);
  FAKE_SYMBOL("sd_bus_unref", fake_unref);
  FAKE_SYMBOL("sd_bus_message_unref", fake_message_unref);
  FAKE_SYMBOL("sd_bus_slot_unref", fake_slot_unref);
  FAKE_SYMBOL("sd_bus_add_match", fake_match);
  FAKE_SYMBOL("sd_bus_set_method_call_timeout", fake_set_timeout);
  FAKE_SYMBOL("sd_bus_process", fake_process);
  FAKE_SYMBOL("sd_bus_get_fd", fake_fd);
  FAKE_SYMBOL("sd_bus_get_events", fake_events);
  FAKE_SYMBOL("sd_bus_get_timeout", fake_timeout);
  FAKE_SYMBOL("sd_bus_message_enter_container", fake_enter);
  FAKE_SYMBOL("sd_bus_message_exit_container", fake_exit);
  FAKE_SYMBOL("sd_bus_message_peek_type", fake_peek);
  FAKE_SYMBOL("sd_bus_message_read", fake_read);
  FAKE_SYMBOL("sd_bus_message_skip", fake_skip);
  FAKE_SYMBOL("sd_bus_call_method", fake_call);
  FAKE_SYMBOL("sd_bus_call_method_async", fake_connect);
  FAKE_SYMBOL("sd_bus_error_free", fake_error_free);
#undef FAKE_SYMBOL
  return __real_dlsym(handle, name);
}

int main() {
  unsetenv("XR_BLUETOOTH_ADAPTER");
  for (bool fail : {false, true}) {
    char path[] = "/tmp/gearvr-shutdown-test-XXXXXX";
    const int fd = mkstemp(path);
    require(fd >= 0, "cannot create cleanup file");
    close(fd);
    cleanup_path = path;
    setenv("OVERRIDE_CONTROLLER_BLE_CLEANUP_FILE", path, 1);
    operations.clear();
    timeouts.clear();
    fail_left_disconnect = fail;
    {
      auto transport = make_platform_ble_transport(GearVrOptions{});
      transport->pump(0, false, nullptr);
      require(records().find("+" + left_path + "\n") != std::string::npos,
              "already-connected controller was not registered");
      require(records().find("+" + right_path + "\n") != std::string::npos,
              "pending connection was not registered");
    }
    require(operations == std::vector<std::string>{
              "Connect:" + right_path, "Disconnect:" + left_path,
              "cancel-connect", "Disconnect:" + right_path},
            "shutdown did not cancel/disconnect exactly the managed Gear VR links");
    require(timeouts.back() == kShutdownCallTimeoutUsec, "shutdown timeout was not bounded");
    require((records().find("-" + left_path + "\n") != std::string::npos) == !fail,
            "failed Disconnect must remain registered for supervisor cleanup");
    require(records().find("-" + right_path + "\n") != std::string::npos,
            "pending BlueZ Connect was not canceled by Disconnect");
    unlink(path);
  }
  unsetenv("OVERRIDE_CONTROLLER_BLE_CLEANUP_FILE");
  duplicate_pairings_keep_one_adapter_route();
  adapter_selection_and_power_changes();
  std::cout << "BlueZ normal/pending/failed shutdown and crash cleanup registration: OK\n";
}
