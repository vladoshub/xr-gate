#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <capture_client/contracts/messages.hpp>
#include <capture_client/transports/capture_service_tcp_transport.hpp>
#include <capture_client/transports/shm_transport.hpp>
#include <xr_tracking/publishers/hmd_pose_shm_publisher.hpp>
#include <xr_tracking/types/tracking_types.hpp>

namespace {

std::atomic_bool g_stop{false};
void handle_signal(int) { g_stop = true; }

constexpr double kPi = 3.141592653589793238462643383279502884;

struct V3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Q {
  double w = 1.0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

bool finite(double v) { return std::isfinite(v); }

V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

V3 cross(const V3& a, const V3& b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

double norm(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

V3 normalized(const V3& v, const V3& fallback = {0.0, 0.0, 1.0}) {
  const double n = norm(v);
  if (!finite(n) || n < 1e-9) return fallback;
  return {v.x / n, v.y / n, v.z / n};
}

Q normalize(Q q) {
  const double n2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
  if (!finite(n2) || n2 <= 0.0) return {};
  const double inv = 1.0 / std::sqrt(n2);
  q.w *= inv;
  q.x *= inv;
  q.y *= inv;
  q.z *= inv;
  if (q.w < 0.0) {
    q.w = -q.w;
    q.x = -q.x;
    q.y = -q.y;
    q.z = -q.z;
  }
  return q;
}

Q q_conj(const Q& q) { return {q.w, -q.x, -q.y, -q.z}; }

Q q_mul_raw(const Q& a, const Q& b) {
  return {a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
          a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
          a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
          a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w};
}

Q q_mul(const Q& a, const Q& b) { return normalize(q_mul_raw(a, b)); }

V3 q_rotate(const Q& q_raw, const V3& v) {
  const Q q = normalize(q_raw);
  const Q p{0.0, v.x, v.y, v.z};
  const Q r = q_mul_raw(q_mul_raw(q, p), q_conj(q));
  return {r.x, r.y, r.z};
}

Q q_from_axis_angle(double ax, double ay, double az, double radians) {
  const double len = std::sqrt(ax*ax + ay*ay + az*az);
  if (!finite(len) || len < 1e-12 || !finite(radians) || std::abs(radians) < 1e-12) return {};
  const double inv_len = 1.0 / len;
  const double half = 0.5 * radians;
  const double s = std::sin(half);
  return normalize({std::cos(half), ax * inv_len * s, ay * inv_len * s, az * inv_len * s});
}

Q q_from_two_unit_vectors(const V3& from_raw, const V3& to_raw) {
  const V3 from = normalized(from_raw);
  const V3 to = normalized(to_raw);
  const double c = std::clamp(dot(from, to), -1.0, 1.0);

  if (c > 0.999999) {
    return {};
  }

  if (c < -0.999999) {
    V3 axis = cross({1.0, 0.0, 0.0}, from);
    if (norm(axis) < 1e-6) {
      axis = cross({0.0, 1.0, 0.0}, from);
    }
    return q_from_axis_angle(axis.x, axis.y, axis.z, kPi);
  }

  const V3 axis = cross(from, to);
  return normalize({1.0 + c, axis.x, axis.y, axis.z});
}

Q q_from_rotation_vector(const V3& rv) {
  const double angle = norm(rv);
  if (!finite(angle) || angle < 1e-12) return {};
  return q_from_axis_angle(rv.x, rv.y, rv.z, angle);
}

double yaw_from_q_z_up(const Q& q_raw) {
  const Q q = normalize(q_raw);
  return std::atan2(2.0 * (q.w*q.z + q.x*q.y),
                    1.0 - 2.0 * (q.y*q.y + q.z*q.z));
}

Q q_from_yaw_z_up(double yaw) {
  const double half = 0.5 * yaw;
  return normalize({std::cos(half), 0.0, 0.0, std::sin(half)});
}

uint64_t now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

double ns_to_ms(int64_t ns) { return static_cast<double>(ns) / 1.0e6; }
int64_t ms_to_ns(double ms) { return static_cast<int64_t>(ms * 1.0e6); }

struct ControlConfig {
  double gravity_magnitude = 9.80665;
  // 0.04 is too slow for head-mounted 3DoF: roll/pitch can visibly settle
  // after startup. 0.35 still keeps accelerometer correction smooth while
  // preventing the temporary "view drifts down" effect.
  double accel_correction_gain = 0.35;
  double gyro_bias_gain = 0.0005;
  double accel_trust_min = 7.0;
  double accel_trust_max = 12.5;
  double max_dt_ms = 30.0;
  bool initial_accel_align = true;
  bool initial_recenter = true;
  bool recenter_full_orientation = true;
  // Startup warmup prevents a visible first-enable pitch/roll transient.
  // We collect a short acceleration window, initialize gravity alignment from
  // the averaged vector, and only then publish hmd_pose_3dof.
  double startup_warmup_ms = 500.0;
  uint32_t startup_min_samples = 120;
  double startup_gyro_bias_max_rad_s = 0.08;
  // Keep output neutral while the internal gravity alignment finishes settling.
  // Without this, full-orientation recenter can still show a short visible pitch
  // drift after the first 3DoF enable.
  double startup_recenter_settle_ms = 1500.0;
  double startup_recenter_settle_gyro_max_rad_s = 0.12;
  // Yaw cannot be corrected by accelerometer. Reduce yaw drift by learning gyro
  // bias while the headset is effectively still, and suppress tiny residual gyro
  // noise that otherwise integrates into slow horizontal drift.
  double stationary_gyro_bias_gain = 0.75;
  double stationary_gyro_norm_rad_s = 0.06;
  double gyro_deadband_rad_s = 0.008;
  uint64_t reset_counter = 0;
  uint64_t recenter_counter = 0;
};

class ControlFileReloader {
 public:
  explicit ControlFileReloader(std::string path, ControlConfig defaults)
      : path_(std::move(path)), cfg_(defaults) {}

  const ControlConfig& config() const { return cfg_; }

  bool maybe_reload(int64_t now_ns_value, double interval_ms = 200.0) {
    if (path_.empty()) return false;
    if (last_load_ns_ != 0 && now_ns_value - last_load_ns_ < static_cast<int64_t>(interval_ms * 1e6)) {
      return false;
    }
    last_load_ns_ = now_ns_value;

    std::ifstream in(path_);
    if (!in) return false;

    try {
      nlohmann::json j;
      in >> j;
      if (!j.is_object()) return false;

      cfg_.gravity_magnitude = j.value("gravity_magnitude", cfg_.gravity_magnitude);
      cfg_.reset_counter = j.value("reset_counter", cfg_.reset_counter);
      cfg_.accel_correction_gain = j.value("imu_3dof_accel_correction_gain", cfg_.accel_correction_gain);
      cfg_.gyro_bias_gain = j.value("imu_3dof_gyro_bias_gain", cfg_.gyro_bias_gain);
      cfg_.accel_trust_min = j.value("imu_3dof_accel_trust_min", cfg_.accel_trust_min);
      cfg_.accel_trust_max = j.value("imu_3dof_accel_trust_max", cfg_.accel_trust_max);
      cfg_.max_dt_ms = j.value("imu_3dof_max_dt_ms", cfg_.max_dt_ms);
      cfg_.initial_accel_align = j.value("imu_3dof_initial_accel_align", cfg_.initial_accel_align);
      cfg_.initial_recenter = j.value("imu_3dof_initial_recenter", cfg_.initial_recenter);
      cfg_.recenter_full_orientation = j.value("imu_3dof_recenter_full_orientation", cfg_.recenter_full_orientation);
      cfg_.startup_warmup_ms = j.value("imu_3dof_startup_warmup_ms", cfg_.startup_warmup_ms);
      cfg_.startup_min_samples = j.value("imu_3dof_startup_min_samples", cfg_.startup_min_samples);
      cfg_.startup_gyro_bias_max_rad_s = j.value("imu_3dof_startup_gyro_bias_max_rad_s", cfg_.startup_gyro_bias_max_rad_s);
      cfg_.startup_recenter_settle_ms = j.value("imu_3dof_startup_recenter_settle_ms", cfg_.startup_recenter_settle_ms);
      cfg_.startup_recenter_settle_gyro_max_rad_s = j.value("imu_3dof_startup_recenter_settle_gyro_max_rad_s", cfg_.startup_recenter_settle_gyro_max_rad_s);
      cfg_.stationary_gyro_bias_gain = j.value("imu_3dof_stationary_gyro_bias_gain", cfg_.stationary_gyro_bias_gain);
      cfg_.stationary_gyro_norm_rad_s = j.value("imu_3dof_stationary_gyro_norm_rad_s", cfg_.stationary_gyro_norm_rad_s);
      cfg_.gyro_deadband_rad_s = j.value("imu_3dof_gyro_deadband_rad_s", cfg_.gyro_deadband_rad_s);
      cfg_.recenter_counter = j.value("imu_3dof_recenter_counter", cfg_.recenter_counter);

      sanitize();
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[imu_3dof_backend][WARN] failed to reload control file "
                << path_ << ": " << e.what() << "\n";
      return false;
    }
  }

 private:
  void sanitize() {
    if (!finite(cfg_.gravity_magnitude) || cfg_.gravity_magnitude < 1.0) cfg_.gravity_magnitude = 9.80665;
    if (!finite(cfg_.accel_correction_gain)) cfg_.accel_correction_gain = 0.04;
    if (!finite(cfg_.gyro_bias_gain)) cfg_.gyro_bias_gain = 0.0005;
    if (!finite(cfg_.accel_trust_min) || cfg_.accel_trust_min < 0.0) cfg_.accel_trust_min = 7.0;
    if (!finite(cfg_.accel_trust_max) || cfg_.accel_trust_max <= cfg_.accel_trust_min) {
      cfg_.accel_trust_max = std::max(cfg_.accel_trust_min + 0.1, 12.5);
    }
    if (!finite(cfg_.max_dt_ms) || cfg_.max_dt_ms <= 0.0) cfg_.max_dt_ms = 30.0;
    if (!finite(cfg_.startup_warmup_ms) || cfg_.startup_warmup_ms < 0.0) cfg_.startup_warmup_ms = 0.0;
    if (cfg_.startup_min_samples > 10000u) cfg_.startup_min_samples = 10000u;
    if (!finite(cfg_.startup_gyro_bias_max_rad_s) || cfg_.startup_gyro_bias_max_rad_s < 0.0) {
      cfg_.startup_gyro_bias_max_rad_s = 0.08;
    }
    if (!finite(cfg_.startup_recenter_settle_ms) || cfg_.startup_recenter_settle_ms < 0.0) {
      cfg_.startup_recenter_settle_ms = 0.0;
    }
    if (!finite(cfg_.startup_recenter_settle_gyro_max_rad_s) || cfg_.startup_recenter_settle_gyro_max_rad_s < 0.0) {
      cfg_.startup_recenter_settle_gyro_max_rad_s = 0.12;
    }
    if (!finite(cfg_.stationary_gyro_bias_gain) || cfg_.stationary_gyro_bias_gain < 0.0) {
      cfg_.stationary_gyro_bias_gain = 0.0;
    }
    if (!finite(cfg_.stationary_gyro_norm_rad_s) || cfg_.stationary_gyro_norm_rad_s < 0.0) {
      cfg_.stationary_gyro_norm_rad_s = 0.0;
    }
    if (!finite(cfg_.gyro_deadband_rad_s) || cfg_.gyro_deadband_rad_s < 0.0) {
      cfg_.gyro_deadband_rad_s = 0.0;
    }
    cfg_.accel_correction_gain = std::clamp(cfg_.accel_correction_gain, 0.0, 1.0);
    cfg_.gyro_bias_gain = std::clamp(cfg_.gyro_bias_gain, 0.0, 0.1);
    cfg_.stationary_gyro_bias_gain = std::clamp(cfg_.stationary_gyro_bias_gain, 0.0, 10.0);
    cfg_.stationary_gyro_norm_rad_s = std::clamp(cfg_.stationary_gyro_norm_rad_s, 0.0, 1.0);
    cfg_.gyro_deadband_rad_s = std::clamp(cfg_.gyro_deadband_rad_s, 0.0, 0.2);
  }

  std::string path_;
  ControlConfig cfg_;
  int64_t last_load_ns_ = 0;
};

class Ahrs3DofFilter {
 public:
  void reset() {
    q_ = {};
    gyro_bias_ = {};
    recenter_origin_ = {};
    initialized_ = false;
    warmup_started_ = false;
    startup_first_ts_ns_ = 0;
    startup_last_ts_ns_ = 0;
    startup_accel_sum_ = {};
    startup_gyro_sum_ = {};
    startup_trusted_samples_ = 0;
    startup_total_samples_ = 0;
    initialized_ts_ns_ = 0;
    recenter_settle_until_ns_ = 0;
    last_ts_ns_ = 0;
    last_recenter_counter_ = 0;
  }

  bool update(const capture_client::ImuSample& sample,
              const ControlConfig& cfg,
              Q* q_out,
              V3* gyro_out,
              bool* recentered) {
    if (recentered) *recentered = false;

    const int64_t ts = sample.timestamp_ns;
    if (ts <= 0) return false;

    const V3 accel{sample.accel_m_s2[0], sample.accel_m_s2[1], sample.accel_m_s2[2]};
    V3 gyro{sample.gyro_rad_s[0], sample.gyro_rad_s[1], sample.gyro_rad_s[2]};
    const double accel_norm = norm(accel);
    const bool accel_trusted = accel_norm >= cfg.accel_trust_min && accel_norm <= cfg.accel_trust_max;

    if (!initialized_) {
      if (!warmup_started_) {
        warmup_started_ = true;
        startup_first_ts_ns_ = ts;
        startup_last_ts_ns_ = ts;
        startup_accel_sum_ = {};
        startup_gyro_sum_ = {};
        startup_trusted_samples_ = 0;
        startup_total_samples_ = 0;
      }

      startup_last_ts_ns_ = ts;
      ++startup_total_samples_;
      if (accel_trusted) {
        startup_accel_sum_ = startup_accel_sum_ + accel;
        startup_gyro_sum_ = startup_gyro_sum_ + gyro;
        ++startup_trusted_samples_;
      }

      const double warmup_elapsed_ms = ns_to_ms(startup_last_ts_ns_ - startup_first_ts_ns_);
      const bool warmup_time_ready = cfg.startup_warmup_ms <= 0.0 || warmup_elapsed_ms >= cfg.startup_warmup_ms;
      const bool warmup_samples_ready = startup_trusted_samples_ >= cfg.startup_min_samples;
      const bool warmup_disabled = cfg.startup_warmup_ms <= 0.0 && cfg.startup_min_samples == 0;

      if (!warmup_disabled && (!warmup_time_ready || !warmup_samples_ready)) {
        return false;
      }

      // Initialize roll/pitch from an averaged gravity window instead of a
      // single first IMU sample. This avoids the visible first-enable
      // "view drifts down" transient while the AHRS settles.
      if (cfg.initial_accel_align && startup_trusted_samples_ > 0) {
        const double inv_n = 1.0 / static_cast<double>(startup_trusted_samples_);
        const V3 avg_accel = startup_accel_sum_ * inv_n;
        const V3 avg_gyro = startup_gyro_sum_ * inv_n;
        const V3 measured_g_body = normalized(avg_accel);
        q_ = q_from_two_unit_vectors(measured_g_body, {0.0, 0.0, 1.0});

        // Estimate gyro bias only when startup was effectively stationary.
        // Otherwise a head turn during activation would be baked in as bias.
        if (norm(avg_gyro) <= cfg.startup_gyro_bias_max_rad_s) {
          gyro_bias_ = avg_gyro;
        } else {
          gyro_bias_ = {};
        }
      } else if (cfg.initial_accel_align && accel_trusted) {
        const V3 measured_g_body = normalized(accel);
        q_ = q_from_two_unit_vectors(measured_g_body, {0.0, 0.0, 1.0});
      } else {
        q_ = {};
      }

      initialized_ = true;
      initialized_ts_ns_ = ts;
      last_ts_ns_ = ts;
      last_recenter_counter_ = cfg.recenter_counter;

      if (cfg.initial_recenter) {
        set_recenter_origin(cfg.recenter_full_orientation);
        if (cfg.recenter_full_orientation && cfg.startup_recenter_settle_ms > 0.0) {
          recenter_settle_until_ns_ = ts + ms_to_ns(cfg.startup_recenter_settle_ms);
        }
        if (recentered) *recentered = true;
      }

      gyro = gyro - gyro_bias_;
      if (norm(gyro) <= cfg.gyro_deadband_rad_s) {
        gyro = {};
      }
      *q_out = output_orientation();
      *gyro_out = gyro;
      return true;
    }

    double dt = static_cast<double>(ts - last_ts_ns_) / 1.0e9;
    last_ts_ns_ = ts;
    const double max_dt_s = cfg.max_dt_ms / 1000.0;
    if (!finite(dt) || dt <= 0.0) {
      dt = 0.0;
    } else if (dt > max_dt_s) {
      // Large gaps usually mean a backend restart or consumer stall. Clamp rather than
      // integrating a huge gyro step.
      dt = max_dt_s;
    }

    const V3 raw_gyro = gyro;
    V3 motion_gyro = raw_gyro - gyro_bias_;

    // Stationary zero-rate update. Accelerometer cannot correct yaw, so this is
    // the main way to remove slow right/left drift caused by residual gyro bias.
    if (dt > 0.0 && accel_trusted && cfg.stationary_gyro_bias_gain > 0.0 &&
        norm(motion_gyro) <= cfg.stationary_gyro_norm_rad_s) {
      const double alpha = std::clamp(cfg.stationary_gyro_bias_gain * dt, 0.0, 0.05);
      gyro_bias_ = gyro_bias_ * (1.0 - alpha) + raw_gyro * alpha;
      motion_gyro = raw_gyro - gyro_bias_;
    }

    if (norm(motion_gyro) <= cfg.gyro_deadband_rad_s) {
      motion_gyro = {};
    }

    gyro = motion_gyro;
    if (accel_trusted) {
      const V3 measured_g_body = normalized(accel);
      const V3 predicted_g_body = normalized(q_rotate(q_conj(q_), {0.0, 0.0, cfg.gravity_magnitude}));
      const V3 error = cross(predicted_g_body, measured_g_body);

      gyro = gyro + error * cfg.accel_correction_gain;
      gyro_bias_ = gyro_bias_ + error * (cfg.gyro_bias_gain * dt);
    }

    if (dt > 0.0) {
      const Q dq = q_from_rotation_vector(gyro * dt);
      q_ = q_mul(q_, dq);
    }

    maybe_recenter(cfg, ts, recentered);
    maybe_update_startup_recenter_settle(cfg, ts, motion_gyro);

    *q_out = output_orientation();
    *gyro_out = motion_gyro;
    return true;
  }

 private:
  void set_recenter_origin(bool full_orientation) {
    if (full_orientation) {
      // Full orientation recenter: current look direction becomes neutral.
      // This is what users expect from VR "reset seated position/view".
      recenter_origin_ = normalize(q_conj(q_));
    } else {
      // Legacy yaw-only recenter: keep absolute pitch/roll from gravity.
      recenter_origin_ = q_from_yaw_z_up(-yaw_from_q_z_up(q_));
    }
  }

  void maybe_recenter(const ControlConfig& cfg, int64_t ts_ns, bool* recentered) {
    if (cfg.recenter_counter == last_recenter_counter_) return;
    last_recenter_counter_ = cfg.recenter_counter;
    set_recenter_origin(cfg.recenter_full_orientation);
    if (cfg.recenter_full_orientation && cfg.startup_recenter_settle_ms > 0.0) {
      recenter_settle_until_ns_ = ts_ns + ms_to_ns(cfg.startup_recenter_settle_ms);
    }
    if (recentered) *recentered = true;
  }

  void maybe_update_startup_recenter_settle(const ControlConfig& cfg,
                                           int64_t ts_ns,
                                           const V3& motion_gyro) {
    if (!cfg.recenter_full_orientation || recenter_settle_until_ns_ == 0) return;
    if (ts_ns > recenter_settle_until_ns_) {
      recenter_settle_until_ns_ = 0;
      return;
    }
    // While the internal gravity estimate is still converging, keep the user's
    // current startup/recenter view stable. Only do this while the headset is
    // nearly stationary so deliberate head motion is not cancelled.
    if (norm(motion_gyro) <= cfg.startup_recenter_settle_gyro_max_rad_s) {
      set_recenter_origin(true);
    }
  }

  Q output_orientation() const {
    return q_mul(recenter_origin_, q_);
  }

  Q q_{};
  V3 gyro_bias_{};
  Q recenter_origin_{};
  bool initialized_ = false;
  bool warmup_started_ = false;
  int64_t startup_first_ts_ns_ = 0;
  int64_t startup_last_ts_ns_ = 0;
  V3 startup_accel_sum_{};
  V3 startup_gyro_sum_{};
  uint32_t startup_trusted_samples_ = 0;
  uint32_t startup_total_samples_ = 0;
  int64_t initialized_ts_ns_ = 0;
  int64_t recenter_settle_until_ns_ = 0;
  int64_t last_ts_ns_ = 0;
  uint64_t last_recenter_counter_ = 0;
};

class IImuInput {
 public:
  virtual ~IImuInput() = default;
  virtual uint64_t latest_sequence() const = 0;
  virtual bool read_sequence(uint64_t sequence, capture_client::ImuSample& out) const = 0;
  virtual std::string description() const = 0;
};

class ShmImuInput final : public IImuInput {
 public:
  ShmImuInput(const std::string& registry_path, const std::string& imu_stream)
      : registry_(registry_path), reader_(registry_.stream(imu_stream)) {}

  uint64_t latest_sequence() const override { return reader_.latest_sequence(); }
  bool read_sequence(uint64_t sequence, capture_client::ImuSample& out) const override {
    return reader_.read_imu_sequence(sequence, out);
  }
  std::string description() const override {
    return "shm stream=" + reader_.info().stream_id + " shm=" + reader_.info().shm_name;
  }

 private:
  capture_client::CaptureRegistry registry_;
  capture_client::ShmStreamReader reader_;
};

class TcpImuInput final : public IImuInput {
 public:
  TcpImuInput(capture_client::CaptureServiceTcpTransportConfig cfg)
      : transport_(std::move(cfg)) {}

  uint64_t latest_sequence() const override { return transport_.imu().latest_sequence(); }
  bool read_sequence(uint64_t sequence, capture_client::ImuSample& out) const override {
    return transport_.imu().read_imu_sequence(sequence, out);
  }
  std::string description() const override { return "capture_service tcp"; }

 private:
  mutable capture_client::CaptureServiceTcpTransport transport_;
};

struct CliConfig {
  std::string transport = "shm";
  std::string registry = "/tmp/capture_service_streams.json";
  std::string imu_stream = "imu0";
  std::string cam0_stream = "camera0";
  std::string cam1_stream = "camera1";
  std::string tcp_host = "127.0.0.1";
  int tcp_port = 45660;
  double tcp_first_packet_timeout_s = 3.0;

  std::string pose_registry = "/tmp/tracking_streams.json";
  std::string pose_stream = "hmd_pose_3dof";
  std::string pose_shm_name = "track_hmd_pose_3dof";
  std::string pose_frame = "tracking_world";
  uint32_t pose_slots = 1024;

  std::string control_file = "/tmp/xr_backend_control.json";
  ControlConfig control_defaults;

  double duration_s = 0.0;
  int print_every = 250;
  uint64_t max_catchup_samples = 4096;
};

std::unique_ptr<IImuInput> make_imu_input(const CliConfig& cfg) {
  if (cfg.transport == "shm") {
    return std::make_unique<ShmImuInput>(cfg.registry, cfg.imu_stream);
  }
  if (cfg.transport == "tcp" || cfg.transport == "capture_tcp") {
    capture_client::CaptureServiceTcpTransportConfig tcp;
    tcp.host = cfg.tcp_host;
    tcp.port = cfg.tcp_port;
    tcp.cam0_stream = cfg.cam0_stream;
    tcp.cam1_stream = cfg.cam1_stream;
    tcp.imu_stream = cfg.imu_stream;
    tcp.subscribe_imu = true;
    tcp.first_packet_timeout_s = cfg.tcp_first_packet_timeout_s;
    return std::make_unique<TcpImuInput>(std::move(tcp));
  }
  throw std::runtime_error("unsupported --transport: " + cfg.transport + "; expected shm or tcp");
}

xr_runtime::HmdPoseF64V1 make_pose(const capture_client::ImuSample& imu,
                                   const Q& q,
                                   const V3& gyro,
                                   uint64_t reset_counter) {
  xr_runtime::HmdPoseF64V1 out{};
  out.version = 1;
  out.size_bytes = sizeof(xr_runtime::HmdPoseF64V1);
  out.sequence = imu.sequence;
  out.timestamp_ns = static_cast<uint64_t>(imu.timestamp_ns);
  out.source_timestamp_ns = static_cast<uint64_t>(imu.timestamp_ns);
  out.reset_counter = reset_counter;

  out.px = 0.0;
  out.py = 0.0;
  out.pz = 0.0;
  out.qw = q.w;
  out.qx = q.x;
  out.qy = q.y;
  out.qz = q.z;

  out.vx = 0.0;
  out.vy = 0.0;
  out.vz = 0.0;
  out.wx = gyro.x;
  out.wy = gyro.y;
  out.wz = gyro.z;

  out.tracking_status = 2;  // tracking
  out.flags = xr_runtime::HMD_FLAG_POSE_VALID | xr_runtime::HMD_FLAG_ANGULAR_VELOCITY_VALID;
  out.confidence = 1.0f;
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  CliConfig cfg;

  CLI::App app{"Device-neutral IMU 3DoF HMD pose backend"};
  app.add_option("--transport", cfg.transport, "capture_service input transport: shm or tcp/capture_tcp");
  app.add_option("--registry", cfg.registry, "capture_service SHM registry path");
  app.add_option("--imu-stream", cfg.imu_stream, "capture_service IMU stream id");
  app.add_option("--cam0-stream", cfg.cam0_stream, "capture_service camera0 stream id for TCP transport handshake");
  app.add_option("--cam1-stream", cfg.cam1_stream, "capture_service camera1 stream id for TCP transport handshake");
  app.add_option("--tcp-host", cfg.tcp_host, "capture_service TCP host");
  app.add_option("--tcp-port", cfg.tcp_port, "capture_service TCP port");
  app.add_option("--tcp-first-packet-timeout-s", cfg.tcp_first_packet_timeout_s, "capture_service TCP first-packet timeout");

  app.add_option("--pose-registry", cfg.pose_registry, "output tracking registry path");
  app.add_option("--pose-stream", cfg.pose_stream, "output HMD pose stream id");
  app.add_option("--pose-shm-name", cfg.pose_shm_name, "output POSIX SHM name");
  app.add_option("--pose-frame", cfg.pose_frame, "output frame id before runtime transforms");
  app.add_option("--pose-slots", cfg.pose_slots, "output ring slot count");

  app.add_option("--control-file", cfg.control_file, "runtime control JSON file with reloadable 3DoF parameters");
  app.add_option("--gravity-magnitude", cfg.control_defaults.gravity_magnitude, "fallback gravity magnitude in m/s^2");
  app.add_option("--accel-correction-gain", cfg.control_defaults.accel_correction_gain, "accelerometer roll/pitch correction gain");
  app.add_option("--gyro-bias-gain", cfg.control_defaults.gyro_bias_gain, "slow gyro bias correction gain");
  app.add_option("--accel-trust-min", cfg.control_defaults.accel_trust_min, "minimum trusted acceleration magnitude");
  app.add_option("--accel-trust-max", cfg.control_defaults.accel_trust_max, "maximum trusted acceleration magnitude");
  app.add_option("--max-dt-ms", cfg.control_defaults.max_dt_ms, "maximum integrated IMU dt after stalls");
  app.add_option("--initial-accel-align", cfg.control_defaults.initial_accel_align, "initialize roll/pitch from accelerometer gravity");
  app.add_option("--initial-recenter", cfg.control_defaults.initial_recenter, "make current startup view direction neutral");
  app.add_option("--recenter-full-orientation", cfg.control_defaults.recenter_full_orientation, "recenter full look direction instead of yaw only");
  app.add_option("--startup-warmup-ms", cfg.control_defaults.startup_warmup_ms, "startup IMU averaging window before publishing first pose");
  app.add_option("--startup-min-samples", cfg.control_defaults.startup_min_samples, "minimum trusted startup IMU samples before first pose");
  app.add_option("--startup-gyro-bias-max-rad-s", cfg.control_defaults.startup_gyro_bias_max_rad_s, "maximum avg startup gyro norm used as bias");
  app.add_option("--startup-recenter-settle-ms", cfg.control_defaults.startup_recenter_settle_ms, "keep full recenter output stable for this many ms after startup/recenter while stationary");
  app.add_option("--startup-recenter-settle-gyro-max-rad-s", cfg.control_defaults.startup_recenter_settle_gyro_max_rad_s, "max gyro norm for startup/recenter settle-origin refresh");
  app.add_option("--stationary-gyro-bias-gain", cfg.control_defaults.stationary_gyro_bias_gain, "stationary zero-rate gyro bias learning gain");
  app.add_option("--stationary-gyro-norm-rad-s", cfg.control_defaults.stationary_gyro_norm_rad_s, "gyro norm below which headset is considered stationary for bias learning");
  app.add_option("--gyro-deadband-rad-s", cfg.control_defaults.gyro_deadband_rad_s, "zero small residual gyro after bias correction to reduce yaw drift");
  app.add_option("--duration", cfg.duration_s, "run duration in seconds; 0 means until Ctrl+C");
  app.add_option("--print-every", cfg.print_every, "print every N published poses; <=0 disables");
  app.add_option("--max-catchup-samples", cfg.max_catchup_samples, "maximum IMU samples to process after lag before jumping to latest");

  CLI11_PARSE(app, argc, argv);

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  try {
    auto input = make_imu_input(cfg);
    std::cout << "[imu_3dof_backend] input attached: " << input->description() << "\n";

    xr_runtime::HmdPoseShmPublisherConfig pub_cfg;
    pub_cfg.registry_path = cfg.pose_registry;
    pub_cfg.stream_id = cfg.pose_stream;
    pub_cfg.shm_name = cfg.pose_shm_name;
    pub_cfg.frame_id = cfg.pose_frame;
    pub_cfg.slot_count = cfg.pose_slots;
    pub_cfg.created_by = "imu_3dof_backend";
    xr_runtime::HmdPoseShmPublisher publisher(std::move(pub_cfg));

    std::cout << "[imu_3dof_backend] publishing " << cfg.pose_stream
              << " -> " << cfg.pose_shm_name
              << " registry=" << cfg.pose_registry
              << " frame=" << cfg.pose_frame << "\n";
    std::cout << "[imu_3dof_backend] control_file=" << cfg.control_file << "\n";

    ControlFileReloader control(cfg.control_file, cfg.control_defaults);
    Ahrs3DofFilter filter;

    uint64_t last_seq = input->latest_sequence();
    uint64_t published = 0;
    uint64_t recenter_events = 0;
    const auto start = std::chrono::steady_clock::now();

    while (!g_stop) {
      const auto now_tp = std::chrono::steady_clock::now();
      if (cfg.duration_s > 0.0 && std::chrono::duration<double>(now_tp - start).count() >= cfg.duration_s) {
        break;
      }

      const uint64_t latest = input->latest_sequence();
      if (latest <= last_seq) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      if (latest - last_seq > cfg.max_catchup_samples) {
        last_seq = latest - cfg.max_catchup_samples;
      }

      while (last_seq < latest && !g_stop) {
        capture_client::ImuSample imu;
        const uint64_t seq = last_seq + 1;
        last_seq = seq;
        if (!input->read_sequence(seq, imu)) {
          continue;
        }

        const int64_t tnow = static_cast<int64_t>(now_ns());
        control.maybe_reload(tnow);
        const auto& cc = control.config();

        Q q;
        V3 gyro;
        bool recentered = false;
        if (!filter.update(imu, cc, &q, &gyro, &recentered)) {
          continue;
        }
        if (recentered) {
          ++recenter_events;
          std::cout << "[imu_3dof_backend] yaw recentered counter=" << cc.recenter_counter << "\n";
        }

        const uint64_t reset_counter = cc.reset_counter + cc.recenter_counter;
        publisher.publish(make_pose(imu, q, gyro, reset_counter));
        ++published;

        if (cfg.print_every > 0 && published % static_cast<uint64_t>(cfg.print_every) == 0) {
          const double elapsed = std::max(1e-9, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
          const double age_ms = ns_to_ms(static_cast<int64_t>(now_ns()) - imu.timestamp_ns);
          std::cout << "[imu_3dof_backend] poses=" << published
                    << " rate=" << (static_cast<double>(published) / elapsed)
                    << " latest_imu_seq=" << imu.sequence
                    << " age_ms=" << age_ms
                    << " q=(" << q.w << "," << q.x << "," << q.y << "," << q.z << ")"
                    << " reset_counter=" << reset_counter
                    << " recenter_events=" << recenter_events
                    << "\n";
        }
      }
    }

    std::cout << "[imu_3dof_backend] done poses=" << published
              << " recenter_events=" << recenter_events << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[imu_3dof_backend][ERROR] " << e.what() << "\n";
    return 1;
  }
}
