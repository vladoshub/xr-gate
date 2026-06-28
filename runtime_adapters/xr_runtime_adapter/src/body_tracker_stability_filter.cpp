#include "body_tracker_stability_filter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xr_runtime_adapter::body_tracker_filter {

uint32_t parse_body_tracker_predicted_status(const std::string& value) {
  if (value == "tracking") return xr_tracking::BODY_TRACKER_STATUS_TRACKING;
  if (value == "stale") return xr_tracking::BODY_TRACKER_STATUS_STALE;
  if (value == "lost") return xr_tracking::BODY_TRACKER_STATUS_LOST;
  throw std::runtime_error("unknown runtime body tracker predicted status: " + value +
                           "; expected tracking, stale, or lost");
}

void BodyTrackerStabilityFilter::configure(BodyTrackerStabilityGateConfig cfg) {
  cfg_ = cfg;
  if (!cfg_.enabled) reset();
}

void BodyTrackerStabilityFilter::reset() {
  for (auto& state : states_) state = {};
  has_template_ = false;
  template_frame_ = {};
  last_synthetic_publish_ns_ = 0;
}

xr_tracking::BodyTrackerSetFrameF32V1 BodyTrackerStabilityFilter::filter_observed(
    xr_tracking::BodyTrackerSetFrameF32V1 frame,
    uint64_t now_ns) {
  if (!cfg_.enabled) return frame;

  const uint64_t sample_ns = frame.source_timestamp_ns != 0
      ? frame.source_timestamp_ns
      : (frame.timestamp_ns != 0 ? frame.timestamp_ns : now_ns);

  frame.tracker_count = std::min<uint32_t>(frame.tracker_count, xr_tracking::BODY_TRACKER_MAX_TRACKERS);
  if (has_template_ && frame.reset_counter != template_frame_.reset_counter) {
    for (auto& state : states_) state = {};
    last_synthetic_publish_ns_ = 0;
  }
  template_frame_ = frame;
  has_template_ = true;

  std::array<uint64_t, xr_tracking::BODY_TRACKER_MAX_TRACKERS> observed_keys{};
  uint32_t observed_key_count = 0;

  xr_tracking::BodyTrackerSetFrameF32V1 out = frame;
  out.tracker_count = 0;
  out.flags &= ~xr_tracking::BODY_TRACKER_FRAME_HAS_TRACKERS;

  for (uint32_t i = 0; i < frame.tracker_count; ++i) {
    const auto& tracker = frame.trackers[i];
    const uint64_t key = tracker_key(tracker, i);
    if (observed_key_count < observed_keys.size()) observed_keys[observed_key_count++] = key;
    if (tracker_pose_is_good(tracker)) {
      update_state(tracker, key, sample_ns);
      append_tracker(out, tracker);
    } else {
      auto predicted = predicted_tracker_for_key(key, now_ns);
      if (predicted) {
        append_tracker(out, *predicted);
      } else {
        append_tracker(out, tracker);
      }
    }
  }

  for (auto& state : states_) {
    if (!state.active) continue;
    bool already_observed = false;
    for (uint32_t i = 0; i < observed_key_count; ++i) {
      if (observed_keys[i] == state.key) {
        already_observed = true;
        break;
      }
    }
    if (already_observed) continue;
    auto predicted = predicted_tracker(state, now_ns);
    if (predicted) append_tracker(out, *predicted);
  }

  if (out.tracker_count > 0) out.flags |= xr_tracking::BODY_TRACKER_FRAME_HAS_TRACKERS;
  return out;
}

std::optional<xr_tracking::BodyTrackerSetFrameF32V1> BodyTrackerStabilityFilter::predicted_frame(uint64_t now_ns) {
  if (!cfg_.enabled || !has_template_) return std::nullopt;
  const int64_t interval_ns = synthetic_interval_ns();
  if (interval_ns > 0 && last_synthetic_publish_ns_ != 0 &&
      static_cast<int64_t>(now_ns - last_synthetic_publish_ns_) < interval_ns) {
    return std::nullopt;
  }

  xr_tracking::BodyTrackerSetFrameF32V1 out = template_frame_;
  out.timestamp_ns = now_ns;
  out.source_timestamp_ns = template_frame_.source_timestamp_ns != 0 ? template_frame_.source_timestamp_ns : now_ns;
  out.tracker_count = 0;
  out.flags &= ~xr_tracking::BODY_TRACKER_FRAME_HAS_TRACKERS;

  for (auto& state : states_) {
    if (!state.active) continue;
    auto predicted = predicted_tracker(state, now_ns);
    if (predicted) append_tracker(out, *predicted);
  }

  if (out.tracker_count == 0) return std::nullopt;
  out.flags |= xr_tracking::BODY_TRACKER_FRAME_HAS_TRACKERS;
  last_synthetic_publish_ns_ = now_ns;
  return out;
}

