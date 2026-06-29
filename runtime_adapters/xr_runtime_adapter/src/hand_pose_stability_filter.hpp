#pragma once

// Runtime-side hand pose stability gate.
// Moved from mercury_hand_tracking so Mercury publishes raw hand tracking and
// xr_runtime_adapter owns runtime smoothing/reacquire policy.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <xr_runtime/contracts/runtime_adapter.hpp>

namespace xr_runtime_adapter::hand_filter {

constexpr uint32_t kHandStatusNoHands = 0u;
constexpr uint32_t kHandStatusTracking = 1u;
constexpr uint32_t kHandStatusDegraded = 2u;

struct HandPoseStabilityFilterConfig {
  bool enabled = false;
  double max_reacquire_jump_m = 0.10;
  uint32_t confirm_frames = 3;
  double confirm_max_step_m = 0.04;
  double hold_lost_ms = 200.0;

  // Reject implausibly fast continuous tracking updates.  This catches the
  // common Mercury loss mode where the hand remains marked as tracking for a
  // few frames but its controller pose starts sliding/falling away before it
  // finally disappears.  Set <= 0 to disable.
  double max_continuity_velocity_mps = 1.25;

  // V2-only runtime smoothing. The gate still keeps the raw Mercury output safe:
  // hold-lost plus prediction covers brief tracking loss, and confirmed reacquire blending
  // avoids hard controller snaps after far-jump confirmation.
  double predict_lost_ms = 0.0;
  double max_prediction_velocity_mps = 2.0;
  double prediction_damping = 0.5;
  double reacquire_blend_ms = 0.0;

  std::string debug_csv;
};

struct HandPoseStabilityFilterStats {
  uint64_t rows = 0;
  uint64_t left_orig_active = 0;
  uint64_t left_gated_active = 0;
  uint64_t left_held = 0;
  uint64_t left_jump_rejected = 0;
  uint64_t left_velocity_rejected = 0;
  uint64_t left_confirmed = 0;
  uint64_t left_predicted = 0;
  uint64_t left_blended = 0;
  uint64_t right_orig_active = 0;
  uint64_t right_gated_active = 0;
  uint64_t right_held = 0;
  uint64_t right_jump_rejected = 0;
  uint64_t right_velocity_rejected = 0;
  uint64_t right_confirmed = 0;
  uint64_t right_predicted = 0;
  uint64_t right_blended = 0;
};

class HandPoseStabilityFilter {
 public:
  HandPoseStabilityFilter() = default;

  explicit HandPoseStabilityFilter(HandPoseStabilityFilterConfig cfg) {
    configure(std::move(cfg));
  }

  void configure(HandPoseStabilityFilterConfig cfg) {
    cfg_ = std::move(cfg);
    reset();

    if (cfg_.enabled && !cfg_.debug_csv.empty()) {
      namespace fs = std::filesystem;
      const fs::path p(cfg_.debug_csv);
      if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
      debug_.open(p, std::ios::out | std::ios::trunc);
      if (!debug_) {
        throw std::runtime_error("failed to open hand stability gate debug csv: " + cfg_.debug_csv);
      }
      debug_ << "sequence,source_timestamp_ns,hand,orig_active,gated_active,mode,jump_m,velocity_mps,"
             << "pending_count,candidate_x,candidate_y,candidate_z,output_x,output_y,output_z\n";
    }
  }

  void reset() {
    left_ = HandState{};
    right_ = HandState{};
    left_v2_ = HandStateV2{};
    right_v2_ = HandStateV2{};
    stats_ = HandPoseStabilityFilterStats{};
  }

  [[nodiscard]] const HandPoseStabilityFilterConfig& config() const { return cfg_; }
  [[nodiscard]] const HandPoseStabilityFilterStats& stats() const { return stats_; }

  xr_runtime::HandTrackingFrameF64V1 filter(xr_runtime::HandTrackingFrameF64V1 frame) {
    if (!cfg_.enabled) return frame;

    ++stats_.rows;

    const auto left = filter_side(frame.left, left_, frame.source_timestamp_ns, "left");
    const auto right = filter_side(frame.right, right_, frame.source_timestamp_ns, "right");

    frame.left = left.side;
    frame.right = right.side;

    recompute_frame_status(frame);
    write_debug(frame.sequence, frame.source_timestamp_ns, "left", left);
    write_debug(frame.sequence, frame.source_timestamp_ns, "right", right);

    return frame;
  }

  xr_runtime::HandTrackingFrameF32V2 filter(xr_runtime::HandTrackingFrameF32V2 frame) {
    if (!cfg_.enabled) return frame;

    ++stats_.rows;

    const auto left = filter_side_v2(frame.left, left_v2_, frame.source_timestamp_ns, "left");
    const auto right = filter_side_v2(frame.right, right_v2_, frame.source_timestamp_ns, "right");

    frame.left = left.side;
    frame.right = right.side;

    recompute_frame_status_v2(frame);
    write_debug(frame.sequence, frame.source_timestamp_ns, "left", left);
    write_debug(frame.sequence, frame.source_timestamp_ns, "right", right);

    return frame;
  }

