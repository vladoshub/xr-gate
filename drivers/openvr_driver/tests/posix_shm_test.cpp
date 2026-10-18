// Exercise the production Linux readers without a running SteamVR instance.
#include "runtime_controller_state_reader.hpp"
#include "runtime_hand_reader.hpp"
#include "runtime_pose_reader.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace xr::openvr_driver;

namespace {
void check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}

template<class Frame, class Header, class Slot>
class Ring {
 public:
  Ring(const std::string& name, const char* magic) {
    // Each fixture uses its own process-specific name. Keep old mappings alive
    // while replacing the public name, just as runtime publisher restarts do.
    shm_unlink(name.c_str());
    fd_ = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    check(fd_ >= 0, "create test SHM");
    size_ = 4096 + 2 * (sizeof(Slot) + sizeof(Frame));
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
    auto* h = reinterpret_cast<Header*>(data_);
    *h = Header{};
    std::memcpy(h->magic, magic, 8);
    h->header_size = 4096;
    h->slot_count = 2;
    h->slot_stride = sizeof(Slot) + sizeof(Frame);
    h->slot_header_size = sizeof(Slot);
    h->payload_size = sizeof(Frame);
  }

  ~Ring() { munmap(data_, size_); close(fd_); }
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  void write(Frame frame, uint64_t sequence = 1, int64_t age_ns = 0) {
    frame.sequence = sequence;
    frame.timestamp_ns = static_cast<uint64_t>(monotonic_now_ns() - age_ns);
    auto* h = reinterpret_cast<Header*>(data_);
    const size_t off = 4096 + ((sequence - 1) % 2) * h->slot_stride;
    auto* slot = reinterpret_cast<Slot*>(data_ + off);
    slot->seq_begin = slot->seq_end = sequence * 2 - 1;
    std::memcpy(data_ + off + sizeof(Slot), &frame, sizeof(frame));
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_end = sequence * 2;
    std::atomic_thread_fence(std::memory_order_release);
    slot->seq_begin = sequence * 2;
    std::atomic_thread_fence(std::memory_order_release);
    h->latest_sequence = sequence;
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
  static bool fresh(const Sample& s) { return runtime_pose_is_fresh(s, 250); }
  static double value(const Sample& s) { return s.pose.px; }
  static Frame frame(float value) {
    Frame f;
    f.px = value;
    f.flags = xr_runtime::RUNTIME_HMD_FLAG_POSE_VALID;
    f.tracking_status = 2;
    f.confidence = 1;
    return f;
  }
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
  static Frame frame(float value) {
    Frame f;
    f.flags = xr_tracking::HAND_FLAG_LEFT_VALID;
    f.left.handedness = 1;
    f.left.flags = xr_tracking::HAND_POSE_VALID;
    f.left.status = static_cast<uint32_t>(xr_tracking::HandTrackingStatus::Tracking);
    f.left.confidence = 1;
    f.left.controller_px = value;
    return f;
  }
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
  static Frame frame(float value) {
    Frame f;
    f.left.role = xr_runtime::CONTROLLER_SIDE_LEFT;
    f.left.flags = xr_runtime::RUNTIME_CONTROLLER_CONNECTED | xr_runtime::RUNTIME_CONTROLLER_POSE_VALID |
                   xr_runtime::RUNTIME_CONTROLLER_INPUT_VALID;
    f.left.thumbstick_x = value;
    f.left.thumbstick_y = value;
    return f;
  }
};

template<class Case>
void lifecycle(const std::string& label) {
  const std::string name = "/xr_openvr_test_" + std::to_string(getpid()) + "_" + label;
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
  first.write(Case::frame(-0.75f));
  check(reader->read_latest(sample, &error) && Case::value(sample) == -0.75,
        label + ": initial attach failed");
  first.write(Case::frame(-0.5f), 2);
  check(reader->read_latest(sample, &error) && Case::value(sample) == -0.5,
        label + ": live updates failed");
  first.write(Case::frame(-0.5f), 2, 300000000);
  check(reader->read_latest(sample, &error) && !Case::fresh(sample),
        label + ": stopped publisher must still be stale");

  TestRing second(name, Case::magic());
  second.write(Case::frame(0.75f)); // Sequence restarts at 1 in the replacement.
  check(reader->read_latest(sample, &error) && Case::value(sample) == 0.75 && Case::fresh(sample),
        label + ": replacement publisher still reads the unlinked old mapping");
  second.write(Case::frame(0.5f), 2);
  check(reader->read_latest(sample, &error) && Case::value(sample) == 0.5,
        label + ": replacement live updates failed");

  shm_unlink(name.c_str());
  check(!reader->read_latest(sample, &error), label + ": removed SHM must not return cached payloads");
  TestRing third(name, Case::magic());
  third.write(Case::frame(-0.25f));
  check(reader->read_latest(sample, &error) && Case::value(sample) == -0.25,
        label + ": recovery after absent publisher failed");
  third.truncate();
  check(!reader->read_latest(sample, &error), label + ": zero-sized segment accepted");
  TestRing invalid(name, "INVALID");
  invalid.write(Case::frame(0));
  check(!reader->read_latest(sample, &error), label + ": invalid replacement header accepted");
  TestRing fourth(name, Case::magic());
  fourth.write(Case::frame(0.25f));
  check(reader->read_latest(sample, &error) && Case::value(sample) == 0.25,
        label + ": recovery after invalid replacement failed");
  std::cout << label << ": restart, removal, truncation, invalid header and freshness passed\n";
}
}

int main() {
  try {
    lifecycle<Pose>("pose");
    lifecycle<Hand>("hand");
    lifecycle<Controller>("controller");
  } catch (const std::exception& error) {
    std::cerr << "OpenVR SHM regression: " << error.what() << '\n';
    return 1;
  }
}
