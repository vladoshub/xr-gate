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

bool observe_startup_yaw(xr_runtime_adapter::imu_alignment::YawStabilityConfirmation& check,
                        int ms, std::optional<float> estimate, cu::Qd provider,
                        cu::Qd optical, float duration = 4000) {
  const float iq[] = {float(provider.x), float(provider.y), float(provider.z), float(provider.w)};
  const float oq[] = {float(optical.x), float(optical.y), float(optical.z), float(optical.w)};
  return check.observe(kStart + uint64_t(ms)*1'000'000, iq, oq, estimate, duration, 8);
}

void startup_yaw_waits_for_stability_without_freezing_real_motion() {
  using xr_runtime_adapter::imu_alignment::YawStabilityConfirmation;
  const float yaw = 0.35f;
  const auto world = cu::q_from_axis_angle(0, 1, 0, yaw);
  const auto mount = cu::q_from_axis_angle(0, 0, 1, 0.7);
  for (int duration : {3000, 4000, 5000}) {
    for (bool moving : {false, true}) {
      YawStabilityConfirmation check;
      for (int ms = 0; ms <= duration; ms += 50) {
        const auto provider = moving ? cu::q_mul(cu::q_from_axis_angle(0, 1, 0, ms*0.002),
            cu::q_from_axis_angle(1, 0, 0, 0.6*std::sin(ms/150.0))) : cu::Qd{};
        auto optical = cu::q_mul(cu::q_mul(world, provider), mount);
        if (ms % 100) {
          optical.w = -optical.w; optical.x = -optical.x;
          optical.y = -optical.y; optical.z = -optical.z;
        }
        const bool ready = observe_startup_yaw(check, ms, ms == 0 ? std::optional(yaw) : std::nullopt,
                                               provider, optical, duration);
        require(ready == (ms == duration), "startup yaw confirmed early or rejected a stable moving grip");
      }
      near(check.candidate_rad(), yaw, "quiet confirmation invented a different physical yaw");
    }
  }
  YawStabilityConfirmation unobservable;
  for (int ms = 0; ms <= 6000; ms += 50) {
    require(!observe_startup_yaw(unobservable, ms, std::nullopt, {}, world),
            "waiting on a static pose invented a yaw estimate");
  }
  require(!unobservable.pending(), "unobservable poses started the confirmation clock");

  YawStabilityConfirmation wrapped;
  const auto opposite = cu::q_from_axis_angle(0, 1, 0, kPi);
  for (int ms = 0; ms <= 4000; ms += 50) {
    const float candidate = ms % 100 ? -kPi + 0.01f : kPi - 0.01f;
    const bool ready = observe_startup_yaw(wrapped, ms, candidate, {}, opposite);
    require(ready == (ms == 4000), "yaw wraparound/sign changed a stable confirmation interval");
  }
}

void startup_yaw_restarts_for_jumps_gaps_and_cached_frames() {
  using xr_runtime_adapter::imu_alignment::YawStabilityConfirmation;
  for (bool pose_jump : {false, true}) {
    YawStabilityConfirmation check;
    for (int ms = 0; ms <= 5500; ms += 50) {
      const float estimate = pose_jump ? 0.0f : ms >= 1500 ? 0.3f : 0.0f;
      const auto optical = pose_jump && ms >= 1500 ? cu::q_from_axis_angle(1, 0, 0, 0.3) : cu::Qd{};
      const bool ready = observe_startup_yaw(check, ms, estimate, {}, optical);
      require(ready == (ms == 5500), "yaw/optical pose jump did not restart the full confirmation interval");
      if (ms == 1500) near(check.elapsed_ms(), 0, "jump retained pre-jump waiting time");
    }
  }
  YawStabilityConfirmation drifting;
  for (int ms = 0; ms <= 6000; ms += 50) {
    require(!observe_startup_yaw(drifting, ms, ms*0.0001f, {}, {}),
            "small consecutive steps hid an unstable accumulated yaw range");
  }
  YawStabilityConfirmation cached;
  for (int ms = 0; ms <= 3950; ms += 50) {
    require(!observe_startup_yaw(cached, ms, 0, {}, {}), "startup confirmed before four seconds");
  }
  for (int i = 0; i < 1000; ++i) {
    require(!observe_startup_yaw(cached, 3950, 0, {}, {}) &&
            !observe_startup_yaw(cached, 3900, 0, {}, {}), "cached/older pose completed confirmation");
  }
  near(cached.elapsed_ms(), 3950, "runtime ticks advanced the source-time clock");
  require(!observe_startup_yaw(cached, 4200, std::nullopt, {}, {}) && !cached.pending(),
          "unobserved interval retained the initial yaw candidate");
  for (int ms = 4250; ms <= 8250; ms += 50) {
    require(observe_startup_yaw(cached, ms, ms == 4250 ? std::optional(0.0f) : std::nullopt, {}, {}) ==
            (ms == 8250), "gap recovery did not require a complete new interval");
  }
  cached.reset();
  require(!observe_startup_yaw(cached, 8300, std::nullopt, {}, {}) && !cached.pending(),
          "reset reused an unsupported pending candidate");
  YawStabilityConfirmation disabled;
  require(observe_startup_yaw(disabled, 0, 0.4f, {}, cu::q_from_axis_angle(0, 1, 0, 0.4), 0),
          "zero duration did not preserve immediate initialization");
}

void first_yaw_confirmation_controls_force_integration_on_both_sides(bool refine_translation = false) {
  for (bool right : {false, true}) {
    for (bool interrupted : {false, true}) {
      Scenario s;
      s.state.left.acceleration_world_yaw_valid = false;
      auto& m = s.cfg.left_imu_motion;
      m.translation_yaw_check_enabled = refine_translation;
      m.translation_yaw_refinement_enabled = refine_translation;
      m.predict_lost_ms = 1200;
      s.cfg.right_orientation_source = s.cfg.left_orientation_source;
      s.cfg.right_imu_motion = m;
      auto& state = right ? s.state.right : s.state.left;
      const float yaw = 0.35f;
      const auto world = cu::q_from_axis_angle(0, 1, 0, yaw);
      const auto mount = cu::q_from_axis_angle(0, 0, 1, 0.7);
      int waiting_started = -1;
      auto tick = [&](int ms, bool optical, float acceleration = 0, uint32_t epoch = 1,
                      uint32_t dropped = 0) {
        const auto ns = s.prepare(ms);
        auto input = s.input; auto hand = s.hand;
        const double angle = interrupted && ms >= 1200
            ? 0.24 + std::min(400, ms - 1200)*0.0008 : std::min(ms, 300)*0.0008;
        const auto provider = cu::q_from_axis_angle(1, 0, 0, angle);
        const auto optical_q = cu::q_mul(cu::q_mul(world, provider), mount);
        auto& imu = input.left.imu;
        imu.orientation_xyzw[0] = provider.x; imu.orientation_xyzw[1] = provider.y;
        imu.orientation_xyzw[2] = provider.z; imu.orientation_xyzw[3] = provider.w;
        imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = epoch;
        imu.reserved[CONTROLLER_IMU_DIAG_DROPPED_BATCHES] = dropped;
        const auto force = cu::q_rotate(cu::q_conj(provider), cu::V3d{acceleration, 9.81, 0});
        imu.samples[0].specific_force_m_s2[0] = force.x;
        imu.samples[0].specific_force_m_s2[1] = force.y;
        imu.samples[0].specific_force_m_s2[2] = force.z;
        hand.left.controller_qx = optical_q.x; hand.left.controller_qy = optical_q.y;
        hand.left.controller_qz = optical_q.z; hand.left.controller_qw = optical_q.w;
        if (right) {
          input.right = input.left; input.left = {};
          hand.right = hand.left; hand.left = {};
          hand.flags = HAND_FLAG_RIGHT_VALID; hand.right.handedness = 2;
        }
        const auto h = optical ? std::optional(hand) : std::nullopt;
        return compose_runtime_controller_state(s.sequence, ns, s.cfg, h, h,
            input, std::nullopt, 9.81f, &s.state);
      };
      int ready_ms = -1;
      for (int ms = 0; ms <= 6500; ms += 20) {
        const bool tracked = !interrupted || ms < 1000 || ms >= 1200;
        const auto out = tick(ms, tracked, tracked ? 0 : 1);
        if (!tracked) {
          require(!state.acceleration_world_yaw_valid && !state.acceleration_yaw_confirmation.pending(),
                  "initial optical loss retained an unfinished confirmation");
          near((right ? out.right : out.left).position[0], 0,
               "unknown startup frame integrated accelerometer force");
        }
        if (state.acceleration_yaw_confirmation.pending() && waiting_started < 0 &&
            (!interrupted || ms >= 1200)) waiting_started = ms;
        if (!state.acceleration_world_yaw_valid) continue;
        require(waiting_started >= 0 && ms - waiting_started >= 4000,
                "native runtime marked initial yaw valid before four stable seconds");
        ready_ms = ms;
        break;
      }
      require(ready_ms > 0, "held grip never completed native startup confirmation");
      require(!state.acceleration_translation_yaw_check.heading_valid() &&
              !state.acceleration_translation_yaw_calibrated,
              "rotation-only startup falsely claimed an independent translation calibration");
      near(state.acceleration_world_yaw_rad, yaw, "stable startup selected an incorrect frame", 1e-4f);
      require(s.cfg.left_imu_motion.acceleration_integration_enabled &&
              s.cfg.right_imu_motion.acceleration_integration_enabled, "confirmation disabled an integration setting");
      RuntimeControllerStateFrameV1 out;
      for (int ms = ready_ms + 20; ms <= ready_ms + 200; ms += 20) out = tick(ms, false, 1);
      const auto& pose = right ? out.right : out.left;
      require(state.acceleration_world_yaw_valid, "ordinary camera loss reset a confirmed yaw");
      near(pose.position[0], 0.02f*std::cos(yaw), "confirmed startup did not integrate world acceleration X", 2e-5f);
      near(pose.position[2], -0.02f*std::sin(yaw), "confirmed startup did not integrate world acceleration Z", 2e-5f);
      tick(ready_ms + 220, false, 1, 1, 1);
      require(state.acceleration_world_yaw_valid, "packet drop discarded the confirmed frame");
      tick(ready_ms + 240, true, 0, 2, 1);
      require(!state.acceleration_world_yaw_valid && !state.acceleration_yaw_confirmation.pending(),
              "AHRS epoch reset inherited a previous confirmation");
    }
  }
}

