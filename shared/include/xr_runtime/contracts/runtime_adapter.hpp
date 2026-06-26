#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <xr_runtime/registry/runtime_paths.hpp>
#include <xr_tracking/types/tracking_types.hpp>

namespace xr_runtime {

inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline double ns_to_ms(int64_t ns) { return double(ns) / 1e6; }

struct StreamInfo {
  std::string stream_id;
  std::string shm_name;
  std::string format_name;
  uint32_t format_version = 1;
  uint32_t payload_size = 0;
  uint32_t slot_count = 0;
  uint32_t header_size = 4096;
  uint32_t slot_header_size = 128;
  uint32_t slot_stride = 0;
  std::string frame_id = "tracking_world";
};

inline std::string normalize_posix_name(std::string name) {
  if (name.empty()) throw std::runtime_error("empty SHM name");
  if (name[0] != '/') name = "/" + name;
  return name;
}

inline nlohmann::json read_json_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("failed to open JSON file: " + path);
  nlohmann::json j;
  in >> j;
  return j;
}

inline StreamInfo stream_info_from_registry(const std::string& registry_path,
                                            const std::string& stream_id) {
  auto j = read_json_file(registry_path);
  if (!j.contains("streams") || !j["streams"].contains(stream_id)) {
    throw std::runtime_error("stream '" + stream_id + "' not found in registry " + registry_path);
  }

  const auto& s = j["streams"][stream_id];

  StreamInfo info;
  info.stream_id = stream_id;
  info.shm_name = s.value("shm_name", "");
  info.format_name = s.value("format_name", s.value("format", ""));
  info.format_version = s.value("format_version", 1);
  info.payload_size = s.value("payload_size", 0);
  info.slot_count = s.value("slot_count", 0);
  info.header_size = s.value("header_size", 4096);
  info.slot_header_size = s.value("slot_header_size", 128);
  info.slot_stride = s.value("slot_stride", uint32_t(info.slot_header_size + info.payload_size));
  info.frame_id = s.value("frame_id", "tracking_world");

  if (info.shm_name.empty()) {
    throw std::runtime_error("stream '" + stream_id + "' has empty shm_name");
  }

  return info;
}


#ifndef _WIN32
class ShmMapping {
 public:
  ShmMapping() = default;

