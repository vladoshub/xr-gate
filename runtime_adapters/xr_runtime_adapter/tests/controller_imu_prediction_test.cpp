#include "coordinate_util.hpp"
#include "hand_pose_stability_filter.hpp"
#include "hand_tracking_freshness.hpp"
#include "override_controller.hpp"

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace xr_runtime;
using namespace xr_runtime_adapter::override_controller;
namespace cu = xr_runtime_adapter::coordinate_util;
constexpr uint64_t kStart = 1'000'000'000ULL;
constexpr float kPi = 3.14159265358979323846f;

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
void near(float actual, float expected, const char* message, float tolerance = 1e-5f) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

struct Scenario {
  RuntimeControllerSynthesisConfig cfg{};
  RuntimeControllerSynthesisState state{};
  ControllerInputV3 input{};
  HandTrackingFrameF32V2 hand{};
  uint64_t sequence = 0;

  Scenario() {
    cfg.left_orientation_source = RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
    auto& m = cfg.left_imu_motion;
    m.position_prediction_enabled = m.acceleration_integration_enabled = true;
    m.prediction_time_decay_enabled = false;
    m.acceleration_deadband_mps2 = 0;
    m.max_prediction_velocity_mps = m.max_prediction_speed_mps = 0;
    m.max_prediction_path_m = 0;
    m.publish_predicted_velocity = true;
    input.left.status = CONTROLLER_INPUT_ACTIVE;
    input.left.flags = CONTROLLER_DEVICE_IMU_PRESENT | CONTROLLER_DEVICE_IMU_ACTIVE;
    input.left.physical_device_hash = 123;
    auto& imu = input.left.imu;
    imu.status = CONTROLLER_IMU_ACTIVE;
    imu.capability_flags = CONTROLLER_IMU_CAP_STREAM_DIAGNOSTICS;
    imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = 1;
    imu.data_flags = CONTROLLER_IMU_ORIENTATION_VALID | CONTROLLER_IMU_ACCELEROMETER_VALID;
    imu.sample_count = 1;
    hand.flags = HAND_FLAG_LEFT_VALID;
    hand.hand_count = 1;
    hand.left.status = 1;
    hand.left.handedness = 1;
    hand.left.flags = HAND_POSE_VALID | HAND_LINEAR_VELOCITY_VALID;
    hand.left.controller_qw = 1;
    hand.left.confidence = 1;
  }
  uint64_t prepare(int ms, float acceleration = 0) {
    const uint64_t t = kStart + uint64_t(ms) * 1'000'000;
    input.sequence = input.left.imu.sequence = ++sequence;
    input.timestamp_ns = input.source_timestamp_ns = t;
    input.left.imu.orientation_timestamp_ns = input.left.imu.latest_sample_timestamp_ns = t;
    auto& sample = input.left.imu.samples[0];
    sample.timestamp_ns = t;
    sample.flags = CONTROLLER_IMU_ACCELEROMETER_VALID;
    sample.specific_force_m_s2[0] = acceleration;
    sample.specific_force_m_s2[1] = 9.81f;
    hand.sequence = sequence;
    hand.timestamp_ns = hand.source_timestamp_ns = t;
    return t;
  }
  RuntimeControllerStateFrameV1 tick(int ms, bool optical, float acceleration = 0,
                                    std::optional<HmdPoseF64V1> hmd = std::nullopt) {
    const auto t = prepare(ms, acceleration);
    const auto h = optical ? std::optional(hand) : std::nullopt;
    return compose_runtime_controller_state(sequence, t, cfg, h, h, input, hmd, 9.81f, &state);
  }
};