bool BodyTrackerStabilityFilter::finite3(float x, float y, float z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

double BodyTrackerStabilityFilter::norm(Vec3 v) {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

BodyTrackerStabilityFilter::Vec3 BodyTrackerStabilityFilter::sub(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

BodyTrackerStabilityFilter::Vec3 BodyTrackerStabilityFilter::scale(Vec3 v, double s) {
  return {v.x * s, v.y * s, v.z * s};
}

BodyTrackerStabilityFilter::Vec3 BodyTrackerStabilityFilter::add(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

BodyTrackerStabilityFilter::Vec3 BodyTrackerStabilityFilter::pose_position(
    const xr_tracking::BodyTrackerF32V1& tracker) {
  return {tracker.pose.px, tracker.pose.py, tracker.pose.pz};
}

void BodyTrackerStabilityFilter::assign_position(xr_tracking::BodyTrackerF32V1& tracker, Vec3 p) {
  tracker.pose.px = static_cast<float>(p.x);
  tracker.pose.py = static_cast<float>(p.y);
  tracker.pose.pz = static_cast<float>(p.z);
}

uint64_t BodyTrackerStabilityFilter::tracker_key(const xr_tracking::BodyTrackerF32V1& tracker,
                                                 uint32_t slot_index) {
  if (tracker.stable_id_hash != 0) return (1ull << 60) ^ tracker.stable_id_hash;
  if (tracker.device_serial_hash != 0) return (2ull << 60) ^ tracker.device_serial_hash;
  if ((tracker.flags & xr_tracking::BODY_TRACKER_FLAG_ROLE_VALID) != 0u &&
      tracker.role != xr_tracking::BODY_TRACKER_ROLE_UNKNOWN) {
    return (3ull << 60) ^ static_cast<uint64_t>(tracker.role);
  }
  if (tracker.tracker_index != 0) return (4ull << 60) ^ static_cast<uint64_t>(tracker.tracker_index);
  return (5ull << 60) ^ static_cast<uint64_t>(slot_index);
}

bool BodyTrackerStabilityFilter::tracker_pose_is_good(const xr_tracking::BodyTrackerF32V1& tracker) {
  const bool pose_flag = (tracker.flags & xr_tracking::BODY_TRACKER_FLAG_POSE_VALID) != 0u;
  const bool position_flag = (tracker.flags & xr_tracking::BODY_TRACKER_FLAG_POSITION_VALID) != 0u;
  if (!pose_flag && !position_flag) return false;
  if (tracker.status == xr_tracking::BODY_TRACKER_STATUS_UNAVAILABLE ||
      tracker.status == xr_tracking::BODY_TRACKER_STATUS_LOST) {
    return false;
  }
  return finite3(tracker.pose.px, tracker.pose.py, tracker.pose.pz);
}

BodyTrackerStabilityFilter::Vec3 BodyTrackerStabilityFilter::clamp_velocity(Vec3 v) const {
  const double max_v = cfg_.max_prediction_velocity_mps;
  if (!std::isfinite(max_v) || max_v <= 0.0) return v;
  const double n = norm(v);
  if (!std::isfinite(n) || n <= max_v || n <= 1e-12) return v;
  return scale(v, max_v / n);
}

BodyTrackerStabilityFilter::Vec3 BodyTrackerStabilityFilter::clamp_acceleration(Vec3 desired,
                                                                                 Vec3 previous,
                                                                                 double dt_s) const {
  const double max_a = cfg_.max_prediction_acceleration_mps2;
  if (!std::isfinite(max_a) || max_a <= 0.0 || !std::isfinite(dt_s) || dt_s <= 1e-6) return desired;
  const Vec3 dv = sub(desired, previous);
  const double max_dv = max_a * dt_s;
  const double n = norm(dv);
  if (!std::isfinite(n) || n <= max_dv || n <= 1e-12) return desired;
  return add(previous, scale(dv, max_dv / n));
}

BodyTrackerStabilityFilter::State& BodyTrackerStabilityFilter::find_or_create_state(uint64_t key) {
  for (auto& state : states_) {
    if (state.active && state.key == key) return state;
  }
  for (auto& state : states_) {
    if (!state.active) {
      state = {};
      state.active = true;
      state.key = key;
      return state;
    }
  }
  auto* oldest = &states_[0];
  for (auto& state : states_) {
    if (state.last_good_ns < oldest->last_good_ns) oldest = &state;
  }
  *oldest = {};
  oldest->active = true;
  oldest->key = key;
  return *oldest;
}

BodyTrackerStabilityFilter::State* BodyTrackerStabilityFilter::find_state(uint64_t key) {
  for (auto& state : states_) {
    if (state.active && state.key == key) return &state;
  }
  return nullptr;
}

void BodyTrackerStabilityFilter::update_state(const xr_tracking::BodyTrackerF32V1& tracker,
                                              uint64_t key,
                                              uint64_t sample_ns) {
  State& state = find_or_create_state(key);
  Vec3 v{};
  bool have_v = false;
  if ((tracker.flags & xr_tracking::BODY_TRACKER_FLAG_LINEAR_VELOCITY_VALID) != 0u &&
      finite3(tracker.pose.vx, tracker.pose.vy, tracker.pose.vz)) {
    v = {tracker.pose.vx, tracker.pose.vy, tracker.pose.vz};
    have_v = true;
  } else if (state.last_good_ns != 0 && sample_ns > state.last_good_ns) {
    const double dt_s = static_cast<double>(sample_ns - state.last_good_ns) / 1e9;
    if (dt_s > 1e-6) {
      v = scale(sub(pose_position(tracker), pose_position(state.last_good)), 1.0 / dt_s);
      have_v = true;
    }
  }

  if (have_v) {
    v = clamp_velocity(v);
    if (state.has_velocity && state.last_good_ns != 0 && sample_ns > state.last_good_ns) {
      const double dt_s = static_cast<double>(sample_ns - state.last_good_ns) / 1e9;
      v = clamp_acceleration(v, state.velocity_mps, dt_s);
      v = clamp_velocity(v);
    }
    state.velocity_mps = v;
    state.has_velocity = true;
  }

  state.last_good = tracker;
  state.last_good_ns = sample_ns;
  state.active = true;
}

std::optional<xr_tracking::BodyTrackerF32V1> BodyTrackerStabilityFilter::predicted_tracker_for_key(uint64_t key,
                                                                                                    uint64_t now_ns) {
  State* state = find_state(key);
  if (!state) return std::nullopt;
  return predicted_tracker(*state, now_ns);
}

std::optional<xr_tracking::BodyTrackerF32V1> BodyTrackerStabilityFilter::predicted_tracker(State& state,
                                                                                           uint64_t now_ns) {
  if (!state.active || state.last_good_ns == 0 || now_ns < state.last_good_ns) return std::nullopt;
  const uint64_t hold_ns = static_cast<uint64_t>(ms_to_ns(cfg_.hold_lost_ms));
  const uint64_t predict_ns = static_cast<uint64_t>(ms_to_ns(cfg_.predict_lost_ms));
  const uint64_t elapsed_ns = now_ns - state.last_good_ns;
  if (elapsed_ns > hold_ns + predict_ns) {
    state.active = false;
    return std::nullopt;
  }

  xr_tracking::BodyTrackerF32V1 out = state.last_good;
  out.status = cfg_.predicted_status;
  out.flags |= xr_tracking::BODY_TRACKER_FLAG_POSE_VALID |
               xr_tracking::BODY_TRACKER_FLAG_POSITION_VALID |
               xr_tracking::BODY_TRACKER_FLAG_ORIENTATION_VALID |
               xr_tracking::BODY_TRACKER_FLAG_CONNECTED;

  const uint64_t prediction_elapsed_ns = elapsed_ns > hold_ns ? (elapsed_ns - hold_ns) : 0;
  if (prediction_elapsed_ns > 0 && predict_ns > 0 && state.has_velocity) {
    const double dt_s = static_cast<double>(std::min<uint64_t>(prediction_elapsed_ns, predict_ns)) / 1e9;
    const double damping = std::clamp(cfg_.prediction_damping, 0.0, 1.0);
    Vec3 v = clamp_velocity(state.velocity_mps);
    const Vec3 delta = scale(v, dt_s * damping);
    assign_position(out, add(pose_position(out), delta));
    out.pose.vx = static_cast<float>(v.x * damping);
    out.pose.vy = static_cast<float>(v.y * damping);
    out.pose.vz = static_cast<float>(v.z * damping);
    out.flags |= xr_tracking::BODY_TRACKER_FLAG_LINEAR_VELOCITY_VALID;
  } else {
    out.pose.vx = 0.0f;
    out.pose.vy = 0.0f;
    out.pose.vz = 0.0f;
  }

  const double total_ns = static_cast<double>(std::max<uint64_t>(hold_ns + predict_ns, 1));
  const double confidence_scale = std::clamp(1.0 - (static_cast<double>(elapsed_ns) / total_ns), 0.05, 1.0);
  if (std::isfinite(out.confidence) && out.confidence > 0.0f) {
    out.confidence = static_cast<float>(out.confidence * confidence_scale);
  }
  return out;
}

void BodyTrackerStabilityFilter::append_tracker(xr_tracking::BodyTrackerSetFrameF32V1& frame,
                                                const xr_tracking::BodyTrackerF32V1& tracker) {
  if (frame.tracker_count >= xr_tracking::BODY_TRACKER_MAX_TRACKERS) return;
  frame.trackers[frame.tracker_count++] = tracker;
}

int64_t BodyTrackerStabilityFilter::synthetic_interval_ns() const {
  if (!std::isfinite(cfg_.synthetic_publish_hz) || cfg_.synthetic_publish_hz <= 0.0) return 0;
  return static_cast<int64_t>(1e9 / cfg_.synthetic_publish_hz);
}

int64_t BodyTrackerStabilityFilter::ms_to_ns(double ms) {
  if (!std::isfinite(ms) || ms <= 0.0) return 0;
  return static_cast<int64_t>(ms * 1000000.0);
}

}  // namespace xr_runtime_adapter::body_tracker_filter
