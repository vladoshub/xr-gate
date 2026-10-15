// Exercise the native transport with sd-bus failures, without a system bus or
// Bluetooth hardware. Only the functions used by this scenario are replaced;
// the production event loop and retry scheduling run unchanged.
#include "../src/providers/gearvr/transport/linux_gearvr_ble_transport.cpp"

#include <cstdlib>

using namespace xr_override_controller::gearvr;

namespace {
unsigned failed_refresh_calls = 0;

int fake_open(sd_bus** bus) {
  static char token;
  *bus = reinterpret_cast<sd_bus*>(&token);
  return 0;
}
sd_bus* fake_unref(sd_bus*) { return nullptr; }
int fake_match(sd_bus*, sd_bus_slot**, const char*, sd_bus_message_handler_t, void*) { return 0; }
int fake_set_timeout(sd_bus*, uint64_t) { return 0; }
int fake_process(sd_bus*, sd_bus_message**) { return 0; }
int fake_fd(sd_bus*) { return -1; }
int fake_events(sd_bus*) { return 0; }
int fake_timeout(sd_bus*, uint64_t* timeout) {
  *timeout = std::numeric_limits<uint64_t>::max();
  return 0;
}
int fake_call(sd_bus*, const char*, const char*, const char*, const char* method,
              sd_bus_error*, sd_bus_message**, const char*, ...) {
  if (std::strcmp(method, "GetManagedObjects") != 0) std::abort();
  ++failed_refresh_calls;
  return -EIO;
}
void fake_error_free(sd_bus_error*) {}
}

extern "C" void* __real_dlsym(void*, const char*);
extern "C" void* __wrap_dlsym(void* handle, const char* name) {
#define FAKE_SYMBOL(symbol, fn) if (std::strcmp(name, symbol) == 0) return reinterpret_cast<void*>(&fn)
  FAKE_SYMBOL("sd_bus_open_system", fake_open);
  FAKE_SYMBOL("sd_bus_unref", fake_unref);
  FAKE_SYMBOL("sd_bus_add_match", fake_match);
  FAKE_SYMBOL("sd_bus_set_method_call_timeout", fake_set_timeout);
  FAKE_SYMBOL("sd_bus_process", fake_process);
  FAKE_SYMBOL("sd_bus_get_fd", fake_fd);
  FAKE_SYMBOL("sd_bus_get_events", fake_events);
  FAKE_SYMBOL("sd_bus_get_timeout", fake_timeout);
  FAKE_SYMBOL("sd_bus_call_method", fake_call);
  FAKE_SYMBOL("sd_bus_error_free", fake_error_free);
#undef FAKE_SYMBOL
  return __real_dlsym(handle, name);
}

int main() {
  auto transport = make_platform_ble_transport(GearVrOptions{});
  transport->pump(0, false, nullptr);
  transport->pump(0, false, nullptr);
  if (failed_refresh_calls != 1) {
    std::cerr << "BlueZ failure retried " << failed_refresh_calls
              << " times without waiting for reconnect backoff\n";
    return 1;
  }
  std::cout << "BlueZ failed refresh respects retry backoff: OK\n";
}