  explicit ShmMapping(const std::string& shm_name) {
    const std::string posix_name = normalize_posix_name(shm_name);
    fd_ = shm_open(posix_name.c_str(), O_RDONLY, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("shm_open failed for " + posix_name + ": " + std::strerror(errno));
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0) {
      const std::string err = std::strerror(errno);
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("fstat failed: " + err);
    }

    size_ = static_cast<size_t>(st.st_size);
    data_ = static_cast<const uint8_t*>(mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0));
    if (data_ == MAP_FAILED) {
      const std::string err = std::strerror(errno);
      data_ = nullptr;
      close(fd_);
      fd_ = -1;
      throw std::runtime_error("mmap failed: " + err);
    }
  }

  ~ShmMapping() { close_mapping(); }

  ShmMapping(const ShmMapping&) = delete;
  ShmMapping& operator=(const ShmMapping&) = delete;

  ShmMapping(ShmMapping&& other) noexcept {
    fd_ = other.fd_;
    data_ = other.data_;
    size_ = other.size_;
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
  }

  ShmMapping& operator=(ShmMapping&& other) noexcept {
    if (this != &other) {
      close_mapping();
      fd_ = other.fd_;
      data_ = other.data_;
      size_ = other.size_;
      other.fd_ = -1;
      other.data_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  void close_mapping() {
    if (data_) {
      munmap(const_cast<uint8_t*>(data_), size_);
      data_ = nullptr;
      size_ = 0;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  int fd_ = -1;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};
#else
// Native Windows runtime builds should use network input (for example
// xr_runtime_adapter --input udp). POSIX SHM is intentionally isolated here so
// future Win32 shared-memory support can be added without changing runtime core.
class ShmMapping {
 public:
  ShmMapping() = default;

  explicit ShmMapping(const std::string& shm_name) {
    (void)shm_name;
    throw std::runtime_error(
        "POSIX SHM tracking input is not available in native Windows builds; use --input udp");
  }

  const uint8_t* data() const { return nullptr; }
  size_t size() const { return 0; }
  explicit operator bool() const { return false; }
};
#endif

template <typename PayloadT>
class TrackingRingReader {
 public:
  TrackingRingReader() = default;

  TrackingRingReader(StreamInfo info, std::string expected_format)
      : info_(std::move(info)),
        expected_format_(std::move(expected_format)),
        shm_(info_.shm_name) {
    if (info_.format_name != expected_format_) {
      throw std::runtime_error("stream '" + info_.stream_id + "' has format '" +
                               info_.format_name + "', expected '" + expected_format_ + "'");
    }

    if (info_.payload_size != 0 && info_.payload_size < sizeof(PayloadT)) {
      throw std::runtime_error("stream '" + info_.stream_id + "' payload_size too small");
    }

    if (info_.slot_count == 0 || info_.slot_stride == 0 ||
        info_.slot_header_size == 0 || info_.header_size == 0) {
      throw std::runtime_error("stream '" + info_.stream_id + "' registry ring metadata is invalid");
    }

    const size_t min_size = static_cast<size_t>(info_.header_size) +
                            static_cast<size_t>(info_.slot_count) *
                                static_cast<size_t>(info_.slot_stride);
    if (shm_.size() < min_size) {
      throw std::runtime_error("stream '" + info_.stream_id + "' SHM is smaller than registry metadata");
    }
  }

  const StreamInfo& info() const { return info_; }

  std::optional<PayloadT> latest() const {
    if (!shm_) return std::nullopt;

    uint64_t latest_seq = 0;
    uint32_t slot_count = info_.slot_count;
    uint32_t header_size = info_.header_size;
    uint32_t slot_stride = info_.slot_stride;
    uint32_t slot_header_size = info_.slot_header_size;

    if (expected_format_ == "HMD_POSE_F64_LE") {
      // HMD pose publisher uses CAPSHM1/capture-service-like header:
      //   magic[0..7] = CAPSHM1
      //   slot_count u64 at offset 32
      //   latest_seq u64 at offset 40
      if (shm_.size() < 48) return std::nullopt;
      const uint64_t header_slot_count = load_u64_le(shm_.data() + 32);
      if (header_slot_count != 0) {
        slot_count = static_cast<uint32_t>(header_slot_count);
      }
      latest_seq = load_u64_le(shm_.data() + 40);
    } else {
      // HAND_TRACKING_V1/V2 use HTRKRG1/new compact ring header.
      if (shm_.size() < sizeof(RingHeaderV1)) return std::nullopt;
      const auto* h = reinterpret_cast<const RingHeaderV1*>(shm_.data());
      latest_seq = h->latest_sequence;
      // Compatibility for early spatial-summary/proxy-mesh publishers that wrote
      // latest_sequence at byte offset 40 instead of offsetof(RingHeaderV1,
      // latest_sequence).  With the packed header that made the canonical field
      // look like (seq << 32), so readers picked the wrong ring slot or saw no
      // payload. Prefer the sane legacy value while old processes/SHM segments
      // are still alive; fixed publishers write the canonical field and leave
      // the legacy read as zero for normal sequence values.
      if (shm_.size() >= 48) {
        const uint64_t legacy_latest_seq = load_u64_le(shm_.data() + 40);
        if (legacy_latest_seq != 0 && legacy_latest_seq < (1ull << 32) &&
            (latest_seq == 0 || latest_seq > (1ull << 32))) {
          latest_seq = legacy_latest_seq;
        }
      }
      if (h->slot_count != 0) slot_count = h->slot_count;
      if (h->header_size != 0) header_size = h->header_size;
      if (h->slot_stride != 0) slot_stride = h->slot_stride;
      if (h->slot_header_size != 0) slot_header_size = h->slot_header_size;
    }

    if (latest_seq == 0 || slot_count == 0 || slot_stride == 0 || slot_header_size == 0) {
      return std::nullopt;
    }

    const uint32_t idx = static_cast<uint32_t>((latest_seq - 1) % slot_count);
    const size_t off = static_cast<size_t>(header_size) + static_cast<size_t>(idx) * slot_stride;
    if (off + slot_header_size + sizeof(PayloadT) > shm_.size()) return std::nullopt;

    const auto* slot = reinterpret_cast<const RingSlotHeaderV1*>(shm_.data() + off);
    const uint64_t seq_begin_1 = slot->seq_begin;
    const uint64_t seq_end_1 = slot->seq_end;

    if (seq_begin_1 != seq_end_1 || (seq_begin_1 & 1ull)) {
      return std::nullopt;
    }

    PayloadT payload {};
    std::memcpy(&payload, shm_.data() + off + slot_header_size, sizeof(PayloadT));

    const uint64_t seq_begin_2 = slot->seq_begin;
    const uint64_t seq_end_2 = slot->seq_end;
    if (seq_begin_1 != seq_begin_2 || seq_end_1 != seq_end_2) {
      return std::nullopt;
    }

    return payload;
  }

 private:
  StreamInfo info_;
  std::string expected_format_;
  ShmMapping shm_;
};

inline Quatd quat_normalize(Quatd q) {
  const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (n <= 0.0 || !std::isfinite(n)) {
    return {};
  }
  q.w /= n;
  q.x /= n;
  q.y /= n;
  q.z /= n;
  return q;
}

inline Quatd quat_multiply(const Quatd& a, const Quatd& b) {
  return {
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

inline Quatd delta_quat_from_angular_velocity(double wx, double wy, double wz, double dt_s) {
  const double omega = std::sqrt(wx * wx + wy * wy + wz * wz);
  if (omega <= 1e-12 || !std::isfinite(omega) || !std::isfinite(dt_s)) {
    return {};
  }

  const double angle = omega * dt_s;
  const double half = 0.5 * angle;
  const double s = std::sin(half) / omega;

  return quat_normalize({
      std::cos(half),
      wx * s,
      wy * s,
      wz * s,
  });
}

inline HmdPoseF64V1 predict_hmd_pose(const HmdPoseF64V1& src,
                                     int64_t target_timestamp_ns,
                                     double max_prediction_ms) {
  HmdPoseF64V1 out = src;

  const int64_t dt_ns = target_timestamp_ns - static_cast<int64_t>(src.timestamp_ns);
  const double max_dt_s = std::max(0.0, max_prediction_ms) / 1000.0;
  double dt_s = static_cast<double>(dt_ns) / 1e9;
  dt_s = std::clamp(dt_s, -max_dt_s, max_dt_s);

  if ((src.flags & HMD_FLAG_LINEAR_VELOCITY_VALID) != 0u) {
    out.px = src.px + src.vx * dt_s;
    out.py = src.py + src.vy * dt_s;
    out.pz = src.pz + src.vz * dt_s;
  }

  if ((src.flags & HMD_FLAG_ANGULAR_VELOCITY_VALID) != 0u) {
    const Quatd q0 = quat_normalize({src.qw, src.qx, src.qy, src.qz});
    const Quatd dq = delta_quat_from_angular_velocity(src.wx, src.wy, src.wz, dt_s);
    const Quatd q1 = quat_normalize(quat_multiply(dq, q0));
    out.qw = q1.w;
    out.qx = q1.x;
    out.qy = q1.y;
    out.qz = q1.z;
  }

  out.timestamp_ns = static_cast<uint64_t>(target_timestamp_ns);
  return out;
}

struct RuntimeFrame {
  bool hmd_valid = false;
  bool hand_valid = false;
  bool hand_v2_valid = false;
  bool tick_frame = false;
  bool hmd_predicted = false;

  HmdPoseF64V1 hmd {};
  HmdPoseF64V1 raw_hmd {};
  HandTrackingFrameF64V1 hand {};
  HandTrackingFrameF32V2 hand_v2 {};

  std::string hmd_frame_id = "tracking_world";
  std::string hand_frame_id = "tracking_world";

  int64_t read_timestamp_ns = 0;
  int64_t target_timestamp_ns = 0;
  double prediction_ms = 0.0;
};

class IXrRuntimeAdapter {
 public:
  virtual ~IXrRuntimeAdapter() = default;

  virtual const char* name() const = 0;
  virtual void start() {}
  virtual void stop() {}
  virtual void consume(const RuntimeFrame& frame) = 0;
};

class LoggingRuntimeAdapter final : public IXrRuntimeAdapter {
 public:
  const char* name() const override { return "logging"; }

  void consume(const RuntimeFrame& frame) override {
    (void)frame;
    ++frames_;
  }

  uint64_t frames() const { return frames_; }

 private:
  uint64_t frames_ = 0;
};

class MonadoOpenXrAdapterStub final : public IXrRuntimeAdapter {
 public:
  const char* name() const override { return "monado_openxr_stub"; }

  void start() override {
    std::cout << "[xr_runtime_adapter] Monado/OpenXR adapter stub started\n";
    std::cout << "[xr_runtime_adapter] TODO: map tracking_world -> OpenXR local space\n";
    std::cout << "[xr_runtime_adapter] TODO: expose HMD pose + hand joints/controller fallback\n";
  }

  void consume(const RuntimeFrame& frame) override {
    (void)frame;
    ++frames_;
  }

  void stop() override {
    std::cout << "[xr_runtime_adapter] Monado/OpenXR adapter stub stopped, frames=" << frames_ << "\n";
  }

 private:
  uint64_t frames_ = 0;
};

class SteamVrOpenVrAdapterStub final : public IXrRuntimeAdapter {
 public:
  const char* name() const override { return "steamvr_openvr_stub"; }

  void start() override {
    std::cout << "[xr_runtime_adapter] SteamVR/OpenVR adapter stub started\n";
    std::cout << "[xr_runtime_adapter] TODO: map tracking_world -> OpenVR tracking space\n";
    std::cout << "[xr_runtime_adapter] TODO: expose HMD pose + controller emulation + optional skeletal hand\n";
  }

  void consume(const RuntimeFrame& frame) override {
    (void)frame;
    ++frames_;
  }

  void stop() override {
    std::cout << "[xr_runtime_adapter] SteamVR/OpenVR adapter stub stopped, frames=" << frames_ << "\n";
  }

 private:
  uint64_t frames_ = 0;
};

inline std::unique_ptr<IXrRuntimeAdapter> make_adapter(const std::string& type) {
  if (type == "logging") {
    return std::make_unique<LoggingRuntimeAdapter>();
  }
  if (type == "monado" || type == "openxr") {
    return std::make_unique<MonadoOpenXrAdapterStub>();
  }
  if (type == "steamvr" || type == "openvr") {
    return std::make_unique<SteamVrOpenVrAdapterStub>();
  }
  throw std::runtime_error("unknown --adapter '" + type + "'; expected logging|monado|steamvr");
}

}  // namespace xr_runtime
