#pragma once

#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xr_tracking {

namespace mercury_runtime_abi {

constexpr uint32_t ABI_VERSION = 1;
constexpr uint32_t HAND_JOINT_COUNT = 21;

#pragma pack(push, 1)

struct JointF32 {
  uint32_t joint_id;
  uint32_t flags;
  float px;
  float py;
  float pz;
  float qw;
  float qx;
  float qy;
  float qz;
  float radius_m;
  float confidence;
};

struct HandSideF32 {
  uint32_t handedness;
  uint32_t status;
  uint32_t flags;
  float confidence;

  float controller_px;
  float controller_py;
  float controller_pz;
  float controller_qw;
  float controller_qx;
  float controller_qy;
  float controller_qz;

  float palm_px;
  float palm_py;
  float palm_pz;
  float palm_qw;
  float palm_qx;
  float palm_qy;
  float palm_qz;

  float wrist_px;
  float wrist_py;
  float wrist_pz;
  float wrist_qw;
  float wrist_qx;
  float wrist_qy;
  float wrist_qz;

  float vx;
  float vy;
  float vz;
  float wx;
  float wy;
  float wz;

  float pinch_strength;
  float grab_strength;
  uint32_t pinch_active;
  uint32_t grab_active;

  uint32_t joint_count;
  uint32_t reserved0;

  JointF32 joints[HAND_JOINT_COUNT];
};

struct FrameF32 {
  uint32_t version;
  uint32_t size_bytes;

  uint64_t sequence;
  uint64_t timestamp_ns;
  uint64_t source_timestamp_ns;
  uint64_t reset_counter;

  uint32_t tracking_status;
  uint32_t flags;
  float confidence;
  uint32_t hand_count;

  HandSideF32 left;
  HandSideF32 right;
};

struct CreateInfo {
  uint32_t abi_version;
  uint32_t size_bytes;
  const char* models_dir;
  const char* calib_json;
  uint32_t flags;
  int32_t orientation0;
  int32_t orientation1;
  int32_t swap_cameras;
  int32_t boundary_circle;
  float boundary0_center_x;
  float boundary0_center_y;
  float boundary0_radius;
  float boundary1_center_x;
  float boundary1_center_y;
  float boundary1_radius;
};

#pragma pack(pop)

static_assert(sizeof(JointF32) == sizeof(HandJointF32V2), "Mercury ABI joint layout mismatch");
static_assert(sizeof(HandSideF32) == sizeof(HandSideF32V2), "Mercury ABI side layout mismatch");
static_assert(sizeof(FrameF32) == sizeof(HandTrackingFrameF32V2), "Mercury ABI frame layout mismatch");

using create_fn = void* (*)(const CreateInfo* info);
using destroy_fn = void (*)(void* ctx);
using process_gray8_fn = int (*)(void* ctx,
                                  const uint8_t* cam0,
                                  uint32_t cam0_width,
                                  uint32_t cam0_height,
                                  uint32_t cam0_stride,
                                  const uint8_t* cam1,
                                  uint32_t cam1_width,
                                  uint32_t cam1_height,
                                  uint32_t cam1_stride,
                                  uint64_t source_timestamp_ns,
                                  uint64_t sequence,
                                  FrameF32* out_frame);
using last_error_fn = const char* (*)(void* ctx);

}  // namespace mercury_runtime_abi

struct MercuryRuntimeConfig {
  std::filesystem::path library_path;
  std::filesystem::path models_dir;
  std::filesystem::path calib_json;
  bool swap_cameras = false;
  bool boundary_circle = false;
  float boundary0_center_x = 0.5f;
  float boundary0_center_y = 0.5f;
  float boundary0_radius = 0.55f;
  float boundary1_center_x = 0.5f;
  float boundary1_center_y = 0.5f;
  float boundary1_radius = 0.55f;
  int orientation0 = 0;
  int orientation1 = 0;
};

class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  explicit DynamicLibrary(const std::filesystem::path& path) { open(path); }
  ~DynamicLibrary() { close(); }

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  DynamicLibrary(DynamicLibrary&& other) noexcept { handle_ = other.handle_; other.handle_ = nullptr; }
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  void open(const std::filesystem::path& path) {
    close();
#if defined(_WIN32)
    handle_ = reinterpret_cast<void*>(LoadLibraryA(path.string().c_str()));
    if (!handle_) {
      throw std::runtime_error("failed to load Mercury runtime DLL: " + path.string());
    }
#else
    handle_ = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
      const char* err = dlerror();
      throw std::runtime_error("failed to dlopen Mercury runtime library: " + path.string() +
                               (err ? std::string("; ") + err : std::string()));
    }
#endif
  }

  void close() noexcept {
    if (!handle_) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
  }

  template <typename Fn>
  Fn symbol(const char* name) const {
    if (!handle_) throw std::runtime_error("dynamic library is not loaded");
#if defined(_WIN32)
    auto* p = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
    auto* p = dlsym(handle_, name);
#endif
    if (!p) throw std::runtime_error(std::string("Mercury runtime symbol not found: ") + name);
    return reinterpret_cast<Fn>(p);
  }

 private:
  void* handle_ = nullptr;
};

