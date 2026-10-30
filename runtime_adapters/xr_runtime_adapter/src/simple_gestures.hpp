#pragma once

#include "emulation_controller.hpp"

#include <algorithm>
#include <cmath>

namespace xr_runtime_adapter::gestures {

inline emulation::Config simple_palm_config() {
  emulation::Config c;
  // Same uncalibrated shape thresholds as the shipped open-palm click preset.
  c.extension_on = .65;
  c.little_extension_on = .55;
  c.thumb_enable_clearance = .78;
  c.thumb_extension_on = .76;
  return c;
}

inline bool simple_head_up(const xr_runtime::HmdPoseF64V1* head,emulation::Vec3& up) {
  if (!head || !(head->flags & xr_runtime::HMD_FLAG_POSE_VALID) ||
      head->tracking_status!=2) return false;
  std::array<double,4> q{{head->qw,head->qx,head->qy,head->qz}};
  double norm=0;for(double v:q){if(!std::isfinite(v))return false;norm+=v*v;}
  if(norm<1e-8 || !std::isfinite(norm))return false;
  norm=std::sqrt(norm);for(auto& v:q)v/=norm;
  const double w=q[0],x=q[1],y=q[2],z=q[3];
  up={2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x)};
  return true;
}

inline bool simple_joint(const xr_runtime::HandSideF32V2& hand,size_t id,
                         double confidence,emulation::Vec3& out) {
  if(id>=hand.joint_count || id>=xr_runtime::HAND_JOINT_COUNT_V2)return false;
  const auto& j=hand.joints[id];
  if(!(j.flags&2) || !std::isfinite(j.confidence) || j.confidence<confidence)return false;
  out={j.px,j.py,j.pz};
  return std::all_of(out.begin(),out.end(),[](double v){return std::isfinite(v);});
}

inline bool simple_thumb_up(const xr_runtime::HandSideF32V2& hand,
                            const xr_runtime::HmdPoseF64V1* head,
                            double confidence,bool holding) {
  // A fist plus a thumb lifted away from the palm and pointing up. The tip may
  // be straight or bent; no signed backward-arch requirement remains.
  emulation::Vec3 up{},mcp{},ip{},tip{};
  if(!simple_head_up(head,up) || !simple_joint(hand,2,confidence,mcp) ||
      !simple_joint(hand,3,confidence,ip) || !simple_joint(hand,4,confidence,tip))return false;
  const auto palm=emulation::measure_hand(hand,confidence);
  const double curl=holding?.45:.65;
  if(!palm.palm_valid || !palm.thumb_valid ||
      palm.thumb_clearance<(holding?.32:.40) ||
      std::any_of(palm.curl.begin(),palm.curl.end(),[curl](double v){return !std::isfinite(v) || v<curl;}))return false;
  double first2=0,last2=0,upward=0;
  emulation::Vec3 first{},last{};
  for(size_t i=0;i<3;++i){
    first[i]=ip[i]-mcp[i];last[i]=tip[i]-ip[i];
    first2+=first[i]*first[i];last2+=last[i]*last[i];upward+=first[i]*up[i];
  }
  const double length=std::sqrt(first2)+std::sqrt(last2);
  if(length<.012 || length>.15 || first2<1e-8 || last2<1e-8)return false;
  const double threshold=std::cos((holding?38.:30.)*3.141592653589793/180.);
  if(upward/std::sqrt(first2)<threshold)return false;
  // An actual touching pinch keeps Trigger.
  emulation::Vec3 index{};
  if(simple_joint(hand,8,confidence,index)) {
    double gap2=0;for(size_t i=0;i<3;++i){const double d=tip[i]-index[i];gap2+=d*d;}
    const double touching=std::clamp(length*.15,.006,.012);
    if(gap2<touching*touching)return false;
  }
  return true;
}

inline bool simple_v_facing(const emulation::HandMeasurement& h,
                            const xr_runtime::HmdPoseF64V1* head, bool holding) {
  emulation::Vec3 up{};
  if(!h.palm_valid || !simple_head_up(head,up))return false;
  const emulation::Vec3 away{{h.palm_center[0]-head->px,h.palm_center[1]-head->py,h.palm_center[2]-head->pz}};
  double upright=0,facing=0,distance2=0;
  for(size_t i=0;i<3;++i){upright+=h.palm_basis[1][i]*up[i];
    facing+=h.palm_basis[2][i]*away[i];distance2+=away[i]*away[i];}
  const double threshold=std::cos((holding?40.:35.)*3.141592653589793/180.);
  return std::isfinite(distance2) && distance2>=.0025 && distance2<=4 &&
      upright>=threshold && facing/std::sqrt(distance2)>=threshold;
}