void world_frame_and_yaw_continuity() {
  for (bool presentation_correction : {false, true}) {
    Scenario s;
    auto& m = s.cfg.left_imu_motion;
    m.hold_lost_ms = 30;
    m.prediction_window_mode = true;
    m.yaw_correction_enabled = presentation_correction;
    m.yaw_correction_continuous = m.yaw_correction_on_reacquire = false;
    s.hand.left.controller_qy = s.hand.left.controller_qw = std::sqrt(0.5f);
    s.tick(0, true);
    s.tick(10, true);
    RuntimeControllerStateFrameV1 out{};
    for (int t = 20; t <= 250; t += 10) out = s.tick(t, false, 1);
    near(out.left.position[0], 0, "IMU acceleration used provider X as world X");
    near(out.left.position[2], -0.02205f, "IMU acceleration not aligned with XR yaw");
    require(s.state.left.acceleration_world_yaw_valid, "physical world alignment missing");
    near(s.state.left.acceleration_world_yaw_rad, kPi / 2, "physical yaw alignment wrong");
    if (presentation_correction) near(out.left.orientation_xyzw[1], std::sqrt(0.5f), "presentation yaw changed");

    // A presentation blend or an ungated candidate cannot rotate the active
    // physical trajectory, nor can dropped delivery invalidate its AHRS frame.
    s.state.left.yaw_correction_valid = s.state.left.yaw_blend_active = true;
    s.state.left.yaw_blend_start_ns = kStart + 250'000'000;
    s.state.left.yaw_blend_from_rad = kPi / 2;
    s.state.left.yaw_blend_to_rad = 0;
    s.state.left.yaw_blend_duration_ms = 100;
    const auto t = s.prepare(300, 1);
    auto rejected = s.hand;
    rejected.left.controller_qy = 0;
    rejected.left.controller_qw = 1;
    compose_runtime_controller_state(s.sequence, t, s.cfg, std::nullopt, rejected,
                                    s.input, std::nullopt, 9.81f, &s.state);
    near(s.state.left.acceleration_world_yaw_rad, kPi / 2, "rejected optical candidate rotated acceleration");
    reset_runtime_controller_imu_prediction(s.state.left);
    require(!s.state.left.has_position_anchor && s.state.left.acceleration_world_yaw_valid,
            "delivery reset discarded physical frame or retained old motion");
    near(s.state.left.acceleration_world_yaw_rad, kPi / 2, "delivery reset changed physical yaw");
    s.input.left.imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = 2;
    s.hand.left.controller_qy = 0;
    s.hand.left.controller_qw = 1;
    s.tick(310, true);
    near(s.state.left.acceleration_world_yaw_rad, 0, "AHRS restart retained old physical yaw");
  }
}

void sample_orientation_matches_force_time() {
  for (bool gyro_available : {false, true}) {
    Scenario s;
    s.tick(0, true);
    s.prepare(20);
    auto& imu = s.input.left.imu;
    imu.orientation_xyzw[2] = std::sin(kPi / 36);
    imu.orientation_xyzw[3] = std::cos(kPi / 36);
    imu.sample_count = 2;
    imu.samples[0].timestamp_ns = kStart + 10'000'000;
    imu.samples[1] = imu.samples[0];
    imu.samples[1].timestamp_ns = kStart + 20'000'000;
    imu.samples[1].specific_force_m_s2[0] = 9.81f * std::sin(kPi / 18);
    imu.samples[1].specific_force_m_s2[1] = 9.81f * std::cos(kPi / 18);
    if (gyro_available) {
      for (auto& sample : imu.samples) {
        sample.flags |= CONTROLLER_IMU_GYROSCOPE_VALID;
        sample.angular_velocity_rad_s[2] = (kPi / 18) / 0.01f;
      }
    }
    const auto before = s.state;
    const auto out = compose_runtime_controller_state(s.sequence, kStart + 20'000'000, s.cfg,
        std::nullopt, std::nullopt, s.input, std::nullopt, 9.81f, &s.state);
    near(out.left.position[0], 0, "pure rotation produced translation", 1e-7f);
    near(out.left.linear_velocity[0], 0, "pure rotation produced velocity", 1e-6f);
    if (gyro_available) {
      // Reordering input records must still reconstruct the same times.
      s.state = before;
      std::swap(imu.samples[0], imu.samples[1]);
      const auto reordered = compose_runtime_controller_state(s.sequence, kStart + 20'000'000, s.cfg,
          std::nullopt, std::nullopt, s.input, std::nullopt, 9.81f, &s.state);
      near(reordered.left.position[0], out.left.position[0], "unordered batch changed integration", 1e-7f);
    }
  }
  // A post mounting offset must cancel with the inverse force transform.
  Scenario s;
  s.cfg.left_imu_orientation.offset_enabled = true;
  s.cfg.left_imu_orientation.offset_rotation_xyzw[0] = std::sin(0.3f);
  s.cfg.left_imu_orientation.offset_rotation_xyzw[3] = std::cos(0.3f);
  s.tick(0, true);
  const auto out = s.tick(10, false, 1);
  near(out.left.position[0], 0.00005f, "mounting offset changed physical acceleration");
  near(out.left.position[1], 0, "mounting offset created gravity leakage");
}