class MercuryRuntimeProcessor {
 public:
  explicit MercuryRuntimeProcessor(MercuryRuntimeConfig cfg)
      : cfg_(std::move(cfg)), lib_(cfg_.library_path) {
    create_ = lib_.symbol<mercury_runtime_abi::create_fn>("xr_mercury_runtime_create");
    destroy_ = lib_.symbol<mercury_runtime_abi::destroy_fn>("xr_mercury_runtime_destroy");
    process_gray8_ = lib_.symbol<mercury_runtime_abi::process_gray8_fn>("xr_mercury_runtime_process_gray8");
    last_error_ = lib_.symbol<mercury_runtime_abi::last_error_fn>("xr_mercury_runtime_last_error");

    models_dir_storage_ = cfg_.models_dir.string();
    calib_json_storage_ = cfg_.calib_json.string();

    mercury_runtime_abi::CreateInfo info{};
    info.abi_version = mercury_runtime_abi::ABI_VERSION;
    info.size_bytes = sizeof(info);
    info.models_dir = models_dir_storage_.c_str();
    info.calib_json = calib_json_storage_.c_str();
    info.orientation0 = cfg_.orientation0;
    info.orientation1 = cfg_.orientation1;
    info.swap_cameras = cfg_.swap_cameras ? 1 : 0;
    info.boundary_circle = cfg_.boundary_circle ? 1 : 0;
    info.boundary0_center_x = cfg_.boundary0_center_x;
    info.boundary0_center_y = cfg_.boundary0_center_y;
    info.boundary0_radius = cfg_.boundary0_radius;
    info.boundary1_center_x = cfg_.boundary1_center_x;
    info.boundary1_center_y = cfg_.boundary1_center_y;
    info.boundary1_radius = cfg_.boundary1_radius;

    ctx_ = create_(&info);
    if (!ctx_) {
      throw std::runtime_error("xr_mercury_runtime_create failed");
    }
  }

  ~MercuryRuntimeProcessor() {
    if (ctx_ && destroy_) destroy_(ctx_);
    ctx_ = nullptr;
  }

  MercuryRuntimeProcessor(const MercuryRuntimeProcessor&) = delete;
  MercuryRuntimeProcessor& operator=(const MercuryRuntimeProcessor&) = delete;

  template <typename StereoPair>
  HandTrackingFrameF32V2 process(const StereoPair& pair, uint64_t reset_counter) {
    mercury_runtime_abi::FrameF32 out{};
    const int rc = process_gray8_(ctx_,
                                  pair.cam0.gray8.data(),
                                  pair.cam0.width,
                                  pair.cam0.height,
                                  pair.cam0.width,
                                  pair.cam1.gray8.data(),
                                  pair.cam1.width,
                                  pair.cam1.height,
                                  pair.cam1.width,
                                  static_cast<uint64_t>(pair.timestamp_ns),
                                  pair.sequence(),
                                  &out);
    if (rc != 0) {
      const char* err = last_error_ ? last_error_(ctx_) : nullptr;
      throw std::runtime_error(std::string("Mercury runtime process failed") +
                               (err ? std::string(": ") + err : std::string()));
    }

    HandTrackingFrameF32V2 frame{};
    static_assert(sizeof(frame) == sizeof(out), "V2 frame ABI/copy size mismatch");
    // HandTrackingFrameF32V2 has default member initializers, so GCC treats it as
    // non-trivial and rejects std::memcpy(&frame, ...) under -Wclass-memaccess.
    // The Mercury ABI frame is intentionally layout-compatible with the generic 21-joint runtime hand tracking contract.
    const auto* src_bytes = reinterpret_cast<const unsigned char*>(&out);
    auto* dst_bytes = reinterpret_cast<unsigned char*>(&frame);
    for (std::size_t i = 0; i < sizeof(frame); ++i) {
      dst_bytes[i] = src_bytes[i];
    }
    frame.reset_counter = reset_counter;
    frame.timestamp_ns = static_cast<uint64_t>(now_ns());
    frame.size_bytes = sizeof(HandTrackingFrameF32V2);
    frame.version = HAND_TRACKING_FORMAT_VERSION_V2;
    return frame;
  }

 private:
  MercuryRuntimeConfig cfg_;
  DynamicLibrary lib_;
  mercury_runtime_abi::create_fn create_ = nullptr;
  mercury_runtime_abi::destroy_fn destroy_ = nullptr;
  mercury_runtime_abi::process_gray8_fn process_gray8_ = nullptr;
  mercury_runtime_abi::last_error_fn last_error_ = nullptr;
  void* ctx_ = nullptr;
  std::string models_dir_storage_;
  std::string calib_json_storage_;
};

}  // namespace xr_tracking
