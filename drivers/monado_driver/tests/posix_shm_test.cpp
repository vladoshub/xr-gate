// Exercise the production Linux readers with real SHM objects, without Monado.
#include "xr_monado_driver/runtime_readers.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace xr::monado_driver;

namespace {
void check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

template<class Frame, class Header, class Slot>
class Ring {
 public:
  Ring(const std::string& name, const char* magic, uint32_t slots = 2) {
    // The old reader mapping stays alive after unlink, exactly as on restart.
    shm_unlink(name.c_str());
    fd_ = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    check(fd_ >= 0, "create test SHM");
    size_ = 4096 + slots * (sizeof(Slot) + sizeof(Frame));
    if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
      close(fd_);
      throw std::runtime_error("size test SHM");
    }
    void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapping == MAP_FAILED) {
      close(fd_);
      throw std::runtime_error("map test SHM");
    }
    data_ = static_cast<uint8_t*>(mapping);
    std::memset(data_, 0, size_);
    initialize(magic, slots);
  }

  ~Ring() { munmap(data_, size_); close(fd_); }
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  Header& header() { return *reinterpret_cast<Header*>(data_); }

  void initialize(const char* magic, uint32_t slots = 2) {
    auto& h = header();
    h = Header{};
    std::memcpy(h.magic, magic, 8);
    h.version = 1;
    h.header_size = 4096;
    h.slot_count = slots;
    h.slot_stride = sizeof(Slot) + sizeof(Frame);
    h.slot_header_size = sizeof(Slot);
    h.payload_size = sizeof(Frame);
  }

  void write(Frame frame, uint64_t sequence = 1, int64_t age_ns = 0) {
    frame.sequence = sequence;
    frame.timestamp_ns = static_cast<uint64_t>(monotonic_now_ns() - age_ns);
    auto& h = header();
    const size_t off = h.header_size + ((sequence - 1) % h.slot_count) * h.slot_stride;
    auto* slot = reinterpret_cast<Slot*>(data_ + off);
    slot->seq_begin = slot->seq_end = sequence * 2 - 1;
    std::memcpy(data_ + off + sizeof(Slot), &frame, sizeof(frame));
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = sequence * 2;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = sequence * 2;
    std::atomic_thread_fence(std::memory_order_release);
    h.latest_sequence = sequence;
  }

  void grow() {
    check(ftruncate(fd_, static_cast<off_t>(size_ + sizeof(Slot) + sizeof(Frame))) == 0,
          "grow test SHM without unlink");
    ++header().slot_count;
  }
  void truncate() { check(ftruncate(fd_, 0) == 0, "truncate test SHM"); }

 private:
  int fd_ = -1;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

struct Pose {
  using Frame = xr_runtime::RuntimeHmdPoseF64V1;
  using Header = xr_runtime::RingHeaderV1;
  using Slot = xr_runtime::RingSlotHeaderV1;
  using Config = RuntimePoseReaderConfig;
  using Sample = RuntimePoseSample;
  static const char* magic() { return "RTPOSE1"; }
  static auto reader(const Config& config) { return create_runtime_pose_reader(config); }
  static bool fresh(const Sample& s) { return runtime_pose_sample_is_fresh(s, 250); }
  static double value(const Sample& s) { return s.pose.px; }
  static uint64_t sequence(const Sample& s) { return s.pose.sequence; }
  static Frame frame(float value) { Frame f; f.px = value; return f; }
};

struct Hand {
  using Frame = xr_tracking::HandTrackingFrameF32V2;
  using Header = xr_tracking::TrackingRingHeaderV1;
  using Slot = xr_tracking::TrackingSlotHeaderV1;
  using Config = RuntimeHandReaderConfig;
  using Sample = RuntimeHandSample;
  static const char* magic() { return "HTRKRG1"; }
  static auto reader(const Config& config) { return create_runtime_hand_reader(config); }
  static bool fresh(const Sample& s) { return runtime_hand_sample_is_fresh(s, 250); }
  static double value(const Sample& s) { return s.frame.left.controller_px; }
  static uint64_t sequence(const Sample& s) { return s.frame.sequence; }
  static Frame frame(float value) { Frame f; f.left.controller_px = value; return f; }
};

struct Controller {
  using Frame = xr_runtime::RuntimeControllerStateFrameV1;
  using Header = xr_runtime::RingHeaderV1;
  using Slot = xr_runtime::RingSlotHeaderV1;
  using Config = RuntimeControllerStateReaderConfig;
  using Sample = RuntimeControllerStateSample;
  static const char* magic() { return "RTCTRL1"; }
  static auto reader(const Config& config) { return create_runtime_controller_state_reader(config); }
  static bool fresh(const Sample& s) { return runtime_controller_state_sample_is_fresh(s, 250); }
  static double value(const Sample& s) { return s.frame.left.thumbstick_x; }
  static uint64_t sequence(const Sample& s) { return s.frame.sequence; }
  static Frame frame(float value) { Frame f; f.left.thumbstick_x = value; return f; }
};

