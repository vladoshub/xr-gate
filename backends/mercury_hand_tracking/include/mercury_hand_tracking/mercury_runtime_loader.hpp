#pragma once

#include <xr_tracking/publishers/hand_tracking_shm_publisher.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
    derive_controller_velocities(frame);
    return frame;
  }

 private:
  struct LastSidePose {
    bool valid = false;
    bool quat_valid = false;
    uint64_t timestamp_ns = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
  };

  struct QuatD {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  static constexpr uint32_t kHandPoseValid = 1u << 0;
  static constexpr uint32_t kHandLinearVelocityValid = 1u << 1;
  static constexpr uint32_t kHandAngularVelocityValid = 1u << 2;
  static constexpr double kMinVelocityDtS = 1e-4;
  static constexpr double kMaxVelocityDtS = 0.25;
  static constexpr double kMaxAngularVelocityRadS = 30.0;

  static bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }

  static bool finite4(float w, float x, float y, float z) {
    return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }

  static void clear_linear_velocity(HandSideF32V2& side) {
    side.vx = 0.0f;
    side.vy = 0.0f;
    side.vz = 0.0f;
    side.flags &= ~kHandLinearVelocityValid;
  }

  static void clear_angular_velocity(HandSideF32V2& side) {
    side.wx = 0.0f;
    side.wy = 0.0f;
    side.wz = 0.0f;
    side.flags &= ~kHandAngularVelocityValid;
  }

  static bool normalize_quat(QuatD& q) {
    const double n2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
    if (!std::isfinite(n2) || n2 < 1e-12) return false;
    const double inv_n = 1.0 / std::sqrt(n2);
    q.w *= inv_n;
    q.x *= inv_n;
    q.y *= inv_n;
    q.z *= inv_n;
    return true;
  }

  static double dot_quat(const QuatD& a, const QuatD& b) {
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
  }

  static QuatD conjugate_quat(const QuatD& q) {
    return QuatD{q.w, -q.x, -q.y, -q.z};
  }

  static QuatD multiply_quat(const QuatD& a, const QuatD& b) {
    return QuatD{
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
  }

  static bool derive_angular_velocity(const HandSideF32V2& side, const LastSidePose& last, double dt_s,
                                      float& wx, float& wy, float& wz) {
    QuatD q_prev{double(last.qw), double(last.qx), double(last.qy), double(last.qz)};
    QuatD q_curr{double(side.controller_qw),
                 double(side.controller_qx),
                 double(side.controller_qy),
                 double(side.controller_qz)};
    if (!normalize_quat(q_prev) || !normalize_quat(q_curr)) return false;

    // q and -q represent the same orientation. Keep the shortest quaternion path
    // so a harmless sign flip does not look like a huge wrist spin.
    if (dot_quat(q_prev, q_curr) < 0.0) {
      q_curr.w = -q_curr.w;
      q_curr.x = -q_curr.x;
      q_curr.y = -q_curr.y;
      q_curr.z = -q_curr.z;
    }

    QuatD q_delta = multiply_quat(q_curr, conjugate_quat(q_prev));
    if (!normalize_quat(q_delta)) return false;
    if (q_delta.w < 0.0) {
      q_delta.w = -q_delta.w;
      q_delta.x = -q_delta.x;
      q_delta.y = -q_delta.y;
      q_delta.z = -q_delta.z;
    }

    const double sin_half = std::sqrt(q_delta.x * q_delta.x + q_delta.y * q_delta.y + q_delta.z * q_delta.z);
    if (!std::isfinite(sin_half)) return false;
    if (sin_half < 1e-9) {
      wx = 0.0f;
      wy = 0.0f;
      wz = 0.0f;
      return true;
    }

    const double angle = 2.0 * std::atan2(sin_half, std::clamp(q_delta.w, -1.0, 1.0));
    const double scale = angle / (sin_half * dt_s);
    double ox = q_delta.x * scale;
    double oy = q_delta.y * scale;
    double oz = q_delta.z * scale;

    const double mag = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (!std::isfinite(mag)) return false;
    if (mag > kMaxAngularVelocityRadS) {
      const double clamp_scale = kMaxAngularVelocityRadS / mag;
      ox *= clamp_scale;
      oy *= clamp_scale;
      oz *= clamp_scale;
    }

    wx = static_cast<float>(ox);
    wy = static_cast<float>(oy);
    wz = static_cast<float>(oz);
    return finite3(wx, wy, wz);
  }

  void update_side_velocity(HandSideF32V2& side, LastSidePose& last, uint64_t timestamp_ns) {
    const bool position_valid = side.status != 0 && ((side.flags & kHandPoseValid) != 0u) &&
                                finite3(side.controller_px, side.controller_py, side.controller_pz);
    const bool orientation_valid = finite4(side.controller_qw, side.controller_qx, side.controller_qy, side.controller_qz);
    if (!position_valid || timestamp_ns == 0) {
      clear_linear_velocity(side);
      clear_angular_velocity(side);
      last = {};
      return;
    }

    if (last.valid && timestamp_ns > last.timestamp_ns) {
      const double dt_s = double(timestamp_ns - last.timestamp_ns) * 1e-9;
      if (dt_s >= kMinVelocityDtS && dt_s <= kMaxVelocityDtS) {
        const float vx = static_cast<float>((double(side.controller_px) - double(last.px)) / dt_s);
        const float vy = static_cast<float>((double(side.controller_py) - double(last.py)) / dt_s);
        const float vz = static_cast<float>((double(side.controller_pz) - double(last.pz)) / dt_s);
        if (finite3(vx, vy, vz)) {
          side.vx = vx;
          side.vy = vy;
          side.vz = vz;
          side.flags |= kHandLinearVelocityValid;
        } else {
          clear_linear_velocity(side);
        }

        if (orientation_valid && last.quat_valid &&
            derive_angular_velocity(side, last, dt_s, side.wx, side.wy, side.wz)) {
          side.flags |= kHandAngularVelocityValid;
        } else {
          clear_angular_velocity(side);
        }
      } else {
        clear_linear_velocity(side);
        clear_angular_velocity(side);
      }
    } else {
      clear_linear_velocity(side);
      clear_angular_velocity(side);
    }

    last.valid = true;
    last.quat_valid = orientation_valid;
    last.timestamp_ns = timestamp_ns;
    last.px = side.controller_px;
    last.py = side.controller_py;
    last.pz = side.controller_pz;
    last.qw = side.controller_qw;
    last.qx = side.controller_qx;
    last.qy = side.controller_qy;
    last.qz = side.controller_qz;
  }

  void derive_controller_velocities(HandTrackingFrameF32V2& frame) {
    const uint64_t timestamp_ns = frame.source_timestamp_ns != 0 ? frame.source_timestamp_ns : frame.timestamp_ns;
    update_side_velocity(frame.left, last_side_pose_[0], timestamp_ns);
    update_side_velocity(frame.right, last_side_pose_[1], timestamp_ns);
  }

  MercuryRuntimeConfig cfg_;
  DynamicLibrary lib_;
  mercury_runtime_abi::create_fn create_ = nullptr;
  mercury_runtime_abi::destroy_fn destroy_ = nullptr;
  mercury_runtime_abi::process_gray8_fn process_gray8_ = nullptr;
  mercury_runtime_abi::last_error_fn last_error_ = nullptr;
  void* ctx_ = nullptr;
  std::array<LastSidePose, 2> last_side_pose_{};
  std::string models_dir_storage_;
  std::string calib_json_storage_;
};

}  // namespace xr_tracking
