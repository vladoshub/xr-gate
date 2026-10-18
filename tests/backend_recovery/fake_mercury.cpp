#include <mercury_hand_tracking/mercury_runtime_loader.hpp>
#include <cstdio>
using namespace xr_tracking::mercury_runtime_abi;
extern "C" void* xr_mercury_runtime_create(const CreateInfo*) {
  std::fprintf(stderr,"AUDIT mercury create called\n");
  return reinterpret_cast<void*>(1);
}
extern "C" void xr_mercury_runtime_destroy(void*) {}
extern "C" const char* xr_mercury_runtime_last_error(void*) { return "audit"; }
extern "C" int xr_mercury_runtime_process_gray8(void*,const uint8_t*,uint32_t,uint32_t,uint32_t,
    const uint8_t*,uint32_t,uint32_t,uint32_t,uint64_t ts,uint64_t seq,FrameF32* out) {
  std::fprintf(stderr,"AUDIT mercury process called\n");
  *out={}; out->version=2; out->size_bytes=sizeof(*out); out->sequence=seq;
  out->source_timestamp_ns=ts; out->timestamp_ns=ts;
  return 0;
}