inline bool simple_pinch(const emulation::HandMeasurement& h, bool holding) {
  return h.thumb_valid && h.thumb_clearance>=.60 && h.index_chain_curl<.80 &&
      h.index_knuckle_curl<.95 && h.pinch_ratio>=0 && h.pinch_ratio<=(holding?.60:.40);
}

inline bool simple_v(const xr_runtime::HandSideF32V2& hand,
                     const std::array<bool,4>& extended,
                     const std::array<bool,4>& curled, bool holding) {
  if (!extended[0] || !extended[1] || !curled[2] || !curled[3]) return false;
  const auto direction=[&hand](size_t base) {
    const auto& a=hand.joints[base];const auto& b=hand.joints[base+3];
    return emulation::Vec3{b.px-a.px,b.py-a.py,b.pz-a.pz};
  };
  const auto a=direction(5),b=direction(9);
  double aa=0,bb=0,ab=0;
  for(size_t i=0;i<3;++i){aa+=a[i]*a[i];bb+=b[i]*b[i];ab+=a[i]*b[i];}
  return std::isfinite(aa) && std::isfinite(bb) && aa>=1e-10 && bb>=1e-10 &&
      ab/std::sqrt(aa*bb)<=std::cos((holding?8.:12.)*3.141592653589793/180.);
}

// SIMPLE_GESTURES alone replaces visual grab/fist with an open palm facing the
// HMD. Pinch, extra bindings and physical-controller input keep their old path.
class SimplePalmGrip {
 public:
  void apply(xr_runtime::HandTrackingFrameF32V2& hands,
             const emulation::Vec3* viewer, uint64_t now, uint64_t origin,
             bool left_enabled, bool right_enabled,
             bool left_physical = false, bool right_physical = false) {
    if (origin != origin_ || hands.reset_counter != reset_) {
      left_ = right_ = {};
      origin_ = origin;
      reset_ = hands.reset_counter;
    }
    const uint64_t stamp = hands.source_timestamp_ns ? hands.source_timestamp_ns : hands.timestamp_ns;
    const bool fresh = hands.sequence && stamp &&
        (stamp <= now ? double(now - stamp) / 1e6 <= config_.max_age_ms : stamp - now <= 2000000);
    update(hands.left, left_, viewer, stamp, fresh && left_enabled, left_physical);
    update(hands.right, right_, viewer, stamp, fresh && right_enabled, right_physical);
    if ((hands.left.flags | hands.right.flags) & xr_runtime::HAND_GRAB_VALID)
      hands.flags |= xr_runtime::HAND_FLAG_GESTURES_VALID;
  }

 private:
  struct Side {
    std::array<bool, 4> extended{};
    bool facing = false, active = false;
    uint64_t since = 0, sample = 0;
  };

  void update(xr_runtime::HandSideF32V2& hand, Side& s,
              const emulation::Vec3* viewer, uint64_t stamp,
              bool enabled, bool physical) {
    const auto write = [&] {
      hand.grab_strength = s.active ? 1.f : 0.f;
      hand.grab_active = s.active ? 1u : 0u;
      hand.flags |= xr_runtime::HAND_GRAB_VALID;
    };
    if (physical) {
      s = {};
      return;  // Do not overwrite controller grip copied into the hand stream.
    }
    const bool valid = enabled && hand.status == 1 &&
        (hand.flags & (xr_runtime::HAND_POSE_VALID | xr_runtime::HAND_JOINTS_VALID)) ==
            (xr_runtime::HAND_POSE_VALID | xr_runtime::HAND_JOINTS_VALID) &&
        std::isfinite(hand.confidence) && hand.confidence >= config_.min_confidence;
    if (!valid) {
      s = {};
      write();
      return;
    }
    if (stamp < s.sample) s = {};
    const auto measured = emulation::measure_hand(hand, config_.min_confidence);
    for (size_t i = 0; i < s.extended.size(); ++i) {
      const double on = i == 3 ? config_.little_extension_on : config_.extension_on;
      const double off = i == 3 ? config_.little_extension_off : config_.extension_off;
      s.extended[i] = measured.curl[i] >= 0 &&
          1 - measured.curl[i] >= (s.extended[i] ? off : on);
    }
    const bool extended = std::all_of(s.extended.begin(), s.extended.end(), [](bool v) { return v; });
    const auto pose = emulation::measure_open_palm_pose(measured, extended, config_, viewer, s.facing, false);
    if (!pose.toward_viewer) {
      s.facing = s.active = false;
      s.since = 0;
    } else if (stamp != s.sample) {
      if (!s.facing) s.since = stamp;
      s.facing = true;
      s.active = double(stamp - s.since) / 1e6 >= config_.button_confirm_ms;
    }
    s.sample = stamp;
    write();
  }

  emulation::Config config_ = simple_palm_config();
  Side left_, right_;
  uint64_t origin_ = 0, reset_ = 0;
};

}  // namespace xr_runtime_adapter::gestures
