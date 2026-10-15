#include "linux_evdev_input_provider.hpp"

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

using namespace xr_override_controller;
namespace fs = std::filesystem;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

// Regular files let the test observe shared file-description lifetime and
// event cursors without requiring /dev/input permissions or physical devices.
// Track explicit grabs so closing a scan alias cannot ungrab the live client.
bool grabbed = false;
unsigned ungrab_count = 0;
extern "C" int __wrap_ioctl(int, unsigned long request, ...) {
  if (request == EVIOCGRAB) {
    va_list args;
    va_start(args, request);
    const int enable = va_arg(args, int);
    va_end(args);
    grabbed = enable != 0;
    if (!enable) ++ungrab_count;
    return 0;
  }
  errno = ENOTTY;
  return -1;
}

struct Fixture {
  fs::path directory;
  fs::path node;
  Fixture() {
    char pattern[] = "/tmp/xr-evdev-rescan-XXXXXX";
    const char* created = mkdtemp(pattern);
    require(created != nullptr, "cannot create test input directory");
    directory = created;
    node = directory / "event999999";
    std::ofstream(node, std::ios::binary) << std::string(128, 'x');
  }
  ~Fixture() { fs::remove_all(directory); }
};

void stable_scans_share_the_live_client() {
  Fixture f;
  LinuxEvdevInputProvider provider(f.directory.string());
  auto active = provider.scan_devices(true);
  require(active.size() == 1 && active[0].readable, "initial device missing");
  const int live_fd = active[0].fd;
  require(lseek(live_fd, 13, SEEK_SET) == 13, "cannot set test event cursor");
  require(provider.set_device_grab(active, {0}, true, nullptr), "test grab failed");
  const unsigned before = ungrab_count;
  auto inventory = provider.scan_devices(false);
  require(inventory.size() == 1 && inventory[0].readable && inventory[0].fd == -1,
          "inventory needs no descriptor of its own");
  provider.close_devices(inventory);
  require(lseek(live_fd, 0, SEEK_CUR) == 13 && grabbed && ungrab_count == before,
          "inventory disturbed the live client");

  for (unsigned i = 0; i < 20; ++i) {
    auto candidate = provider.scan_devices(true);
    require(candidate.size() == 1 && candidate[0].fd != live_fd,
            "candidate needs its own descriptor lifetime");
    require(lseek(candidate[0].fd, 0, SEEK_CUR) == 13,
            "periodic scan reopened the evdev client instead of duplicating it");
    require((fcntl(candidate[0].fd, F_GETFD) & FD_CLOEXEC) != 0,
            "scan descriptor can leak into child processes");
    provider.close_devices(candidate);
    require(fcntl(live_fd, F_GETFD) >= 0 && grabbed && ungrab_count == before,
            "candidate cleanup closed or ungrabbed the live device");
  }

  // A true mapping change closes the old view and adopts the candidate.
  auto adopted = provider.scan_devices(true);
  const int adopted_fd = adopted[0].fd;
  provider.set_device_grab(active, {0}, false, nullptr);
  provider.close_devices(active);
  require(fcntl(live_fd, F_GETFD) == -1 && errno == EBADF, "old view leaked");
  auto next = provider.scan_devices(true);
  require(lseek(next[0].fd, 0, SEEK_CUR) == 13,
          "reattach lost the live client cache");
  provider.close_devices(next);
  provider.close_devices(adopted);
  require(fcntl(adopted_fd, F_GETFD) == -1 && errno == EBADF, "adopted view leaked");
  auto reopened = provider.scan_devices(true);
  require(lseek(reopened[0].fd, 0, SEEK_CUR) == 0,
          "last close left a hidden persistent client");
  provider.close_devices(reopened);
}

void replaced_nodes_get_a_new_client() {
  Fixture f;
  LinuxEvdevInputProvider provider(f.directory.string());
  auto old = provider.scan_devices(true);
  require(lseek(old[0].fd, 23, SEEK_SET) == 23, "cannot set old event cursor");
  fs::rename(f.node, f.directory / "old-node");
  std::ofstream(f.node, std::ios::binary) << std::string(128, 'n');
  auto replacement = provider.scan_devices(true);
  require(lseek(replacement[0].fd, 0, SEEK_CUR) == 0,
          "reused a disconnected client for a replaced device node");
  provider.close_devices(old);
  require(fcntl(replacement[0].fd, F_GETFD) >= 0,
          "old device cleanup closed its replacement");
  provider.close_devices(replacement);
  auto inventory = provider.scan_devices(false);
  require(inventory.size() == 1 && inventory[0].readable && inventory[0].fd == -1,
          "read-only inventory retained an open descriptor");
}

