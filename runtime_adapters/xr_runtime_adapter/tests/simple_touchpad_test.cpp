#include "simple_touchpad.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace xr_runtime;
using xr_runtime_adapter::gestures::SimpleTouchpad;
namespace em=xr_runtime_adapter::emulation;
void check(bool v,const char* reason) { if(!v)throw std::runtime_error(reason); }
void near(double a,double b,const char* reason) { check(std::abs(a-b)<.0001,reason); }

struct Pose {
  HandTrackingFrameF32V2 hands{};
  HmdPoseF64V1 head{};
};
Pose recorded() {
  std::ifstream in(std::filesystem::path(__FILE__).parent_path()/"data/emulation_palm_thumb_poses.json");
  nlohmann::json data;in>>data;
  const auto& item=data.at("cases").at(7);
  check(item.at("id")==8,"expected recorded palms-away case");
  Pose p;
  p.hands.version=2;p.hands.size_bytes=sizeof(p.hands);p.hands.hand_count=2;
  p.hands.sequence=1;p.hands.timestamp_ns=p.hands.source_timestamp_ns=1000000000;
  auto v=item.at("viewer").get<em::Vec3>();
  p.head.px=v[0];p.head.py=v[1];p.head.pz=v[2];p.head.qw=1;
  p.head.flags=HMD_FLAG_POSE_VALID;p.head.tracking_status=2;
  for(bool left:{true,false}) {
    auto& h=left?p.hands.left:p.hands.right;
    const auto& j=item.at("hands").at(left?"left":"right");
    h.handedness=left?1:2;h.status=j.at("status");h.flags=j.at("flags");h.confidence=j.at("confidence");h.joint_count=21;
    for(size_t i=0;i<21;++i) {
      auto xyz=j.at("joints").at(i).get<em::Vec3>();auto& out=h.joints[i];
      out.px=xyz[0];out.py=xyz[1];out.pz=xyz[2];out.flags=2;out.confidence=1;out.joint_id=i;
    }
  }
  return p;
}
void advance(Pose& p,uint64_t ms=30) {
  ++p.hands.sequence;p.hands.source_timestamp_ns+=ms*1000000;p.hands.timestamp_ns=p.hands.source_timestamp_ns;
}
void move(HandSideF32V2& h,double x,double y,double z) {
  for(auto& j:h.joints) { j.px+=x;j.py+=y;j.pz+=z; }
}
void fist(HandSideF32V2& h,size_t first=0) {
  const auto m=em::measure_hand(h,.3);const auto n=m.palm_basis[2];
  for(size_t f=first;f<4;++f) {
    auto& base=h.joints[5+4*f];auto& pip=h.joints[6+4*f];
    em::Vec3 v{pip.px-base.px,pip.py-base.py,pip.pz-base.pz};
    double length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);for(auto& d:v)d/=length;
    for(size_t k=1;k<4;++k) {
      auto& j=h.joints[5+4*f+k];
      const double along=k==3?0:.025,normal=k==1?0:.02;
      j.px=base.px+along*v[0]+normal*n[0];
      j.py=base.py+along*v[1]+normal*n[1];
      j.pz=base.pz+along*v[2]+normal*n[2];
    }
  }
}
void vee(HandSideF32V2& h) {
  const auto m=em::measure_hand(h,.3);
  fist(h,2);
  for(size_t f=0;f<2;++f) {
    const auto base=h.joints[5+4*f];
    for(size_t k=1;k<4;++k) {
      auto& j=h.joints[5+4*f+k];const double d=.025*k;
      const double spread=f==0?-.25:.25;
      j.px=base.px+d*(m.palm_basis[1][0]+spread*m.palm_basis[0][0]);
      j.py=base.py+d*(m.palm_basis[1][1]+spread*m.palm_basis[0][1]);
      j.pz=base.pz+d*(m.palm_basis[1][2]+spread*m.palm_basis[0][2]);
    }
  }
}
RuntimeControllerStateFrameV1 step(SimpleTouchpad& pad,Pose& p,bool left=true,bool right=true,
                                  const HmdPoseF64V1* head=nullptr,uint64_t origin=0) {
  pad.apply(p.hands,head?head:&p.head,p.hands.timestamp_ns,origin,left,right);
  RuntimeControllerStateFrameV1 out{};pad.publish(out);return out;
}
RuntimeControllerStateFrameV1 activate(SimpleTouchpad& pad,Pose& p,bool left=true,bool right=true) {
  step(pad,p,left,right);advance(p,45);return step(pad,p,left,right);
}

