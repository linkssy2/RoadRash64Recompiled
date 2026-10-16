#include "rr64_prediction_replay.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_prediction_cop_state.hpp"
#include "rr64_custom_cop.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "recomp.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <source_location>
extern "C" int rr64_custom_cop_active(){return 1;}
rr64::netplay::Status test_status;
rr64::authority::Outcome test_outcome;
unsigned outcome_reads=0;
namespace rr64::netplay {
Status get_status(){return test_status;}
bool authority_get_outcome(unsigned slot,authority::Outcome &out,bool &mode){
 ++outcome_reads;
 if(slot!=13)return false;out=test_outcome;mode=true;return true;
}
}
extern "C" void rr64_custom_cop_equipment(unsigned char*,unsigned){}
extern "C" void func_8006B740(unsigned char*,recomp_context*){std::abort();}
extern "C" void func_8006B9E8(unsigned char*,recomp_context*){std::abort();}
void check(bool b,std::source_location where=std::source_location::current()){if(!b){std::fprintf(stderr,"prediction side-effect check failed line %u\n",where.line());std::exit(1);}}
int main(){
 using namespace rr64;
 std::vector<unsigned char> memory(16*1024*1024);auto *m=memory.data();
 const unsigned actor=0x800d8570,bike=0x80100000;
 engine::write_u32(m,0x800a6578,1);engine::write_u32(m,actor+0xe0,bike);engine::write_u32(m,actor+0x20,7);
 auto hold=[&](float time){
  engine::write_float(m,0x800a1820,time);
  recomp_context ctx{};ctx.r4=static_cast<std::int64_t>(static_cast<std::int32_t>(actor));ctx.r6=0x20;
  rr64_custom_cop_control(m,&ctx);
 };
 check(rr64_custom_cop_siren(m,bike)==0);
 prediction::CopPostsState historical;check(prediction::capture_cop_posts(historical));
 check(!prediction::seed_cop_posts(historical));
 prediction::CopPostsState continued;
 check(!prediction::replay_cop_posts(continued));
 {
  prediction::ReplayScope replay;check(replay.valid());
  {prediction::ReplayScope nested;check(!nested.valid());}
  check(prediction::active());hold(1.f);hold(2.1f);
  check(rr64_custom_cop_siren(m,bike)==1);
  check(prediction::replay_cop_posts(continued));
 }
 check(!prediction::active() && rr64_custom_cop_siren(m,bike)==0);
 {prediction::ReplayScope replay;check(!prediction::replay_cop_posts(continued));
  check(prediction::seed_cop_posts(continued));check(rr64_custom_cop_siren(m,bike)==1);}
 check(rr64_custom_cop_siren(m,bike)==0);
 {prediction::ReplayScope replay;check(replay.valid());check(rr64_custom_cop_siren(m,bike)==0);}
 // Normal play still uses the real state and retains the original long-press.
 hold(3.f);hold(4.1f);check(rr64_custom_cop_siren(m,bike)==1);
 {
  prediction::ReplayScope replay;
  check(prediction::seed_cop_posts(historical));
  check(rr64_custom_cop_siren(m,bike)==0);
  check(!prediction::capture_cop_posts(historical));
 }
 check(rr64_custom_cop_siren(m,bike)==1);
 {prediction::ReplayScope replay;check(rr64_custom_cop_siren(m,bike)==1);}
 check(rr64_custom_cop_siren(m,bike)==1);
 test_status.active=test_status.connected=test_status.authoritative=test_status.is_host=true;
 test_status.authority_humans=0x2001;engine::write_u32(m,0x800a656c,14);
 const unsigned remote_actor=actor+13*0x118,remote_bike=0x80110000;
 engine::write_u32(m,remote_actor+0xe0,remote_bike);engine::write_u32(m,remote_actor+0x20,7);
 check(rr64_custom_cop_siren(m,remote_bike)==0);
 for(float time:{10.f,11.1f}){
  engine::write_float(m,0x800a1820,time);recomp_context c{};
  c.r4=static_cast<std::int64_t>(static_cast<std::int32_t>(remote_actor));c.r6=0x20;
  rr64_custom_cop_control(m,&c);
 }
 check(rr64_custom_cop_siren(m,remote_bike)==1 && rr64_custom_cop_siren(m,bike)==1);
 {prediction::ReplayScope replay(prediction::capture_session_rules(test_status));check(prediction::seed_cop_posts(historical));check(rr64_custom_cop_siren(m,remote_bike)==0);}
 check(rr64_custom_cop_siren(m,remote_bike)==1);
 test_status.is_host=false;test_status.local_slot=13;test_status.replicated_riders=true;
 test_outcome.role=7;test_outcome.siren=1;check(rr64_custom_cop_siren(m,bike)==1);
 test_outcome.siren=0;check(rr64_custom_cop_siren(m,bike)==0);
 {
  const auto reads=outcome_reads;
  prediction::ReplayScope replay(prediction::capture_session_rules(test_status));
  check(prediction::seed_cop_posts(historical));
  test_outcome.siren=1;
  // Movement prediction uses the mapped local cop roster. Private evaluation
  // uses its historical siren, never a newer host outcome.
  check(rr64_custom_cop_siren(m,bike)==0 && outcome_reads==reads);
 }
 check(rr64_custom_cop_siren(m,remote_bike)==-1); // guest13 represents network host0
 test_status={};check(rr64_custom_cop_siren(m,remote_bike)==-1); // local stays four-slot
 std::puts("native cop replay isolation and unchanged live siren behavior passed");
}