size_t open_fd_count() {
  auto* dir = opendir("/proc/self/fd");
  check(dir != nullptr, "open /proc/self/fd");
  size_t count = 0;
  while (readdir(dir) != nullptr) ++count;
  closedir(dir);
  return count;
}

template<class Predicate>
void eventually(Predicate predicate, const std::string& message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    if (predicate()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error(message);
}

template<class Case>
void lifecycle(const std::string& label) {
  const std::string name = "/xr_monado_test_" + std::to_string(getpid()) + "_" + label;
  struct Cleanup { std::string name; ~Cleanup() { shm_unlink(name.c_str()); } } cleanup{name};
  shm_unlink(name.c_str());
  using TestRing = Ring<typename Case::Frame, typename Case::Header, typename Case::Slot>;
  typename Case::Config config;
  config.transport = "shm";
  config.shm_name = name;
  auto reader = Case::reader(config);
  typename Case::Sample sample;
  std::string error;
  check(!reader->read_latest(sample, &error), label + ": missing publisher accepted");
  TestRing first(name, Case::magic());
  eventually([&] { return !reader->read_latest(sample, &error) &&
                          error == "ring has no committed frames yet"; }, label + ": empty ring accepted");
  auto read_value = [&](float expected) {
    return reader->read_latest(sample, &error) && Case::value(sample) == expected;
  };
  first.write(Case::frame(-0.75f), 50);
  eventually([&] { return read_value(-0.75f); }, label + ": initial attach failed");
  first.write(Case::frame(-0.5f), 51);
  check(read_value(-0.5f), label + ": live update failed");
  first.write(Case::frame(-0.25f));
  check(read_value(-0.25f), label + ": sequence reset without unlink failed");

  TestRing second(name, Case::magic(), 4);
  second.write(Case::frame(0.75f));
  // No stale timeout: the old timestamp is still fresh, and both sequences are 1.
  check(read_value(0.75f) && Case::fresh(sample), label + ": still reads replaced SHM");
  second.write(Case::frame(0.5f), 2);
  check(read_value(0.5f), label + ": replacement live update failed");
  second.write(Case::frame(0.5f), 2, 300000000);
  check(read_value(0.5f) && !Case::fresh(sample), label + ": stopped stream became fresh");
  std::this_thread::sleep_for(std::chrono::milliseconds(550));
  check(read_value(0.5f) && !Case::fresh(sample), label + ": stall reattach refreshed stale data");
  second.write(Case::frame(0.25f), 3);
  check(read_value(0.25f) && Case::fresh(sample), label + ": resume after stall failed");

  shm_unlink(name.c_str());
  check(!reader->read_latest(sample, &error) && Case::sequence(sample) == 0,
        label + ": removed SHM returned a cached sample");
  auto current = std::make_unique<TestRing>(name, Case::magic());
  current->write(Case::frame(-0.25f));
  eventually([&] { return read_value(-0.25f); }, label + ": missing-stream recovery failed");
  const auto fd_count = open_fd_count();
  for (int i = 0; i < 20; ++i) {
    current = std::make_unique<TestRing>(name, Case::magic());
    current->write(Case::frame(0.75f));
    check(read_value(0.75f), label + ": repeated restart failed");
  }
  check(open_fd_count() == fd_count, label + ": restart leaked file descriptors");

  current->grow();
  current->write(Case::frame(0.5f));
  check(read_value(0.5f), label + ": resizing the same object failed");
  current->truncate();
  check(!reader->read_latest(sample, &error) && Case::sequence(sample) == 0,
        label + ": truncated SHM accepted");
  current = std::make_unique<TestRing>(name, "INVALID");
  current->write(Case::frame(0));
  eventually([&] { return !reader->read_latest(sample, &error) && error == "SHM magic mismatch"; },
             label + ": invalid replacement accepted");
  current->initialize(Case::magic());
  current->write(Case::frame(0.25f));
  eventually([&] { return read_value(0.25f); }, label + ": partial-header recovery failed");
  current->header().slot_count = std::numeric_limits<uint32_t>::max();
  check(!reader->read_latest(sample, &error), label + ": oversized ring layout accepted");
  current->initialize(Case::magic());
  current->write(Case::frame(-0.75f));
  eventually([&] { return read_value(-0.75f); }, label + ": invalid-layout recovery failed");
  std::cout << label << ": restart, stall, removal, resize, truncation, partial header and FD checks passed\n";
}
}  // namespace

int main() {
  try {
    lifecycle<Pose>("pose");
    lifecycle<Controller>("controller");
    lifecycle<Hand>("hand");
  } catch (const std::exception& error) {
    std::cerr << "Monado SHM regression: " << error.what() << '\n';
    return 1;
  }
}
