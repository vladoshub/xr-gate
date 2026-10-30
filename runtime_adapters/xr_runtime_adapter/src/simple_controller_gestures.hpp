#pragma once

#include "simple_gestures.hpp"

namespace xr_runtime_adapter::gestures {

// Geometry owns simple-mode input. Backend button/strength values must not
// bypass exclusivity or turn a folded index into a pinch.
class SimpleControllerGestures {
 public:
  void apply(xr_runtime::HandTrackingFrameF32V2& hands, const emulation::Vec3* viewer,
             uint64_t now, uint64_t origin, bool left_enabled, bool right_enabled,
             bool left_physical, bool right_physical, bool left_pad, bool right_pad) {
    if (origin!=origin_ || hands.reset_counter!=reset_) {
      left_=right_={};menu_={};origin_=origin;reset_=hands.reset_counter;
    }
    const uint64_t stamp=hands.source_timestamp_ns?hands.source_timestamp_ns:hands.timestamp_ns;
    const bool fresh=hands.sequence && stamp &&
        (stamp<=now ? double(now-stamp)/1e6<=config_.max_age_ms : stamp-now<=2000000);
    palm_grip_.apply(hands,viewer,now,origin,left_enabled&&!left_pad,right_enabled&&!right_pad,
                     left_physical,right_physical);
    update(hands.left,left_,stamp,fresh&&left_enabled,left_physical,left_pad,true);
    update(hands.right,right_,stamp,fresh&&right_enabled,right_physical,right_pad,false);
    const bool inward=pointing_together(hands.left,hands.right);
    if (!inward) menu_={};
    else {
      confirm(menu_,Pose::Menu,stamp);
      clear(hands.left);clear(hands.right);
      if(menu_.active==Pose::Menu)hands.left.reserved0=xr_runtime::CONTROLLER_BUTTON_MENU;
    }
    hands.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
  }

  void publish(xr_runtime::RuntimeControllerStateFrameV1& out) {
    publish_side(out.left,counters_left_);publish_side(out.right,counters_right_);
  }

 private:
  enum class Pose {None,V,ThumbUp,Pinch,Fist,Menu};
  struct Channel {Pose candidate=Pose::None,active=Pose::None;uint64_t since=0,sample=0;};
  struct Side {
    std::array<bool,4> extended{},curled{};
    bool valid=false,point=false,pinch=false,thumb_up=false;
    Channel channel;
  };
  struct Counters {uint64_t previous=0;std::array<uint32_t,32> press{},release{};};
  static constexpr uint64_t managed=xr_runtime::CONTROLLER_BUTTON_TRIGGER|xr_runtime::CONTROLLER_BUTTON_GRIP|
      xr_runtime::CONTROLLER_BUTTON_MENU|xr_runtime::CONTROLLER_BUTTON_A|xr_runtime::CONTROLLER_BUTTON_B|
      xr_runtime::CONTROLLER_BUTTON_X|xr_runtime::CONTROLLER_BUTTON_Y;

