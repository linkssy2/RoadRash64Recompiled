#define RR64_EXPERIMENTAL_COURSE 1
#include "../src/rr64_prediction_capture.cpp"
#include "../src/rr64_prediction_reconcile.cpp"
#include "../src/rr64_authoritative_eject.hpp"
#include <cstdio>
#include <cstdlib>
namespace {
rr64::prediction::FrameOutput live_native;
rr64::netplay::AuthorityReplayPlan network_plan;
bool approve_network=false,fail_execute=false,has_frame=true;
bool exercise_eject=false;
unsigned eject_transitions=0;
unsigned tickets=0;
constexpr unsigned bike=0x80100000,rider=0x80300000;
void check(bool b){if(!b)std::abort();}
}
namespace rr64::netplay {
Status get_status(){Status s;s.active=s.connected=s.authoritative=true;s.replicated_riders=true;s.local_slot=1;
 s.authority_humans=3;s.phase=Phase::Race;s.game_setup.revision=7;return s;}
bool authority_prepare_replay(AuthorityReplayPlan &p){if(!has_frame)return false;p=network_plan;return true;}
bool authority_commit_replay(std::uint64_t t){++tickets;if(!approve_network || t!=network_plan.ticket)return false;has_frame=false;return true;}
}
namespace ultramodern::rr64 {
bool copy_guest_snapshot(unsigned char *m,unsigned char *out,std::size_t size){std::memcpy(out,m,size);return true;}
}
namespace rr64::mk64_items {
bool capture_replay(ReplayState &out) noexcept {out=live_native.items;return true;}
bool correct_replay(unsigned char*,ReplayState &out,const Snapshot &state) noexcept {
 if(!valid(state))return false;
 out.state=state;out.elapsed_us=std::uint64_t(state.clock)*1000000/30;return true;
}
}
namespace rr64::prediction {
bool capture_cop_rules(CopRulesState &s){s=live_native.rules;return true;}
bool capture_cop_posts(CopPostsState &s){s=live_native.posts;return true;}
bool capture_manual_eject(ManualEjectState &s){s=live_native.eject;return true;}
void commit_cop_rules(const CopRulesState &s)noexcept{live_native.rules=s;}
void commit_cop_posts(const CopPostsState &s)noexcept{live_native.posts=s;}
void commit_manual_eject(const ManualEjectState &s)noexcept{live_native.eject=s;}
// This fixture isolates commit ownership; native ordering has its own original-
// instruction equivalence test, and history-stage sequencing is tested separately.
bool replay_native_order(Resources&,const FrameInput&){return true;}
bool replay_native_streaming(Resources&,const StreamingInput&,const ResourceInventory&){return true;}
bool replay_native_frame(Resources &resources,const FrameInput &input,FrameOutput &out){
 float x=0;engine::read_float(resources.memory(),bike+engine::bike::body_position,x);
 engine::write_float(resources.memory(),bike+engine::bike::body_position,x+1);
 out={input.rules,input.posts,input.eject,input.items};out.posts[0].cue+=1;
 if(!exercise_eject)check(input.items.state==network_plan.frame.mk64_items);
 out.items.elapsed_us+=input.replay_duration_us;
 if(exercise_eject){
  if(!authority::eject_step(resources.memory(),0,(input.command.actions&authority::action_eject)!=0,out.eject,
    [&](unsigned){++eject_transitions;return true;}))return false;
 }
 return !fail_execute;
}
bool capture_prediction_movement(unsigned char *m,const authority::Stamp&,unsigned,unsigned,bool,netplay::AuthorityFrame &out,const char **,bool){
 out=network_plan.frame;return engine::read_float(m,bike+engine::bike::body_position,out.riders[1].position_x);
}
}
int main(){
 using namespace rr64;
 std::vector<unsigned char> m(engine::kRdramSize),rom(16),saved(engine::kRdramSize);
 engine::write_u32(m.data(),0x800a656c,2);
 engine::write_u16(m.data(),0x800d8594,1);
 // A live bike owns a render object/descriptor, also consulted by simulation.
 engine::write_u32(m.data(),bike+8,0x80600000);
 engine::write_u32(m.data(),0x80600028,0x80600100);
 engine::write_u32(m.data(),0x800d8570+0xe0,bike);engine::write_u32(m.data(),0x800d8570+0xe4,rider);
 engine::write_u32(m.data(),0x800d8570+0xe8,0x80500000);
 // Canonical remote host maps to guest slot 1; its live results must survive
 // reconciliation even when the historical authority frame contains older ones.
 engine::write_u32(m.data(),0x800d8570+0x118+0xe8,0x80500100);
 engine::write_u32(m.data(),0x80500158,88);
 engine::write_u32(m.data(),bike+engine::bike::rider_pointer,rider);engine::write_u32(m.data(),rider+engine::rider::bike_pointer,bike);
 engine::write_u16(m.data(),bike+engine::bike::rider_attached,1);
 engine::write_u16(m.data(),rider+engine::rider::bike_attached,1);
 engine::write_u16(m.data(),0x8009ce90,32);
 for(unsigned i=0;i<32;++i)engine::write_u16(m.data(),0x800b1580+i*2,i);
 engine::write_u32(m.data(),0x800b1970,1);engine::write_u32(m.data(),0x800b1974,0x800b1980);
 authority::NativeTiming timing;timing.substeps=1;for(auto &bits:timing.bits)bits=std::bit_cast<unsigned>(.01f);
 check(prediction::restore_timing(m.data(),timing));
 recomp_context ctx{};ctx.f_odd=&ctx.f0.u32h;
 authority::Command command{7,1,0,0,0};
 engine::write_float(m.data(),bike+engine::bike::body_position,10);
 live_native.posts[0].cue=5;
 live_native.items.state.enabled=1;live_native.items.state.clock=10;live_native.items.state.random=42;
 live_native.items.state.riders[1].boost_until=100;live_native.items.state.riders[1].boost_speed=30;
 check(rr64_prediction_capture_before(m.data(),&command,&ctx));
 engine::write_float(m.data(),bike+engine::bike::body_position,11);live_native.posts[0].cue=6;
 check(rr64_prediction_capture_after(m.data()));
 // Newer host-owned/native state must not rewind to the journal's values.
 live_native.rules.win_started=99;
 live_native.posts[1].cue=88;
 live_native.posts[1].muted=false;
 live_native.items.state.clock=50; // New live effects cannot enter historical replay.
 network_plan.ticket=4;network_plan.frame.stamp={7,1,{}};
 network_plan.frame.mk64_items=live_native.items.state;
 network_plan.frame.mk64_items.clock=20;network_plan.frame.mk64_items.riders[1].boost_speed=40;
 auto &state=network_plan.frame.riders[1];state.active=true;state.root.valid=state.rider_position_valid=1;state.position_x=12;
 state.root.bike_attached=state.root.rider_attached=1;
 network_plan.frame.outcomes[1].busts=4;
 network_plan.frame.outcomes[0].valid=1;network_plan.frame.outcomes[0].busts=2;
 network_plan.commands[0]=command;network_plan.count=1;
 const auto before=m;const auto epoch=history->epoch;
 const auto inventory=history->native.front().resources;
 history->native.front().resources.unrepresented=1;
 check(prediction::reconcile_movement(m.data(),rom));
 check(m==before && history->epoch==epoch && tickets==0 && has_frame);
 history->native.front().resources=inventory;

 fail_execute=true;check(!prediction::reconcile_movement(m.data(),rom) && m==before && tickets==0 && history->epoch==epoch);
 fail_execute=false;check(!prediction::reconcile_movement(m.data(),rom) && m==before && tickets==1 && history->epoch==epoch);
 check(prediction::local_correction_presentation.sample(prediction::mounted_pose(m.data(),7,0),prediction::presentation_time_us())==std::array<float,3>{});
 approve_network=true;check(prediction::reconcile_movement(m.data(),rom) && tickets==2 && history->epoch!=epoch);
 float x=0;engine::read_float(m.data(),bike+engine::bike::body_position,x);check(x==13 && live_native.posts[0].cue==6);
 const auto displayed=prediction::local_correction_presentation.sample(prediction::mounted_pose(m.data(),7,0),prediction::presentation_time_us());
 check(displayed[0]<-1.5f && displayed[0]>=-2.0f && displayed[1]==0 && displayed[2]==0);
 check(prediction::last_reconcile.stage==static_cast<unsigned>(prediction::ReconcileStage::Applied));
 check(prediction::last_reconcile.distance==2.0f && prediction::last_reconcile.pending==1);
 unsigned busts=0;std::uint16_t busted=0;engine::read_u32(m.data(),0x80500058,busts);engine::read_u16(m.data(),0x8050004c,busted);
 check(busts==4 && busted==0);
 check(live_native.rules.win_started==99 && live_native.posts[1].cue==88 && !live_native.posts[1].muted);
 check(live_native.items.state.clock==50 && live_native.items.state.riders[1].boost_speed==30);
 check(history->native.front().items.state==network_plan.frame.mk64_items);
 check(history->native.back().items.state==network_plan.frame.mk64_items &&
       history->native.back().items.elapsed_us==std::uint64_t(20)*1000000/30+command.duration_us);
 engine::read_u32(m.data(),0x80500158,busts);check(busts==88);
 check(history->guest.restore(7,1,saved.data(),saved.size()));engine::read_float(saved.data(),bike+engine::bike::body_position,x);check(x==13);
 const auto committed=m;check(prediction::reconcile_movement(m.data(),rom) && m==committed && tickets==2);
 // The first half of an eject interval has already run on the host. Replaying
 // only its tail must retain verified protection without invoking eject again.
 exercise_eject=true;
 // Also cover a fully acknowledged edge and an acknowledged edge followed by
 // a wholly unentered command. Both must reject stale baseline protection.
 for(unsigned phase=0;phase<3;++phase)for(unsigned rejection=0;rejection<4;++rejection){
  rr64_prediction_capture_reset();m=before;live_native={};has_frame=true;approve_network=false;
  timing.bits[0]=std::bit_cast<unsigned>(.025f);check(prediction::restore_timing(m.data(),timing));
  command={7,1,0,0,0,authority::action_eject,25000};
  engine::write_float(m.data(),bike+engine::bike::durability_current,80);
  engine::write_float(m.data(),bike+engine::bike::durability_capacity,100);
  engine::write_u16(m.data(),bike+engine::bike::drive_control_lockout,0);
  engine::write_u16(m.data(),rider+engine::rider::ejected,0);
  check(rr64_prediction_capture_before(m.data(),&command,&ctx));
  live_native.eject[0]={bike,80,true};
  engine::write_u16(m.data(),bike+engine::bike::drive_control_lockout,1);
  engine::write_u16(m.data(),rider+engine::rider::ejected,1);
  check(rr64_prediction_capture_after(m.data()));
  if(phase==2){
   command.sequence=2;command.actions=0;
   check(rr64_prediction_capture_before(m.data(),&command,&ctx));
   check(rr64_prediction_capture_after(m.data()));
  }
  live_native.rules.win_started=99;live_native.posts[1].cue=88;
  live_native.eject[1]={bike+engine::bike::stride,33,true};
  network_plan={};network_plan.ticket=100+phase*4+rejection;network_plan.count=phase==1?0:1;network_plan.commands[0]=command;
  network_plan.frame.stamp={7,2,{}};
  network_plan.frame.stamp.acknowledged[1]=phase?1:0;
  network_plan.frame.stamp.simulated_us[1]=phase?25000:12500;
  auto &ejected=network_plan.frame.riders[1];ejected.active=true;ejected.root.valid=ejected.rider_position_valid=1;
  ejected.root.bike_attached=ejected.root.rider_attached=1;ejected.root.drive_lockout=rejection==3?0:1;
  ejected.root.ejected=rejection==1?0:1;ejected.root.durability=rejection>=2?60:80;ejected.root.durability_capacity=100;
  network_plan.frame.outcomes[1].valid=1;
  const auto before_attempt=m;const auto before_native=live_native.eject;
  const auto edge_epoch=history->epoch;
  check(!prediction::reconcile_movement(m.data(),rom));
  check(m==before_attempt && live_native.eject==before_native && history->epoch==edge_epoch && has_frame);
  approve_network=true;check(prediction::reconcile_movement(m.data(),rom));
  check(live_native.eject[0].active==(rejection==0) && eject_transitions==0);
  check(live_native.eject[1]==before_native[1] && live_native.posts[1].cue==88 && live_native.rules.win_started==99);
  if(!rejection)check(live_native.eject[0].bike==bike && live_native.eject[0].durability==80);
  float health=0;engine::read_float(m.data(),bike+engine::bike::durability_current,health);
  check(health==ejected.root.durability);
  // The next ordinary native eject maintenance must not heal host damage.
  check(authority::eject_step(m.data(),0,false,live_native.eject,[](unsigned){std::abort();return false;}));
  engine::read_float(m.data(),bike+engine::bike::durability_current,health);
  check(health==ejected.root.durability);
 }
 std::puts("connected reconciliation: private correction/replay, rejected ticket isolation, live/native/history commit and duplicate retirement pass; executor is synthetic");
}