  std::string summary_string() const {
    std::ostringstream os;
    os << "hand_stability_gate: " << (cfg_.enabled ? "enabled" : "disabled") << "\n";
    if (!cfg_.enabled) return os.str();
    os << "hand_gate_max_jump_m: " << cfg_.max_reacquire_jump_m << "\n";
    os << "hand_gate_confirm_frames: " << cfg_.confirm_frames << "\n";
    os << "hand_gate_confirm_max_step_m: " << cfg_.confirm_max_step_m << "\n";
    os << "hand_gate_hold_lost_ms: " << cfg_.hold_lost_ms << "\n";
    os << "hand_gate_max_continuity_velocity_mps: " << cfg_.max_continuity_velocity_mps << "\n";
    os << "hand_gate_predict_lost_ms: " << cfg_.predict_lost_ms << "\n";
    os << "hand_gate_max_prediction_velocity_mps: " << cfg_.max_prediction_velocity_mps << "\n";
    os << "hand_gate_prediction_damping: " << cfg_.prediction_damping << "\n";
    os << "hand_gate_reacquire_blend_ms: " << cfg_.reacquire_blend_ms << "\n";
    os << "hand_gate_rows: " << stats_.rows << "\n";
    os << "hand_gate_left_orig_active: " << stats_.left_orig_active << "\n";
    os << "hand_gate_left_gated_active: " << stats_.left_gated_active << "\n";
    os << "hand_gate_left_held: " << stats_.left_held << "\n";
    os << "hand_gate_left_jump_rejected: " << stats_.left_jump_rejected << "\n";
    os << "hand_gate_left_velocity_rejected: " << stats_.left_velocity_rejected << "\n";
    os << "hand_gate_left_confirmed: " << stats_.left_confirmed << "\n";
    os << "hand_gate_left_predicted: " << stats_.left_predicted << "\n";
    os << "hand_gate_left_blended: " << stats_.left_blended << "\n";
    os << "hand_gate_right_orig_active: " << stats_.right_orig_active << "\n";
    os << "hand_gate_right_gated_active: " << stats_.right_gated_active << "\n";
    os << "hand_gate_right_held: " << stats_.right_held << "\n";
    os << "hand_gate_right_jump_rejected: " << stats_.right_jump_rejected << "\n";
    os << "hand_gate_right_velocity_rejected: " << stats_.right_velocity_rejected << "\n";
    os << "hand_gate_right_confirmed: " << stats_.right_confirmed << "\n";
    os << "hand_gate_right_predicted: " << stats_.right_predicted << "\n";
    os << "hand_gate_right_blended: " << stats_.right_blended << "\n";
    if (!cfg_.debug_csv.empty()) os << "hand_gate_debug_csv: " << cfg_.debug_csv << "\n";
    return os.str();
  }

 private:
  struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  struct HandState {
    bool has_last_good = false;
    xr_runtime::HandSideF64V1 last_good{};
    uint64_t last_good_source_ts = 0;

    bool has_pending = false;
    Vec3 pending_palm{};
    uint32_t pending_count = 0;
  };

  struct HandStateV2 {
    bool has_last_good = false;
    xr_runtime::HandSideF32V2 last_good{};
    uint64_t last_good_source_ts = 0;

    bool has_pending = false;
    Vec3 pending_controller{};
    uint32_t pending_count = 0;

    bool has_velocity = false;
    Vec3 velocity_mps{};

    bool blend_active = false;
    xr_runtime::HandSideF32V2 blend_from{};
    uint64_t blend_start_ts = 0;
  };

  struct SideDecision {
    xr_runtime::HandSideF64V1 side{};
    bool orig_active = false;
    bool gated_active = false;
    std::string mode = "inactive";
    double jump_m = std::numeric_limits<double>::quiet_NaN();
    double velocity_mps = std::numeric_limits<double>::quiet_NaN();
    uint32_t pending_count = 0;
    bool has_candidate = false;
    Vec3 candidate{};
    bool has_output = false;
    Vec3 output{};
  };

  struct SideDecisionV2 {
    xr_runtime::HandSideF32V2 side{};
    bool orig_active = false;
    bool gated_active = false;
    std::string mode = "inactive";
    double jump_m = std::numeric_limits<double>::quiet_NaN();
    double velocity_mps = std::numeric_limits<double>::quiet_NaN();
    uint32_t pending_count = 0;
    bool has_candidate = false;
    Vec3 candidate{};
    bool has_output = false;
    Vec3 output{};
  };

  static bool finite_pose(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  }

  static bool nonzero_pose(const Vec3& v) {
    return std::fabs(v.x) > 1e-12 || std::fabs(v.y) > 1e-12 || std::fabs(v.z) > 1e-12;
  }

  static Vec3 palm(const xr_runtime::HandSideF64V1& s) {
    return {s.palm_px, s.palm_py, s.palm_pz};
  }

  static Vec3 controller_v2(const xr_runtime::HandSideF32V2& s) {
    return {s.controller_px, s.controller_py, s.controller_pz};
  }

  static double distance_m(const Vec3& a, const Vec3& b) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  static double velocity_mps_between(const Vec3& from,
                                     const Vec3& to,
                                     uint64_t from_ts,
                                     uint64_t to_ts) {
    if (to_ts <= from_ts) return 0.0;
    const double dt_s = static_cast<double>(to_ts - from_ts) / 1e9;
    if (!std::isfinite(dt_s) || dt_s <= 1e-6) return 0.0;
    return distance_m(from, to) / dt_s;
  }

  static bool continuity_velocity_ok(double velocity_mps,
                                     const HandPoseStabilityFilterConfig& cfg) {
    const double max_v = cfg.max_continuity_velocity_mps;
    if (max_v <= 0.0) return true;
    if (!std::isfinite(velocity_mps)) return false;
    return velocity_mps <= max_v;
  }

  static Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
  static Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  static Vec3 scale(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
  static Vec3 lerp_vec(const Vec3& a, const Vec3& b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
  }

  static Vec3 clamp_velocity(const Vec3& v, const HandPoseStabilityFilterConfig& cfg) {
    const double max_v = std::max(0.0, cfg.max_prediction_velocity_mps);
    if (max_v <= 0.0) return {0.0, 0.0, 0.0};
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!std::isfinite(n) || n <= 1e-12) return {0.0, 0.0, 0.0};
    if (n <= max_v) return v;
    return scale(v, max_v / n);
  }

  static bool is_tracking_status(const xr_runtime::HandSideF64V1& s) {
    return s.status == kHandStatusTracking;
  }

  static bool side_pose_is_active(const xr_runtime::HandSideF64V1& s) {
    const Vec3 p = palm(s);
    return is_tracking_status(s) && finite_pose(p) && nonzero_pose(p);
  }