void append_key(const fs::path& node, unsigned code) {
  input_event event{};
  event.type = EV_KEY;
  event.code = code;
  event.value = 1;
  std::ofstream out(node, std::ios::binary | std::ios::app);
  out.write(reinterpret_cast<const char*>(&event), sizeof(event));
}

void zero_timeout_reads_ready_fd() {
  Fixture f;
  std::ofstream(f.node, std::ios::binary | std::ios::trunc).close();
  append_key(f.node, KEY_A);
  LinuxEvdevInputProvider provider(f.directory.string());
  auto devices = provider.scan_devices(true);
  const auto event = provider.wait_event(devices, 0, false);
  require(event && event->code == KEY_A, "zero-timeout poll skipped a ready fd");
  require(!provider.wait_event(devices, 0, false), "zero-timeout poll did not finish after draining input");
  provider.close_devices(devices);
}

void positive_timeout_waits_for_idle_fd() {
  int fds[2];
  require(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0, "cannot create idle input pipe");
  LinuxEvdevInputProvider provider;
  std::vector<DeviceInfo> devices(1);
  devices[0].fd = fds[0];
  const auto before = std::chrono::steady_clock::now();
  const auto event = provider.wait_event(devices, 1, false);
  const auto elapsed = std::chrono::steady_clock::now() - before;
  close(fds[0]);
  close(fds[1]);
  require(!event && elapsed >= std::chrono::microseconds(500),
          "positive evdev wait rounded down to an immediate select");
}

void candidate_cleanup_keeps_pending_input() {
  Fixture f;
  std::ofstream(f.node, std::ios::binary | std::ios::trunc).close();
  append_key(f.node, KEY_LEFTCTRL);
  append_key(f.node, KEY_A);
  LinuxEvdevInputProvider provider(f.directory.string());
  auto active = provider.scan_devices(true);
  const auto first = provider.wait_event(active, 2, false);
  require(first && first->code == KEY_LEFTCTRL, "modifier not read");
  auto candidate = provider.scan_devices(true);
  provider.close_devices(candidate);
  const auto pending = provider.wait_event(active, 0, false);
  require(pending && pending->code == KEY_A, "candidate cleanup discarded pending input");
  append_key(f.node, KEY_C);
  const auto stop = provider.wait_event(active, 2, false);
  require(stop && stop->stop_requested, "candidate cleanup forgot the held Ctrl modifier");
  provider.close_devices(active);
}

void real_reattach_flushes_old_input() {
  Fixture f;
  std::ofstream(f.node, std::ios::binary | std::ios::trunc).close();
  append_key(f.node, KEY_LEFTCTRL);
  append_key(f.node, KEY_A);
  LinuxEvdevInputProvider provider(f.directory.string());
  auto active = provider.scan_devices(true);
  require(provider.wait_event(active, 2, false).has_value(), "old input not read");
  auto candidate = provider.scan_devices(true);
  provider.close_devices(active);
  provider.flush_events(candidate);
  require(!provider.wait_event(candidate, 0, false), "old view left queued input after reattach");
  append_key(f.node, KEY_C);
  const auto event = provider.wait_event(candidate, 2, false);
  require(event && event->code == KEY_C && !event->stop_requested,
          "old view left held modifiers after reattach");
  provider.close_devices(candidate);
}

void provider_destruction_closes_unreleased_views() {
  Fixture f;
  int fd = -1;
  {
    LinuxEvdevInputProvider provider(f.directory.string());
    const auto view = provider.scan_devices(true);
    fd = view[0].fd;
    require(fcntl(fd, F_GETFD) >= 0, "listing did not open test device");
  }
  require(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "provider destruction leaked a device");
}

int main() {
  positive_timeout_waits_for_idle_fd();
  zero_timeout_reads_ready_fd();
  stable_scans_share_the_live_client();
  replaced_nodes_get_a_new_client();
  candidate_cleanup_keeps_pending_input();
  real_reattach_flushes_old_input();
  provider_destruction_closes_unreleased_views();
  std::cout << "evdev rescan aliases, grab lifetime, reattach, node replacement and pending input: OK\n";
}