void translation_checks_direction_with_source_time_and_unknown_velocity() {
  using xr_runtime_adapter::imu_alignment::TranslationYawCheck;
  // A legitimate provider frame near 180 degrees must remain usable too.
  for (float expected_yaw : {0.3f, kPi - 0.05f, -kPi + 0.07f}) {
    for (int imu_delay_ms : {0, 80}) {
      TranslationYawCheck check;
      const auto frame = cu::q_from_axis_angle(0, 1, 0, expected_yaw);
      const auto acceleration = cu::q_rotate(frame, cu::V3d{2, 0, 1});
      const float force[] = {2, 0, 1};
      int imu_ms = 0;
      for (int ms = 0; ms <= 900; ms += 33) {
        for (; imu_ms <= ms - imu_delay_ms; imu_ms += 5) {
          check.observe_force(kStart + uint64_t(imu_ms)*1'000'000, force, 0.2f);
        }
        // Irregular exposures, a large position offset and arbitrary initial
        // velocity must not change the inferred force-frame direction.
        if (ms > 0 && ms % 99 == 0) continue;
        const double t = ms / 1000.0;
        const float position[] = {
            float(3.2 + 0.4*t + 0.5*acceleration.x*t*t), 0,
            float(-1.3 - 0.6*t + 0.5*acceleration.z*t*t)};
        const auto ns = kStart + uint64_t(ms)*1'000'000;
        check.observe_optical(ns, position);
        check.observe_optical(ns, position);
        check.update();
      }
      require(check.valid(), "translation could not check delayed/irregular source-time motion");
      near(std::cos(check.yaw_hint_rad() - expected_yaw), 1,
           "translation confused position/velocity with yaw", 1e-5f);
      near(check.select_hemisphere(expected_yaw), expected_yaw,
           "translation flipped a correct physical frame");
      near(std::cos(check.select_hemisphere(expected_yaw + kPi) - expected_yaw), 1,
           "translation retained the opposite force-frame hemisphere", 1e-6f);
      const float hint = check.yaw_hint_rad();
      check.clear_motion();
      check.update();
      require(check.valid() && check.support_count() == 0,
              "ordinary delivery reset discarded the checked physical frame");
      near(check.yaw_hint_rad(), hint, "delivery reset changed the retained direction");
    }
  }
}

void translation_rejects_unobservable_or_incomplete_motion() {
  using xr_runtime_adapter::imu_alignment::TranslationYawCheck;
  // Static/linear optics, rapid rotation, uncovered packet gaps, and unequal
  // curvature amplitudes do not establish independent translation evidence.
  for (int mode = 0; mode < 6; ++mode) {
    TranslationYawCheck check;
    const float acceleration[] = {mode == 1 ? 0.0f : 3.0f, 0, 0};
    for (int ms = 0; ms <= 900; ms += 10) {
      const auto ns = kStart + uint64_t(ms)*1'000'000;
      if (mode != 3 || ms % 200 < 20) {
        check.observe_force(ns, acceleration, mode == 2 ? 2.0f : 0.2f);
      }
      const double t = ms / 1000.0;
      const double curvature = mode <= 1 ? 0 : mode == 4 ? 0.01 : mode == 5 ? 5 : 1.5;
      const float position[] = {float(0.3 + 0.4*t + curvature*t*t), 0, float(0.1 - 0.6*t)};
      if (ms % 30 == 0) check.observe_optical(ns, position);
      check.update();
    }
    require(!check.valid(), "unobservable/uncovered motion established a force-frame hint");
    near(check.select_hemisphere(-kPi), -kPi,
         "unconfirmed translation altered the rotation calibration");
  }
  TranslationYawCheck repeated;
  const float acceleration[] = {3, 0, 0};
  for (int ms = 0; ms <= 360; ms += 10) {
    const auto ns = kStart + uint64_t(ms)*1'000'000;
    repeated.observe_force(ns, acceleration, 0);
    if (ms % 30 == 0) {
      const double t = ms / 1000.0;
      const float position[] = {float(1.5*t*t), 0, 0};
      repeated.observe_optical(ns, position);
    }
  }
  repeated.update();
  require(repeated.support_count() == 1, "single exposure window did not qualify");
  for (int ms = 370; ms <= 700; ms += 10) {
    repeated.observe_force(kStart + uint64_t(ms)*1'000'000, acceleration, 0);
    repeated.update();
  }
  require(!repeated.valid() && repeated.support_count() == 1,
          "runtime ticks counted a cached optical window as fresh evidence");
}