void fold_knuckle(HandSideF32V2& hand,size_t finger) {
  const auto h=em::measure_hand(hand,.3);
  const auto& base=hand.joints[5+4*finger];const auto& pip=hand.joints[6+4*finger];
  em::Vec3 direction{pip.px-base.px,pip.py-base.py,pip.pz-base.pz};double normal=0;
  for(size_t i=0;i<3;++i)normal+=direction[i]*h.palm_basis[2][i];
  for(size_t i=0;i<3;++i)direction[i]-=normal*h.palm_basis[2][i];
  const double length=std::hypot(direction[0],direction[1],direction[2]);
  for(size_t i=0;i<3;++i)direction[i]=.5*direction[i]/length+std::sqrt(.75)*h.palm_basis[2][i];
  for(size_t k=1;k<4;++k) {
    auto& j=hand.joints[5+4*finger+k];const double d=.02*double(k);
    j.px=base.px+d*direction[0];j.py=base.py+d*direction[1];j.pz=base.pz+d*direction[2];
  }
}

void pinch_gap(HandSideF32V2& hand,double gap) {
  const auto h=em::measure_hand(hand,.3);const auto& index=hand.joints[8];const auto base=hand.joints[1];
  const auto& a=hand.joints[5];const auto& b=hand.joints[17];
  const double width=std::hypot(a.px-b.px,a.py-b.py,a.pz-b.pz);
  const em::Vec3 tip{index.px+gap*width*h.palm_basis[0][0],index.py+gap*width*h.palm_basis[0][1],index.pz+gap*width*h.palm_basis[0][2]};
  for(size_t k=1;k<=3;++k) {
    auto& j=hand.joints[1+k];const double t=double(k)/3;
    j.px=base.px+t*(tip[0]-base.px);j.py=base.py+t*(tip[1]-base.py);j.pz=base.pz+t*(tip[2]-base.pz);
  }
}

