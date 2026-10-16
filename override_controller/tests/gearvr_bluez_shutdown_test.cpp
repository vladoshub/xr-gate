// Run the real BlueZ session lifecycle against a small in-memory ObjectManager.
// No system bus or Bluetooth hardware is used.
#include "../src/providers/gearvr/transport/linux_gearvr_ble_transport.cpp"

#include <cstdarg>
#include <iterator>

using namespace xr_override_controller::gearvr;

namespace {
const std::string left_path = "/org/bluez/hci2/dev_AA_BB_CC_DD_EE_01";
const std::string right_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_02";
std::string cleanup_path;
std::vector<std::string> operations;
std::vector<uint64_t> timeouts;
bool fail_left_disconnect = false;

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
      property("Paired", 'b', paired ? "1" : "0"),
      property("Connected", 'b', connected ? "1" : "0"),
      property("ServicesResolved", 'b', "0")
    }}}}
  }}}};
}
Node root{'r', "", {{'a', "{oa{sa{sv}}}", {
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
int fake_match(sd_bus*, sd_bus_slot**, const char*, sd_bus_message_handler_t, void*) { return 0; }
int fake_set_timeout(sd_bus*, uint64_t value) { timeouts.push_back(value); return 0; }
int fake_process(sd_bus*, sd_bus_message**) { return 0; }
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
  require(records().find("+" + std::string(path) + "\n") != std::string::npos,
          "connection was not registered before Connect");
  static char token;
  *slot = reinterpret_cast<sd_bus_slot*>(&token);
  operations.push_back("Connect:" + std::string(path));
  return 0;
}
void fake_error_free(sd_bus_error*) {}
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
  std::cout << "BlueZ normal/pending/failed shutdown and crash cleanup registration: OK\n";
}