void stale_and_held_optical_frames() {
  Scenario s;
  s.cfg.left_imu_motion.hold_lost_ms = 30;
  s.hand.left.controller_px = 0.2f;
  s.prepare(0);
  const auto frozen = s.hand;
  xr_runtime_adapter::hand_filter::HandPoseStabilityFilter gate;
  xr_runtime_adapter::hand_filter::HandPoseStabilityFilterConfig gate_cfg;
  gate_cfg.enabled = true;
  gate_cfg.hold_lost_ms = 30;
  gate_cfg.predict_lost_ms = 600;
  gate_cfg.reacquire_blend_ms = 0;
  gate.configure(gate_cfg);
  bool predicted = false;
  RuntimeControllerStateFrameV1 out{};
  for (int ms = 0; ms <= 1500; ms += 10) {
    const auto t = s.prepare(ms, 1);
    auto frame = frozen;
    if (ms > 300) xr_runtime_adapter::mark_hand_frame_unavailable(frame, t);
    const auto filtered = gate.filter(frame);
    out = compose_runtime_controller_state(s.sequence, t, s.cfg, filtered, frame,
        s.input, std::nullopt, 9.81f, &s.state);
    predicted = predicted || s.state.left.prediction_active;
  }
  require(predicted, "stale optical stream never entered prediction");
  require(!s.state.left.has_position_anchor, "stale optical stream kept refreshing anchor");
  require((out.left.flags & RUNTIME_CONTROLLER_POSE_INVALID) != 0, "stale optical never expired");
  const auto held = xr_runtime_adapter::held_hand_frame(frozen, kStart + 200'000'000);
  require(held.left.status == 2 && (held.left.flags & HAND_POSE_VALID) != 0,
          "held pose lost validity or impersonated real optical tracking");
  require((held.left.flags & HAND_LINEAR_VELOCITY_VALID) == 0, "held pose retained stale velocity");
  HandTrackingFrameF64V1 legacy{};
  legacy.left.status = 1;
  legacy.left.flags = HAND_POSE_VALID;
  xr_runtime_adapter::mark_hand_frame_unavailable(legacy, kStart);
  require(legacy.left.status == 0 && legacy.source_timestamp_ns == kStart,
          "legacy stale stream not marked unavailable");
}

