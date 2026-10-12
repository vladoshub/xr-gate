#include "coordinate_util.hpp"
#include "hand_pose_stability_filter.hpp"
#include "hand_tracking_freshness.hpp"
#include "override_controller.hpp"
#include <xr_override_controller/imu/controller_imu_processor.hpp>

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
    // Most tests below exercise prediction with a known provider->XR frame.
    // Automatic identification and its unobservable cases have separate tests.
    state.left.acceleration_world_yaw_valid = true;
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

void initialized_gravity_preserves_optical_motion() {
  for (bool right : {false, true}) {
    Scenario s;
    s.cfg.left_imu_motion.hold_lost_ms = 0;
    s.cfg.left_imu_motion.predict_lost_ms = 1000;
    s.cfg.right_orientation_source = s.cfg.left_orientation_source;
    s.cfg.right_imu_motion = s.cfg.left_imu_motion;
    xr_override_controller::imu::ControllerImuProcessor processor;
    const auto basis = cu::q_from_axis_angle(1, 0, 0, -kPi / 2);
    for (int epoch = 1; epoch <= 2; ++epoch) {
      processor.reset();
      s.state = {};
      (right ? s.state.right : s.state.left).acceleration_world_yaw_valid = true;
      const float tilt = epoch == 1 ? 2 * kPi / 3 : kPi;
      const cu::V3d force{0, 9.81 * std::sin(tilt), 9.81 * std::cos(tilt)};
      const auto converted_force = cu::q_rotate(basis, force);
      for (int ms = 0; ms <= 600; ms += 10) {
        const auto t = s.prepare(ms + epoch * 1000);
        xr_override_controller::imu::RawControllerImuSample sample{};
        sample.host_timestamp_ns = t;
        sample.sample_interval_seconds = 0.01f;
        sample.gyroscope_valid = sample.accelerometer_valid = true;
        sample.specific_force_m_s2 = {float(force.x), float(force.y), float(force.z)};
        const auto q = processor.process(sample);
        // The provider converts AHRS world +Z and sensor axes to XR +Y.
        const auto orientation = cu::q_mul(cu::q_mul(basis, cu::Qd{q.w, q.x, q.y, q.z}), cu::q_conj(basis));
        auto input = s.input;
        input.left.imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = epoch;
        input.left.imu.orientation_xyzw[0] = orientation.x;
        input.left.imu.orientation_xyzw[1] = orientation.y;
        input.left.imu.orientation_xyzw[2] = orientation.z;
        input.left.imu.orientation_xyzw[3] = orientation.w;
        auto& force_sample = input.left.imu.samples[0];
        force_sample.specific_force_m_s2[0] = converted_force.x;
        force_sample.specific_force_m_s2[1] = converted_force.y;
        force_sample.specific_force_m_s2[2] = converted_force.z;
        auto hand = s.hand;
        hand.left.vz = 0.6f;
        if (right) {
          input.right = input.left; input.left = {};
          hand.right = hand.left; hand.left = {};
          hand.flags = HAND_FLAG_RIGHT_VALID;
        }
        const auto optical = ms == 0 ? std::optional(hand) : std::nullopt;
        const auto output = compose_runtime_controller_state(s.sequence, t, s.cfg,
            optical, optical, input, std::nullopt, 9.81f, &s.state);
        const auto& side = right ? output.right : output.left;
        near(side.position[0], 0, "tilted startup invented lateral motion", 0.001f);
        near(side.position[1], 0, "tilted startup invented vertical motion", 0.001f);
        near(side.position[2], 0.6f * ms / 1000,
             "tilted startup reversed optical launch velocity", 0.001f);
      }
    }
  }
}

