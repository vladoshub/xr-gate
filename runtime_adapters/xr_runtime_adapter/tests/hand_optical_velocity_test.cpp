#include <mercury_hand_tracking/mercury_runtime_loader.hpp>
#include "hand_pose_stability_filter.hpp"
#include "override_controller.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace hand = xr_runtime_adapter::hand_filter;
struct PoseInput { float x; float qw; float qy; uint32_t status; };
struct Camera { std::vector<uint8_t> gray8; uint32_t width=16, height=1; };
struct Pair {
  Camera cam0, cam1;
  uint64_t timestamp_ns=0, sequence_value=0;
  uint64_t sequence() const { return sequence_value; }
};
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void near(float actual, float expected, const char* message, float tolerance=1e-4f) {
  require(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,message);
}
bool valid_v(const xr_runtime::HandSideF32V2& side) {
  return (side.flags & xr_runtime::HAND_LINEAR_VELOCITY_VALID)!=0;
}
struct Fixture {
  xr_tracking::MercuryRuntimeProcessor processor;
  uint64_t sequence=0;
  explicit Fixture(const char* library) : processor(config(library)) {}
  static xr_tracking::MercuryRuntimeConfig config(const char* library) {
    xr_tracking::MercuryRuntimeConfig cfg;
    cfg.library_path=library;
    return cfg;
  }
  xr_runtime::HandTrackingFrameF32V2 tick(uint64_t ms, float x, float angle=0,
                                        uint32_t status=1, bool negate=false,
                                        uint64_t reset_counter=0) {
    const float sign=negate ? -1.0f : 1.0f;
    PoseInput input{x, sign*std::cos(angle/2), sign*std::sin(angle/2),status};
    Pair pair;
    pair.timestamp_ns=ms*1'000'000;
    pair.sequence_value=++sequence;
    pair.cam0.gray8.resize(sizeof(input));
    std::memcpy(pair.cam0.gray8.data(),&input,sizeof(input));
    pair.cam1=pair.cam0;
    const auto raw=processor.process(pair,reset_counter);
    xr_runtime::HandTrackingFrameF32V2 frame{};
    static_assert(sizeof(frame)==sizeof(raw));
    const auto* from=reinterpret_cast<const unsigned char*>(&raw);
    auto* to=reinterpret_cast<unsigned char*>(&frame);
    std::copy(from,from+sizeof(raw),to);
    return frame;
  }
};
hand::HandPoseStabilityFilterConfig config(bool window=true) {
  hand::HandPoseStabilityFilterConfig cfg;
  cfg.enabled=true;
  cfg.hold_lost_ms=100;
  cfg.predict_lost_ms=0;
  cfg.max_reacquire_jump_m=.25;
  cfg.max_continuity_velocity_mps=7;
  cfg.prediction_window_mode=window;
  cfg.prediction_window_ms=120;
  return cfg;
}
void backend_timestamps(const char* library) {
  Fixture f(library);
  require(!valid_v(f.tick(1000,0).left),"first backend pose has velocity");
  auto frame=f.tick(1033,.033f,.033f);
  near(frame.left.vx,1,"backend used publication time for velocity");
  near(frame.left.wy,1,"backend angular velocity frame is wrong");
  require(frame.source_timestamp_ns==1'033'000'000ULL,"camera timestamp lost");
  require(!valid_v(f.tick(1033,.1f).left),"duplicate backend velocity is valid");
  require(!valid_v(f.tick(1010,10).left),"out-of-order backend velocity is valid");
  frame=f.tick(1066,.066f,.033f,1,true);
  near(frame.left.vx,1,"invalid timestamp poisoned backend position anchor");
  near(frame.left.wy,0,"quaternion sign flip became angular velocity");
  require(!valid_v(f.tick(1100,0,0,0).left),"lost backend pose has velocity");
  require(!valid_v(f.tick(1133,1).left),"first reacquired backend pose has velocity");
  near(f.tick(1166,1.033f).left.vx,1,"backend did not reinitialize after loss");
  require(!valid_v(f.tick(1500,2).left),"long backend interval was differentiated");
  require(!valid_v(f.tick(1533,10,0,1,false,1).left),"reset counter retained old derivative");
  near(f.tick(1566,10.033f,0,1,false,1).left.vx,1,"reset backend velocity failed");
}
void accepted_pose_velocity(const char* library, bool window) {
  Fixture f(library);
  hand::HandPoseStabilityFilter gate(config(window));
  require(!valid_v(gate.filter(f.tick(1000,0)).left),"first accepted pose has velocity");
  gate.set_diagnostics_enabled(true);
  auto rejected=gate.filter(f.tick(1033,.3f,1));
  require(gate.last_diagnostics().left.mode.find("reject_")==0, "diagnostics lost outlier rejection reason");
  require(rejected.left.status!=1,"position outlier was accepted");
  require(!valid_v(rejected.left),"held outlier retained velocity");
  auto raw=f.tick(1066,.066f,.066f);
  require(raw.left.vx<-7,"fixture no longer reproduces backend contamination");
  auto accepted=gate.filter(raw);
  require(gate.last_diagnostics().left.mode.find("accept_")==0, "diagnostics lost accepted pose decision");
  require(accepted.left.status==1 && valid_v(accepted.left),"good pose not accepted");
  near(accepted.left.vx,1,"rejected position poisoned published velocity");
  near(accepted.left.wy,1,"rejected quaternion poisoned published angular velocity");
  auto duplicate=f.tick(1066,.2f);
  near(gate.filter(duplicate).left.controller_px,.066f,"duplicate replaced accepted position");
  near(gate.filter(duplicate).left.vx,1,"runtime tick erased accepted velocity");
  gate.filter(f.tick(1010,10));
  near(gate.filter(f.tick(1099,.099f,.099f)).left.vx,1,"old frame poisoned runtime velocity");
  require(!valid_v(gate.filter(f.tick(1132,0,0,0)).left),"lost pose has velocity");
  accepted=gate.filter(f.tick(1165,.165f));
  require(!valid_v(accepted.left),"first optical return differentiated across loss");
  near(gate.filter(f.tick(1198,.198f)).left.vx,1,"runtime velocity did not recover");
  accepted=gate.filter(f.tick(500,1,0,1,false,1));
  require(!valid_v(accepted.left) && accepted.left.status==1,"new epoch used old cache");
}
void window_noise_and_limits(const char* library) {
  Fixture a(library),b(library);
  hand::HandPoseStabilityFilter window(config(true)),instant(config(false));
  double window_error=0,instant_error=0;
  for(int i=0;i<12;++i) {
    const float x=i*.033f + (i%2 ? .005f : -.005f);
    const auto va=window.filter(a.tick(1000+i*33,x)).left;
    const auto vb=instant.filter(b.tick(1000+i*33,x)).left;
    if(i>=4) { window_error+=std::abs(va.vx-1); instant_error+=std::abs(vb.vx-1); }
  }
  require(window_error<instant_error*.4,"accepted pose window did not suppress noise");
  hand::OpticalHandVelocity motion;
  auto side=a.tick(2000,0).left;
  motion.observe(side,2'000'000'000ULL,false,120,2);
  side=a.tick(2010,.5f,1).left;
  side=motion.observe(side,2'010'000'000ULL,false,120,2);
  near(side.vx,2,"published linear velocity is unbounded");
  near(side.wy,30,"published angular velocity is unbounded");
  Fixture f(library);
  hand::HandPoseStabilityFilter gate(config());
  gate.filter(f.tick(1000,0));
  require(!valid_v(gate.filter(f.tick(1400,.01f)).left),"runtime differentiated across long gap");
}
void reacquire_and_sides(const char* library) {
  Fixture f(library);
  auto cfg=config();
  cfg.hold_lost_ms=0; cfg.predict_lost_ms=500;
  cfg.reacquire_blend_ms=60;
  cfg.publish_predicted_velocity=true; cfg.prediction_damping=1;
  cfg.prediction_time_decay_enabled=false;
  hand::HandPoseStabilityFilter gate(cfg);
  gate.filter(f.tick(1000,0));
  gate.filter(f.tick(1033,.033f));
  auto predicted=gate.filter(f.tick(1066,0,0,0));
  require(predicted.left.status==2,"prediction fixture did not predict");
  auto returning=f.tick(1099,.099f);
  returning.left.vx=50;
  auto blend=gate.filter(returning);
  require(blend.left.status==2,"reacquire did not blend");
  require(std::abs(blend.left.vx)<=2,"reacquire used raw contaminated target velocity");
  gate.filter(f.tick(1132,.132f));
  near(gate.filter(f.tick(1165,.165f)).left.vx,1,"completed blend restored raw velocity");
  gate.reset();
  auto first=f.tick(2000,0);
  first.right=first.left; first.right.handedness=2; first.right.controller_px=.1f;
  gate.filter(first);
  auto second=f.tick(2033,.033f);
  second.right=second.left; second.right.handedness=2; second.right.controller_px=.034f;
  const auto both=gate.filter(second);
  near(both.left.vx,1,"left history mixed with right");
  near(both.right.vx,-2,"right history mixed with left");
}

void long_session_preserves_flick_velocity_through_controller_synthesis(const char* library) {
  namespace controller = xr_runtime_adapter::override_controller;
  using namespace xr_runtime;
  Fixture f(library);
  hand::HandPoseStabilityFilter gate(config());
  controller::RuntimeControllerSynthesisConfig cfg;
  controller::RuntimeControllerSynthesisState state;
  ControllerInputV3 input;
  cfg.left_orientation_source=cfg.right_orientation_source=
      controller::RuntimeControllerOrientationSource::ImuOverrideControllerRuntime;
  cfg.left_imu_motion.position_prediction_enabled=true;
  cfg.right_imu_motion=cfg.left_imu_motion;
  for(auto* side : {&input.left,&input.right}) {
    side->status=CONTROLLER_INPUT_ACTIVE;
    side->flags=CONTROLLER_DEVICE_IMU_ACTIVE | CONTROLLER_DEVICE_BUTTONS_VALID;
    side->buttons=CONTROLLER_BUTTON_TRIGGER;
    side->trigger=1;
    side->imu.status=CONTROLLER_IMU_ACTIVE;
    side->imu.data_flags=CONTROLLER_IMU_ORIENTATION_VALID;
    side->imu.orientation_xyzw[3]=1;
  }
  float previous_x=0;
  double peak_speed=0;
  for(uint64_t ms=0;ms<=360'000;ms+=33) {
    // Repeated 0.63 m/s motions stay in the camera field throughout six
    // minutes. Exercise the real loader derivative, accepted-pose window and
    // controller synthesis rather than injecting precomputed output velocity.
    const float x=.1f*std::sin(2*3.14159265358979323846*double(ms%1000)/1000);
    auto raw=f.tick(1000+ms,x);
    raw.right=raw.left;
    raw.right.handedness=2;
    raw.right.controller_px=-x;
    raw.right.vx=-raw.left.vx;
    raw.flags |= HAND_FLAG_RIGHT_VALID;
    raw.hand_count=2;
    const auto accepted=gate.filter(raw);
    input.sequence=raw.sequence;
    input.timestamp_ns=raw.timestamp_ns;
    for(auto* side : {&input.left,&input.right}) {
      side->imu.sequence=raw.sequence;
      side->imu.orientation_timestamp_ns=side->imu.latest_sample_timestamp_ns=raw.source_timestamp_ns;
    }
    // Runtime resets its IMU synthesis state when override_controller restarts.
    // That must not be necessary to recover an accepted optical derivative.
    if(ms==180'015) state={};
    const auto output=controller::compose_runtime_controller_state(raw.sequence,raw.timestamp_ns,
        cfg,accepted,raw,input,std::nullopt,9.81f,&state);
    if(ms>0) {
      near(raw.left.vx,(x-previous_x)/.033f,"long backend session lost camera derivative");
      require(valid_v(accepted.left) && valid_v(accepted.right),
              "long session lost accepted optical velocity");
      near(output.left.linear_velocity[0],accepted.left.vx,
           "controller synthesis erased measured left flick velocity");
      near(output.right.linear_velocity[0],accepted.right.vx,
           "controller synthesis erased measured right flick velocity");
      near(output.left.trigger,1,"long session changed left trigger");
      near(output.right.trigger,1,"long session changed right trigger");
      peak_speed=std::max(peak_speed,std::abs(double(output.left.linear_velocity[0])));
    }
    previous_x=x;
  }
  require(peak_speed>.5,"optical flick was over-smoothed before controller output");
}
int main(int argc,char** argv) {
  try {
    require(argc==2,"mock library path required");
    backend_timestamps(argv[1]);
    accepted_pose_velocity(argv[1],false);
    accepted_pose_velocity(argv[1],true);
    window_noise_and_limits(argv[1]);
    reacquire_and_sides(argv[1]);
    long_session_preserves_flick_velocity_through_controller_synthesis(argv[1]);
    std::cout<<"Optical velocities: timestamps, accepted poses, noise, limits, reacquire and both sides: OK\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';
    return 1;
  }
}