void camera_velocity_and_head_motion() {
  Scenario s;
  s.hand.left.controller_px = 0.1f;
  s.hand.left.vx = 1;
  cu::StreamTransformConfig transform;
  transform.hmd_relative.enabled = transform.hmd_relative.rotate_with_hmd_orientation = true;
  cu::V3d hmd_position{};
  cu::Qd hmd_orientation{std::sqrt(0.5), 0, std::sqrt(0.5), 0};
  const auto camera_hand = s.hand;
  cu::apply_hand_frame_transform(s.hand, transform, &hmd_position, &hmd_orientation);
  s.tick(0, true);
  const auto out = s.tick(100, false);
  near(out.left.position[0], 0, "camera velocity was not rotated by HMD");
  near(out.left.position[2], -0.2f, "world launch velocity wrong");

  HmdPoseF64V1 hmd{};
  hmd.flags = HMD_FLAG_LINEAR_VELOCITY_VALID | HMD_FLAG_ANGULAR_VELOCITY_VALID;
  hmd.vx = 0.3;
  hmd.wy = 2;
  auto hand = camera_hand;
  cu::apply_hand_frame_transform(hand, transform, &hmd_position, &hmd_orientation, true, true, &hmd);
  near(hand.left.vx, 0.1f, "moving camera translation/rotation not included");
  near(hand.left.vz, -1, "moving camera lost local velocity");
  near(hand.left.wy, 2, "moving camera angular velocity not included");

  Scenario window;
  auto& m = window.cfg.left_imu_motion;
  m.prediction_window_mode = true;
  m.hold_lost_ms = 30;
  m.reacquire_blend_ms = 180;
  auto optical_tick = [&](int ms, float x, float vx) {
    window.hand = camera_hand;
    window.hand.left.controller_px = x;
    window.hand.left.vx = vx;
    cu::apply_hand_frame_transform(window.hand, transform, &hmd_position, &hmd_orientation);
    window.tick(ms, true);
  };
  optical_tick(0, 0.1f, 0);
  optical_tick(10, 0.1f, 0);
  for (int t = 20; t <= 80; t += 10) window.tick(t, false);
  for (int t = 90; t <= 180; t += 10) optical_tick(t, 0.13f + float(t - 90) / 1000, 1);
  for (int t = 190; t <= 210; t += 10) window.tick(t, false);
  const auto lost = window.tick(220, false);
  near(lost.left.position[0], 0, "blend reintroduced camera-space velocity");
  require(window.state.left.anchor_velocity_mps[2] < 0, "blend did not use world velocity history");
}

void interrupted_reacquire_rebases_motion() {
  Scenario s;
  s.cfg.left_imu_motion.reacquire_blend_ms = 180;
  s.tick(0, true);
  for (int t = 10; t <= 200; t += 10) s.tick(t, false, 1);
  const auto before = s.tick(210, true);
  const auto after = s.tick(220, false);
  near(before.left.position[0], 0.02f, "reacquire did not start at published pose");
  near(after.left.position[0], 0.022f, "interrupted blend added old IMU deltas twice");

  Scenario frozen;
  auto& m = frozen.cfg.left_imu_motion;
  m.hold_lost_ms = 30;
  m.reacquire_blend_ms = 180;
  m.max_prediction_path_m = 0.005;
  frozen.tick(0, true);
  for (int t = 10; t <= 200; t += 10) frozen.tick(t, false, 1);
  require(frozen.state.left.prediction_freeze_reason == RuntimeControllerImuPredictionFreezeReason::MaxPredictionPath,
          "path limit did not latch");
  frozen.hand.left.controller_px = 0.01f;
  frozen.tick(210, true);
  const auto returned = frozen.tick(220, true);
  for (int t = 230; t <= 250; t += 10) frozen.tick(t, false, 1);
  const auto restarted = frozen.tick(260, false, 1);
  require(frozen.state.left.prediction_freeze_reason == RuntimeControllerImuPredictionFreezeReason::None,
          "optical return retained old freeze reason");
  require(restarted.left.position[0] > returned.left.position[0], "new loss remained frozen after reacquire");
}