void rigid_grip_does_not_change_world_alignment() {
  for (bool right : {false, true}) {
    Scenario s;
    s.state.left.acceleration_world_yaw_valid = false;
    if (right) {
      s.cfg.right_orientation_source = s.cfg.left_orientation_source;
      s.cfg.right_imu_motion = s.cfg.left_imu_motion;
    }
    const float world_yaw = right ? -0.95f : 0.75f;
    const auto frame = cu::q_from_axis_angle(0, 1, 0, world_yaw);
    const auto mount = cu::q_mul(
        cu::q_from_axis_angle(1, 0, 0, right ? -1.3 : 1.7),
        cu::q_from_axis_angle(0, 0, 1, right ? -0.7 : 1.4));
    cu::Qd provider;
    const auto tick = [&](int ms, bool tracked) {
      const auto now = s.prepare(ms);
      auto input = s.input;
      auto hand = s.hand;
      if (tracked) {
        provider = cu::q_mul(cu::q_from_axis_angle(0, 1, 0, 0.001 * ms),
            cu::q_mul(cu::q_from_axis_angle(1, 0, 0, 0.8 + 0.85 * std::sin(ms / 170.0)),
                      cu::q_from_axis_angle(0, 0, 1, 0.35 * std::sin(ms / 130.0))));
      }
      const auto optical = cu::q_mul(cu::q_mul(frame, provider), mount);
      auto& imu = input.left.imu;
      imu.orientation_xyzw[0] = provider.x;
      imu.orientation_xyzw[1] = provider.y;
      imu.orientation_xyzw[2] = provider.z;
      imu.orientation_xyzw[3] = provider.w;
      const auto force = cu::q_rotate(cu::q_conj(provider), {tracked ? 0.0 : 1.0, 9.81, 0});
      imu.samples[0].specific_force_m_s2[0] = force.x;
      imu.samples[0].specific_force_m_s2[1] = force.y;
      imu.samples[0].specific_force_m_s2[2] = force.z;
      hand.left.controller_qx = optical.x;
      hand.left.controller_qy = optical.y;
      hand.left.controller_qz = optical.z;
      hand.left.controller_qw = optical.w;
      if (right) {
        std::swap(input.left, input.right);
        std::swap(hand.left, hand.right);
        hand.flags = HAND_FLAG_RIGHT_VALID;
        hand.right.handedness = 2;
      }
      const auto h = tracked ? std::optional(hand) : std::nullopt;
      return compose_runtime_controller_state(s.sequence, now, s.cfg, h, h,
          input, std::nullopt, 9.81f, &s.state);
    };
    for (int ms = 0; ms <= 600; ms += 20) {
      tick(ms, true);
      if (ms < 200) continue;
      const auto& state = right ? s.state.right : s.state.left;
      require(state.acceleration_world_yaw_valid, "rigid wrist motion did not identify world yaw");
      const float error = std::remainder(state.acceleration_world_yaw_rad - world_yaw, 2 * kPi);
      if (std::abs(error) > 1e-4f) {
        std::cerr << (right ? "right" : "left") << " physical yaw at " << ms << "ms: expected="
                  << world_yaw << " actual=" << state.acceleration_world_yaw_rad << '\n';
      }
      near(error, 0, "fixed grip changed physical world yaw", 1e-4f);
    }
    auto retained = right ? s.state.right : s.state.left;
    const auto observations = retained.acceleration_world_alignment.observation_count();
    reset_runtime_controller_imu_prediction(retained);
    require(retained.acceleration_world_yaw_valid &&
            retained.acceleration_world_alignment.observation_count() == observations,
            "delivery reset discarded orientation calibration history");
    RuntimeControllerStateFrameV1 out;
    for (int ms = 610; ms <= 800; ms += 10) out = tick(ms, false);
    const auto& side = right ? out.right : out.left;
    near(side.position[0], 0.02f * std::cos(world_yaw), "grip reversed physical prediction X");
    near(side.position[2], -0.02f * std::sin(world_yaw), "grip reversed physical prediction Z");

    // A retained frame can be far from the new estimate after a long period
    // of AHRS yaw drift. It must recover while real optical poses are present.
    auto& stale_frame = right ? s.state.right : s.state.left;
    stale_frame.acceleration_world_yaw_rad = world_yaw - 1;
    for (int ms = 820; ms <= 2600; ms += 20) tick(ms, true);
    near(stale_frame.acceleration_world_yaw_rad, world_yaw,
         "large coherent yaw drift left physical alignment permanently stale", 0.01f);
  }
}