  static bool is_tracking_status_v2(const xr_runtime::HandSideF32V2& s) {
    return s.status == kHandStatusTracking;
  }

  static bool side_pose_is_active_v2(const xr_runtime::HandSideF32V2& s) {
    const Vec3 p = controller_v2(s);
    return is_tracking_status_v2(s) && finite_pose(p) && nonzero_pose(p);
  }

  static uint64_t hold_lost_ns(const HandPoseStabilityFilterConfig& cfg) {
    return static_cast<uint64_t>(std::max(0.0, cfg.hold_lost_ms) * 1e6);
  }

  static uint64_t predict_lost_ns(const HandPoseStabilityFilterConfig& cfg) {
    return static_cast<uint64_t>(std::max(0.0, cfg.predict_lost_ms) * 1e6);
  }

  // V2 lost-hand output is intentionally split into two phases:
  //   1) hold_lost_ms: keep the last accepted pose stable;
  //   2) predict_lost_ms: extrapolate from the last accepted velocity.
  // This makes the total graceful-loss window hold_lost_ms + predict_lost_ms.
  static uint64_t lost_output_ns_v2(const HandPoseStabilityFilterConfig& cfg) {
    const uint64_t hold_ns = hold_lost_ns(cfg);
    const uint64_t predict_ns = predict_lost_ns(cfg);
    if (std::numeric_limits<uint64_t>::max() - hold_ns < predict_ns) {
      return std::numeric_limits<uint64_t>::max();
    }
    return hold_ns + predict_ns;
  }

  static bool elapsed_since_last_good_v2(const HandStateV2& state,
                                         uint64_t source_ts,
                                         uint64_t& elapsed_ns) {
    if (!state.has_last_good) return false;
    if (source_ts < state.last_good_source_ts) return false;
    elapsed_ns = source_ts - state.last_good_source_ts;
    return true;
  }

  static bool within_lost_output_window_v2(const HandStateV2& state,
                                           uint64_t source_ts,
                                           const HandPoseStabilityFilterConfig& cfg) {
    uint64_t elapsed_ns = 0;
    if (!elapsed_since_last_good_v2(state, source_ts, elapsed_ns)) return false;
    return elapsed_ns <= lost_output_ns_v2(cfg);
  }

  static bool prediction_elapsed_v2(const HandStateV2& state,
                                    uint64_t source_ts,
                                    const HandPoseStabilityFilterConfig& cfg,
                                    uint64_t& prediction_elapsed_ns) {
    uint64_t elapsed_ns = 0;
    if (!elapsed_since_last_good_v2(state, source_ts, elapsed_ns)) return false;
    const uint64_t hold_ns = hold_lost_ns(cfg);
    const uint64_t predict_ns = predict_lost_ns(cfg);
    if (predict_ns == 0 || elapsed_ns <= hold_ns) return false;
    const uint64_t after_hold_ns = elapsed_ns - hold_ns;
    if (after_hold_ns > predict_ns) return false;
    prediction_elapsed_ns = after_hold_ns;
    return true;
  }

  static uint64_t reacquire_blend_ns(const HandPoseStabilityFilterConfig& cfg) {
    return static_cast<uint64_t>(std::max(0.0, cfg.reacquire_blend_ms) * 1e6);
  }

  static bool within_hold_window(const HandState& state,
                                 uint64_t source_ts,
                                 const HandPoseStabilityFilterConfig& cfg) {
    if (!state.has_last_good) return false;
    if (source_ts < state.last_good_source_ts) return false;
    return source_ts - state.last_good_source_ts <= hold_lost_ns(cfg);
  }

  static bool within_hold_window_v2(const HandStateV2& state,
                                    uint64_t source_ts,
                                    const HandPoseStabilityFilterConfig& cfg) {
    if (!state.has_last_good) return false;
    if (source_ts < state.last_good_source_ts) return false;
    return source_ts - state.last_good_source_ts <= hold_lost_ns(cfg);
  }

  static void clear_side(xr_runtime::HandSideF64V1& s) {
    const uint32_t handedness = s.handedness;
    s = xr_runtime::HandSideF64V1{};
    s.handedness = handedness;
    s.status = kHandStatusNoHands;
    s.flags = 0;
    s.confidence = 0.0f;
    s.joint_count = 0;
  }

  static void clear_side_v2(xr_runtime::HandSideF32V2& s) {
    const uint32_t handedness = s.handedness;
    s = xr_runtime::HandSideF32V2{};
    s.handedness = handedness;
    s.status = kHandStatusNoHands;
    s.flags = 0;
    s.confidence = 0.0f;
    s.joint_count = 0;
  }

  static void mark_degraded_hold(xr_runtime::HandSideF64V1& s,
                                 uint64_t source_ts,
                                 const HandState& state,
                                 const HandPoseStabilityFilterConfig& cfg) {
    s.status = kHandStatusDegraded;
    s.flags |= xr_runtime::HAND_POSE_VALID;

    const uint64_t age_ns = source_ts >= state.last_good_source_ts
                                ? source_ts - state.last_good_source_ts
                                : 0;
    const double hold_ns = std::max(1.0, static_cast<double>(hold_lost_ns(cfg)));
    const double t = std::clamp(static_cast<double>(age_ns) / hold_ns, 0.0, 1.0);
    s.confidence = static_cast<float>(std::max(0.0, static_cast<double>(s.confidence) * (1.0 - t)));
  }

  static void mark_degraded_hold_v2(xr_runtime::HandSideF32V2& s,
                                    uint64_t source_ts,
                                    const HandStateV2& state,
                                    const HandPoseStabilityFilterConfig& cfg) {
    s.status = kHandStatusDegraded;
    s.flags |= xr_runtime::HAND_POSE_VALID;

    const uint64_t age_ns = source_ts >= state.last_good_source_ts
                                ? source_ts - state.last_good_source_ts
                                : 0;
    const double fade_ns = std::max(1.0, static_cast<double>(lost_output_ns_v2(cfg)));
    const double t = std::clamp(static_cast<double>(age_ns) / fade_ns, 0.0, 1.0);
    s.confidence = static_cast<float>(std::max(0.0, static_cast<double>(s.confidence) * (1.0 - t)));
  }

