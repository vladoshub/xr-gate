#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace xr_runtime {

// Sideband presentation metadata. ControllerInputV3 and its IMU calibration
// remain unchanged. PROVIDED distinguishes an explicit identity/disabled offset
// from an omitted setting, which asks the consumer to use its own defaults.
constexpr uint32_t CONTROLLER_RUNTIME_OFFSET_PROVIDED = 1;
constexpr uint32_t CONTROLLER_RUNTIME_OFFSET_ENABLED = 2;
constexpr uint32_t CONTROLLER_RUNTIME_OFFSET_PRE_MULTIPLY = 4;
struct ControllerRuntimeOrientationOffsetV1 {
  uint32_t flags = 0;
  uint32_t reserved = 0;
  float rotation_xyzw[4] = {0, 0, 0, 1};
};
struct ControllerRuntimeProfileV1 {
  uint32_t version = 1;
  uint32_t size_bytes = sizeof(ControllerRuntimeProfileV1);
  uint64_t revision = 0;
  ControllerRuntimeOrientationOffsetV1 left;
  ControllerRuntimeOrientationOffsetV1 right;
};
static_assert(sizeof(ControllerRuntimeProfileV1) == 64, "runtime profile ABI");

constexpr uint32_t CONTROLLER_INPUT_TCP_PROFILE_MAGIC = 0x43525031;  // CRP1
constexpr uint32_t CONTROLLER_INPUT_TCP_PROFILE_SUPPORTED = 1;
constexpr uint64_t CONTROLLER_INPUT_TCP_PROFILE_HELLO = 0x3150524352415258ULL;
constexpr size_t CONTROLLER_RUNTIME_PROFILE_SHM_OFFSET = 128;

// Fits in the reserved 4096-byte ring header. Atomic words keep both writer and
// late readers safe without changing high-frequency controller samples.
struct alignas(8) ControllerRuntimeProfileShmV1 {
  std::atomic<uint64_t> guard{0};
  std::atomic<uint64_t> words[sizeof(ControllerRuntimeProfileV1) / 8]{};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free, "profile SHM requires lock-free uint64");

inline void write_controller_runtime_profile(ControllerRuntimeProfileShmV1& shm,
                                             const ControllerRuntimeProfileV1& profile) {
  uint64_t words[sizeof(profile) / 8];
  std::memcpy(words, &profile, sizeof(profile));
  shm.guard.store(profile.revision * 2 - 1, std::memory_order_seq_cst);
  for (size_t i = 0; i < sizeof(profile) / 8; ++i)
    shm.words[i].store(words[i], std::memory_order_seq_cst);
  shm.guard.store(profile.revision * 2, std::memory_order_seq_cst);
}

inline bool read_controller_runtime_profile(const ControllerRuntimeProfileShmV1& shm,
                                            ControllerRuntimeProfileV1& profile) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto before = shm.guard.load(std::memory_order_seq_cst);
    if (before == 0 || (before & 1)) continue;
    uint64_t words[sizeof(profile) / 8];
    for (size_t i = 0; i < sizeof(profile) / 8; ++i)
      words[i] = shm.words[i].load(std::memory_order_seq_cst);
    if (before != shm.guard.load(std::memory_order_seq_cst)) continue;
    std::memcpy(&profile, words, sizeof(profile));
    if (profile.version != 1 || profile.size_bytes != sizeof(profile))
      throw std::runtime_error("unsupported controller runtime profile ABI");
    return true;
  }
  return false;
}

}  // namespace xr_runtime