void motion_alignment_uses_source_time_and_observable_rotations() {
  using xr_runtime_adapter::imu_alignment::WorldYawAlignment;
  const auto provider_at = [](int ms) {
    return cu::q_mul(cu::q_from_axis_angle(0, 1, 0, ms * 0.0015),
        cu::q_mul(cu::q_from_axis_angle(1, 0, 0, 0.8 * std::sin(ms / 160.0)),
                  cu::q_from_axis_angle(0, 0, 1, 0.6 * std::sin(ms / 190.0))));
  };
  const auto observe_imu = [](WorldYawAlignment& a, int ms, cu::Qd q, float sign = 1) {
    const float xyzw[] = {float(q.x)*sign, float(q.y)*sign, float(q.z)*sign, float(q.w)*sign};
    a.observe_imu(kStart + uint64_t(ms)*1'000'000, xyzw);
  };
  const auto observe_optical = [](WorldYawAlignment& a, int ms, cu::Qd q, float& yaw, float sign = 1) {
    const float xyzw[] = {float(q.x)*sign, float(q.y)*sign, float(q.z)*sign, float(q.w)*sign};
    return a.observe_optical(kStart + uint64_t(ms)*1'000'000, xyzw, yaw);
  };
  WorldYawAlignment timed;
  const float expected_yaw = 1.1f;
  const auto frame = cu::q_from_axis_angle(0, 1, 0, expected_yaw);
  const auto mount = cu::q_mul(cu::q_from_axis_angle(1, 0, 0, 2.2),
                              cu::q_from_axis_angle(0, 1, 0, -1.8));
  int imu_ms = 0, identified = 0;
  float yaw = 0;
  for (int optical_ms = 0; optical_ms <= 1100; optical_ms += 37) {
    // Runtime receives the image 45 ms after exposure; the newest IMU pose
    // is already ahead of it. Interpolate the historical exposure-time pose.
    for (; imu_ms <= optical_ms + 45; imu_ms += 10) {
      observe_imu(timed, imu_ms, provider_at(imu_ms), imu_ms % 20 ? -1 : 1);
    }
    const auto optical = cu::q_mul(cu::q_mul(frame, provider_at(optical_ms)), mount);
    if (observe_optical(timed, optical_ms, optical, yaw, optical_ms % 2 ? -1 : 1)) {
      ++identified;
      near(yaw, expected_yaw, "calibration used latest IMU instead of optical source time", 0.002f);
    }
  }
  require(identified > 10, "delayed optical stream failed to identify physical yaw");
  const auto count = timed.observation_count();
  require(!observe_optical(timed, 1073, {}, yaw) && !observe_optical(timed, 1000, {}, yaw) &&
          timed.observation_count() == count, "duplicate/older optical pose altered calibration");
  timed.clear_optical_motion();
  require(timed.observation_count() == 0 && !observe_optical(timed, 1073, {}, yaw),
          "lost optics reused a cached rotation to start calibration");

  WorldYawAlignment delayed_imu;
  imu_ms = 0;
  identified = 0;
  for (int optical_ms = 0; optical_ms <= 1100; optical_ms += 37) {
    for (; imu_ms <= optical_ms - 80; imu_ms += 10) {
      observe_imu(delayed_imu, imu_ms, provider_at(imu_ms));
    }
    const auto optical = cu::q_mul(cu::q_mul(frame, provider_at(optical_ms)), mount);
    if (observe_optical(delayed_imu, optical_ms, optical, yaw)) {
      ++identified;
      near(yaw, expected_yaw, "late IMU delivery changed physical frame", 0.002f);
    }
  }
  require(identified > 8, "late IMU delivery prevented queued optical calibration");

  for (bool yaw_only : {false, true}) {
    WorldYawAlignment unobservable;
    for (int ms = 0; ms <= 500; ms += 20) {
      const auto provider = yaw_only ? cu::q_from_axis_angle(0, 1, 0, ms * 0.002) : cu::Qd{};
      observe_imu(unobservable, ms, provider);
      const auto optical = cu::q_mul(cu::q_mul(frame, provider), mount);
      require(!observe_optical(unobservable, ms, optical, yaw),
              "static/yaw-only poses invented a physical frame");
    }
  }

  WorldYawAlignment inconsistent;
  for (int ms = 0; ms <= 600; ms += 20) {
    observe_imu(inconsistent, ms, provider_at(ms));
    require(!observe_optical(inconsistent, ms, mount, yaw),
            "uncorrelated optical and IMU rotations identified physical yaw");
  }
  WorldYawAlignment gap;
  observe_imu(gap, 0, {});
  observe_imu(gap, 120, provider_at(120));
  require(!observe_optical(gap, 60, provider_at(60), yaw) && gap.observation_count() == 0,
          "calibration interpolated across unobserved IMU gap");

  Scenario velocity_only;
  velocity_only.state.left.acceleration_world_yaw_valid = false;
  velocity_only.cfg.left_imu_motion.prediction_window_mode = true;
  velocity_only.tick(0, true);
  velocity_only.hand.left.controller_px = 0.01f;
  velocity_only.tick(20, true);
  const auto predicted = velocity_only.tick(30, false, 12);
  require(!velocity_only.state.left.acceleration_world_yaw_valid,
          "unobservable poses unexpectedly identified physical frame");
  near(predicted.left.position[0], 0.015f, "unknown frame stopped optical-velocity prediction");
  near(predicted.left.linear_velocity[0], 0.5f, "unknown frame used arbitrary acceleration");
}