  static float lerp_float(float a, float b, double t) {
    return static_cast<float>(static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * t);
  }

  static void normalize_quat(float& qw, float& qx, float& qy, float& qz) {
    const double n = std::sqrt(static_cast<double>(qw) * qw + static_cast<double>(qx) * qx +
                               static_cast<double>(qy) * qy + static_cast<double>(qz) * qz);
    if (!std::isfinite(n) || n <= 1e-12) {
      qw = 1.0f; qx = 0.0f; qy = 0.0f; qz = 0.0f;
      return;
    }
    qw = static_cast<float>(qw / n);
    qx = static_cast<float>(qx / n);
    qy = static_cast<float>(qy / n);
    qz = static_cast<float>(qz / n);
  }

  static void lerp_quat(float aqw, float aqx, float aqy, float aqz,
                        float bqw, float bqx, float bqy, float bqz,
                        double t,
                        float& oqw, float& oqx, float& oqy, float& oqz) {
    // Same-hemisphere nlerp to avoid long-way interpolation when signs differ.
    const double dot = static_cast<double>(aqw) * bqw + static_cast<double>(aqx) * bqx +
                       static_cast<double>(aqy) * bqy + static_cast<double>(aqz) * bqz;
    if (dot < 0.0) {
      bqw = -bqw; bqx = -bqx; bqy = -bqy; bqz = -bqz;
    }
    oqw = lerp_float(aqw, bqw, t);
    oqx = lerp_float(aqx, bqx, t);
    oqy = lerp_float(aqy, bqy, t);
    oqz = lerp_float(aqz, bqz, t);
    normalize_quat(oqw, oqx, oqy, oqz);
  }

  static void translate_side_v2(xr_runtime::HandSideF32V2& s, const Vec3& delta) {
    s.controller_px += static_cast<float>(delta.x);
    s.controller_py += static_cast<float>(delta.y);
    s.controller_pz += static_cast<float>(delta.z);
    s.palm_px += static_cast<float>(delta.x);
    s.palm_py += static_cast<float>(delta.y);
    s.palm_pz += static_cast<float>(delta.z);
    s.wrist_px += static_cast<float>(delta.x);
    s.wrist_py += static_cast<float>(delta.y);
    s.wrist_pz += static_cast<float>(delta.z);
    for (uint32_t i = 0; i < xr_runtime::HAND_JOINT_COUNT_V2; ++i) {
      s.joints[i].px += static_cast<float>(delta.x);
      s.joints[i].py += static_cast<float>(delta.y);
      s.joints[i].pz += static_cast<float>(delta.z);
    }
  }

  static xr_runtime::HandSideF32V2 predicted_side_v2(const HandStateV2& state,
                                         uint64_t source_ts,
                                         const HandPoseStabilityFilterConfig& cfg,
                                         uint64_t prediction_elapsed_ns) {
    xr_runtime::HandSideF32V2 out = state.last_good;
    const uint64_t dt_ns = std::min(prediction_elapsed_ns, predict_lost_ns(cfg));
    const double dt_s = static_cast<double>(dt_ns) / 1e9;
    const double damping = std::clamp(cfg.prediction_damping, 0.0, 1.0);
    Vec3 v = state.has_velocity ? state.velocity_mps : Vec3{};
    v = clamp_velocity(v, cfg);
    const Vec3 delta = scale(v, dt_s * damping);
    translate_side_v2(out, delta);
    out.vx = static_cast<float>(v.x * damping);
    out.vy = static_cast<float>(v.y * damping);
    out.vz = static_cast<float>(v.z * damping);
    mark_degraded_hold_v2(out, source_ts, state, cfg);
    return out;
  }

  static xr_runtime::HandSideF32V2 blend_side_v2(const xr_runtime::HandSideF32V2& from,
                                    const xr_runtime::HandSideF32V2& to,
                                    double t) {
    t = std::clamp(t, 0.0, 1.0);
    xr_runtime::HandSideF32V2 out = to;
    out.status = t < 1.0 ? kHandStatusDegraded : to.status;
    out.flags |= xr_runtime::HAND_POSE_VALID;
    out.confidence = lerp_float(from.confidence, to.confidence, t);

    out.controller_px = lerp_float(from.controller_px, to.controller_px, t);
    out.controller_py = lerp_float(from.controller_py, to.controller_py, t);
    out.controller_pz = lerp_float(from.controller_pz, to.controller_pz, t);
    lerp_quat(from.controller_qw, from.controller_qx, from.controller_qy, from.controller_qz,
              to.controller_qw, to.controller_qx, to.controller_qy, to.controller_qz,
              t, out.controller_qw, out.controller_qx, out.controller_qy, out.controller_qz);

    out.palm_px = lerp_float(from.palm_px, to.palm_px, t);
    out.palm_py = lerp_float(from.palm_py, to.palm_py, t);
    out.palm_pz = lerp_float(from.palm_pz, to.palm_pz, t);
    lerp_quat(from.palm_qw, from.palm_qx, from.palm_qy, from.palm_qz,
              to.palm_qw, to.palm_qx, to.palm_qy, to.palm_qz,
              t, out.palm_qw, out.palm_qx, out.palm_qy, out.palm_qz);

    out.wrist_px = lerp_float(from.wrist_px, to.wrist_px, t);
    out.wrist_py = lerp_float(from.wrist_py, to.wrist_py, t);
    out.wrist_pz = lerp_float(from.wrist_pz, to.wrist_pz, t);
    lerp_quat(from.wrist_qw, from.wrist_qx, from.wrist_qy, from.wrist_qz,
              to.wrist_qw, to.wrist_qx, to.wrist_qy, to.wrist_qz,
              t, out.wrist_qw, out.wrist_qx, out.wrist_qy, out.wrist_qz);

    const uint32_t n = std::min<uint32_t>(xr_runtime::HAND_JOINT_COUNT_V2, std::min(from.joint_count, to.joint_count));
    out.joint_count = xr_runtime::HAND_JOINT_COUNT_V2;
    for (uint32_t i = 0; i < xr_runtime::HAND_JOINT_COUNT_V2; ++i) {
      out.joints[i].joint_id = i;
      if (i >= n) continue;
      out.joints[i].px = lerp_float(from.joints[i].px, to.joints[i].px, t);
      out.joints[i].py = lerp_float(from.joints[i].py, to.joints[i].py, t);
      out.joints[i].pz = lerp_float(from.joints[i].pz, to.joints[i].pz, t);
      lerp_quat(from.joints[i].qw, from.joints[i].qx, from.joints[i].qy, from.joints[i].qz,
                to.joints[i].qw, to.joints[i].qx, to.joints[i].qy, to.joints[i].qz,
                t, out.joints[i].qw, out.joints[i].qx, out.joints[i].qy, out.joints[i].qz);
      out.joints[i].radius_m = lerp_float(from.joints[i].radius_m, to.joints[i].radius_m, t);
      out.joints[i].confidence = lerp_float(from.joints[i].confidence, to.joints[i].confidence, t);
    }
    return out;
  }

