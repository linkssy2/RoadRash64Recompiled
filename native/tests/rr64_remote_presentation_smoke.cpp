#include "rr64_remote_presentation.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
using rr64::netplay::RiderState;
using rr64::online_race_sync::RemotePresentation;
void check(bool b) { if(!b) std::abort(); }
RiderState make(unsigned tick,float x) {
 RiderState r{}; r.tick=tick;r.active=true;r.root.valid=r.rider_position_valid=1;
 r.root.bike_attached=r.root.rider_attached=1;
 r.position_x=x;r.front_wheel_x=x+2;r.rear_wheel_x=x-2;r.rider_x=x+1;
 r.root.bike_origin={x-1,0,0};r.root.bike_rotation={0,0,0,1};r.root.bike_velocity={3,4,5};return r;
}
int main() {
 RemotePresentation h; auto state=make(1,0);auto r=h.sample(state,0);check(r.position_x==0);
 state=make(2,10);h.sample(state,50000);
 r=h.sample(state,100000);check(std::abs(r.position_x-5)<.001f);
 check(r.front_wheel_x-r.position_x==2 && r.rear_wheel_x-r.position_x==-2 && r.rider_x-r.position_x==1);
 check(r.root.bike_origin[0]-r.position_x==-1 && r.root.bike_velocity==state.root.bike_velocity && r.root.bike_rotation==state.root.bike_rotation);
 // No unbounded extrapolation when packets stop.
 r=h.sample(state,200000);check(r.position_x==10);
 // Teleports and attachment transitions are not dragged through old history.
 state=make(3,900);r=h.sample(state,210000);check(r.position_x==900);
 state=make(4,920);state.root.ejected=1;r=h.sample(state,220000);check(r.position_x==920);
 state=make(5,940);r=h.sample(state,230000);check(r.position_x==940);
 state=make(1,2);r=h.sample(state,240000);check(r.position_x==2);
 // Synthetic 20 Hz updates sampled at 100 Hz: 1 unit steps after buffering,
 // instead of the 5-unit packet jumps. Repeated ticks must not restart smoothing.
 h.reset();float prev=0;unsigned smooth=0;
 for(int ms=0;ms<=1000;ms+=10) {
   auto s=make(ms/50+1,float(ms/50)*5);r=h.sample(s,ms*1000);
   if(ms>=200) {check(std::abs((r.position_x-prev)-1)<.001f);++smooth;}
   prev=r.position_x;
 }
 check(smooth==81);
 h.reset();r=h.sample(make(1,33),0);check(r.position_x==33);
 // Attack timing follows the same buffered bracket as the bike, not the
 // newest packet. A newly started attack must not bleed into earlier frames.
 h.reset();auto a=make(1,0),b=make(2,10);
 a.root.attack={1,7,1,0,{0.2f,0.1f,1,0,0,0}};
 b.root.attack={1,7,1,0,{0.4f,0.3f,1,0,0,0}};
 h.sample(a,0);h.sample(b,50000);r=h.sample(b,100000);
 check(std::abs(r.root.attack.clocks[0]-.3f)<.001f);
 h.reset();a.root.attack.clocks[0]=0;
 h.sample(a,0);h.sample(b,50000);r=h.sample(b,100000);
 check(r.root.attack.clocks[0]==0);
 std::puts("remote presentation checks passed");
}
