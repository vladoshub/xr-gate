#include <mercury_hand_tracking/mercury_runtime_loader.hpp>

// Only the pose-producing C ABI is mocked; the real loader and runtime gate
// perform all timestamp, velocity, rejection and reacquire calculations.
struct PoseInput { float x; float qw; float qy; uint32_t status; };
namespace abi = xr_tracking::mercury_runtime_abi;
extern "C" void* xr_mercury_runtime_create(const abi::CreateInfo*) {
  static int context;
  return &context;
}
extern "C" void xr_mercury_runtime_destroy(void*) {}
extern "C" const char* xr_mercury_runtime_last_error(void*) { return ""; }
extern "C" int xr_mercury_runtime_process_gray8(void*, const uint8_t* pixels,
    uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t, uint32_t,
    uint64_t timestamp, uint64_t sequence, abi::FrameF32* out) {
  PoseInput input{};
  std::memcpy(&input, pixels, sizeof(input));
  *out = {};
  out->source_timestamp_ns = timestamp;
  out->timestamp_ns = timestamp;
  out->sequence = sequence;
  out->tracking_status = input.status;
  out->flags = input.status ? 1u : 0u;
  out->hand_count = input.status ? 1u : 0u;
  out->left.handedness = 1;
  out->left.status = input.status;
  out->left.flags = input.status ? 1u : 0u;
  out->left.controller_px = input.x;
  out->left.controller_pz = -.3f;
  out->left.confidence = 1;
  out->left.controller_qw = input.qw;
  out->left.controller_qy = input.qy;
  return 0;
}