  static void update_velocity_v2(HandStateV2& state,
                                 const xr_runtime::HandSideF32V2& accepted,
                                 uint64_t source_ts,
                                 const HandPoseStabilityFilterConfig& cfg) {
    if (state.has_last_good && source_ts > state.last_good_source_ts) {
      const double dt_s = static_cast<double>(source_ts - state.last_good_source_ts) / 1e9;
      if (dt_s > 1e-6) {
        const Vec3 v = scale(sub(controller_v2(accepted), controller_v2(state.last_good)), 1.0 / dt_s);
        state.velocity_mps = clamp_velocity(v, cfg);
        state.has_velocity = true;
      }
    }
    state.last_good = accepted;
    state.last_good_source_ts = source_ts;
    state.has_last_good = true;
  }

  SideDecision reject_continuity_velocity(const xr_runtime::HandSideF64V1& input,
                                           HandState& state,
                                           uint64_t source_ts,
                                           const char* hand_name,
                                           const SideDecision& in_decision) {
    SideDecision d = in_decision;
    stat_velocity_rejected(hand_name)++;
    if (within_hold_window(state, source_ts, cfg_)) {
      d.side = state.last_good;
      mark_degraded_hold(d.side, source_ts, state, cfg_);
      d.gated_active = true;
      d.mode = "reject_velocity_hold";
      d.has_output = true;
      d.output = palm(d.side);
      stat_gated_active(hand_name)++;
      stat_held(hand_name)++;
      return d;
    }

    // Keep last_good while an active-but-implausible candidate is being rejected.
    // Resetting here would make the next bogus active frame become accept_initial
    // and visibly snap the controller to the wrong place.  The state is reset only
    // after the input is truly inactive beyond the hold window.
    clear_side(d.side);
    d.gated_active = false;
    d.mode = "reject_velocity_inactive";
    (void)input;
    return d;
  }

  SideDecisionV2 reject_continuity_velocity_v2(const xr_runtime::HandSideF32V2& input,
                                               HandStateV2& state,
                                               uint64_t source_ts,
                                               const char* hand_name,
                                               const SideDecisionV2& in_decision) {
    SideDecisionV2 d = in_decision;
    stat_velocity_rejected(hand_name)++;
    if (within_lost_output_window_v2(state, source_ts, cfg_)) {
      uint64_t prediction_elapsed_ns = 0;
      if (state.has_velocity && prediction_elapsed_v2(state, source_ts, cfg_, prediction_elapsed_ns)) {
        d.side = predicted_side_v2(state, source_ts, cfg_, prediction_elapsed_ns);
        d.gated_active = true;
        d.mode = "reject_velocity_predict";
        d.has_output = true;
        d.output = controller_v2(d.side);
        stat_gated_active(hand_name)++;
        stat_predicted(hand_name)++;
        return d;
      }

      d.side = state.last_good;
      mark_degraded_hold_v2(d.side, source_ts, state, cfg_);
      d.gated_active = true;
      d.mode = "reject_velocity_hold";
      d.has_output = true;
      d.output = controller_v2(d.side);
      stat_gated_active(hand_name)++;
      stat_held(hand_name)++;
      return d;
    }

    // Keep last_good for active velocity outliers for the same reason as the V1
    // path: do not let a still-active bogus floor candidate reinitialize as a
    // fresh hand.  A truly inactive frame beyond the lost-output window still resets state.
    clear_side_v2(d.side);
    d.gated_active = false;
    d.mode = "reject_velocity_inactive";
    (void)input;
    return d;
  }

