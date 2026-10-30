#pragma once

#include "simple_gestures.hpp"

#include <cstdio>

namespace xr_runtime_adapter::gestures {

// An explicitly armed, per-hand touchpad. Centre and axes are in current HMD
// coordinates (+X right, +Y up), never the hand/controller presentation frame.
class SimpleTouchpad {
 public:
  static constexpr double default_radius_m = .10;
  explicit SimpleTouchpad(bool enabled = true, double radius_m = default_radius_m)
      : enabled_(enabled), radius_m_(radius_m) {}
  bool active() const { return left_.active || right_.active; }
  bool left_active() const { return left_.active; }
  bool right_active() const { return right_.active; }
  bool left_reserved() const { return left_.active || left_.exit_vee; }
  bool right_reserved() const { return right_.active || right_.exit_vee; }
  bool left_exit_held() const { return left_.exit_vee; }
  bool right_exit_held() const { return right_.exit_vee; }

  void apply(xr_runtime::HandTrackingFrameF32V2& hands, const xr_runtime::HmdPoseF64V1* head,
             uint64_t now, uint64_t origin, bool left_enabled, bool right_enabled,
             bool left_physical = false, bool right_physical = false) {
    if (origin != origin_ || hands.reset_counter != reset_ ||
        (head && head->reset_counter != head_reset_)) {
      cancel(left_); cancel(right_);
      origin_ = origin; reset_ = hands.reset_counter;
      if (head) head_reset_ = head->reset_counter;
    }
    const uint64_t stamp = hands.source_timestamp_ns ? hands.source_timestamp_ns : hands.timestamp_ns;
    const bool fresh = enabled_ && hands.sequence && stamp &&
        (stamp <= now ? double(now - stamp) / 1e6 <= config_.max_age_ms : stamp - now <= 2000000);
    const bool valid_head = head && (head->flags & xr_runtime::HMD_FLAG_POSE_VALID) != 0 &&
        head->tracking_status == 2 && std::isfinite(head->px) && std::isfinite(head->py) &&
        std::isfinite(head->pz) && normalize_head(*head);
    const emulation::Vec3 viewer = valid_head ? emulation::Vec3{head->px, head->py, head->pz}
                                             : emulation::Vec3{};
    update(hands.left, left_, viewer, stamp, fresh && valid_head && left_enabled && !left_physical,head);
    update(hands.right, right_, viewer, stamp, fresh && valid_head && right_enabled && !right_physical,head);
  }

  void publish(xr_runtime::RuntimeControllerStateFrameV1& out) {
    if (!enabled_) return;
    publish_side(out.left, left_);
    publish_side(out.right, right_);
    if (left_.active) out.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_LEFT_INPUT_VALID;
    if (right_.active) out.flags |= xr_runtime::RUNTIME_CONTROLLER_FRAME_RIGHT_INPUT_VALID;
  }

 private:
  struct Side {
    bool active = false, away = false, click = false, vee = false, thumb_up = false;
    bool candidate = false, owned = false, previous_click = false, arming = false;
    bool exit_vee = false, exit_candidate = false;
    uint32_t presses = 0, releases = 0;
    uint64_t sample = 0, since = 0, arm_since = 0;
    uint64_t exit_since = 0;
    std::array<bool, 4> extended{}, curled{};
    emulation::Vec3 centre{};
    float x = 0, y = 0;
  };

  static void cancel(Side& s) {
    s.active = s.away = s.click = s.vee = s.thumb_up = s.candidate = s.arming = false;
    s.sample = s.since = s.arm_since = 0;
    s.exit_vee = s.exit_candidate = false; s.exit_since = 0;
    s.extended = {}; s.curled = {};
    s.x = s.y = 0;
    // Keep published click counters across releases, loss and recentering.
  }

  static void reserve(xr_runtime::HandSideF32V2& hand, bool click = false) {
    hand.pinch_strength=hand.grab_strength=0;
    hand.pinch_active=hand.grab_active=0;
    hand.reserved0=click ? static_cast<uint32_t>(xr_runtime::CONTROLLER_BUTTON_THUMBSTICK) : 0;
    hand.flags |= xr_runtime::HAND_PINCH_VALID | xr_runtime::HAND_GRAB_VALID;
  }

  bool normalize_head(const xr_runtime::HmdPoseF64V1& head) {
    q_ = {head.qw, head.qx, head.qy, head.qz};
    double n = 0;
    for (double v : q_) { if (!std::isfinite(v)) return false; n += v*v; }
    if (!std::isfinite(n) || n < 1e-8) return false;
    n = std::sqrt(n);
    for (double& v : q_) v /= n;
    return true;
  }