void rejected_new_rotations_do_not_reapply_old_calibration_evidence() {
  using xr_runtime_adapter::imu_alignment::WorldYawAlignment;
  WorldYawAlignment alignment;
  const auto world = cu::q_from_axis_angle(0, 1, 0, 0.6);
  const auto mount = cu::q_from_axis_angle(0, 0, 1, 1.2);
  float yaw = 0;
  auto observe = [&](int ms, bool corrupted) {
    const auto imu = cu::q_from_axis_angle(1, 0, 0, ms * 0.003);
    auto optical = cu::q_mul(cu::q_mul(world, imu), mount);
    if (corrupted) optical = cu::q_mul(cu::q_from_axis_angle(0, 1, 0, kPi), mount);
    const float iq[] = {float(imu.x), float(imu.y), float(imu.z), float(imu.w)};
    const float oq[] = {float(optical.x), float(optical.y), float(optical.z), float(optical.w)};
    const auto ns = kStart + uint64_t(ms) * 1'000'000;
    alignment.observe_imu(ns, iq);
    return alignment.observe_optical(ns, oq, yaw);
  };
  bool calibrated = false;
  for (int ms = 0; ms <= 220; ms += 20) calibrated = observe(ms, false) || calibrated;
  require(calibrated, "consistent rotations did not establish calibration");
  near(yaw, 0.6f, "known rigid rotation calibrated wrong yaw");
  for (int ms = 240; ms <= 420; ms += 20) {
    require(!observe(ms, true),
            "rejected optical rotations repeatedly reused old calibration pairs");
  }
  require(observe(440, false), "fresh corresponding rotations did not resume calibration");
  near(yaw, 0.6f, "rejected rotations changed the resumed physical frame");
}