  SideDecision filter_side(const xr_runtime::HandSideF64V1& input,
                           HandState& state,
                           uint64_t source_ts,
                           const char* hand_name) {
    SideDecision d;
    d.side = input;
    d.orig_active = side_pose_is_active(input);

    if (d.orig_active) {
      d.has_candidate = true;
      d.candidate = palm(input);
      stat_orig_active(hand_name)++;
    }

    if (d.orig_active && !state.has_last_good) {
      state.last_good = input;
      state.last_good_source_ts = source_ts;
      state.has_last_good = true;
      state.has_pending = false;
      state.pending_count = 0;

      d.gated_active = true;
      d.mode = "accept_initial";
      d.jump_m = 0.0;
      d.has_output = true;
      d.output = d.candidate;
      stat_gated_active(hand_name)++;
      return d;
    }

    if (d.orig_active) {
      const Vec3 last = palm(state.last_good);
      d.jump_m = distance_m(d.candidate, last);
      d.velocity_mps = velocity_mps_between(last, d.candidate, state.last_good_source_ts, source_ts);

      if (d.jump_m <= cfg_.max_reacquire_jump_m && !continuity_velocity_ok(d.velocity_mps, cfg_)) {
        return reject_continuity_velocity(input, state, source_ts, hand_name, d);
      }

      if (d.jump_m <= cfg_.max_reacquire_jump_m) {
        state.last_good = input;
        state.last_good_source_ts = source_ts;
        state.has_last_good = true;
        state.has_pending = false;
        state.pending_count = 0;

        d.gated_active = true;
        d.mode = "accept_continuity";
        d.has_output = true;
        d.output = d.candidate;
        stat_gated_active(hand_name)++;
        return d;
      }

      const bool pending_continues = state.has_pending &&
                                     distance_m(d.candidate, state.pending_palm) <= cfg_.confirm_max_step_m;
      if (pending_continues) {
        state.pending_count += 1;
      } else {
        state.has_pending = true;
        state.pending_palm = d.candidate;
        state.pending_count = 1;
      }
      d.pending_count = state.pending_count;

      if (state.pending_count >= cfg_.confirm_frames) {
        state.last_good = input;
        state.last_good_source_ts = source_ts;
        state.has_last_good = true;
        state.has_pending = false;
        state.pending_count = 0;

        d.gated_active = true;
        d.mode = "accept_confirmed_reacquire";
        d.has_output = true;
        d.output = d.candidate;
        stat_gated_active(hand_name)++;
        stat_confirmed(hand_name)++;
        return d;
      }

      stat_jump_rejected(hand_name)++;
      if (within_hold_window(state, source_ts, cfg_)) {
        d.side = state.last_good;
        mark_degraded_hold(d.side, source_ts, state, cfg_);
        d.gated_active = true;
        d.mode = "reject_jump_hold";
        d.has_output = true;
        d.output = palm(d.side);
        stat_gated_active(hand_name)++;
        stat_held(hand_name)++;
        return d;
      }

      clear_side(d.side);
      d.gated_active = false;
      d.mode = "reject_jump_inactive";
      return d;
    }

    state.has_pending = false;
    state.pending_count = 0;

    const Vec3 inactive_candidate = palm(input);
    if (finite_pose(inactive_candidate) && nonzero_pose(inactive_candidate)) {
      d.has_candidate = true;
      d.candidate = inactive_candidate;
    }

    if (within_hold_window(state, source_ts, cfg_)) {
      d.side = state.last_good;
      mark_degraded_hold(d.side, source_ts, state, cfg_);
      d.gated_active = true;
      d.mode = "hold_lost";
      d.has_output = true;
      d.output = palm(d.side);
      stat_gated_active(hand_name)++;
      stat_held(hand_name)++;
      return d;
    }

    // The hand has been inactive beyond the hold window. Drop stale last_good
    // so the next valid detection can initialize normally instead of being
    // rejected forever against an old controller pose.
    state = HandState{};
    clear_side(d.side);
    d.gated_active = false;
    d.mode = "inactive";
    return d;
  }