  emulation::Vec3 head_local(const emulation::Vec3& point, const emulation::Vec3& viewer) const {
    const double w=q_[0], x=q_[1], y=q_[2], z=q_[3];
    const double px=point[0]-viewer[0], py=point[1]-viewer[1], pz=point[2]-viewer[2];
    // Transpose of the normalized head-to-world rotation.
    return {(1-2*(y*y+z*z))*px + 2*(x*y+w*z)*py + 2*(x*z-w*y)*pz,
            2*(x*y-w*z)*px + (1-2*(x*x+z*z))*py + 2*(y*z+w*x)*pz,
            2*(x*z+w*y)*px + 2*(y*z-w*x)*py + (1-2*(x*x+y*y))*pz};
  }

  static double dot(const emulation::Vec3& a, const emulation::Vec3& b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
  }

  static bool facing_away(const emulation::HandMeasurement& h,
                          const emulation::Vec3& viewer) {
    emulation::Vec3 toward{};
    for (size_t i=0;i<3;++i) toward[i]=viewer[i]-h.palm_center[i];
    const double distance=std::sqrt(dot(toward,toward));
    // The 35-degree cone gates entry only. Use the
    // palm bones, independent of finger curl, thumb pose and presentation yaw.
    return std::isfinite(distance) && distance >= .05 && distance <= 2 &&
        -dot(h.palm_basis[2],toward)/distance >= .8191520442889918;
  }

  // Entry is deliberately stricter than gameplay finger extension. Its relaxed
  // MCP threshold accepts forward-pointing fingers with folded knuckles, which
  // must never seize an entire touchpad session while making/releasing a pinch.
  static bool activation_pose(const xr_runtime::HandSideF32V2& hand,
                              const emulation::HandMeasurement& h,
                              const emulation::Vec3& viewer) {
    if (!h.thumb_valid || h.thumb_clearance < .85 || h.thumb_straightness < .85 ||
        !std::isfinite(h.pinch_ratio) || h.pinch_ratio < .85 ||
        std::any_of(h.curl.begin(),h.curl.end(),[](double v){return v > .20;})) return false;
    if (!facing_away(h,viewer)) return false;
    const auto& wrist=hand.joints[0];
    for (size_t f=0;f<4;++f) {
      const auto& base=hand.joints[5+4*f];
      const auto& pip=hand.joints[6+4*f];
      emulation::Vec3 meta{base.px-wrist.px,base.py-wrist.py,base.pz-wrist.pz};
      const emulation::Vec3 proximal{pip.px-base.px,pip.py-base.py,pip.pz-base.pz};
      const double normal=dot(meta,h.palm_basis[2]);
      for (size_t i=0;i<3;++i) meta[i]-=normal*h.palm_basis[2][i];
      const double length=std::sqrt(dot(meta,meta));
      if (!std::isfinite(length) || length < 1e-5) return false;
      // Flexion plane only: sideways finger spread is welcome. At most 45
      // degrees at every knuckle; a claw can have straight phalanges/curl=0.
      const double forward=dot(proximal,meta)/length;
      if (forward <= 0 || std::abs(dot(proximal,h.palm_basis[2])) > forward) return false;
    }
    return true;
  }

