#include "hmd_pose_stability_filter.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace xr_runtime_adapter::hmd_filter {
namespace {

constexpr double kQuatNormMin = 1e-12;
constexpr double kNsPerMs = 1000000.0;

bool finite3(double x, double y, double z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool finite4(double w, double x, double y, double z) {
  return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

double ns_to_ms_local(int64_t ns) {
  return static_cast<double>(ns) / kNsPerMs;
}

int64_t ms_to_ns_local(double ms) {
  if (!std::isfinite(ms) || ms <= 0.0) return 0;
  return static_cast<int64_t>(ms * kNsPerMs);
}

double distance_m(const xr_runtime::HmdPoseF64V1& a,
                  const xr_runtime::HmdPoseF64V1& b) {
  const double dx = a.px - b.px;
  const double dy = a.py - b.py;
  const double dz = a.pz - b.pz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void zero_velocities(xr_runtime::HmdPoseF64V1& hmd) {
  hmd.vx = 0.0;
  hmd.vy = 0.0;
  hmd.vz = 0.0;
  hmd.wx = 0.0;
  hmd.wy = 0.0;
  hmd.wz = 0.0;
  hmd.flags &= ~xr_runtime::HMD_FLAG_LINEAR_VELOCITY_VALID;
  hmd.flags &= ~xr_runtime::HMD_FLAG_ANGULAR_VELOCITY_VALID;
}

}  // namespace

void HmdPoseStabilityFilter::configure(HmdPoseStabilityFilterConfig cfg) {
  if (!std::isfinite(cfg.window_ms) || cfg.window_ms <= 0.0) {
    cfg.window_ms = 250.0;
  }
  if (!std::isfinite(cfg.max_distance_m) || cfg.max_distance_m <= 0.0) {
    cfg.max_distance_m = 0.50;
  }
  if (!std::isfinite(cfg.log_interval_ms) || cfg.log_interval_ms < 0.0) {
    cfg.log_interval_ms = 250.0;
  }
  cfg_ = cfg;
  reset();
}

void HmdPoseStabilityFilter::reset() {
  has_last_good_ = false;
  last_good_ = {};
  last_good_receive_ns_ = 0;
  last_reset_counter_ = 0;
  unstable_since_ns_ = 0;
  last_unstable_log_ns_ = 0;
  rejected_count_ = 0;
  startup_pose_count_ = 0;
  held_last_good_count_ = 0;
}

bool HmdPoseStabilityFilter::is_pose_finite_and_normal(const xr_runtime::HmdPoseF64V1& hmd) {
  if (hmd.sequence == 0) return false;
  if (!finite3(hmd.px, hmd.py, hmd.pz)) return false;
  if (!finite4(hmd.qw, hmd.qx, hmd.qy, hmd.qz)) return false;

  const double q2 = hmd.qw * hmd.qw + hmd.qx * hmd.qx +
                    hmd.qy * hmd.qy + hmd.qz * hmd.qz;
  if (!std::isfinite(q2) || q2 <= kQuatNormMin) return false;

  // Reject obviously broken absolute positions even on the first sample.
  // Normal XR local/world tracking should not start kilometers away.
  const double p2 = hmd.px * hmd.px + hmd.py * hmd.py + hmd.pz * hmd.pz;
  if (!std::isfinite(p2) || p2 > 1000000.0) return false;  // > 1000 m radius.

  return true;
}

xr_runtime::HmdPoseF64V1 HmdPoseStabilityFilter::make_startup_pose(
    const xr_runtime::HmdPoseF64V1& input,
    int64_t now_ns) {
  xr_runtime::HmdPoseF64V1 out{};
  out.version = input.version != 0 ? input.version : 1;
  out.size_bytes = input.size_bytes != 0 ? input.size_bytes : sizeof(xr_runtime::HmdPoseF64V1);
  out.sequence = input.sequence != 0 ? input.sequence : 1;
  out.timestamp_ns = static_cast<uint64_t>(now_ns);
  out.source_timestamp_ns = input.source_timestamp_ns != 0
      ? input.source_timestamp_ns
      : static_cast<uint64_t>(now_ns);
  out.reset_counter = input.reset_counter;
  out.px = 0.0;
  out.py = 0.0;
  out.pz = 0.0;
  out.qw = 1.0;
  out.qx = 0.0;
  out.qy = 0.0;
  out.qz = 0.0;
  out.tracking_status = 2;  // tracking: SteamVR/OpenVR initialization expects a valid pose.
  out.flags = xr_runtime::HMD_FLAG_POSE_VALID;
  out.confidence = 1.0f;
  out.reserved0 = 0;
  zero_velocities(out);
  return out;
}

xr_runtime::HmdPoseF64V1 HmdPoseStabilityFilter::make_substituted_pose(
    xr_runtime::HmdPoseF64V1 pose,
    const xr_runtime::HmdPoseF64V1& input,
    int64_t now_ns) {
  pose.sequence = input.sequence != 0 ? input.sequence : pose.sequence;
  pose.timestamp_ns = static_cast<uint64_t>(now_ns);
  pose.source_timestamp_ns = input.source_timestamp_ns != 0
      ? input.source_timestamp_ns
      : static_cast<uint64_t>(now_ns);
  pose.reset_counter = input.reset_counter;
  pose.tracking_status = 2;  // tracking: keep runtime initialized while rejecting garbage.
  pose.flags |= xr_runtime::HMD_FLAG_POSE_VALID;
  zero_velocities(pose);
  return pose;
}

double HmdPoseStabilityFilter::current_unstable_ms(int64_t now_ns) const {
  if (unstable_since_ns_ <= 0 || now_ns <= unstable_since_ns_) return 0.0;
  return ns_to_ms_local(now_ns - unstable_since_ns_);
}

bool HmdPoseStabilityFilter::should_log_unstable(int64_t now_ns) const {
  if (last_unstable_log_ns_ == 0) return true;
  const int64_t interval_ns = ms_to_ns_local(cfg_.log_interval_ms);
  return interval_ns <= 0 || now_ns - last_unstable_log_ns_ >= interval_ns;
}

void HmdPoseStabilityFilter::maybe_log_unstable(
    const HmdPoseStabilityFilterDecision& decision,
    const xr_runtime::HmdPoseF64V1& input,
    int64_t now_ns) {
  if (!should_log_unstable(now_ns)) return;
  last_unstable_log_ns_ = now_ns;

  std::cout << "[hmd_pose_stability_filter] unstable_ms=" << decision.unstable_ms
            << " reason=" << decision.reason
            << " action=" << (decision.used_last_good_pose ? "hold_last_good" :
                                (decision.used_startup_pose ? "startup_pose" : "substitute"))
            << " input_seq=" << input.sequence
            << " dt_ms=" << decision.dt_ms
            << " step_cm=" << (decision.step_distance_m * 100.0)
            << " allowed_cm=" << (decision.allowed_distance_m * 100.0)
            << " rejected_count=" << rejected_count_
            << "\n";
}

xr_runtime::HmdPoseF64V1 HmdPoseStabilityFilter::filter(
    const xr_runtime::HmdPoseF64V1& input,
    int64_t receive_timestamp_ns,
    HmdPoseStabilityFilterDecision* decision_out) {
  HmdPoseStabilityFilterDecision decision{};
  decision.first_good_pose_seen = has_last_good_;

  if (!cfg_.enabled) {
    if (decision_out) *decision_out = decision;
    return input;
  }

  const int64_t now_ns = receive_timestamp_ns > 0 ? receive_timestamp_ns : xr_runtime::now_ns();

  if (has_last_good_ && input.reset_counter != last_reset_counter_) {
    has_last_good_ = false;
    last_good_ = {};
    last_good_receive_ns_ = 0;
    last_reset_counter_ = input.reset_counter;
    unstable_since_ns_ = 0;
    last_unstable_log_ns_ = 0;
  }

  const bool normal_pose = is_pose_finite_and_normal(input);
  if (!normal_pose) {
    decision.input_stable = false;
    decision.output_substituted = true;
    decision.reason = "invalid_pose";

    if (unstable_since_ns_ == 0) unstable_since_ns_ = now_ns;
    decision.unstable_ms = current_unstable_ms(now_ns);
    ++rejected_count_;

    xr_runtime::HmdPoseF64V1 out{};
    if (has_last_good_) {
      out = make_substituted_pose(last_good_, input, now_ns);
      decision.used_last_good_pose = true;
      ++held_last_good_count_;
    } else {
      out = make_startup_pose(input, now_ns);
      decision.used_startup_pose = true;
      ++startup_pose_count_;
    }
    maybe_log_unstable(decision, input, now_ns);
    if (decision_out) *decision_out = decision;
    return out;
  }

  if (!has_last_good_) {
    last_good_ = input;
    last_good_receive_ns_ = now_ns;
    last_reset_counter_ = input.reset_counter;
    unstable_since_ns_ = 0;
    last_unstable_log_ns_ = 0;
    decision.first_good_pose_seen = true;
    if (decision_out) *decision_out = decision;
    return input;
  }

  int64_t dt_ns = now_ns - last_good_receive_ns_;
  if (dt_ns <= 0 && input.timestamp_ns > last_good_.timestamp_ns) {
    dt_ns = static_cast<int64_t>(input.timestamp_ns - last_good_.timestamp_ns);
  }
  if (dt_ns <= 0) {
    dt_ns = ms_to_ns_local(cfg_.window_ms);
  }

  const double dt_ms = ns_to_ms_local(dt_ns);
  const double distance = distance_m(last_good_, input);
  const double allowed = cfg_.max_distance_m * std::max(dt_ms / cfg_.window_ms, 1e-6);

  decision.dt_ms = dt_ms;
  decision.step_distance_m = distance;
  decision.allowed_distance_m = allowed;

  if (std::isfinite(distance) && std::isfinite(allowed) && distance <= allowed) {
    last_good_ = input;
    last_good_receive_ns_ = now_ns;
    last_reset_counter_ = input.reset_counter;
    unstable_since_ns_ = 0;
    last_unstable_log_ns_ = 0;
    if (decision_out) *decision_out = decision;
    return input;
  }

  decision.input_stable = false;
  decision.output_substituted = true;
  decision.used_last_good_pose = true;
  decision.reason = "speed_jump";
  if (unstable_since_ns_ == 0) unstable_since_ns_ = now_ns;
  decision.unstable_ms = current_unstable_ms(now_ns);
  ++rejected_count_;
  ++held_last_good_count_;

  xr_runtime::HmdPoseF64V1 out = make_substituted_pose(last_good_, input, now_ns);
  maybe_log_unstable(decision, input, now_ns);
  if (decision_out) *decision_out = decision;
  return out;
}

std::string HmdPoseStabilityFilter::summary_string() const {
  std::ostringstream os;
  os << "hmd_pose_stability_filter_enabled: " << (cfg_.enabled ? "true" : "false") << "\n";
  if (cfg_.enabled) {
    os << "hmd_pose_stability_window_ms: " << cfg_.window_ms << "\n";
    os << "hmd_pose_stability_max_distance_cm: " << (cfg_.max_distance_m * 100.0) << "\n";
    os << "hmd_pose_stability_rejected: " << rejected_count_ << "\n";
    os << "hmd_pose_stability_held_last_good: " << held_last_good_count_ << "\n";
    os << "hmd_pose_stability_startup_pose: " << startup_pose_count_ << "\n";
  }
  return os.str();
}

}  // namespace xr_runtime_adapter::hmd_filter