void checked_force_frame_survives_rotation_refits_and_resets() {
  for (bool right : {false, true}) {
    for (bool enabled : {false, true}) {
      for (float expected_yaw : {0.3f, kPi - 0.05f}) {
        Scenario s;
        auto& m = s.cfg.left_imu_motion;
        m.translation_yaw_check_enabled = enabled;
        m.prediction_window_mode = true;
        m.prediction_window_ms = 120;
        m.hold_lost_ms = 0;
        m.predict_lost_ms = 1200;
        s.cfg.right_orientation_source = s.cfg.left_orientation_source;
        s.cfg.right_imu_motion = m;
        auto& state = right ? s.state.right : s.state.left;
        state.acceleration_world_yaw_valid = true;
        state.acceleration_world_yaw_rad = expected_yaw + kPi;
        const auto frame = cu::q_from_axis_angle(0, 1, 0, expected_yaw);
        const auto incorrect_frame = cu::q_from_axis_angle(0, 1, 0, expected_yaw + kPi);
        const auto acceleration = cu::q_rotate(frame, cu::V3d{2, 0, 1});
        auto tick = [&](int ms, bool optical, uint32_t epoch, uint32_t dropped, uint64_t device) {
          const auto ns = s.prepare(ms, 2);
          auto input = s.input;
          auto hand = s.hand;
          auto& imu = input.left.imu;
          input.left.physical_device_hash = device;
          imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = epoch;
          imu.reserved[CONTROLLER_IMU_DIAG_DROPPED_BATCHES] = dropped;
          // During the second half of optical acquisition, the orientation
          // fit keeps offering the wrong 180-degree branch. Translation must
          // prevent it from restoring that frame on either controller side.
          const double angle = std::max(0, ms - 350) * 0.0009;
          const auto provider = cu::q_from_axis_angle(1, 0, 0, angle);
          const auto force = cu::q_rotate(cu::q_conj(provider), cu::V3d{2, 9.81, 1});
          imu.orientation_xyzw[0] = provider.x; imu.orientation_xyzw[1] = provider.y;
          imu.orientation_xyzw[2] = provider.z; imu.orientation_xyzw[3] = provider.w;
          imu.samples[0].flags |= CONTROLLER_IMU_GYROSCOPE_VALID;
          imu.samples[0].angular_velocity_rad_s[0] = ms > 350 ? 0.9f : 0;
          imu.samples[0].specific_force_m_s2[0] = force.x;
          imu.samples[0].specific_force_m_s2[1] = force.y;
          imu.samples[0].specific_force_m_s2[2] = force.z;
          const auto optical_q = cu::q_mul(incorrect_frame, provider);
          hand.left.controller_qx = optical_q.x; hand.left.controller_qy = optical_q.y;
          hand.left.controller_qz = optical_q.z; hand.left.controller_qw = optical_q.w;
          const double t = ms / 1000.0;
          hand.left.controller_px = 0.5*acceleration.x*t*t;
          hand.left.controller_pz = 0.5*acceleration.z*t*t;
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
        RuntimeControllerStateFrameV1 out;
        for (int ms = 0; ms <= 900; ms += 10) out = tick(ms, true, 1, 0, 123);
        require(state.acceleration_world_alignment.pair_count() >= 5,
                "test did not exercise later rotation-based calibration");
        require(state.acceleration_translation_yaw_check.valid() == enabled,
                "translation option did not control independent direction checking");
        require(state.acceleration_yaw_half_turn_corrections == (enabled ? 1u : 0u),
                "force-frame correction repeated or did not run");
        near(std::cos(state.acceleration_world_yaw_rad - expected_yaw), enabled ? 1 : -1,
             "later rotation fits selected the wrong force-frame branch", 1e-5f);
        for (int ms = 910; ms <= 1100; ms += 10) out = tick(ms, false, 1, 0, 123);
        const auto& pose = right ? out.right : out.left;
        if (enabled) {
          near(pose.position[0], 0.5f*acceleration.x*1.1f*1.1f,
               "checked frame did not preserve physical position after camera loss", 2e-5f);
          near(pose.position[2], 0.5f*acceleration.z*1.1f*1.1f,
               "checked frame predicted the opposite depth direction", 2e-5f);
          near(pose.linear_velocity[0], acceleration.x*1.1f,
               "checked frame left an incorrect launch/braking velocity", 2e-5f);
        }
        tick(1110, false, 1, 1, 123);
        require(state.acceleration_translation_yaw_check.valid() == enabled &&
                state.acceleration_yaw_half_turn_corrections == (enabled ? 1u : 0u),
                "packet drop discarded the confirmed direction");
        reset_runtime_controller_imu_prediction(state);
        require(state.acceleration_translation_yaw_check.valid() == enabled,
                "prediction reset discarded the physical hemisphere");
        tick(1120, true, 2, 1, 123);
        require(!state.acceleration_translation_yaw_check.valid() &&
                !state.acceleration_world_yaw_valid && state.acceleration_yaw_half_turn_corrections == 0,
                "new AHRS epoch retained the old force-frame evidence");
        // A different physical controller likewise cannot inherit a hint.
        state.acceleration_world_yaw_valid = true;
        tick(1130, true, 2, 1, 456);
        require(!state.acceleration_world_yaw_valid && !state.acceleration_translation_yaw_check.valid(),
                "physical device change retained a prior controller's yaw");
      }
    }
  }
}

void translation_heading_uses_changing_acceleration_without_fitting_bias() {
  using xr_runtime_adapter::imu_alignment::TranslationYawCheck;
  for (float yaw : {-2.45f, 0.6f, kPi - 0.03f}) {
    for (int delay : {0, 80}) {
      TranslationYawCheck check;
      int force_ms = 0;
      const auto world = cu::q_from_axis_angle(0, 1, 0, yaw);
      constexpr double omega = 6.0;
      for (int ms = 0; ms <= 3000; ms += 30) {
        for (; force_ms <= ms - delay; force_ms += 5) {
          const float a[] = {float(-0.20*omega*omega*std::sin(omega*force_ms/1000.0) + 0.4),
                             0, float(-0.10*omega*omega*std::sin(omega*force_ms/1000.0) - 0.3)};
          check.observe_force(kStart + uint64_t(force_ms)*1'000'000, a, 0.1f);
        }
        const double t = ms/1000.0;
        const auto p = cu::q_rotate(world, cu::V3d{0.20*std::sin(omega*t), 0, 0.10*std::sin(omega*t)});
        const float position[] = {float(2 + 0.3*t + p.x), 0, float(-3 - 0.2*t + p.z)};
        const auto ns = kStart + uint64_t(ms)*1'000'000;
        check.observe_optical(ns, position);
        check.observe_optical(ns, position);
        check.update(true);
      }
      require(check.heading_valid(), "changing translation did not identify the complete force yaw");
      require(std::cos(check.heading_rad() - yaw) > 0.9998f,
              "constant acceleration bias or delayed delivery rotated the force heading");
      require(check.heading_coherence() >= 0.97f, "heading was confirmed without coherent support");
      // Drain any covered delayed poses before testing cached evidence.
      const float zero[] = {0, 0, 0};
      for (; force_ms <= 3300; force_ms += 5) {
        check.observe_force(kStart + uint64_t(force_ms)*1'000'000, zero, 0);
        check.update(true);
      }
      const auto consumed = check.heading_timestamp_ns();
      const auto support = check.heading_support_count();
      for (int i = 0; i < 30; ++i) check.update(true);
      require(check.heading_timestamp_ns() == consumed && check.heading_support_count() == support,
              "cached optical poses repeatedly refreshed full-heading calibration");
      require(!check.fresh_heading(consumed, consumed), "already consumed heading was treated as fresh");
      require(!check.fresh_heading(consumed + 151'000'000, 0), "expired heading was treated as fresh");
      require(!check.fresh_heading(consumed - 1, 0), "future heading was treated as source-time evidence");
      check.clear_optical_motion();
      require(check.heading_valid() && check.heading_timestamp_ns() == consumed,
              "optical loss revoked a confirmed physical heading");
    }
  }
  // Removing the quadratic nuisance must not invent observability from
  // constant acceleration, or fit noisy/rapidly rotating/uncovered motion.
  for (int mode = 0; mode < 5; ++mode) {
    TranslationYawCheck check;
    for (int ms = 0; ms <= 2500; ms += 10) {
      const double t = ms/1000.0;
      const float a[] = {mode == 0 ? 2.0f : float(-7.2*std::sin(6*t)), 0, 0};
      if (mode != 2 || ms % 200 < 20) {
        check.observe_force(kStart + uint64_t(ms)*1'000'000, a, mode == 1 ? 2.0f : 0.1f);
      }
      const float position[] = {mode == 0 ? float(t*t) : mode == 3 ? float(0.2*std::sin(13*t))
          : mode == 4 ? float(0.002*std::sin(6*t)) : float(0.2*std::sin(6*t)), 0, 0};
      if (ms % 30 == 0) check.observe_optical(kStart + uint64_t(ms)*1'000'000, position);
      check.update(true);
    }
    require(!check.heading_valid(), "unobservable or inconsistent motion established a refined heading");
  }
}

void refined_force_heading_calibrates_both_sides_without_changing_aim(bool rotation_first = false) {
  for (bool right : {false, true}) {
    Scenario s;
    s.state.left.acceleration_world_yaw_valid = false;
    auto& m = s.cfg.left_imu_motion;
    m.translation_yaw_refinement_enabled = true;
    m.acceleration_yaw_stable_ms = 4000;
    m.prediction_window_mode = true;
    m.prediction_window_ms = 120;
    m.predict_lost_ms = 1200;
    s.cfg.right_orientation_source = s.cfg.left_orientation_source;
    s.cfg.right_imu_motion = m;
    const float yaw = right ? 0.6f : -2.4f;
    const auto world = cu::q_from_axis_angle(0, 1, 0, yaw);
    // Optics offers a different constant local grip. Position calibration
    // must work without adopting that rendered orientation or its yaw error.
    const auto optical_q = cu::q_from_axis_angle(0, 1, 0, yaw + 1.1f);
    auto& state = right ? s.state.right : s.state.left;
    const int visible_ms = rotation_first ? 10000 : 6000;
    const auto position = [&](int ms) {
      const double t = std::max(0, ms - 5000)/1000.0;
      return rotation_first ? 0.2*(1 - std::cos(6*t)) : 0.2*std::sin(6*ms/1000.0);
    };
    int ready_ms = -1;
    uint32_t checked_refinements = 0;
    auto tick = [&](int ms, bool optical, uint32_t epoch = 1, uint64_t device = 123) {
      const auto ns = s.prepare(ms);
      auto input = s.input;
      input.left.physical_device_hash = device;
      auto hand = s.hand;
      const double t = ms/1000.0;
      const auto p = cu::q_rotate(world, cu::V3d{position(ms), 0, 0});
      auto& imu = input.left.imu;
      const double provider_angle = rotation_first ? 0.25*std::sin(3*t)
          : ms > 3000 ? 0.15*std::sin(3*t) : 0;
      const auto provider = cu::q_from_axis_angle(1, 0, 0, provider_angle);
      imu.orientation_xyzw[0] = provider.x; imu.orientation_xyzw[3] = provider.w;
      imu.reserved[CONTROLLER_IMU_DIAG_EPOCH] = epoch;
      imu.samples[0].flags |= CONTROLLER_IMU_GYROSCOPE_VALID;
      // Provider-world force is along provider X; the independently recorded
      // optical path is along XR world_yaw * X.
      const double acceleration = rotation_first
          ? ms >= 5000 ? 7.2*std::cos(6*(t - 5)) : 0
          : -7.2*std::sin(6*t);
      const auto body_force = cu::q_rotate(cu::q_conj(provider),
          cu::V3d{acceleration, 9.81, 0});
      imu.samples[0].specific_force_m_s2[0] = body_force.x;
      imu.samples[0].specific_force_m_s2[1] = body_force.y;
      imu.samples[0].specific_force_m_s2[2] = body_force.z;
      imu.samples[0].angular_velocity_rad_s[0] = rotation_first ? 0.75*std::cos(3*t)
          : ms > 3000 ? 0.45*std::cos(3*t) : 0;
      hand.left.controller_px = p.x; hand.left.controller_pz = p.z;
      const auto noisy_grip = cu::q_mul(cu::q_mul(optical_q, provider),
          cu::q_from_axis_angle(0, 0, 1, rotation_first || ms > 3000 ? 0.3 : ms > 2000 ? 0.3*std::sin(3*t) : 0));
      hand.left.controller_qx = noisy_grip.x; hand.left.controller_qy = noisy_grip.y;
      hand.left.controller_qz = noisy_grip.z; hand.left.controller_qw = noisy_grip.w;
      if (right) {
        input.right = input.left; input.left = {};
        hand.right = hand.left; hand.left = {};
        hand.flags = HAND_FLAG_RIGHT_VALID; hand.right.handedness = 2;
      }
      const auto h = optical ? std::optional(hand) : std::nullopt;
      return compose_runtime_controller_state(s.sequence, ns, s.cfg, h, h,
          input, std::nullopt, 9.81f, &s.state);
    };
    for (int ms = 0; ms <= visible_ms; ms += 10) {
      const auto out = tick(ms, true);
      const auto& pose = right ? out.right : out.left;
      const double angle = rotation_first ? 0.25*std::sin(3*ms/1000.0)
          : ms > 3000 ? 0.15*std::sin(3*ms/1000.0) : 0;
      near(pose.orientation_xyzw[3], std::cos(angle/2),
           "physical heading calibration changed IMU aiming orientation");
      near(pose.orientation_xyzw[0], std::sin(angle/2),
           "physical heading calibration attenuated the gyro response");
      if (state.acceleration_world_yaw_valid && ready_ms < 0) ready_ms = ms;
      if (state.acceleration_translation_yaw_refinements != checked_refinements) {
        const float error = std::atan2(std::sin(state.acceleration_world_yaw_rad -
            state.acceleration_translation_yaw_check.heading_rad()),
            std::cos(state.acceleration_world_yaw_rad - state.acceleration_translation_yaw_check.heading_rad()));
        near(error, 0, "calibrated=yes left the confirmed force heading partly unapplied", 1e-5f);
        checked_refinements = state.acceleration_translation_yaw_refinements;
      }
      if (ms < 4000) require(!state.acceleration_world_yaw_valid, "full heading skipped startup stability");
      if (rotation_first && ms == 5000) {
        require(state.acceleration_world_yaw_valid && !state.acceleration_translation_yaw_calibrated &&
                !state.acceleration_translation_yaw_check.heading_valid(),
                "optional refinement blocked or mislabeled the existing rotation-based startup");
        near(state.acceleration_world_yaw_rad, yaw + 1.1f,
             "rotation-only phase failed to exercise the different force heading", 1e-4f);
      }
    }
    require(ready_ms >= 4000 && state.acceleration_translation_yaw_calibrated,
            "full physical yaw did not calibrate at startup for both hands");
    require(std::cos(state.acceleration_world_yaw_rad - yaw) > 0.9999f,
            "startup force yaw inherited the optical grip orientation");
    if (rotation_first) require(state.acceleration_translation_yaw_refinements > 0,
                               "accepted translation never refined the prior rotation-derived frame");
    const float retained = state.acceleration_world_yaw_rad;
    const auto hint_ns = state.acceleration_translation_heading_consumed_ns;
    for (int ms = visible_ms + 10; ms <= visible_ms + 200; ms += 10) {
      const auto out = tick(ms, false);
      near(state.acceleration_world_yaw_rad, retained, "optical loss changed the calibrated force frame");
      const auto& pose = right ? out.right : out.left;
      const auto p = cu::q_rotate(world, cu::V3d{position(ms), 0, 0});
      near(pose.position[0], p.x, "calibrated loss prediction moved in the wrong X direction", 0.008f);
      near(pose.position[2], p.z, "calibrated loss prediction moved in the wrong depth direction", 0.008f);
    }
    reset_runtime_controller_imu_prediction(state);
    require(state.acceleration_translation_yaw_calibrated &&
            state.acceleration_translation_heading_consumed_ns == hint_ns,
            "ordinary prediction reset discarded or refreshed confirmed translation evidence");
    tick(visible_ms + 210, true, 2);
    require(!state.acceleration_world_yaw_valid && !state.acceleration_translation_yaw_calibrated &&
            !state.acceleration_translation_yaw_check.heading_valid(),
            "new AHRS epoch inherited the full-heading calibration");
    state.acceleration_world_yaw_valid = true;
    state.acceleration_translation_yaw_calibrated = true;
    tick(visible_ms + 220, true, 2, 456);
    require(!state.acceleration_world_yaw_valid && !state.acceleration_translation_yaw_calibrated &&
            !state.acceleration_translation_yaw_check.heading_valid(),
            "a different physical device inherited the full-heading calibration");
  }
}

void irregular_optical_window_retains_acceleration_time() {
  xr_runtime_adapter::prediction_window::PositionWindowEstimator<> window;
  const uint64_t start = 40'000'000'000'000ULL;
  for (int ms : {0, 17, 41, 100}) {
    const double t = ms / 1000.0;
    window.add(start + uint64_t(ms) * 1'000'000, -0.3 * t + 1.5 * t * t, 0, 0, 120);
  }
  double velocity[3] = {};
  require(window.estimate_velocity(velocity), "irregular window did not estimate velocity");
  const double weight = window.velocity_alignment_interval_seconds(start, start + 180'000'000);
  near(static_cast<float>(velocity[0] + 3.0 * weight), 0.24f,
       "window/IMU time alignment lost a constant acceleration", 1e-6f);
  double split_weight = 0;
  uint64_t previous = start;
  for (int ms : {5, 31, 70, 125, 180}) {
    const uint64_t now = start + uint64_t(ms) * 1'000'000;
    split_weight += window.velocity_alignment_interval_seconds(previous, now);
    previous = now;
  }
  near(static_cast<float>(split_weight), static_cast<float>(weight),
       "splitting delivery intervals changed the launch correction", 1e-7f);
}

void window_launch_and_braking_share_the_same_imu_cursor() {
  for (bool right : {false, true}) {
    for (bool burst : {false, true}) {
      // Complete, disabled, missing and older-than-camera IMU observations.
      for (int mode : {0, 1, 2, 3}) {
        Scenario s;
        auto& m = s.cfg.left_imu_motion;
        m.prediction_window_mode = true;
        m.prediction_window_ms = 120;
        m.predict_lost_ms = 1000;
        m.acceleration_integration_enabled = mode != 1;
        s.cfg.right_orientation_source = s.cfg.left_orientation_source;
        s.cfg.right_imu_motion = m;
        s.state.right.acceleration_world_yaw_valid = true;
        auto position = [](int ms) {
          const float t = (ms - 200) / 1000.0f;
          if (t <= 0) return 0.0f;
          if (t <= 0.12f) return -t * t;
          if (t <= 0.24f) {
            const float u = t - 0.12f;
            return -0.0144f - 0.24f * u + u * u;
          }
          return -0.0288f;
        };
        std::vector<ControllerInputV3> pending;
        float stopped_position = 0;
        for (int ms = 0; ms <= (mode == 0 ? 700 : 340); ms += 10) {
          const int sample_ms = std::max(0, ms - (mode == 3 ? 160 : 20));
          const float a = sample_ms > 200 && sample_ms <= 320 ? -2.0f
              : sample_ms > 320 && sample_ms <= 440 ? 2.0f : 0.0f;
          const auto now = s.prepare(ms, a);
          const uint64_t sample_ns = kStart + uint64_t(sample_ms) * 1'000'000;
          auto input = s.input;
          input.left.imu.samples[0].timestamp_ns = sample_ns;
          input.left.imu.orientation_timestamp_ns = sample_ns;
          input.left.imu.latest_sample_timestamp_ns = sample_ns;
          if (mode == 2 && sample_ms >= 240 && sample_ms < 320) input.left.imu.sample_count = 0;
          const int camera_ms = std::max(0, ms - 40) / 30 * 30;
          auto hand = s.hand;
          hand.sequence = uint64_t(camera_ms / 30 + 1);
          hand.source_timestamp_ns = kStart + uint64_t(camera_ms) * 1'000'000;
          hand.left.controller_px = position(camera_ms);
          if (right) {
            input.right = input.left;
            input.left = {};
            hand.right = hand.left;
            hand.left = {};
            hand.flags = HAND_FLAG_RIGHT_VALID;
          }
          pending.push_back(input);
          if (burst && ms % 20 != 0) continue;
          const auto optical = ms <= 340 ? std::optional(hand) : std::nullopt;
          const auto out = compose_runtime_controller_state(s.sequence, now, s.cfg,
              optical, optical, input, std::nullopt, 9.81f, &s.state, &pending);
          pending.clear();
          const auto& state = right ? s.state.right : s.state.left;
          const auto& side = right ? out.right : out.left;
          if (ms == 340) {
            if (mode == 0) {
              near(state.anchor_velocity_mps[0], -0.24f,
                   "launch velocity was left at the historical optical-window time", 1e-5f);
              require(state.anchor_velocity_alignment_samples != 0, "complete IMU history was not used");
            } else {
              double raw[3] = {};
              state.position_history.estimate_velocity(raw);
              near(state.anchor_velocity_mps[0], static_cast<float>(raw[0]),
                   "disabled or incomplete acceleration changed the optical launch");
              require(state.anchor_velocity_alignment_samples == 0, "unavailable acceleration was used");
            }
          }
          if (ms >= 480) {
            near(side.linear_velocity[0], 0, "braking a stopped hand left a rebound velocity", 1e-5f);
            if (ms == 480) stopped_position = side.position[0];
            else near(side.position[0], stopped_position, "stopped hand kept moving after braking", 1e-5f);
          }
        }
        auto& state = right ? s.state.right : s.state.left;
        reset_runtime_controller_imu_prediction(state);
        require(state.acceleration_history_count == 0 && state.anchor_velocity_alignment_samples == 0,
                "prediction reset retained the old launch history");
      }
    }
  }
}

void late_pre_optical_force_is_not_reintegrated_after_loss() {
  for (bool right : {false, true}) {
    for (int hold : {0, 60}) {
      Scenario s;
      auto& m = s.cfg.left_imu_motion;
      m.prediction_window_mode = true; m.prediction_window_ms = 120;
      m.hold_lost_ms = hold; m.predict_lost_ms = 1200;
      s.cfg.right_orientation_source = s.cfg.left_orientation_source;
      s.cfg.right_imu_motion = m; s.state.right.acceleration_world_yaw_valid = true;
      auto tick = [&](int ms, bool optical) {
        const auto ns = s.prepare(ms);
        auto input = s.input; auto hand = s.hand;
        auto& imu = input.left.imu;
        // Delivery lags the optical observation by 80ms. A packet received
        // during loss may still describe the already observed optical path.
        imu.latest_sample_timestamp_ns = imu.orientation_timestamp_ns = ns-80'000'000;
        imu.samples[0].timestamp_ns = ns-80'000'000;
        imu.samples[0].specific_force_m_s2[0] = 2;
        hand.left.controller_px = 0.35f*ms/1000;
        if (right) {
          input.right = input.left; input.left = {};
          hand.right = hand.left; hand.left = {}; hand.flags = HAND_FLAG_RIGHT_VALID;
          hand.right.handedness = 2;
        }
        const auto h = optical ? std::optional(hand) : std::nullopt;
        return compose_runtime_controller_state(s.sequence, ns, s.cfg, h, h,
            input, std::nullopt, 9.81f, &s.state);
      };
      for (int ms = 0; ms <= 120; ms += 15) tick(ms, true);
      auto& state = right ? s.state.right : s.state.left;
      near(state.anchor_velocity_mps[0], 0.35f, "delayed input changed optical launch velocity");
      for (int ms = 135; ms <= 390; ms += 15) {
        const auto out = tick(ms, false);
        // The 195->210ms delivery interval straddles measurement time 120ms:
        // only its final 10ms belong to unseen motion. Later intervals count
        // normally, including acceleration during an intentional hold.
        const float expected_dv = 2*std::max(0, ms-80-120)/1000.0f;
        near(state.acceleration_velocity_delta_mps[0], expected_dv,
             "late pre-anchor force was counted as new post-loss acceleration", 1e-5f);
        if (ms > 120+hold) near((right ? out.right : out.left).linear_velocity[0], 0.35f+expected_dv,
            "source-time clipping removed real post-anchor force", 1e-5f);
      }
    }
  }
}

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
    // This test isolates the rotation estimator; startup waiting has its own
    // native tests with the default four-second confirmation above.
    s.cfg.left_imu_motion.acceleration_yaw_stable_ms = 0;
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
  near(after.left.position[0], before.left.position[0],
       "interrupted blend extrapolated old prediction instead of measured rest");

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

void interrupted_correction_does_not_launch_stationary_hand() {
  for (bool right : {false, true}) {
    for (bool window : {false, true}) {
      for (float speed_limit : {0.0f, 2.0f}) {
        Scenario s;
        auto& motion = s.cfg.left_imu_motion;
        motion.hold_lost_ms = 0;
        motion.reacquire_blend_ms = 180;
        motion.prediction_window_mode = window;
        motion.prediction_window_ms = 120;
        motion.max_prediction_speed_mps = speed_limit;
        s.cfg.right_imu_motion = motion;
        s.cfg.right_orientation_source = s.cfg.left_orientation_source;
        s.state.right.acceleration_world_yaw_valid = true;
        auto tick = [&](int ms, bool optical) {
          const auto now = s.prepare(ms);
          auto input = s.input;
          auto hand = s.hand;
          if (right) {
            input.right = input.left; input.left = {};
            hand.right = hand.left; hand.left = {};
            hand.flags = HAND_FLAG_RIGHT_VALID;
            hand.right.handedness = 2;
          }
          const auto h = optical ? std::optional(hand) : std::nullopt;
          return compose_runtime_controller_state(s.sequence, now, s.cfg, h, h,
              input, std::nullopt, 9.81f, &s.state);
        };
        tick(0, true);
        tick(33, true);
        for (int ms = 43; ms <= 203; ms += 10) tick(ms, false);
        s.hand.left.controller_px = -0.12f;
        tick(210, true);
        RuntimeControllerStateFrameV1 before;
        for (int ms = 220; ms <= 260; ms += 10) before = tick(ms, true);
        const auto& state = right ? s.state.right : s.state.left;
        const auto& side = right ? before.right : before.left;
        require(state.reacquire_blend_active, "stationary target did not enter correction");
        require(side.position[0] < 0 && side.position[0] > -0.12f,
                "stationary target was not reached through a partial correction");
        near(state.anchor_velocity_mps[0], 0,
             "position correction became stationary hand's prediction velocity");
        if (speed_limit > 0) require(side.linear_velocity[0] < -0.1f,
             "moving correction stopped reporting its actual output velocity");
        const auto after = tick(270, false);
        const auto& lost = right ? after.right : after.left;
        near(lost.position[0], side.position[0],
             "loss during correction launched a physically stationary hand");
        near(lost.linear_velocity[0], 0,
             "synthetic correction velocity survived into physical prediction");
      }
    }
  }
}

void optical_prediction_uses_measurements_before_output_smoothing() {
  for (bool right : {false, true}) {
    for (bool window : {false, true}) {
      Scenario s;
      auto& motion = s.cfg.left_imu_motion;
      motion.hold_lost_ms = 0;
      motion.reacquire_blend_ms = 180;
      motion.prediction_window_mode = window;
      motion.prediction_window_ms = 120;
      motion.max_prediction_speed_mps = 2;
      s.cfg.right_imu_motion = motion;
      s.cfg.right_orientation_source = s.cfg.left_orientation_source;
      s.state.right.acceleration_world_yaw_valid = true;
      s.hand.left.controller_pz = 0.285391f;
      auto initial_tick = [&](int ms, bool optical) {
        const auto now = s.prepare(ms);
        auto input = s.input;
        auto hand = s.hand;
        if (right) {
          input.right = input.left; input.left = {};
          hand.right = hand.left; hand.left = {};
          hand.flags = HAND_FLAG_RIGHT_VALID;
          hand.right.handedness = 2;
        }
        const auto h = optical ? std::optional(hand) : std::nullopt;
        return compose_runtime_controller_state(s.sequence, now, s.cfg, h, h,
            input, std::nullopt, 9.81f, &s.state);
      };
      initial_tick(0, true);
      initial_tick(33, true);
      for (int ms = 43; ms <= 203; ms += 10) initial_tick(ms, false);

      RuntimeControllerStateFrameV1 before;
      for (int ms = 210; ms <= 287; ms += 11) {
        const auto now = s.prepare(ms);
        const int source_ms = ms >= 277 ? 277 : (ms >= 243 ? 243 : 210);
        auto measured = s.hand;
        measured.sequence = 100 + (source_ms == 210 ? 0 : (source_ms == 243 ? 1 : 2));
        measured.source_timestamp_ns = kStart + uint64_t(source_ms) * 1'000'000;
        measured.left.controller_pz = 0.134558f - 0.26f * float(source_ms - 210) / 1000;
        measured.left.vz = -0.26f;
        if (source_ms == 210) measured.left.flags &= ~HAND_LINEAR_VELOCITY_VALID;
        auto displayed = measured;
        // The three accepted positions are moved by the preceding output
        // smoother. Their slope must not become physical controller velocity.
        displayed.left.controller_pz = source_ms == 210 ? 0.157955f
            : (source_ms == 243 ? 0.130088f : 0.119767f);
        displayed.left.vz = -1.0f;
        auto input = s.input;
        if (right) {
          input.right = input.left; input.left = {};
          for (auto* hand : {&measured, &displayed}) {
            hand->right = hand->left; hand->left = {};
            hand->flags = HAND_FLAG_RIGHT_VALID;
            hand->right.handedness = 2;
          }
        }
        before = compose_runtime_controller_state(s.sequence, now, s.cfg,
            displayed, measured, input, std::nullopt, 9.81f, &s.state);
      }
      const auto& state = right ? s.state.right : s.state.left;
      near(state.anchor_velocity_mps[2], -0.26f,
           "output smoothing contaminated the measured optical velocity");
      const auto previous = right ? before.right : before.left;
      const auto now = s.prepare(298);
      auto input = s.input;
      if (right) { input.right = input.left; input.left = {}; }
      const auto after = compose_runtime_controller_state(s.sequence, now, s.cfg,
          std::nullopt, std::nullopt, input, std::nullopt, 9.81f, &s.state);
      const auto& side = right ? after.right : after.left;
      near(side.position[2], previous.position[2] - 0.26f * 0.011f,
           "second loss extrapolated correction instead of measured motion");
      near(side.linear_velocity[2], -0.26f,
           "second loss published artificial correction speed");
    }
  }
}

void optical_velocity_requires_matching_accepted_measurement() {
  Scenario s;
  s.hand.left.vx = 0.1f;
  s.tick(0, true);
  auto now = s.prepare(10);
  auto older = s.hand;
  older.sequence -= 1;
  older.source_timestamp_ns -= 10'000'000;
  older.left.vx = -5;
  compose_runtime_controller_state(s.sequence, now, s.cfg, s.hand, older,
      s.input, std::nullopt, 9.81f, &s.state);
  near(s.state.left.anchor_velocity_mps[0], 0.1f,
       "an older yaw-reference frame seeded the optical velocity");

  now = s.prepare(20);
  auto measured = s.hand;
  measured.left.controller_px = 100;
  measured.left.vx = -5;
  auto rejected = s.hand;
  rejected.left.status = 2;
  const auto out = compose_runtime_controller_state(s.sequence, now, s.cfg,
      rejected, measured, s.input, std::nullopt, 9.81f, &s.state);
  near(s.state.left.anchor_velocity_mps[0], 0.1f,
       "a gate-rejected optical measurement replaced launch velocity");
  near(out.left.position[0], 0.001f,
       "a rejected optical position became a new prediction anchor");
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
  auto recovery = distance.tick(freeze_ms + 10, false, -5, hmd);
  require(recovery.left.position[0] >= frozen-0.000001f,
          "braking instantly reversed an outward velocity");
  for (int t = freeze_ms + 20; t <= freeze_ms + 500; t += 10) {
    recovery = distance.tick(t, false, -5, hmd);
  }
  require(recovery.left.position[0] < frozen &&
          distance.state.left.prediction_freeze_reason == RuntimeControllerImuPredictionFreezeReason::None,
          "distance freeze did not recover inward from published pose");
}

void distance_freeze_keeps_physical_velocity_through_braking() {
  for (bool right : {false, true}) {
    for (bool decay : {false, true}) {
      Scenario s;
      auto& m=s.cfg.left_imu_motion;
      m.hold_lost_ms=0; m.predict_lost_ms=1500;
      m.prediction_time_decay_enabled=decay;
      m.prediction_distance.max_distance_m=0.25;
      s.hand.left.controller_px=0.2f; s.hand.left.vx=0.5f;
      s.cfg.right_orientation_source=s.cfg.left_orientation_source;
      s.cfg.right_imu_motion=m; s.state.right.acceleration_world_yaw_valid=true;
      HmdPoseF64V1 hmd{}; hmd.flags=HMD_FLAG_POSE_VALID;
      auto tick=[&](int ms,bool optical,float acceleration) {
        const auto now=s.prepare(ms,acceleration);
        auto input=s.input;auto hand=s.hand;
        if(right) {
          input.right=input.left; input.left={};
          hand.right=hand.left; hand.left={};hand.flags=HAND_FLAG_RIGHT_VALID;
        }
        const auto h=optical?std::optional(hand):std::nullopt;
        return compose_runtime_controller_state(s.sequence,now,s.cfg,h,h,input,hmd,9.81f,&s.state);
      };
      tick(0,true,0);
      auto& state=right?s.state.right:s.state.left;
      int freeze_ms=10;
      for(;freeze_ms<200;freeze_ms+=10) {
        tick(freeze_ms,false,0);
        if(state.prediction_freeze_reason==RuntimeControllerImuPredictionFreezeReason::MaxDistance)break;
      }
      require(freeze_ms<200,"outward motion did not hit distance limit");
      const float frozen=state.position_m[0];
      // Braking an outward velocity is not inward motion. After 500 ms at
      // -1 m/s^2 the real hand has stopped outside the limit, not returned.
      for(int dt=10;dt<=500;dt+=10) {
        const auto out=tick(freeze_ms+dt,false,-1);
        near((right?out.right:out.left).position[0],frozen,
             "distance freeze turned outward braking into inward motion");
        require(state.prediction_freeze_reason==RuntimeControllerImuPredictionFreezeReason::MaxDistance,
                "braking manufactured a distance recovery");
      }
      for(int dt=510;dt<=600;dt+=10) {
        const auto out=tick(freeze_ms+dt,false,0);
        near((right?out.right:out.left).position[0],frozen,
             "stationary hand outside distance limit resumed motion");
      }
      bool recovered=false;
      for(int dt=610;dt<=1100;dt+=10) {
        const auto out=tick(freeze_ms+dt,false,-4);
        if(state.prediction_freeze_reason==RuntimeControllerImuPredictionFreezeReason::None) {
          require((right?out.right:out.left).position[0]<frozen,"real inward motion did not move inward");
          recovered=true;break;
        }
      }
      require(recovered,"distance limit never recovered from new inward motion");
    }
  }
}

void distance_recovery_does_not_spend_frozen_time_as_speed_budget() {
  Scenario s;
  auto& m=s.cfg.left_imu_motion;
  m.hold_lost_ms=0;m.predict_lost_ms=2000;
  m.max_prediction_speed_mps=1;
  m.prediction_distance.max_distance_m=0.15;
  s.hand.left.controller_px=0.3f;
  HmdPoseF64V1 hmd{};hmd.flags=HMD_FLAG_POSE_VALID;
  auto last=s.tick(0,true,0,hmd);bool recovered=false;
  for(int ms=10;ms<=1200;ms+=10) {
    const auto out=s.tick(ms,false,-1,hmd);
    require(std::abs(out.left.position[0]-last.left.position[0])<=0.010001f,
            "distance recovery spent held time as an instantaneous speed budget");
    if(s.state.left.prediction_freeze_reason==RuntimeControllerImuPredictionFreezeReason::None && ms>10) recovered=true;
    last=out;
  }
  require(recovered,"outside optical anchor never made a bounded inward recovery");
}

void presentation_yaw_matches_camera_time_with_delayed_delivery() {
  for (bool right : {false, true}) {
    for (bool smooth : {false, true}) {
    for (const auto delays : {std::pair<int,int>{0,40}, {35,0}, {70,20}}) {
      Scenario s;
      auto& motion = s.cfg.left_imu_motion;
      motion.yaw_correction_enabled = true;
      motion.yaw_correction_smooth = smooth;
      motion.yaw_correction_continuous = true;
      motion.yaw_correction_trigger_filter = false;
      motion.yaw_correction_interval_ms = 0;
      // Interpolation across an abrupt stop may differ by under one degree;
      // delivery-time comparisons would invent errors of 14-24 degrees here.
      motion.yaw_correction_deadband_deg = 2.0f;
      motion.yaw_correction_blend_ms = 0;
      if (right) {
        s.cfg.right_orientation_source = s.cfg.left_orientation_source;
        s.cfg.right_imu_motion = motion;
      }
      const auto angle_at = [](int ms) { return std::min(ms, 400) * 0.006f; };
      constexpr float anchor = 0.35f;
      for (int ms = 0; ms <= 600; ms += 10) {
        const auto tick_ns = s.prepare(ms);
        const int imu_ms = ms - delays.first, camera_ms = ms - delays.second;
        auto& imu = s.input.left.imu;
        imu.orientation_timestamp_ns = imu.latest_sample_timestamp_ns =
            kStart + static_cast<int64_t>(imu_ms) * 1'000'000;
        imu.orientation_xyzw[1] = std::sin(angle_at(imu_ms) * 0.5f);
        imu.orientation_xyzw[3] = std::cos(angle_at(imu_ms) * 0.5f);
        s.hand.source_timestamp_ns = kStart + static_cast<int64_t>(camera_ms) * 1'000'000;
        s.hand.left.controller_qy = std::sin((anchor + angle_at(camera_ms)) * 0.5f);
        s.hand.left.controller_qw = std::cos((anchor + angle_at(camera_ms)) * 0.5f);
        if (right) {
          s.input.right = s.input.left;
          s.hand.right = s.hand.left;
          s.hand.right.handedness = 2;
          s.hand.flags |= HAND_FLAG_RIGHT_VALID;
        }
        compose_runtime_controller_state(s.sequence, tick_ns, s.cfg, s.hand, s.hand,
                                         s.input, std::nullopt, 9.81f, &s.state);
        const auto& state = right ? s.state.right : s.state.left;
        if (ms >= 100) {
          require(state.yaw_correction_valid, "delayed delivery never produced a matched yaw pair");
          near(state.yaw_correction_rad, anchor, "camera latency became false yaw drift", 0.001f);
          require(state.yaw_apply_count == 1, "a rigid turn or stop triggered another yaw correction");
        }
      }
    }
    }
  }
}

struct SmoothYawScenario {
  Scenario s;
  bool right;
  explicit SmoothYawScenario(bool right_side = false) : right(right_side) {
    auto& m = s.cfg.left_imu_motion;
    m.position_prediction_enabled = m.acceleration_integration_enabled = false;
    m.yaw_correction_enabled = m.yaw_correction_smooth = true;
    if (right) {
      s.cfg.right_orientation_source = s.cfg.left_orientation_source;
      s.cfg.right_imu_motion = m;
    }
  }
  RuntimeControllerImuSideRuntimeState& state() { return right ? s.state.right : s.state.left; }
  RuntimeControllerImuMotionConfig& config() { return right ? s.cfg.right_imu_motion : s.cfg.left_imu_motion; }
  RuntimeControllerSideStateV1 tick(int ms, float imu_deg, float error_deg,
                                   bool optical = true, float confidence = 1,
                                   bool accepted = true, bool cached = false) {
    const auto previous = s.hand;
    const auto now = s.prepare(ms);
    const float a = imu_deg * kPi / 180;
    s.input.left.imu.orientation_xyzw[1] = std::sin(a / 2);
    s.input.left.imu.orientation_xyzw[3] = std::cos(a / 2);
    const float b = (imu_deg + error_deg) * kPi / 180;
    s.hand.left.controller_qy = std::sin(b / 2);
    s.hand.left.controller_qw = std::cos(b / 2);
    s.hand.left.confidence = confidence;
    if (cached) s.hand = previous;
    if (right) {
      s.input.right = s.input.left;
      s.hand.right = s.hand.left;
      s.hand.right.handedness = 2;
      s.hand.flags |= HAND_FLAG_RIGHT_VALID;
    }
    auto filtered = s.hand;
    if (!accepted) {
      filtered.left.status = filtered.right.status = 2;
    }
    const auto frame = compose_runtime_controller_state(s.sequence, now, s.cfg,
        optical ? std::optional(filtered) : std::nullopt,
        optical ? std::optional(s.hand) : std::nullopt,
        s.input, std::nullopt, 9.81f, &s.state);
    return right ? frame.right : frame.left;
  }
};

float angle_delta(float a, float b) { return std::remainder(a - b, 2 * kPi); }

void smooth_yaw_preserves_aiming_and_bounds_drift_correction() {
  for (bool right : {false, true}) {
    SmoothYawScenario aim(right);
    for (int ms = 0; ms <= 2000; ms += 10) {
      // Include micro-aiming, a large turn and wrapping through +/-180 degrees.
      const float degrees = ms < 1000 ? 0.2f * std::sin(ms / 50.f) : (ms - 1000) * 0.4f;
      const auto out = aim.tick(ms, degrees, 20);
      const auto expected = cu::q_from_axis_angle(0, 1, 0, (degrees + 20) * kPi / 180);
      const float dot = out.orientation_xyzw[3] * expected.w + out.orientation_xyzw[1] * expected.y;
      near(std::abs(dot), 1, "smooth yaw attenuated useful gyro movement", 1e-5f);
      near(aim.state().yaw_correction_rad, 20 * kPi / 180,
           "rigid optical/IMU motion invented drift", 1e-5f);
    }
    SmoothYawScenario drift(right);
    drift.tick(0, 0, 0);
    float last = 0;
    unsigned changes_between_camera_frames = 0;
    for (int ms = 10; ms <= 5000; ms += 10) {
      // Optical at ~33Hz, rendering at 100Hz. Cached frames are not new evidence,
      // but the accepted fresh target advances smoothly between camera updates.
      drift.tick(ms, 0, 10, true, 1, true, ms % 30 != 0 && ms != 10);
      const float correction = drift.state().yaw_correction_rad;
      const float delta = angle_delta(correction, last);
      require(delta >= -1e-6f && delta <= 2 * kPi / 180 * 0.01f + 1e-6f,
              "smooth drift correction exceeded degrees-per-second limit");
      if (ms % 30 != 0 && delta > 1e-6f) ++changes_between_camera_frames;
      last = correction;
    }
    require(last > 7 * kPi / 180 && last < 10 * kPi / 180,
            "continuous correction did not reduce persistent drift");
    require(changes_between_camera_frames > 20,
            "correction was stepped only on optical frames");
    SmoothYawScenario noise(right);
    noise.tick(0, 0, 0);
    for (int ms = 10; ms <= 2000; ms += 10) noise.tick(ms, 0, ms % 20 ? 0.4f : -0.4f);
    near(noise.state().yaw_correction_rad, 0, "sub-deadband optical noise moved aim");
  }
}

void smooth_yaw_rejects_bad_optics_and_recovers_without_a_snap() {
  SmoothYawScenario x;
  x.tick(0, 0, 0);
  x.tick(10, 0, 10);
  const float before_loss = x.state().yaw_correction_rad;
  x.tick(20, 0, 90, false);
  x.tick(10000, 30, 90, false);
  near(x.state().yaw_correction_rad, before_loss, "yaw moved during optical loss");
  x.tick(10010, 30, 40);
  require(std::abs(angle_delta(x.state().yaw_correction_rad, before_loss)) <=
              2 * kPi / 180 * 0.01f + 1e-6f,
          "reacquire spent time lost as an instantaneous correction budget");
  const auto good = x.state().yaw_correction_rad;
  x.tick(10020, 30, 80, true, 0.1f);
  x.tick(10030, 30, 80, true, 1, false);
  near(x.state().yaw_correction_rad, good, "low-confidence or rejected optics changed yaw");
  x.tick(10040, 30, 10);
  const auto last = x.state().yaw_correction_rad;
  x.tick(10200, 30, 10, true, 1, true, true);
  near(x.state().yaw_correction_rad, last, "stale cached optical pose changed yaw");
  require(x.state().yaw_last_action == "smooth_stale", "stale target was not diagnosed");

  SmoothYawScenario jump;
  jump.tick(0, 0, 0);
  jump.tick(10, 0, 60);
  jump.tick(20, 0, 0);
  near(jump.state().yaw_correction_rad, 0, "single optical yaw outlier moved aim");
  for (int ms = 30; ms < 230; ms += 10) {
    jump.tick(ms, 0, 60);
    near(jump.state().yaw_correction_rad, 0, "unconfirmed jump spent correction time");
  }
  jump.tick(230, 0, 60);
  require(jump.state().yaw_correction_rad > 0 && jump.state().yaw_correction_rad < 0.001f,
          "persistent changed reference was rejected forever or applied with a snap");

  SmoothYawScenario wrapped;
  wrapped.config().yaw_smooth_deadband_deg = 0;
  wrapped.tick(0, 0, 179);
  for (int ms = 10; ms <= 3000; ms += 10) wrapped.tick(ms, 0, -179);
  const auto turn = angle_delta(wrapped.state().yaw_correction_rad, 179 * kPi / 180);
  require(turn > 1 * kPi / 180 && turn < 2 * kPi / 180,
          "yaw wrap selected a long turn or froze correction");

  SmoothYawScenario disabled;
  disabled.config().yaw_correction_continuous = false;
  disabled.tick(0, 0, 0);
  for (int ms = 10; ms <= 200; ms += 10) disabled.tick(ms, 0, 10);
  near(disabled.state().yaw_correction_rad, 0, "CONTINUOUS=0 did not disable smooth updates");
  disabled.state().yaw_correction_requested = true;
  disabled.tick(210, 0, 10);
  require(disabled.state().yaw_correction_rad > 0,
          "explicit reacquire request did not start a bounded smooth correction");
}

void smooth_yaw_uses_elapsed_time_and_requires_observable_timed_poses() {
  for (int tick_ms : {5, 10, 25}) {
    SmoothYawScenario x;
    x.config().yaw_smooth_deadband_deg = 0;
    x.config().yaw_smooth_max_speed_deg_s = 360;
    x.tick(0, 0, 0);
    for (int ms = tick_ms; ms <= 2000; ms += tick_ms) x.tick(ms, 0, 10);
    near(x.state().yaw_correction_rad, 10 * kPi / 180 * (1 - std::exp(-1.f)),
         "complementary gain depends on runtime tick frequency", 1e-5f);
  }
  for (bool future : {false, true}) {
    SmoothYawScenario x;
    const auto now = x.s.prepare(0);
    x.s.hand.source_timestamp_ns = future ? now + 100'000'000 : 0;
    compose_runtime_controller_state(x.s.sequence, now, x.s.cfg, x.s.hand, x.s.hand,
        x.s.input, std::nullopt, 9.81f, &x.s.state);
    require(!x.state().yaw_correction_valid,
            "missing/future source time established a smooth yaw reference");
  }
  SmoothYawScenario vertical;
  vertical.tick(0, 0, 0);
  const auto now = vertical.s.prepare(10);
  vertical.s.input.left.imu.orientation_xyzw[0] = std::sin(kPi / 4);
  vertical.s.input.left.imu.orientation_xyzw[3] = std::cos(kPi / 4);
  vertical.s.hand.left.controller_qx = std::sin(kPi / 4);
  vertical.s.hand.left.controller_qw = std::cos(kPi / 4);
  compose_runtime_controller_state(vertical.s.sequence, now, vertical.s.cfg,
      vertical.s.hand, vertical.s.hand, vertical.s.input, std::nullopt, 9.81f, &vertical.s.state);
  near(vertical.state().yaw_correction_rad, 0, "vertical pose invented a yaw correction");
  require(vertical.state().yaw_last_action == "smooth_unobservable",
          "vertical heading was not diagnosed as unobservable");
}

int main() {
  smooth_yaw_preserves_aiming_and_bounds_drift_correction();
  smooth_yaw_rejects_bad_optics_and_recovers_without_a_snap();
  smooth_yaw_uses_elapsed_time_and_requires_observable_timed_poses();
  presentation_yaw_matches_camera_time_with_delayed_delivery();
  startup_yaw_waits_for_stability_without_freezing_real_motion();
  startup_yaw_restarts_for_jumps_gaps_and_cached_frames();
  first_yaw_confirmation_controls_force_integration_on_both_sides();
  first_yaw_confirmation_controls_force_integration_on_both_sides(true);
  translation_checks_direction_with_source_time_and_unknown_velocity();
  translation_rejects_unobservable_or_incomplete_motion();
  checked_force_frame_survives_rotation_refits_and_resets();
  translation_heading_uses_changing_acceleration_without_fitting_bias();
  refined_force_heading_calibrates_both_sides_without_changing_aim();
  refined_force_heading_calibrates_both_sides_without_changing_aim(true);
  irregular_optical_window_retains_acceleration_time();
  window_launch_and_braking_share_the_same_imu_cursor();
  distance_freeze_keeps_physical_velocity_through_braking();
  distance_recovery_does_not_spend_frozen_time_as_speed_budget();
  static_assert(sizeof(ControllerInputV3) == 1432, "controller V3 ABI changed");
  late_pre_optical_force_is_not_reintegrated_after_loss();
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
  interrupted_correction_does_not_launch_stationary_hand();
  optical_prediction_uses_measurements_before_output_smoothing();
  optical_velocity_requires_matching_accepted_measurement();
  time_decay_integrates_published_velocity();
  speed_distance_and_timeout_limits();
  std::cout << "IMU prediction: startup confirmation, force-frame direction, rigid grips, timed calibration, velocity fallback, sample time, stale optics, reacquire and limits: OK\n";
}