  void update(xr_runtime::HandSideF32V2& hand, Side& s,
              const emulation::Vec3& viewer, uint64_t stamp, bool enabled,
              const xr_runtime::HmdPoseF64V1* head) {
    const bool valid = enabled && hand.status == 1 &&
        (hand.flags & (xr_runtime::HAND_POSE_VALID | xr_runtime::HAND_JOINTS_VALID)) ==
            (xr_runtime::HAND_POSE_VALID | xr_runtime::HAND_JOINTS_VALID) &&
        std::isfinite(hand.confidence) && hand.confidence >= config_.min_confidence;
    if (!valid) {
      // Do not clear controller input copied into this side: the adapter's
      // physical-source path already cancelled the session before composition.
      cancel(s); return;
    }
    if (stamp < s.sample) cancel(s);
    const bool thumb_up=simple_thumb_up(hand,head,config_.min_confidence,s.thumb_up);
    const auto h = emulation::measure_hand(hand, config_.min_confidence);
    if (!h.palm_valid || std::any_of(h.curl.begin(),h.curl.end(),
        [](double v) { return !std::isfinite(v) || v < 0; })) {
      cancel(s);
      return;
    }
    for (size_t i=0;i<4;++i) {
      const double on=i==3 ? config_.little_extension_on : config_.extension_on;
      const double off=i==3 ? config_.little_extension_off : config_.extension_off;
      s.extended[i] = 1-h.curl[i] >= (s.extended[i] ? off : on);
      s.curled[i] = h.curl[i] >= (s.curled[i] ? config_.curl_off : config_.curl_on);
    }
    s.thumb_up=thumb_up;
    s.vee=!thumb_up && simple_v(hand,s.extended,s.curled,s.vee || s.exit_vee) &&
        simple_v_facing(h,head,s.vee || s.exit_vee);
    if (s.exit_vee) {
      // Consume the exit pose until released; it must not also send X/A.
      if (s.vee) { reserve(hand); s.sample=stamp; return; }
      s.exit_vee=false;
    }
    if (s.active && s.vee) {
      if (stamp!=s.sample) {
        if (!s.exit_candidate || double(stamp-s.sample)/1e6>config_.max_age_ms) s.exit_since=stamp;
        s.exit_candidate=true;
        if (double(stamp-s.exit_since)/1e6>=config_.button_confirm_ms) {
          cancel(s); s.exit_vee=true; reserve(hand); s.sample=stamp; return;
        }
      }
    } else { s.exit_candidate=false; s.exit_since=0; }
    const bool extended=std::all_of(s.extended.begin(),s.extended.end(),[](bool v){return v;});
    const auto palm=emulation::measure_open_palm_pose(h,extended,config_,&viewer,false,s.away);
    // Palm orientation only controls entry; an active panel permits wrist rotation.
    const auto point=head_local(h.palm_center,viewer);
    s.away=palm.away_from_viewer;
    if (!s.active) {
      if (thumb_up || !s.away || !activation_pose(hand,h,viewer)) {
        s.arming=false; s.arm_since=0;
      } else if (stamp!=s.sample) {
        if (!s.arming || double(stamp-s.sample)/1e6 > config_.max_age_ms) s.arm_since=stamp;
        s.arming=true;
        // A single noisy open sample cannot interrupt a pinch. Source time,
        // not repeated adapter ticks, confirms entry in the usual 45 ms.
        if (double(stamp-s.arm_since)/1e6 >= config_.button_confirm_ms) {
          s.active = s.owned = true;
          s.centre = point;  // Capture on recognition, never on later reopening.
          s.arming=false; s.arm_since=0;
        }
      }
    }
    if (s.active) {
      double x=(point[0]-s.centre[0])/radius_m_, y=(point[1]-s.centre[1])/radius_m_;
      const double scale=std::max(1.0,std::hypot(x,y));
      s.x=static_cast<float>(x/scale); s.y=static_cast<float>(y/scale);
      if (!thumb_up) { s.click=s.candidate=false; s.since=0; }
      else if (stamp!=s.sample) {
        if (!s.candidate) { s.candidate=true; s.since=stamp; }
        s.click=double(stamp-s.since)/1e6 >= config_.button_confirm_ms;
      }
      // Clear backend input before the simple recognizer adds the allowed
      // pinch and palm Grip channels alongside the authoritative thumb click.
      reserve(hand,s.click);
    }
    s.sample=stamp;
  }

  static void publish_side(xr_runtime::RuntimeControllerSideStateV1& out, Side& s) {
    using namespace xr_runtime;
    const uint64_t bit=CONTROLLER_BUTTON_THUMBSTICK;
    if (out.source_mask & RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT) {
      // Physical input and its tap counters retain priority unchanged.
      if (s.previous_click) { ++s.releases; s.previous_click=false; }
      return;
    }
    if (s.active) {
      constexpr uint64_t allowed=CONTROLLER_BUTTON_TRIGGER|CONTROLLER_BUTTON_GRIP;
      out.buttons=(out.buttons & allowed) | (s.click ? bit : 0);
      out.touches=(out.touches & allowed) | bit;
      out.thumbstick_x=s.x; out.thumbstick_y=s.y;
      out.flags |= RUNTIME_CONTROLLER_CONNECTED | RUNTIME_CONTROLLER_INPUT_VALID;
      out.source_mask |= RUNTIME_CONTROLLER_SOURCE_HAND_GESTURES;
      out.last_input_ns=s.sample;
      std::snprintf(out.debug_source,sizeof(out.debug_source),"simple_touchpad");
    }
    if (s.owned) {
      const bool clicked=(out.buttons & bit)!=0;
      out.changed_buttons &= ~bit;
      if (clicked != s.previous_click) {
        if (clicked) ++s.presses; else ++s.releases;
        out.changed_buttons |= bit;
      }
      s.previous_click=clicked;
      out.press_counters[5]=s.presses; out.release_counters[5]=s.releases;
    }
  }

  bool enabled_;
  double radius_m_;
  emulation::Config config_=simple_palm_config();
  std::array<double,4> q_{{1,0,0,0}};
  Side left_,right_;
  uint64_t origin_=0,reset_=0,head_reset_=0;
};

}  // namespace xr_runtime_adapter::gestures