  static emulation::Vec3 sub(const emulation::Vec3& a,const emulation::Vec3& b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
  static double dot(const emulation::Vec3& a,const emulation::Vec3& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
  static bool unit(emulation::Vec3& v) {double n=std::sqrt(dot(v,v));if(!std::isfinite(n)||n<1e-5)return false;for(auto& x:v)x/=n;return true;}
  static emulation::Vec3 joint(const xr_runtime::HandSideF32V2& h,size_t i) {return {h.joints[i].px,h.joints[i].py,h.joints[i].pz};}
  static void clear(xr_runtime::HandSideF32V2& h) {
    h.pinch_strength=h.grab_strength=0;h.pinch_active=h.grab_active=0;h.reserved0=0;
    h.flags |= xr_runtime::HAND_PINCH_VALID|xr_runtime::HAND_GRAB_VALID;
  }
  static void confirm(Channel& c,Pose pose,uint64_t stamp) {
    if(pose==Pose::None){c={};return;}
    if(stamp<c.sample)c={};
    if(c.candidate!=pose){c.candidate=pose;c.active=Pose::None;c.since=stamp;}
    if(stamp!=c.sample && double(stamp-c.since)/1e6>=45)c.active=pose;
    c.sample=stamp;
  }

  void update(xr_runtime::HandSideF32V2& h,Side& s,uint64_t stamp,bool enabled,bool physical,bool pad,bool left) {
    if(physical){s={};return;}
    if(pad){s={};return;} // Touchpad already reserved and wrote its click.
    const float palm_grip=h.grab_strength;
    clear(h);
    const bool valid=enabled && h.status==1 &&
        (h.flags&(xr_runtime::HAND_POSE_VALID|xr_runtime::HAND_JOINTS_VALID))==
        (xr_runtime::HAND_POSE_VALID|xr_runtime::HAND_JOINTS_VALID) &&
        std::isfinite(h.confidence) && h.confidence>=config_.min_confidence;
    if(!valid){s={};return;}
    if(stamp<s.channel.sample)s={};
    const auto m=emulation::measure_hand(h,config_.min_confidence);
    if(!m.palm_valid || std::any_of(m.curl.begin(),m.curl.end(),[](double v){return !std::isfinite(v)||v<0;})){s={};return;}
    s.valid=true;
    for(size_t i=0;i<4;++i) {
      const double on=i==3?config_.little_extension_on:config_.extension_on;
      const double off=i==3?config_.little_extension_off:config_.extension_off;
      s.extended[i]=1-m.curl[i]>=(s.extended[i]?off:on);
      s.curled[i]=m.curl[i]>=(s.curled[i]?config_.curl_off:config_.curl_on);
    }
    s.point=s.extended[0] && s.curled[1] && s.curled[2] && s.curled[3];
    auto index=sub(joint(h,8),joint(h,5)),middle=sub(joint(h,12),joint(h,9));
    const bool directions=unit(index)&&unit(middle);
    const bool was_v=s.channel.candidate==Pose::V;
    const bool vee=s.extended[0] && s.extended[1] && s.curled[2] && s.curled[3] && directions &&
        dot(index,middle)<=std::cos((was_v?8.:12.)*3.141592653589793/180.);
    const bool closed=std::all_of(s.curled.begin(),s.curled.end(),[](bool v){return v;});
    const bool closed_index=m.index_chain_curl>=.80 || m.index_knuckle_curl>=.95;
    // A thumb held above the folded knuckles is distinct from the thumb lying
    // across a fist. Palm-local distances work at arbitrary wrist orientations.
    s.thumb_up=closed && closed_index && m.thumb_valid &&
        m.thumb_clearance>=(s.thumb_up?.70:.78) && m.thumb_straightness>=(s.thumb_up?.70:.76) &&
        m.thumb_pose[11]>=(s.thumb_up?.15:.25);
    const bool tucked=m.thumb_valid && (m.thumb_clearance<.73 || m.thumb_straightness<.72 || m.thumb_pose[11]<.15);
    // Reuse emulation's scale-normalized tip distance and folded-index guard.
    // The former simple path had an ~3 mm release band after two response curves.
    s.pinch=m.thumb_valid && m.thumb_clearance>=.60 && m.index_chain_curl<.80 &&
        m.index_knuckle_curl<.95 && m.pinch_ratio>=0 && m.pinch_ratio<=(s.pinch?.60:.40);
    const Pose pose=vee?Pose::V:s.pinch?Pose::Pinch:s.thumb_up?Pose::ThumbUp:
        closed&&closed_index&&tucked?Pose::Fist:Pose::None;
    confirm(s.channel,pose,stamp);
    if(s.channel.active==Pose::V)h.reserved0=left?xr_runtime::CONTROLLER_BUTTON_X:xr_runtime::CONTROLLER_BUTTON_A;
    else if(s.channel.active==Pose::ThumbUp)h.reserved0=left?xr_runtime::CONTROLLER_BUTTON_Y:xr_runtime::CONTROLLER_BUTTON_B;
    else if(s.channel.active==Pose::Pinch){
      h.pinch_strength=static_cast<float>(std::clamp((.60-m.pinch_ratio)/.20,0.,1.));h.pinch_active=1;
    } else if(s.channel.active==Pose::Fist){h.pinch_strength=h.grab_strength=1;h.pinch_active=h.grab_active=1;}
    else if(pose==Pose::None && palm_grip>0){h.grab_strength=palm_grip;h.grab_active=1;}
  }

  bool pointing_together(const xr_runtime::HandSideF32V2& l,const xr_runtime::HandSideF32V2& r) const {
    if(!left_.valid || !right_.valid || !left_.point || !right_.point)return false;
    auto across=sub(joint(r,5),joint(l,5));const double span=std::sqrt(dot(across,across));
    auto tips=sub(joint(r,8),joint(l,8));
    if(span<.08 || span>1.0 || std::sqrt(dot(tips,tips))>.55 || !unit(across))return false;
    auto a=sub(joint(l,8),joint(l,5)),b=sub(joint(r,8),joint(r,5));
    const double threshold=menu_.active==Pose::Menu?.70:.80;
    return unit(a)&&unit(b)&&dot(a,across)>=threshold && dot(b,across)<=-threshold;
  }

  static void publish_side(xr_runtime::RuntimeControllerSideStateV1& out,Counters& c) {
    if(out.source_mask&xr_runtime::RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT){c.previous=0;return;}
    const uint64_t buttons=out.buttons&managed,changed=buttons^c.previous;
    for(size_t i=0;i<32;++i)if(managed&(uint64_t(1)<<i)) {
      if(changed&(uint64_t(1)<<i)){if(buttons&(uint64_t(1)<<i))++c.press[i];else ++c.release[i];}
      out.press_counters[i]=c.press[i];out.release_counters[i]=c.release[i];
    }
    out.changed_buttons=(out.changed_buttons&~managed)|changed;c.previous=buttons;
  }

  SimplePalmGrip palm_grip_;
  emulation::Config config_=simple_palm_config();
  Side left_,right_;Channel menu_;Counters counters_left_,counters_right_;
  uint64_t origin_=0,reset_=0;
};
}  // namespace xr_runtime_adapter::gestures