void time_decay_integrates_published_velocity() {
  Scenario s;
  s.cfg.left_imu_motion.prediction_time_decay_enabled = true;
  s.tick(0, true);
  float previous = 0;
  RuntimeControllerStateFrameV1 out{};
  for (int t = 10; t <= 600; t += 10) {
    previous = out.left.position[0];
    out = s.tick(t, false, 1);
    const float seconds = float(t) / 1000;
    near(out.left.position[0], 0.5f * seconds * seconds - seconds * seconds * seconds / 1.8f,
         "decay position disagrees with integral of velocity");
    near(out.left.linear_velocity[0], seconds * (1 - seconds / 0.6f), "decay velocity wrong");
  }
  require((out.left.position[0] - previous) / 0.01f < 0.006f, "decay position still moving at full speed at timeout");
  near(out.left.position[0], 0.06f, "decay end position wrong");

  // The same timed history arriving in one runtime tick must give the same
  // result, rather than applying that tick's final decay to the whole history.
  Scenario delayed;
  delayed.cfg = s.cfg;
  delayed.tick(0, true);
  std::vector<ControllerInputV3> history;
  for (int t = 10; t <= 600; t += 10) {
    delayed.prepare(t, 1);
    history.push_back(delayed.input);
  }
  const auto batch = compose_runtime_controller_state(delayed.sequence, kStart + 600'000'000,
      delayed.cfg, std::nullopt, std::nullopt, delayed.input, std::nullopt, 9.81f, &delayed.state, &history);
  near(batch.left.position[0], out.left.position[0], "delivery batching changed decay trajectory");

  Scenario duplicate;
  duplicate.cfg = s.cfg;
  duplicate.tick(0, true);
  duplicate.tick(10, false, 1);
  const auto cursor = duplicate.state.left.last_acceleration_sample_timestamp_ns;
  const auto between = compose_runtime_controller_state(3, kStart + 20'000'000, duplicate.cfg,
      std::nullopt, std::nullopt, duplicate.input, std::nullopt, 9.81f, &duplicate.state);
  near(between.left.position[0], 0.0001472222f, "runtime tick did not propagate last IMU velocity");
  require(duplicate.state.left.last_acceleration_sample_timestamp_ns == cursor,
          "runtime tick reintegrated duplicate IMU sample");
}

void speed_distance_and_timeout_limits() {
  Scenario s;
  s.cfg.left_imu_motion.max_prediction_speed_mps = 2;
  s.hand.left.vx = 1;
  s.tick(0, true);
  float last = 0;
  for (int t = 10; t <= 600; t += 10) {
    const auto out = s.tick(t, false, 6);
    require(out.left.position[0] - last <= 0.020001f, "final position exceeded speed limit");
    require(out.left.linear_velocity[0] <= 2.00001f, "velocity exceeded speed limit");
    last = out.left.position[0];
  }
  const auto expired = s.tick(610, false);
  require((expired.left.flags & RUNTIME_CONTROLLER_POSE_INVALID) != 0, "prediction did not expire");

  Scenario distance;
  distance.cfg.left_imu_motion.prediction_distance.max_distance_m = 0.15;
  distance.hand.left.controller_px = 0.1;
  distance.hand.left.vx = 1;
  HmdPoseF64V1 hmd{};
  hmd.flags = HMD_FLAG_POSE_VALID;
  hmd.tracking_status = 2;
  distance.tick(0, true, 0, hmd);
  int freeze_ms = 10;
  for (; freeze_ms <= 100; freeze_ms += 10) {
    distance.tick(freeze_ms, false, 0, hmd);
    if (distance.state.left.prediction_freeze_reason ==
        RuntimeControllerImuPredictionFreezeReason::MaxDistance) break;
  }
  require(distance.state.left.prediction_freeze_reason == RuntimeControllerImuPredictionFreezeReason::MaxDistance,
          "distance limit did not freeze");
  const float frozen = distance.state.left.position_m[0];
  const auto recovery = distance.tick(freeze_ms + 10, false, -1, hmd);
  require(recovery.left.position[0] < frozen &&
          distance.state.left.prediction_freeze_reason == RuntimeControllerImuPredictionFreezeReason::None,
          "distance freeze did not recover inward from published pose");
}

int main() {
  static_assert(sizeof(ControllerInputV3) == 1432, "controller V3 ABI changed");
  world_frame_and_yaw_continuity();
  sample_orientation_matches_force_time();
  stale_and_held_optical_frames();
  camera_velocity_and_head_motion();
  interrupted_reacquire_rebases_motion();
  time_decay_integrates_published_velocity();
  speed_distance_and_timeout_limits();
  std::cout << "IMU prediction: world frames, sample time, stale optics, head motion, reacquire, decay and limits: OK\n";
}
