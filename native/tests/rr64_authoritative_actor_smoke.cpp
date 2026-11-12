#include "rr64_netplay.hpp"
#include "rr64_engine_layout.hpp"
#include "recomp.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <source_location>
extern "C" void generated_actor_loop(unsigned char*,recomp_context*);
extern "C" int rr64_authority_step_begin(unsigned char*,void*);
extern "C" void rr64_authority_step_finish(unsigned char*);
using namespace rr64;
namespace {netplay::Status status;authority::Step step;unsigned pre=0,overrides=0,driven=0,reactions=0,finished=0,pass=0;}
void check(bool b,std::source_location at=std::source_location::current()){if(!b){std::fprintf(stderr,"generated actor loop check failed at %u (pass %u)\n",at.line(),pass);std::exit(1);}}
gpr guest(unsigned p){return static_cast<std::int64_t>(static_cast<std::int32_t>(p));}
namespace rr64::netplay {
void authority_pin_frame(){}
bool authority_race_gate(bool){return true;}
Status get_status(){return status;}
bool authority_queue_input_recorded(std::uint16_t b,std::int8_t x,std::int8_t y,authority::Command &c,std::uint8_t actions,unsigned duration_us){c={status.game_setup.revision,1,b,x,y};return authority_queue_input(b,x,y);}
void authority_fail(const char*){check(false);}
bool authority_queue_input(std::uint16_t,std::int8_t,std::int8_t){return true;}
bool authority_begin_step(authority::Step &out,unsigned duration_us){out=step;return true;}
bool authority_finish_step(std::uint64_t tick,authority::Stamp &stamp){check(tick==step.tick);++finished;stamp.round=step.round;stamp.tick=tick;return true;}
}
extern "C" int rr64_online_authority_capture(unsigned char*,const void*){return 1;}
void pre_update(unsigned char*,recomp_context*){++pre;}
void override_controls(unsigned char*,recomp_context*){++overrides;}
void move(unsigned char*,recomp_context *c){
 unsigned slot=(static_cast<unsigned>(c->r4)-0x800d8570)/0x118;
 check(slot<14 && c->r6==step.inputs[slot].buttons);++driven;
 check(c->r5==(pass?0:step.inputs[slot].buttons)); // press once, hold across substeps
}
extern "C" void func_800404BC(unsigned char*,recomp_context*){++reactions;}
extern "C" recomp_func_t *get_function(std::int32_t address){
 switch(static_cast<unsigned>(address)){
 case 0x800524cc:case 0x8005264c:return pre_update;
 case 0x80040500:case 0x80040558:return override_controls;
 case 0x8004090c:return move;
 default:check(false);return nullptr;
 }
}
int main(){
 for(unsigned count:{2u,4u,14u})for(bool transition:{false,true})for(unsigned passes:{1u,2u}){
  std::vector<unsigned char> memory(16*1024*1024);auto *m=memory.data();
  status={};status.active=status.connected=status.authoritative=status.is_host=true;
  status.phase=netplay::Phase::Race;status.local_slot=0;status.game_setup.revision=count+unsigned(transition)*20+passes*100;
  status.authority_humans=(1u<<count)-1;step={};step.round=status.game_setup.revision;step.tick=1;
  engine::write_u32(m,0x800a656c,count);
  for(unsigned i=0;i<4;++i)engine::write_float(m,0x8009cba8+i*4,.016667f);
  for(unsigned s=0;s<count;++s){
   const unsigned a=0x800d8570+s*0x118,b=0x80100000+s*engine::bike::stride,r=0x80300000+s*engine::rider::stride;
   engine::write_u32(m,a+4,s);engine::write_u16(m,a+0x24,1);engine::write_u16(m,a+0x26,s?1:0);
   engine::write_u32(m,a+0xe0,b);engine::write_u32(m,a+0xe4,r);
   engine::write_u32(m,a+0x108,transition?0x8004ead0:0x8004eb6c);
   engine::write_u32(m,a+0x10c,transition?0x80040558:0x800515c4);engine::write_u32(m,a+0x110,0x8004090c);
   engine::write_float(m,r+0x5c4,1.f);
   step.inputs[s]={step.round,1,static_cast<std::uint16_t>(0x8020+s),0,0};
  }
  recomp_context ctx{};ctx.r23=guest(0x800d8570);ctx.r29=guest(0x807f0000);ctx.r30=guest(0x807e0000);
  for(unsigned i=0;i<4;++i)engine::write_u32(m,0x807f0040+i*4,0x807f0028+i*(i<2?2:4));
  // Original stack outputs: two halfwords followed by two floats.
  engine::write_u32(m,0x807f0048,0x807f002c);engine::write_u32(m,0x807f004c,0x807f0030);
  const auto saved=memory;pre=overrides=driven=reactions=finished=0;
  rr64_authority_step_begin(m,nullptr);
  for(pass=0;pass<passes;++pass){generated_actor_loop(m,&ctx);check(finished==0);}
  rr64_authority_step_finish(m);
  check(pre==count*passes && overrides==count*passes && driven==count*passes && reactions==count*passes && finished==1);
  for(unsigned s=0;s<count;++s){unsigned a=0x800d8570+s*0x118,index=0,callback=0;std::uint16_t flag=0;
   engine::read_u32(m,a+4,index);engine::read_u16(m,a+0x26,flag);engine::read_u32(m,a+0x108,callback);
   check(index==s && flag==(s?1:0) && callback==(transition?0x8004ead0u:0x8004eb6cu));
  }
 }
 std::puts("actual generated actor loop: 2/4/14 humans, paired callbacks, one/two passes and single input edges passed (physics mocked)");
}

extern "C" void rr64_prediction_capture_reset(){}
extern "C" int rr64_prediction_capture_before(unsigned char*,const void*,const void*){return 1;}
extern "C" int rr64_prediction_capture_after(unsigned char*){return 1;}

extern "C" unsigned rr64_authority_take_actions(unsigned){return 0;}
extern "C" int rr64_authority_eject_step(unsigned char*,void*,unsigned,unsigned){return 1;}

extern "C" void rr64_authority_reset_eject(){}
#include "rr64_prediction_reconcile.hpp"
extern "C" void rr64_prediction_reconcile_step(unsigned char*){}
extern "C" void rr64_prediction_verify_before(unsigned char*,void*){}
extern "C" void rr64_prediction_verify_after(unsigned char*){}