  SideDecisionV2 filter_side_v2(const xr_runtime::HandSideF32V2& input,
                                HandStateV2& state,
                                uint64_t source_ts,
                                const char* hand_name) {
    SideDecisionV2 d;
    d.side = input;
    d.orig_active = side_pose_is_active_v2(input);

    if (d.orig_active) {
      d.has_candidate = true;
      d.candidate = controller_v2(input);
      stat_orig_active(hand_name)++;
    }

    if (d.orig_active && state.blend_active) {
      const uint64_t duration_ns = std::max<uint64_t>(1, reacquire_blend_ns(cfg_));
      const uint64_t elapsed_ns = source_ts >= state.blend_start_ts ? source_ts - state.blend_start_ts : 0;
      const double t = std::clamp(static_cast<double>(elapsed_ns) / static_cast<double>(duration_ns), 0.0, 1.0);
      xr_runtime::HandSideF32V2 out = blend_side_v2(state.blend_from, input, t);
      update_velocity_v2(state, out, source_ts, cfg_);

      if (t >= 1.0) {
        update_velocity_v2(state, input, source_ts, cfg_);
        state.blend_active = false;
        out = input;
      }

      state.has_pending = false;
      state.pending_count = 0;
      d.side = out;
      d.gated_active = true;
      d.mode = t >= 1.0 ? "blend_reacquire_done" : "blend_reacquire";
      d.has_output = true;
      d.output = controller_v2(d.side);
      stat_gated_active(hand_name)++;
      stat_blended(hand_name)++;
      return d;
    }

    if (d.orig_active && !state.has_last_good) {
      update_velocity_v2(state, input, source_ts, cfg_);
      state.has_pending = false;
      state.pending_count = 0;
      state.blend_active = false;

      d.gated_active = true;
      d.mode = "accept_initial";
      d.jump_m = 0.0;
      d.has_output = true;
      d.output = d.candidate;
      stat_gated_active(hand_name)++;
      return d;
    }

    if (d.orig_active) {
      const Vec3 last = controller_v2(state.last_good);
      d.jump_m = distance_m(d.candidate, last);
      d.velocity_mps = velocity_mps_between(last, d.candidate, state.last_good_source_ts, source_ts);

      if (d.jump_m <= cfg_.max_reacquire_jump_m && !continuity_velocity_ok(d.velocity_mps, cfg_)) {
        return reject_continuity_velocity_v2(input, state, source_ts, hand_name, d);
      }

      if (d.jump_m <= cfg_.max_reacquire_jump_m) {
        update_velocity_v2(state, input, source_ts, cfg_);
        state.has_pending = false;
        state.pending_count = 0;
        state.blend_active = false;

        d.gated_active = true;
        d.mode = "accept_continuity";
        d.has_output = true;
        d.output = d.candidate;
        stat_gated_active(hand_name)++;
        return d;
      }

      // Confirmation should prove that the reacquire candidate is stable, but it
      // must not override the jump gate. Otherwise a wrong far-away candidate can
      // be rejected for a few frames and then accepted only because it stayed
      // consistently wrong.
      const double confirmed_reacquire_max_jump_m = cfg_.max_reacquire_jump_m * 1.5;
      // If the graceful lost-output window has expired, the old last_good pose is stale. Allow a far reacquire candidate to enter the stability-confirmation path; it still must remain stable for confirm_frames with per-frame steps <= confirm_max_step_m.
const bool lost_output_window_expired_for_reacquire = !within_lost_output_window_v2(state, source_ts, cfg_); const bool candidate_within_confirmed_gate = d.jump_m <= confirmed_reacquire_max_jump_m || lost_output_window_expired_for_reacquire;

      const bool pending_continues = candidate_within_confirmed_gate &&
                                     state.has_pending &&
                                     distance_m(d.candidate, state.pending_controller) <= cfg_.confirm_max_step_m;
      if (!candidate_within_confirmed_gate) {
        state.has_pending = false;
        state.pending_count = 0;
      } else if (pending_continues) {
        state.pending_count += 1;
      } else {
        state.has_pending = true;
        state.pending_controller = d.candidate;
        state.pending_count = 1;
      }
      d.pending_count = state.pending_count;

      if (candidate_within_confirmed_gate && state.pending_count >= cfg_.confirm_frames) {
        state.has_pending = false;
        state.pending_count = 0;
        stat_confirmed(hand_name)++;

        const uint64_t blend_ns = reacquire_blend_ns(cfg_);
        if (blend_ns > 0 && state.has_last_good) {
          state.blend_active = true;
          state.blend_from = state.last_good;
          state.blend_start_ts = source_ts;
          xr_runtime::HandSideF32V2 out = blend_side_v2(state.blend_from, input, 0.0);
          update_velocity_v2(state, out, source_ts, cfg_);

          d.side = out;
          d.gated_active = true;
          d.mode = "accept_confirmed_reacquire_blend";
          d.has_output = true;
          d.output = controller_v2(d.side);
          stat_gated_active(hand_name)++;
          stat_blended(hand_name)++;
          return d;
        }

        update_velocity_v2(state, input, source_ts, cfg_);
        state.blend_active = false;

        d.gated_active = true;
        d.mode = "accept_confirmed_reacquire";
        d.has_output = true;
        d.output = d.candidate;
        stat_gated_active(hand_name)++;
        return d;
      }

      stat_jump_rejected(hand_name)++;
      if (within_lost_output_window_v2(state, source_ts, cfg_)) {
        uint64_t prediction_elapsed_ns = 0;
        if (state.has_velocity && prediction_elapsed_v2(state, source_ts, cfg_, prediction_elapsed_ns)) {
          d.side = predicted_side_v2(state, source_ts, cfg_, prediction_elapsed_ns);
          d.gated_active = true;
          d.mode = "reject_jump_predict";
          d.has_output = true;
          d.output = controller_v2(d.side);
          stat_gated_active(hand_name)++;
          stat_predicted(hand_name)++;
          return d;
        }

        d.side = state.last_good;
        mark_degraded_hold_v2(d.side, source_ts, state, cfg_);
        d.gated_active = true;
        d.mode = "reject_jump_hold";
        d.has_output = true;
        d.output = controller_v2(d.side);
        stat_gated_active(hand_name)++;
        stat_held(hand_name)++;
        return d;
      }

      clear_side_v2(d.side);
      d.gated_active = false;
      d.mode = "reject_jump_inactive";
      return d;
    }

    state.has_pending = false;
    state.pending_count = 0;
    state.blend_active = false;

    const Vec3 inactive_candidate = controller_v2(input);
    if (finite_pose(inactive_candidate) && nonzero_pose(inactive_candidate)) {
      d.has_candidate = true;
      d.candidate = inactive_candidate;
    }

    if (within_lost_output_window_v2(state, source_ts, cfg_)) {
      uint64_t prediction_elapsed_ns = 0;
      if (state.has_velocity && prediction_elapsed_v2(state, source_ts, cfg_, prediction_elapsed_ns)) {
        d.side = predicted_side_v2(state, source_ts, cfg_, prediction_elapsed_ns);
        d.gated_active = true;
        d.mode = "predict_lost";
        d.has_output = true;
        d.output = controller_v2(d.side);
        stat_gated_active(hand_name)++;
        stat_predicted(hand_name)++;
        return d;
      }

      d.side = state.last_good;
      mark_degraded_hold_v2(d.side, source_ts, state, cfg_);
      d.gated_active = true;
      d.mode = "hold_lost";
      d.has_output = true;
      d.output = controller_v2(d.side);
      stat_gated_active(hand_name)++;
      stat_held(hand_name)++;
      return d;
    }

    // The hand has been inactive beyond the lost-output window. Drop stale last_good
    // so the next valid detection can initialize normally instead of being
    // rejected forever against an old controller pose.
    state = HandStateV2{};
    clear_side_v2(d.side);
    d.gated_active = false;
    d.mode = "inactive";
    return d;
  }

  uint64_t& stat_orig_active(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_orig_active : stats_.right_orig_active;
  }

  uint64_t& stat_gated_active(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_gated_active : stats_.right_gated_active;
  }

  uint64_t& stat_held(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_held : stats_.right_held;
  }

  uint64_t& stat_jump_rejected(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_jump_rejected : stats_.right_jump_rejected;
  }

  uint64_t& stat_velocity_rejected(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_velocity_rejected : stats_.right_velocity_rejected;
  }

  uint64_t& stat_confirmed(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_confirmed : stats_.right_confirmed;
  }

  uint64_t& stat_predicted(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_predicted : stats_.right_predicted;
  }

  uint64_t& stat_blended(const char* hand_name) {
    return std::string(hand_name) == "left" ? stats_.left_blended : stats_.right_blended;
  }