void optical_hold_retains_measured_velocity_change() {
  for (bool right : {false, true}) {
    // Regular arrival, a queued multi-sample batch, and one sample interval
    // spanning the hold boundary must describe the same physical motion.
    for (int delivery = 0; delivery < 3; ++delivery) {
      Scenario s;
      s.cfg.left_imu_motion.hold_lost_ms = 30;
      s.cfg.right_imu_motion = s.cfg.left_imu_motion;
      s.cfg.right_orientation_source = s.cfg.left_orientation_source;
      s.state.right.acceleration_world_yaw_valid = true;
      auto tick = [&](int ms, bool optical, bool all_samples = false) {
        const auto ns = s.prepare(ms, 4);
        auto input = s.input;
        auto hand = s.hand;
        if (all_samples) {
          input.left.imu.sample_count = 4;
          for (uint32_t i = 0; i < 4; ++i) {
            auto& sample = input.left.imu.samples[i];
            sample.timestamp_ns = kStart + (i + 1) * 10'000'000ULL;
            sample.flags = CONTROLLER_IMU_ACCELEROMETER_VALID | CONTROLLER_IMU_GYROSCOPE_VALID;
            sample.specific_force_m_s2[0] = 4;
            sample.specific_force_m_s2[1] = 9.81f;
          }
        }
        if (right) {
          input.right = input.left; input.left = {};
          hand.right = hand.left; hand.left = {};
          hand.flags = HAND_FLAG_RIGHT_VALID;
          hand.right.handedness = 2;
        }
        const auto h = optical ? std::optional(hand) : std::nullopt;
        return compose_runtime_controller_state(s.sequence, ns, s.cfg, h, h,
            input, std::nullopt, 9.81f, &s.state);
      };
      tick(0, true);
      if (delivery == 0) {
        for (int ms = 10; ms <= 30; ms += 10) {
          const auto out = tick(ms, false);
          const auto& side = right ? out.right : out.left;
          near(side.position[0], 0, "measured acceleration moved held output");
          near(side.linear_velocity[0], 0, "hold exposed velocity to downstream prediction");
        }
      }
      const auto out = tick(40, false, delivery == 1);
      const auto& side = right ? out.right : out.left;
      // Constant 4 m/s2 for 40 ms: actual velocity is 0.16 m/s. Position stays
      // held for 30 ms, then advances by 0.12 * 0.01 + 0.5 * 4 * 0.01^2.
      near(side.linear_velocity[0], 0.16f, "hold discarded measured velocity change");
      near(side.position[0], 0.0014f, "prediction restarted velocity after optical hold");
    }
  }
}