void face_away(HandSideF32V2& hand,const HmdPoseF64V1& head,double degrees) {
  const auto h=em::measure_hand(hand,.3);
  const auto dot=[](em::Vec3 a,em::Vec3 b){double d=0;for(size_t i=0;i<3;++i)d+=a[i]*b[i];return d;};
  const auto cross=[](em::Vec3 a,em::Vec3 b){return em::Vec3{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
  em::Vec3 away{h.palm_center[0]-head.px,h.palm_center[1]-head.py,h.palm_center[2]-head.pz};
  double n=std::sqrt(dot(away,away));for(auto& v:away)v/=n;
  auto across=h.palm_basis[0];const double d=dot(across,away);
  for(size_t i=0;i<3;++i)across[i]-=d*away[i];
  n=std::sqrt(dot(across,across));for(auto& v:across)v/=n;
  const double angle=degrees*3.141592653589793/180;
  em::Vec3 target{};for(size_t i=0;i<3;++i)target[i]=std::cos(angle)*away[i]+std::sin(angle)*across[i];
  auto axis=cross(h.palm_basis[2],target);const double sine=std::sqrt(dot(axis,axis)),cosine=dot(h.palm_basis[2],target);
  if(sine<1e-6)return;
  for(auto& v:axis)v/=sine;
  for(auto& j:hand.joints) {
    em::Vec3 v{j.px-h.palm_center[0],j.py-h.palm_center[1],j.pz-h.palm_center[2]};
    const auto turn=cross(axis,v);const double along=dot(axis,v);
    for(size_t i=0;i<3;++i)v[i]=v[i]*cosine+turn[i]*sine+axis[i]*along*(1-cosine)+h.palm_center[i];
    j.px=v[0];j.py=v[1];j.pz=v[2];
  }
}

void activation_guards() {
  for(bool left:{false,true}) {
    for(int variant=0;variant<9;++variant) {
      auto p=recorded();auto& hand=left?p.hands.left:p.hands.right;
      if(variant<4)fold_knuckle(hand,size_t(variant));
      else if(variant<8)pinch_gap(hand,std::array<double,4>{0,.4,.6,.84}[variant-4]);
      else face_away(hand,p.head,40);
      const auto m=em::measure_hand(hand,.3);const em::Vec3 viewer{p.head.px,p.head.py,p.head.pz};
      check(em::measure_open_palm_pose(m,true,xr_runtime_adapter::gestures::simple_palm_config(),&viewer,false,false).away_from_viewer,
            "guard fixture did not reproduce old permissive open palm");
      SimpleTouchpad pad(true,.30);
      for(int frame=0;frame<10;++frame) {
        auto out=step(pad,p,left,!left);const auto& side=left?out.left:out.right;
        check(!(left?pad.left_active():pad.right_active()) && side.touches==0,"pinch/claw/oblique hand armed panel");
        advance(p);
      }
    }
    // A deliberate open palm still works on each hand just inside the angle.
    auto p=recorded();auto& hand=left?p.hands.left:p.hands.right;face_away(hand,p.head,34);
    SimpleTouchpad pad(true,.30);step(pad,p,left,!left);
    pad.apply(p.hands,&p.head,p.hands.timestamp_ns+60000000,0,left,!left);
    check(!pad.active(),"duplicate optical frame confirmed activation");
    advance(p,44);step(pad,p,left,!left);check(!pad.active(),"activation confirmed before 45 ms");
    advance(p,1);auto out=step(pad,p,left,!left);
    check((left?out.left:out.right).touches==CONTROLLER_BUTTON_THUMBSTICK,"deliberate open palm failed to activate");
    // Shape restrictions govern entry only. A held V keeps live axes inside
    // the orientation cone, without requiring the fingers to stay open.
    advance(p);vee(hand);step(pad,p,left,!left);advance(p,60);move(hand,.15,.15,0);
    face_away(hand,p.head,20);
    out=step(pad,p,left,!left);const auto& side=left?out.left:out.right;
    check(side.buttons==CONTROLLER_BUTTON_THUMBSTICK,"guard cancelled active V click");
    near(side.thumbstick_x,.5,"held V lost X motion");near(side.thumbstick_y,.5,"held V lost Y motion");
  }
  // An isolated open frame, pinch, loss or discontinuous source cannot arm.
  for(int reason=0;reason<4;++reason) {
    auto p=recorded();const auto open=p.hands.right;SimpleTouchpad pad(true,.30);step(pad,p,false,true);
    advance(p,30);
    if(reason==0)pinch_gap(p.hands.right,.4);
    if(reason==1)p.hands.right.status=2;
    if(reason==2)face_away(p.hands.right,p.head,40);
    if(reason==3)advance(p,200);
    step(pad,p,false,true);check(!pad.active(),"interrupted activation seized panel");
    p.hands.right=open;advance(p,30);step(pad,p,false,true);check(!pad.active(),"interrupted arming time was reused");
    advance(p,45);auto out=step(pad,p,false,true);
    check(out.right.touches==CONTROLLER_BUTTON_THUMBSTICK,"fresh deliberate palm could not rearm");
  }
}

void wrist_rotation_keeps_panel_active() {
  for(bool left:{false,true}) {
    auto p=recorded();SimpleTouchpad pad;activate(pad,p);
    auto& hand=left?p.hands.left:p.hands.right;const auto open=hand;
    advance(p);vee(hand);step(pad,p);advance(p,45);step(pad,p);
    for(double angle:{34.9,35.1,90.,160.}) {
      advance(p);face_away(hand,p.head,angle);move(hand,.01,0,0);
      auto out=step(pad,p);const auto side=left?out.left:out.right;
      check(side.touches==CONTROLLER_BUTTON_THUMBSTICK && side.buttons==CONTROLLER_BUTTON_THUMBSTICK,
            "wrist rotation cancelled held panel click");
      check(side.thumbstick_x>0,"rotated V lost axes");
      check(side.release_counters[5]==0,"rotation released click");
    }
    // An open palm toward the viewer is also not an exit during a session.
    hand=open;face_away(hand,p.head,160);advance(p);auto out=step(pad,p);
    const auto side=left?out.left:out.right;
    check(side.touches==CONTROLLER_BUTTON_THUMBSTICK && side.buttons==0,
          "open palm rotation exited panel instead of only releasing click");
    advance(p);hand.status=0;out=step(pad,p);
    check((left?out.left:out.right).touches==0,"lost hand failed to exit panel");
    hand=open;move(hand,.025,0,0);advance(p);out=activate(pad,p);
    near((left?out.left:out.right).thumbstick_x,0,"new activation failed to recapture centre");
  }
}

void thumb_exit_is_confirmed_and_consumed() {
  for(bool left:{false,true}) {
    auto p=recorded();SimpleTouchpad pad;activate(pad,p);
    auto& hand=left?p.hands.left:p.hands.right;const auto open=hand;
    advance(p);vee(hand);face_away(hand,p.head,20);step(pad,p);
    advance(p,45);auto out=step(pad,p);
    check((left?out.left:out.right).buttons==CONTROLLER_BUTTON_THUMBSTICK,"test click was not held");
    fist(hand);
    const auto m=em::measure_hand(hand,.3);const auto base=hand.joints[5];
    for(size_t i=0;i<4;++i) {
      auto& j=hand.joints[1+i];const double d=-.035+double(i)*(.11/3);
      j.px=base.px+.025*m.palm_basis[0][0]+d*m.palm_basis[1][0];
      j.py=base.py+.025*m.palm_basis[0][1]+d*m.palm_basis[1][1];
      j.pz=base.pz+.025*m.palm_basis[0][2]+d*m.palm_basis[1][2];
    }
    advance(p);out=step(pad,p);
    check(left?pad.left_active():pad.right_active(),"thumb exit skipped confirmation");
    check((left?out.left:out.right).buttons==0,"thumb exit pose pressed the panel");
    pad.apply(p.hands,&p.head,p.hands.timestamp_ns+60000000,0,true,true);
    check(left?pad.left_active():pad.right_active(),"duplicate source confirmed thumb exit");
    advance(p,44);step(pad,p);check(left?pad.left_active():pad.right_active(),"thumb exited before 45 ms");
    advance(p,1);out=step(pad,p);
    check(!(left?pad.left_active():pad.right_active()),"thumb up did not exit panel");
    check(left?pad.left_reserved():pad.right_reserved(),"exit thumb did not reserve ordinary button input");
    const auto side=left?out.left:out.right;
    check(side.buttons==0 && side.touches==0 && side.release_counters[5]==1,"thumb exit left click/touch held");
    near(side.thumbstick_x,0,"thumb exit left axes held");
    check((left?out.right:out.left).touches==CONTROLLER_BUTTON_THUMBSTICK,"thumb exit cancelled other hand");
    for(int i=0;i<6;++i){advance(p);step(pad,p);check(left?pad.left_reserved():pad.right_reserved(),"held exit thumb escaped suppression");}
    hand=open;move(hand,.025,0,0);face_away(hand,p.head,20);advance(p);out=activate(pad,p);
    check((left?out.left:out.right).touches==CONTROLLER_BUTTON_THUMBSTICK,"release/reopen did not rearm");
    near((left?out.left:out.right).thumbstick_x,0,"thumb exit failed to recapture centre");
  }
}

void centre_axes_and_radius() {
  for(bool left:{false,true}) {
    auto p=recorded();const auto initial=p;SimpleTouchpad pad(true,.30);
    auto out=activate(pad,p);
    auto side=left?out.left:out.right;
    near(side.thumbstick_x,0,"activation did not capture X centre");near(side.thumbstick_y,0,"activation did not capture Y centre");
    check(side.touches==CONTROLLER_BUTTON_THUMBSTICK && side.buttons==0,"open palm did not touch without click");
    check((side.flags&RUNTIME_CONTROLLER_INPUT_VALID)!=0,"touch-only input not valid");
    const auto run=[&](double x,double y,double z) {
      advance(p);p.hands.left=initial.hands.left;p.hands.right=initial.hands.right;
      move(left?p.hands.left:p.hands.right,x,y,z);
      face_away(left?p.hands.left:p.hands.right,p.head,20);
      auto state=step(pad,p);return left?state.left:state.right;
    };
    for(double sign:{-1.,1.}) {
      side=run(.15*sign,0,0);near(side.thumbstick_x,.5*sign,"head-relative X wrong");near(side.thumbstick_y,0,"X leaked into Y");
      side=run(0,.15*sign,0);near(side.thumbstick_y,.5*sign,"head-relative Y wrong");near(side.thumbstick_x,0,"Y leaked into X");
    }
    side=run(.3,.3,0);near(side.thumbstick_x,std::sqrt(.5),"diagonal not radially limited");near(side.thumbstick_y,std::sqrt(.5),"diagonal Y wrong");
    side=run(.6,0,0);near(side.thumbstick_x,1,"outside radius not clamped");
    side=run(0,0,.1);near(side.thumbstick_x,0,"depth moved X");near(side.thumbstick_y,0,"depth moved Y");
  }
}
void head_coordinates_and_two_centres() {
  auto p=recorded();SimpleTouchpad pad(true,.30);
  // A 90-degree head yaw makes head-right point along world -Z.
  p.head.qw=p.head.qy=std::sqrt(.5);
  activate(pad,p,true,false);advance(p);
  move(p.hands.left,0,0,-.15);move(p.hands.right,.15,0,0);
  face_away(p.hands.left,p.head,20);
  face_away(p.hands.right,p.head,20);
  step(pad,p);advance(p,45);auto out=step(pad,p);
  near(out.left.thumbstick_x,.5,"head yaw ignored");near(out.right.thumbstick_x,0,"right centre not independent");
  advance(p);move(p.hands.right,0,0,-.15);
  face_away(p.hands.right,p.head,20);
  out=step(pad,p);near(out.left.thumbstick_x,.5,"centre moved each open frame");near(out.right.thumbstick_x,.5,"right movement wrong");
  // Translate head and both hands together: head-relative axes stay unchanged.
  advance(p);p.head.px+=.5;p.head.py+=.2;p.head.pz+=.1;
  move(p.hands.left,.5,.2,.1);move(p.hands.right,.5,.2,.1);
  out=step(pad,p);near(out.left.thumbstick_x,.5,"common translation moved pad");near(out.right.thumbstick_x,.5,"common translation moved right pad");
  // Apply a common 3D rotation (yaw/pitch/roll) plus translation after capture.
  // Head-local coordinates must be invariant, even while the head turns.
  const double a=.8, norm=std::sqrt(14.);
  const std::array<double,4> r{{std::cos(a/2),std::sin(a/2)/norm,2*std::sin(a/2)/norm,3*std::sin(a/2)/norm}};
  const auto transform=[&](double px,double py,double pz) {
    const double w=r[0],x=r[1],y=r[2],z=r[3];
    return em::Vec3{(1-2*(y*y+z*z))*px+2*(x*y-w*z)*py+2*(x*z+w*y)*pz+.4,
        2*(x*y+w*z)*px+(1-2*(x*x+z*z))*py+2*(y*z-w*x)*pz-.2,
        2*(x*z-w*y)*px+2*(y*z+w*x)*py+(1-2*(x*x+y*y))*pz+.1};
  };
  for(auto* h:{&p.hands.left,&p.hands.right})for(auto& j:h->joints) {
    auto point=transform(j.px,j.py,j.pz);j.px=point[0];j.py=point[1];j.pz=point[2];
  }
  auto viewer=transform(p.head.px,p.head.py,p.head.pz);
  p.head.px=viewer[0];p.head.py=viewer[1];p.head.pz=viewer[2];
  const std::array<double,4> q{{p.head.qw,p.head.qx,p.head.qy,p.head.qz}};
  p.head.qw=r[0]*q[0]-r[1]*q[1]-r[2]*q[2]-r[3]*q[3];
  p.head.qx=r[0]*q[1]+r[1]*q[0]+r[2]*q[3]-r[3]*q[2];
  p.head.qy=r[0]*q[2]-r[1]*q[3]+r[2]*q[0]+r[3]*q[1];
  p.head.qz=r[0]*q[3]+r[1]*q[2]-r[2]*q[1]+r[3]*q[0];
  advance(p);out=step(pad,p);
  near(out.left.thumbstick_x,.5,"combined head/hand 3D rotation moved X");near(out.left.thumbstick_y,0,"combined rotation moved Y");
  near(out.right.thumbstick_x,.5,"combined rotation moved right panel");
}
void click_arming_reservation_and_release() {
  auto p=recorded();const auto open=p;SimpleTouchpad cold;
  fist(p.hands.left);fist(p.hands.right);
  auto out=step(cold,p);advance(p,100);out=step(cold,p);
  check(!cold.active() && out.left.buttons==0 && out.right.buttons==0,"cold fist armed/clicked touchpad");
  p=recorded();vee(p.hands.left);vee(p.hands.right);
  out=step(cold,p);advance(p,100);out=step(cold,p);
  check(!cold.active() && out.left.buttons==0 && out.right.buttons==0,"cold V armed/clicked touchpad");
  p=recorded();SimpleTouchpad pad(true,.30);activate(pad,p);
  advance(p);move(p.hands.right,.15,0,0);vee(p.hands.right);
  face_away(p.hands.right,p.head,20);
  p.hands.right.pinch_strength=p.hands.right.grab_strength=1;p.hands.right.pinch_active=p.hands.right.grab_active=1;
  p.hands.right.reserved0=CONTROLLER_BUTTON_A|CONTROLLER_BUTTON_TRIGGER;
  out=step(pad,p);
  check(p.hands.right.pinch_active==0 && p.hands.right.grab_active==0 && p.hands.right.reserved0==0,"panel leaked legacy buttons");
  pad.apply(p.hands,&p.head,p.hands.timestamp_ns+60000000,0,true,true);out={};pad.publish(out);
  check(out.right.buttons==0,"duplicate hand sample confirmed click");
  advance(p,60);out=step(pad,p);
  check(out.right.buttons==CONTROLLER_BUTTON_THUMBSTICK && out.right.press_counters[5]==1,"armed V did not click once");
  near(out.right.thumbstick_x,.5,"V shifted captured centre");
  for(int n=0;n<4;++n){advance(p);out=step(pad,p);check(out.right.press_counters[5]==1,"held V repeated click");}
  advance(p);fist(p.hands.right);out=step(pad,p);
  check(out.right.buttons==0 && out.right.release_counters[5]==1,"fist did not release V click");
  advance(p,60);out=step(pad,p);check(out.right.buttons==0,"held fist clicked panel");
  advance(p);p.hands.right=open.hands.right;move(p.hands.right,.15,0,0);
  face_away(p.hands.right,p.head,20);out=step(pad,p);
  check(out.right.buttons==0 && out.right.touches==CONTROLLER_BUTTON_THUMBSTICK && out.right.release_counters[5]==1,"reopen did not release click while keeping touch");
  near(out.right.thumbstick_x,.5,"reopen recentered pad");
  advance(p);p.hands.right.status=0;out=step(pad,p);
  check(out.right.touches==0,"lost hand did not exit panel");
  advance(p);p.hands.right=open.hands.right;out=activate(pad,p);
  check(out.right.touches==CONTROLLER_BUTTON_THUMBSTICK,"open palm did not rearm");
  near(out.right.thumbstick_x,0,"new activation did not recenter");
}
void resets_loss_disabled_and_physical() {
  for(int reason=0;reason<11;++reason) {
    auto p=recorded();SimpleTouchpad pad(true,.30);activate(pad,p);advance(p);vee(p.hands.right);step(pad,p);advance(p,60);
    auto out=step(pad,p);check(out.right.buttons==CONTROLLER_BUTTON_THUMBSTICK,"test V was not held");
    if(reason==0)pad.apply(p.hands,nullptr,p.hands.timestamp_ns,0,true,true);
    if(reason==1)pad.apply(p.hands,&p.head,p.hands.timestamp_ns+200000000,0,true,true);
    if(reason==2)pad.apply(p.hands,&p.head,p.hands.timestamp_ns,1,true,true);
    if(reason==3){++p.hands.reset_counter;pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    if(reason==4){++p.head.reset_counter;pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    if(reason==5){p.hands.right.status=2;pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    if(reason==6)pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,false);
    if(reason==7){p.head.qw=std::numeric_limits<double>::quiet_NaN();pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    if(reason==8){p.hands.right.joints[8].px=std::numeric_limits<float>::quiet_NaN();pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    if(reason==9){p.head.flags=0;pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    if(reason==10){p.head.tracking_status=4;pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true);}
    out={};pad.publish(out);check(out.right.buttons==0 && out.right.touches==0 && out.right.release_counters[5]==1,"loss/reset/block did not release panel");
  }
  auto p=recorded();auto original=p.hands;SimpleTouchpad disabled(false);step(disabled,p);
  check(std::memcmp(&original,&p.hands,sizeof(original))==0 && !disabled.active(),"disabled pad modified legacy hands");
  SimpleTouchpad pad(true,.30);pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true,false,true);
  RuntimeControllerStateFrameV1 out{};out.right.source_mask=RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;out.right.thumbstick_x=.7;out.right.buttons=32;out.right.press_counters[5]=42;
  auto before=out.right;pad.publish(out);check(std::memcmp(&before,&out.right,sizeof(before))==0,"physical pad input/counters overwritten");
}
}
int main() {
  try {
    for(double radius:{.10,.20}) {
      auto p=recorded();SimpleTouchpad pad(true,radius);activate(pad,p);
      advance(p);move(p.hands.right,.05,0,0);face_away(p.hands.right,p.head,20);auto out=step(pad,p);
      near(out.right.thumbstick_x,.05/radius,"configured radius ignored");
    }
    check(SimpleTouchpad::default_radius_m==.10,"default radius must be 10 cm");
    activation_guards();wrist_rotation_keeps_panel_active();thumb_exit_is_confirmed_and_consumed();centre_axes_and_radius();head_coordinates_and_two_centres();click_arming_reservation_and_release();resets_loss_disabled_and_physical();
    std::cout<<"Simple touchpad: configurable radius, head axes, free wrist rotation, thumb exit, independent hands, click release and recenter passed\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
