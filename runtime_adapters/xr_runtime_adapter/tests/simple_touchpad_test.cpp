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
void fist(HandSideF32V2& h) {
  const auto m=em::measure_hand(h,.3);const auto n=m.palm_basis[2];
  for(size_t f=0;f<4;++f) {
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
RuntimeControllerStateFrameV1 step(SimpleTouchpad& pad,Pose& p,bool left=true,bool right=true,
                                  const HmdPoseF64V1* head=nullptr,uint64_t origin=0) {
  pad.apply(p.hands,head?head:&p.head,p.hands.timestamp_ns,origin,left,right);
  RuntimeControllerStateFrameV1 out{};pad.publish(out);return out;
}
void centre_axes_and_radius() {
  for(bool left:{false,true}) {
    auto p=recorded();const auto initial=p;SimpleTouchpad pad;
    auto out=step(pad,p);
    auto side=left?out.left:out.right;
    near(side.thumbstick_x,0,"activation did not capture X centre");near(side.thumbstick_y,0,"activation did not capture Y centre");
    check(side.touches==CONTROLLER_BUTTON_THUMBSTICK && side.buttons==0,"open palm did not touch without click");
    check((side.flags&RUNTIME_CONTROLLER_INPUT_VALID)!=0,"touch-only input not valid");
    const auto run=[&](double x,double y,double z) {
      advance(p);p.hands.left=initial.hands.left;p.hands.right=initial.hands.right;
      move(left?p.hands.left:p.hands.right,x,y,z);
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
  auto p=recorded();SimpleTouchpad pad;
  // A 90-degree head yaw makes head-right point along world -Z.
  p.head.qw=p.head.qy=std::sqrt(.5);
  step(pad,p,true,false);advance(p);
  move(p.hands.left,0,0,-.15);move(p.hands.right,.15,0,0);
  auto out=step(pad,p);
  near(out.left.thumbstick_x,.5,"head yaw ignored");near(out.right.thumbstick_x,0,"right centre not independent");
  advance(p);move(p.hands.right,0,0,-.15);
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
  p=recorded();SimpleTouchpad pad;step(pad,p);
  advance(p);move(p.hands.right,.15,0,0);fist(p.hands.right);
  p.hands.right.pinch_strength=p.hands.right.grab_strength=1;p.hands.right.pinch_active=p.hands.right.grab_active=1;
  p.hands.right.reserved0=CONTROLLER_BUTTON_A|CONTROLLER_BUTTON_TRIGGER;
  out=step(pad,p);
  check(p.hands.right.pinch_active==0 && p.hands.right.grab_active==0 && p.hands.right.reserved0==0,"panel leaked legacy buttons");
  pad.apply(p.hands,&p.head,p.hands.timestamp_ns+60000000,0,true,true);out={};pad.publish(out);
  check(out.right.buttons==0,"duplicate hand sample confirmed click");
  advance(p,60);out=step(pad,p);
  check(out.right.buttons==CONTROLLER_BUTTON_THUMBSTICK && out.right.press_counters[5]==1,"armed fist did not click once");
  near(out.right.thumbstick_x,.5,"fist shifted captured centre");
  for(int n=0;n<4;++n){advance(p);out=step(pad,p);check(out.right.press_counters[5]==1,"held fist repeated click");}
  advance(p);p.hands.right=open.hands.right;move(p.hands.right,.15,0,0);out=step(pad,p);
  check(out.right.buttons==0 && out.right.touches==CONTROLLER_BUTTON_THUMBSTICK && out.right.release_counters[5]==1,"reopen did not release click while keeping touch");
  near(out.right.thumbstick_x,.5,"reopen recentered pad");
  // Use the separately recorded palm-to-self pose as the explicit exit.
  std::ifstream in(std::filesystem::path(__FILE__).parent_path()/"data/emulation_palm_thumb_poses.json");nlohmann::json data;in>>data;
  const auto& self=data.at("cases").at(0);auto viewer=self.at("viewer").get<em::Vec3>();
  p.head.px=viewer[0];p.head.py=viewer[1];p.head.pz=viewer[2];
  for(size_t i=0;i<21;++i){auto v=self.at("hands").at("right").at("joints").at(i).get<em::Vec3>();auto& j=p.hands.right.joints[i];j.px=v[0];j.py=v[1];j.pz=v[2];}
  advance(p);out=step(pad,p);check(out.right.touches==0,"palm-to-self did not exit panel");
  advance(p);p.hands.right=open.hands.right;out=step(pad,p);near(out.right.thumbstick_x,0,"new activation did not recenter");
}
void resets_loss_disabled_and_physical() {
  for(int reason=0;reason<11;++reason) {
    auto p=recorded();SimpleTouchpad pad;step(pad,p);advance(p);fist(p.hands.right);step(pad,p);advance(p,60);
    auto out=step(pad,p);check(out.right.buttons==CONTROLLER_BUTTON_THUMBSTICK,"test fist was not held");
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
  SimpleTouchpad pad;pad.apply(p.hands,&p.head,p.hands.timestamp_ns,0,true,true,false,true);
  RuntimeControllerStateFrameV1 out{};out.right.source_mask=RUNTIME_CONTROLLER_SOURCE_CONTROLLER_INPUT;out.right.thumbstick_x=.7;out.right.buttons=32;out.right.press_counters[5]=42;
  auto before=out.right;pad.publish(out);check(std::memcmp(&before,&out.right,sizeof(before))==0,"physical pad input/counters overwritten");
}
}
int main() {
  try { centre_axes_and_radius();head_coordinates_and_two_centres();click_arming_reservation_and_release();resets_loss_disabled_and_physical();
    std::cout<<"Simple touchpad: centre, 30 cm radius, head axes, armed fist, conflicts, loss and opt-out passed\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