void world_frame_and_yaw_continuity() {
  for (bool presentation_correction : {false, true}) {
    Scenario s;
    s.state.left.acceleration_world_yaw_rad = kPi / 2;
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
    // The 30 ms hold retains 0.03 m/s of acceleration-induced velocity.
    // Over the following 210 ms: 0.03 * 0.21 + 0.5 * 0.21^2 = 0.02835 m.
    near(out.left.position[2], -0.02835f, "IMU acceleration not aligned with XR yaw");
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
    require(!s.state.left.acceleration_world_yaw_valid,
            "AHRS restart retained old physical yaw or identified it from one pose");
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
  // Presentation offsets in either multiplication order must not rotate the
  // physical force or change its gravity removal.
  for (bool pre_multiply : {false, true}) {
    Scenario s;
    s.cfg.left_imu_orientation.offset_enabled = true;
    s.cfg.left_imu_orientation.offset_pre_multiply = pre_multiply;
    s.cfg.left_imu_orientation.offset_rotation_xyzw[0] = std::sin(0.3f);
    s.cfg.left_imu_orientation.offset_rotation_xyzw[3] = std::cos(0.3f);
    s.tick(0, true);
    const auto out = s.tick(10, false, 1);
    near(out.left.position[0], 0.00005f, "mounting offset changed physical acceleration");
    near(out.left.position[1], 0, "mounting offset created gravity leakage");
  }
}

void physical_yaw_does_not_flip_at_vertical() {
  // Optical and inertial pitch can differ by a few degrees, especially near
  // the edge of the camera view. Crossing vertical must not reverse the world
  // acceleration. Exercise both sides and both quaternion representations.
  for (bool right : {false, true}) {
    for (float pitch_deg : {-91.0f, -90.0f, -89.0f, 89.0f, 90.0f, 91.0f}) {
      for (float world_yaw : {-kPi, -0.6f, 0.0f, 0.6f, kPi}) {
        for (float quaternion_sign : {-1.0f, 1.0f}) {
          Scenario s;
          auto& known_frame = right ? s.state.right : s.state.left;
          known_frame.acceleration_world_yaw_valid = true;
          known_frame.acceleration_world_yaw_rad = world_yaw;
          if (right) {
            s.cfg.right_orientation_source = s.cfg.left_orientation_source;
            s.cfg.right_imu_motion = s.cfg.left_imu_motion;
          }
          const auto provider = cu::q_from_axis_angle(1, 0, 0, pitch_deg * kPi / 180);
          const auto optical = cu::q_mul(cu::q_from_axis_angle(0, 1, 0, world_yaw),
              cu::q_from_axis_angle(1, 0, 0, (pitch_deg + 2) * kPi / 180));
          const auto tick = [&](int ms, bool tracked) {
            const auto now = s.prepare(ms);
            auto input = s.input;
            auto hand = s.hand;
            auto& imu = input.left.imu;
            imu.orientation_xyzw[0] = provider.x;
            imu.orientation_xyzw[1] = provider.y;
            imu.orientation_xyzw[2] = provider.z;
            imu.orientation_xyzw[3] = provider.w;
            const auto force = cu::q_rotate(cu::q_conj(provider), {tracked ? 0.0 : 1.0, 9.81, 0});
            imu.samples[0].specific_force_m_s2[0] = force.x;
            imu.samples[0].specific_force_m_s2[1] = force.y;
            imu.samples[0].specific_force_m_s2[2] = force.z;
            hand.left.controller_qx = optical.x * quaternion_sign;
            hand.left.controller_qy = optical.y * quaternion_sign;
            hand.left.controller_qz = optical.z * quaternion_sign;
            hand.left.controller_qw = optical.w * quaternion_sign;
            if (right) {
              std::swap(input.left, input.right);
              std::swap(hand.left, hand.right);
              hand.flags = HAND_FLAG_RIGHT_VALID;
              hand.right.handedness = 2;
            }
            const auto h = tracked ? std::optional(hand) : std::nullopt;
            return compose_runtime_controller_state(s.sequence, now, s.cfg, h, h,
                input, std::nullopt, 9.81f, &s.state);
          };
          tick(0, true);
          tick(10, true);
          RuntimeControllerStateFrameV1 out{};
          for (int ms = 20; ms <= 210; ms += 10) out = tick(ms, false);
          const auto& side = right ? out.right : out.left;
          const auto& state = right ? s.state.right : s.state.left;
          require(state.acceleration_world_yaw_valid, "vertical pose prevented physical alignment");
          near(std::remainder(state.acceleration_world_yaw_rad - world_yaw, 2 * kPi),
               0, "pitch mismatch reversed physical yaw");
          near(side.position[0], 0.02f * std::cos(world_yaw), "vertical pose reversed prediction X");
          near(side.position[1], 0, "vertical pose leaked gravity into position");
          near(side.position[2], -0.02f * std::sin(world_yaw), "vertical pose reversed prediction Z");
        }
      }
    }
  }

  Scenario ambiguous;
  ambiguous.state.left.acceleration_world_yaw_rad = 0.6f;
  ambiguous.hand.left.controller_qy = std::sin(0.3f);
  ambiguous.hand.left.controller_qw = std::cos(0.3f);
  ambiguous.tick(0, true);
  // An optical half-turn about X is incompatible with yaw-only alignment.
  // Retain the established frame rather than inventing a direction.
  ambiguous.hand.left.controller_qx = 1;
  ambiguous.hand.left.controller_qy = 0;
  ambiguous.hand.left.controller_qw = 0;
  ambiguous.tick(10, true);
  near(ambiguous.state.left.acceleration_world_yaw_rad, 0.6f,
       "ambiguous optical rotation replaced trusted physical yaw");

  Scenario unaligned;
  unaligned.state.left.acceleration_world_yaw_valid = false;
  unaligned.hand.left.controller_qx = 1;
  unaligned.hand.left.controller_qw = 0;
  unaligned.tick(0, true);
  const auto out = unaligned.tick(10, false, 1);
  require(!unaligned.state.left.acceleration_world_yaw_valid,
          "ambiguous optical rotation invented initial physical yaw");
  near(out.left.position[0], 0, "unaligned force invented translation");
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

void distant_reacquire_respects_speed_and_finishes_without_snap() {
  for (bool right : {false, true}) {
    Scenario s;
    auto& m = s.cfg.left_imu_motion;
    m.max_prediction_speed_mps = 2;
    m.reacquire_blend_ms = 180;
    s.cfg.right_imu_motion = m;
    s.cfg.right_orientation_source = s.cfg.left_orientation_source;
    s.state.right.acceleration_world_yaw_valid = true;
    s.hand.left.controller_px = -0.563f;
    s.hand.left.controller_py = 2.112f;
    s.hand.left.controller_pz = -0.579f;
    auto tick = [&](int ms, bool optical) {
      const auto t = s.prepare(ms);
      auto input = s.input;
      auto hand = s.hand;
      if (right) {
        input.right = input.left; input.left = {};
        hand.right = hand.left; hand.left = {};
        hand.flags = HAND_FLAG_RIGHT_VALID;
        hand.right.handedness = 2;
      }
      const auto h = optical ? std::optional(hand) : std::nullopt;
      return compose_runtime_controller_state(s.sequence, t, s.cfg, h, h,
          input, std::nullopt, 9.81f, &s.state);
    };
    auto out = tick(0, true);
    for (int ms = 10; ms <= 300; ms += 10) out = tick(ms, false);
    s.hand.left.controller_px = -0.126f;
    s.hand.left.controller_py = 1.808f;
    s.hand.left.controller_pz = -0.860f;
    auto previous = right ? out.right : out.left;
    for (int ms = 310; ms <= 900; ms += 10) {
      // A moving optical target must also be followed without a snap at the
      // nominal 180 ms deadline. This reproduces the roughly 60 cm mismatch
      // of the left controller's first reacquire in the hardware log.
      s.hand.left.controller_px += 0.002f;
      out = tick(ms, true);
      const auto& side = right ? out.right : out.left;
      float step_sq = 0, velocity_sq = 0;
      for (int axis = 0; axis < 3; ++axis) {
        const float step = side.position[axis] - previous.position[axis];
        step_sq += step * step;
        velocity_sq += side.linear_velocity[axis] * side.linear_velocity[axis];
      }
      require(std::sqrt(step_sq) <= 0.02001f,
              "optical reacquire exceeded configured output speed");
      require(std::sqrt(velocity_sq) <= 2.00001f,
              "reacquire published velocity above output speed");
      const auto& state = right ? s.state.right : s.state.left;
      if (ms == 490) require(state.reacquire_blend_active,
          "reacquire snapped to a distant target at the nominal deadline");
      previous = side;
    }
    const auto& state = right ? s.state.right : s.state.left;
    const auto& side = right ? out.right : out.left;
    require(!state.reacquire_blend_active, "rate-limited reacquire never completed");
    near(side.position[0], s.hand.left.controller_px, "reacquire missed moving target");
    near(side.position[1], s.hand.left.controller_py, "reacquire missed target height");
    near(side.position[2], s.hand.left.controller_pz, "reacquire missed target depth");
  }

  Scenario interrupted;
  interrupted.cfg.left_imu_motion.max_prediction_speed_mps = 2;
  interrupted.cfg.left_imu_motion.reacquire_blend_ms = 180;
  interrupted.tick(0, true);
  for (int ms = 10; ms <= 200; ms += 10) interrupted.tick(ms, false);
  interrupted.hand.left.controller_px = 0.6f;
  interrupted.tick(210, true);
  RuntimeControllerStateFrameV1 before;
  for (int ms = 220; ms <= 400; ms += 10) before = interrupted.tick(ms, true);
  require(interrupted.state.left.reacquire_blend_active,
          "distant correction ended before reaching its target");
  const auto after = interrupted.tick(410, false);
  require(after.left.position[0] >= before.left.position[0] &&
          after.left.position[0] - before.left.position[0] <= 0.02001f,
          "loss during extended reacquire did not continue from published pose");
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
  initialized_gravity_preserves_optical_motion();
  rigid_grip_does_not_change_world_alignment();
  motion_alignment_uses_source_time_and_observable_rotations();
  rejected_new_rotations_do_not_reapply_old_calibration_evidence();
  optical_hold_retains_measured_velocity_change();
  world_frame_and_yaw_continuity();
  sample_orientation_matches_force_time();
  physical_yaw_does_not_flip_at_vertical();
  stale_and_held_optical_frames();
  camera_velocity_and_head_motion();
  interrupted_reacquire_rebases_motion();
  distant_reacquire_respects_speed_and_finishes_without_snap();
  time_decay_integrates_published_velocity();
  speed_distance_and_timeout_limits();
  std::cout << "IMU prediction: rigid grips, timed rotation calibration, velocity fallback, sample time, stale optics, reacquire and limits: OK\n";
}