  static bool side_is_gated_valid(const xr_runtime::HandSideF64V1& s) {
    return (s.status == kHandStatusTracking ||
            s.status == kHandStatusDegraded) &&
           finite_pose(palm(s)) && nonzero_pose(palm(s));
  }

  static void recompute_frame_status(xr_runtime::HandTrackingFrameF64V1& frame) {
    frame.flags = 0;
    frame.hand_count = 0;
    frame.confidence = 0.0f;

    bool any_degraded = false;

    if (side_is_gated_valid(frame.left)) {
      frame.flags |= xr_runtime::HAND_FLAG_LEFT_VALID;
      ++frame.hand_count;
      frame.confidence = std::max(frame.confidence, frame.left.confidence);
      any_degraded = any_degraded ||
                     frame.left.status == kHandStatusDegraded;
    }

    if (side_is_gated_valid(frame.right)) {
      frame.flags |= xr_runtime::HAND_FLAG_RIGHT_VALID;
      ++frame.hand_count;
      frame.confidence = std::max(frame.confidence, frame.right.confidence);
      any_degraded = any_degraded ||
                     frame.right.status == kHandStatusDegraded;
    }

    if (frame.hand_count == 0) {
      frame.tracking_status = kHandStatusNoHands;
      frame.confidence = 0.0f;
      return;
    }

    frame.tracking_status = static_cast<uint32_t>(any_degraded ? kHandStatusDegraded
                                                               : kHandStatusTracking);
  }


  static bool side_is_gated_valid_v2(const xr_runtime::HandSideF32V2& s) {
    return (s.status == kHandStatusTracking ||
            s.status == kHandStatusDegraded) &&
           finite_pose(controller_v2(s)) && nonzero_pose(controller_v2(s));
  }

  static void recompute_frame_status_v2(xr_runtime::HandTrackingFrameF32V2& frame) {
    frame.flags = 0;
    frame.hand_count = 0;
    frame.confidence = 0.0f;

    bool any_degraded = false;
    bool any_joints_valid = false;

    if (side_is_gated_valid_v2(frame.left)) {
      frame.flags |= xr_runtime::HAND_FLAG_LEFT_VALID;
      ++frame.hand_count;
      frame.confidence = std::max(frame.confidence, frame.left.confidence);
      any_degraded = any_degraded ||
                     frame.left.status == kHandStatusDegraded;
      any_joints_valid = any_joints_valid ||
                         ((frame.left.flags & xr_runtime::HAND_JOINTS_VALID) != 0 &&
                          frame.left.joint_count == xr_runtime::HAND_JOINT_COUNT_V2);
    }

    if (side_is_gated_valid_v2(frame.right)) {
      frame.flags |= xr_runtime::HAND_FLAG_RIGHT_VALID;
      ++frame.hand_count;
      frame.confidence = std::max(frame.confidence, frame.right.confidence);
      any_degraded = any_degraded ||
                     frame.right.status == kHandStatusDegraded;
      any_joints_valid = any_joints_valid ||
                         ((frame.right.flags & xr_runtime::HAND_JOINTS_VALID) != 0 &&
                          frame.right.joint_count == xr_runtime::HAND_JOINT_COUNT_V2);
    }

    if (any_joints_valid) {
      frame.flags |= xr_runtime::HAND_FLAG_JOINTS_VALID;
    }

    if (frame.hand_count == 0) {
      frame.tracking_status = kHandStatusNoHands;
      frame.confidence = 0.0f;
      return;
    }

    frame.tracking_status = static_cast<uint32_t>(any_degraded ? kHandStatusDegraded
                                                               : kHandStatusTracking);
  }

  static void write_vec(std::ofstream& os, bool valid, const Vec3& v) {
    if (!valid) {
      os << ",,,";
      return;
    }
    os << ',' << v.x << ',' << v.y << ',' << v.z;
  }

  void write_debug(uint64_t sequence,
                   uint64_t source_ts,
                   const char* hand_name,
                   const SideDecision& d) {
    if (!debug_) return;
    debug_ << sequence << ',' << source_ts << ',' << hand_name << ','
           << (d.orig_active ? 1 : 0) << ','
           << (d.gated_active ? 1 : 0) << ','
           << d.mode << ',';
    if (std::isfinite(d.jump_m)) {
      debug_ << std::setprecision(10) << d.jump_m;
    }
    debug_ << ',';
    if (std::isfinite(d.velocity_mps)) {
      debug_ << std::setprecision(10) << d.velocity_mps;
    }
    debug_ << ',' << d.pending_count;
    write_vec(debug_, d.has_candidate, d.candidate);
    write_vec(debug_, d.has_output, d.output);
    debug_ << '\n';
  }


  void write_debug(uint64_t sequence,
                   uint64_t source_ts,
                   const char* hand_name,
                   const SideDecisionV2& d) {
    if (!debug_) return;
    debug_ << sequence << ',' << source_ts << ',' << hand_name << ','
           << (d.orig_active ? 1 : 0) << ','
           << (d.gated_active ? 1 : 0) << ','
           << d.mode << ',';
    if (std::isfinite(d.jump_m)) {
      debug_ << std::setprecision(10) << d.jump_m;
    }
    debug_ << ',';
    if (std::isfinite(d.velocity_mps)) {
      debug_ << std::setprecision(10) << d.velocity_mps;
    }
    debug_ << ',' << d.pending_count;
    write_vec(debug_, d.has_candidate, d.candidate);
    write_vec(debug_, d.has_output, d.output);
    debug_ << '\n';
  }

  HandPoseStabilityFilterConfig cfg_;
  HandState left_;
  HandState right_;
  HandStateV2 left_v2_;
  HandStateV2 right_v2_;
  HandPoseStabilityFilterStats stats_;
  std::ofstream debug_;
};

}  // namespace xr_runtime_adapter::hand_filter
