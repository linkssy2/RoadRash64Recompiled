#include "rr64_prediction_replay.hpp"
#include "rr64_authoritative_dynamics_native.hpp"
#include "rr64_authoritative_capture.hpp"
#include "rr64_authoritative_native.hpp"
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <source_location>
extern "C" int rr64_authority_step_begin(unsigned char*,void*);
extern "C" int rr64_authority_translate(unsigned char*,void*);
extern "C" void rr64_authority_step_finish(unsigned char*);
extern "C" void rr64_authority_actor_route(void*,unsigned);
extern "C" void rr64_prediction_reconcile_step(unsigned char*){}
extern "C" void rr64_prediction_verify_before(unsigned char*,void*){}
extern "C" void rr64_prediction_verify_after(unsigned char*){}
using namespace rr64;
namespace {bool race_released=true;netplay::Status status;authority::Step next;unsigned queued=0,finished=0,captured=0,native=0;std::uint16_t last_changed=0;unsigned faults=0;}
void check(bool b,std::source_location at=std::source_location::current()){
 if(!b){std::fprintf(stderr,"step check failed line %u\n",at.line());std::exit(1);}
}
namespace rr64::netplay {
void authority_pin_frame(){}
bool authority_race_gate(bool){return race_released;}
Status get_status(){return status;}
bool authority_queue_input_recorded(std::uint16_t b,std::int8_t x,std::int8_t y,authority::Command &c,std::uint8_t actions,unsigned duration_us){c={status.game_setup.revision,1,b,x,y};return authority_queue_input(b,x,y);}
void authority_fail(const char*){++faults;}
bool authority_queue_input(std::uint16_t,std::int8_t,std::int8_t){++queued;return true;}
bool authority_begin_step(authority::Step &s,unsigned duration_us){s=next;return true;}
bool authority_finish_step(std::uint64_t tick,authority::Stamp &stamp){check(tick==next.tick);++finished;stamp.round=next.round;stamp.tick=tick;return true;}
}
extern "C" int rr64_online_authority_capture(unsigned char*,const void *stamp){check(static_cast<const authority::Stamp*>(stamp)->tick==next.tick);++captured;return 1;}
extern "C" void func_80040664(unsigned char *m,recomp_context *ctx){
 check(!rr64_authority_translate(m,ctx)); // the real hook must not recurse
 unsigned index=99;engine::read_u32(m,static_cast<unsigned>(ctx->r4)+4,index);check(index==0);
 std::uint16_t held=0;engine::read_u16(m,engine::globals::controller_buttons,held);
 engine::read_u16(m,engine::globals::controller_changed_buttons,last_changed);
 engine::write_u16(m,static_cast<unsigned>(ctx->r5),held);++native;
}
int main(){
 std::vector<unsigned char> memory(16*1024*1024);auto *m=memory.data();
 status.active=status.connected=status.authoritative=status.is_host=true;
 status.phase=netplay::Phase::Race;status.local_slot=0;status.game_setup.revision=7;status.authority_humans=0x3fff;
 next.round=7;next.tick=1;
 engine::write_u32(m,0x800a656c,14);
 for(unsigned i=0;i<4;++i)engine::write_float(m,0x8009cba8+i*4,.016667f);
 for(unsigned s=0;s<14;++s){
  unsigned a=0x800d8570+s*0x118;
  engine::write_u16(m,a+0x24,1);engine::write_u32(m,a+4,s);
  next.inputs[s]={7,1,static_cast<std::uint16_t>(0x8000+s),10,-20};
 }
 race_released=false;check(!rr64_authority_step_begin(m,nullptr) && queued==0 && faults==0);
 race_released=true;check(rr64_authority_step_begin(m,nullptr));check(queued==1);
 {prediction::ReplayScope replay;rr64_authority_step_begin(m,nullptr);rr64_authority_step_finish(m);check(queued==1 && finished==0 && faults==0);}
 for(unsigned s=0;s<14;++s){
  auto before=memory;recomp_context ctx{};ctx.r4=static_cast<std::int64_t>(static_cast<std::int32_t>(0x800d8570+s*0x118));ctx.r5=static_cast<std::int64_t>(static_cast<std::int32_t>(0x807e0000));
  ctx.r16=ctx.r4;ctx.r3=static_cast<std::int64_t>(static_cast<std::int32_t>(0x8004eb6c));
  rr64_authority_actor_route(&ctx,0);check(ctx.r3==static_cast<std::int64_t>(static_cast<std::int32_t>(0x800524cc)));
  ctx.r3=static_cast<std::int64_t>(static_cast<std::int32_t>(0x8004ead0));
  rr64_authority_actor_route(&ctx,0);check(ctx.r3==static_cast<std::int64_t>(static_cast<std::int32_t>(0x8005264c)));
  ctx.r2=1;rr64_authority_actor_route(&ctx,1);check(ctx.r2==0);
  ctx.r2=static_cast<std::int64_t>(static_cast<std::int32_t>(0x800515c4));
  rr64_authority_actor_route(&ctx,2);check(ctx.r2==static_cast<std::int64_t>(static_cast<std::int32_t>(0x80040500)));
  ctx.r2=static_cast<std::int64_t>(static_cast<std::int32_t>(0x80040558));
  rr64_authority_actor_route(&ctx,2);check(static_cast<unsigned>(ctx.r2)==0x80040558);
  check(rr64_authority_translate(m,&ctx)==1);
  std::uint16_t held=0;engine::read_u16(m,0x807e0000,held);check(held==0x8000+s);
  engine::write_u16(m,0x807e0000,0);check(memory==before);
 }
 rr64_authority_step_finish(m);check(native==14 && finished==1 && captured==1);
 rr64_authority_step_finish(m);check(finished==1);
 engine::write_u16(m,0x800A2192,1);
 for(bool host:{true,false}){
  status.is_host=host;
  rr64_authority_step_begin(m,nullptr);rr64_authority_step_finish(m);
  check(queued==1 && finished==1 && faults==0);
 }
 status.is_host=true;engine::write_u16(m,0x800A2192,0);
 ++next.tick;rr64_authority_step_begin(m,nullptr);rr64_authority_step_finish(m);check(finished==1 && faults==1); // skipped active humans cannot be ACKed
 status.authoritative=false;rr64_authority_step_begin(m,nullptr);
 recomp_context ctx{};ctx.r4=static_cast<std::int64_t>(static_cast<std::int32_t>(0x800d8570));
 check(!rr64_authority_translate(m,&ctx));rr64_authority_step_finish(m);check(queued==2 && finished==1);
 status.authoritative=true;ctx.r5=static_cast<std::int64_t>(static_cast<std::int32_t>(0x807e0000));
 rr64_authority_step_begin(m,nullptr);check(rr64_authority_translate(m,&ctx)==1 && last_changed==0x8000);
 status.authoritative=false;rr64_authority_step_begin(m,nullptr);

 // Full host capture uses reciprocal actor links, including non-contiguous
 // human identities and shuffled bike allocations. A single bad pair aborts.
 engine::write_u32(m,0x800a656c,14);
 for(unsigned s=0;s<14;++s){
  unsigned a=0x800d8570+s*0x118,b=0x80100000+(13-s)*engine::bike::stride,r=0x80300000+s*engine::rider::stride;
  engine::write_u32(m,a+0xe0,b);engine::write_u32(m,a+0xe4,r);
  engine::write_u32(m,b+engine::bike::rider_pointer,r);engine::write_u32(m,r+engine::rider::bike_pointer,b);
  engine::write_u32(m,r+0x20,s%4);
  engine::write_u32(m,r+0x5d4,s%3);engine::write_float(m,r+0x5d8,0.25f+s);
  engine::write_float(m,b+0x4cc,0.5f+s);
  for(unsigned vector=0;vector<4;++vector)for(unsigned axis=0;axis<3;++axis)
   engine::write_float(m,r+authority::rider_dynamics_offsets[vector]+axis*4,float(s*9+vector*3+axis));
  engine::write_u32(m,a+0x18,s);engine::write_u32(m,a+0x1c,s+1);
  const unsigned route=0x80500000+s*0x64;engine::write_u32(m,a+0xe8,route);
  engine::write_u32(m,a+0x20,s==13?7:5);engine::write_u32(m,route+0x58,s);
  engine::write_u16(m,route+0x48,1);engine::write_u16(m,route+0x4c,s==6);
  engine::write_u16(m,route+0x52,s%2);engine::write_u16(m,route+0x4e,s);
  engine::write_float(m,route+8,0.25f);engine::write_float(m,route+0xc,120.f);
  engine::write_float(m,route+0x20,950.f+s);
 }
 auto capture=[](unsigned char*,unsigned bike,netplay::RiderState &s){s.root.valid=s.rider_position_valid=1;s.position_x=float(bike);return true;};
 netplay::AuthorityFrame frame{};authority::Stamp stamp{7,45,{}};
 for(unsigned i=0;i<4;++i)engine::write_float(m,0x8009cba8+i*4,0.01f*(i+1));
 engine::write_u16(m,0x800a659a,1);
 check(authority::capture_frame(m,stamp,0x2001,99,frame,capture));
 // Network capture still requires humans; only explicit diagnostic capture
 // accepts an all-AI roster, and marks every captured rider as AI.
 netplay::AuthorityFrame demo;
 check(!authority::capture_frame(m,stamp,0,99,demo,capture));
 check(authority::capture_frame(m,stamp,0,99,demo,capture,nullptr,true));
 check(demo.riders[0].host_ai && demo.riders[13].host_ai);
 for(unsigned s=0;s<14;++s){
  check(frame.outcomes[s].finished==s%2 && frame.outcomes[s].progress_gate==s);
  check(frame.outcomes[s].progress==std::array<float,3>{0.25f,120.f,950.f+s});
 }
 check(frame.timing.substeps==2 && frame.timing.bits[2]==std::bit_cast<std::uint32_t>(0.01f*3));
 for(unsigned s=0;s<14;++s){check(frame.riders[s].active && frame.riders[s].bike==s && frame.riders[s].character==s+1);check(frame.riders[s].host_ai==(s!=0 && s!=13));}
 check(frame.dynamics[13].values[8]==125.f && frame.dynamics[13].damping_mode==1);
 check(frame.dynamics[13].values[11]==128.f && frame.dynamics[13].effect==1 && frame.dynamics[13].effect_remaining==13.25f);
 const unsigned restoredRider=0x80600000;
 const unsigned restoredBike=0x80610000;
 engine::write_u32(m,restoredRider+engine::rider::bike_pointer,restoredBike);
 engine::write_u32(m,restoredBike+engine::bike::rider_pointer,restoredRider);
 auto expected=memory;
 // Independent byte copy of the audited scalar/vector ranges, preserving
 // holes and local links. Source allocation is deliberately reversed by slot.
 for(auto pair:std::array<std::array<unsigned,2>,2>{{
      {0x80100000u+0x108u,restoredBike+0x108u},
      {0x80300000u+13u*engine::rider::stride+0x28u,restoredRider+0x28u}}}) {
   for(auto range:std::array<std::array<unsigned,2>,4>{{{0x60,2},{0x64,28},{0xf4,24},{0x10c,112}}})
     for(unsigned i=0;i<range[1];++i){
       std::uint8_t value=0;engine::read_u8(m,pair[0]+range[0]+i,value);
       engine::write_s8(expected.data(),pair[1]+range[0]+i,static_cast<std::int8_t>(value));
     }
 }
 check(frame.dynamics[13].recovery_age==13.5f);
 engine::write_float(expected.data(),restoredBike+0x4cc,13.5f);
 engine::write_u32(expected.data(),restoredRider+0x20,frame.dynamics[13].damping_mode);
 engine::write_u32(expected.data(),restoredRider+0x5d4,1);engine::write_float(expected.data(),restoredRider+0x5d8,13.25f);
 for(unsigned vector=0;vector<4;++vector)for(unsigned axis=0;axis<3;++axis)
  engine::write_float(expected.data(),restoredRider+authority::rider_dynamics_offsets[vector]+axis*4,frame.dynamics[13].values[vector*3+axis]);
 check(authority::restore_dynamics(m,restoredRider,frame.dynamics[13]) && memory==expected);
 auto invalidDynamics=frame.dynamics[13];invalidDynamics.values[8]=std::bit_cast<float>(0x7fc00000u);
 check(!authority::restore_dynamics(m,restoredRider,invalidDynamics) && memory==expected);
 invalidDynamics=frame.dynamics[13];invalidDynamics.effect=3;
 check(!authority::restore_dynamics(m,restoredRider,invalidDynamics) && memory==expected);
 check(!authority::restore_dynamics(m,0x807ffffcu,frame.dynamics[13]) && memory==expected);
 check(frame.stamp.tick==45 && frame.riders[13].sample_time_us==99);
 check(frame.outcomes[13].role==7 && frame.outcomes[13].busts==13 && frame.outcomes[6].busted==1);
 engine::write_u32(m,0x80300000+6*engine::rider::stride+engine::rider::bike_pointer,0);
 auto rejected=frame;rejected.stamp.tick=999;
 check(!authority::capture_frame(m,stamp,0x2001,99,rejected,capture));check(rejected.stamp.tick==999);
 engine::write_u32(m,0x80300000+6*engine::rider::stride+engine::rider::bike_pointer,0x80100000+7*engine::bike::stride);
 // A valid reciprocal pair still cannot belong to two active roster entries.
 engine::write_u32(m,0x800d8570+0x118+0xe0,0x80100000+13*engine::bike::stride);
 engine::write_u32(m,0x800d8570+0x118+0xe4,0x80300000);
 check(!authority::capture_frame(m,stamp,0x2001,99,rejected,capture));check(rejected.stamp.tick==999);
}

extern "C" void rr64_prediction_capture_reset(){}
extern "C" int rr64_prediction_capture_before(unsigned char*,const void*,const void*){return 1;}
extern "C" int rr64_prediction_capture_after(unsigned char*){return 1;}

extern "C" unsigned rr64_authority_take_actions(unsigned){return 0;}
extern "C" int rr64_authority_eject_step(unsigned char*,void*,unsigned,unsigned){return 1;}

extern "C" void rr64_authority_reset_eject(){}
