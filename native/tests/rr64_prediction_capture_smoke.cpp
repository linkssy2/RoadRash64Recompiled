#define RR64_EXPERIMENTAL_COURSE 1
#include "../src/rr64_prediction_capture.cpp"
#include "../src/rr64_prediction_verify_inputs.hpp"
#include "../src/rr64_authoritative_eject.hpp"
#include <cstdio>
#include <cstdlib>
namespace { unsigned captures=0;float clock_value=1;bool compiled_terrain=false; }
extern "C" int rr64_terrain_streaming_bounded(unsigned char*) { return compiled_terrain; }
namespace rr64::mk64_items {
bool capture_replay(ReplayState &out) noexcept {
 out={};out.state.enabled=1;out.state.clock=unsigned(clock_value);out.state.random=42;
 out.state.riders[13].boost_until=100;out.state.riders[13].boost_speed=30;
 out.elapsed_us=unsigned(clock_value)*1000;return true;
}
}
namespace rr64::netplay {Status get_status(){Status s;s.active=s.connected=s.authoritative=s.is_host=true;s.local_slot=0;s.authority_humans=0x2001;return s;}}
namespace ultramodern::rr64 {
bool copy_guest_snapshot(unsigned char *m,unsigned char *out,std::size_t size){++captures;std::memcpy(out,m,size);return true;}
}
namespace rr64::prediction {
bool capture_manual_eject(ManualEjectState &out){out[0].durability=clock_value;return true;}
bool capture_cop_rules(CopRulesState &out){out.win_started=clock_value;return true;}
bool capture_cop_posts(CopPostsState &out){out[13].cue=clock_value;return true;}
}
void check(bool value){if(!value)std::abort();}
int main(){
 // COP1 rounding belongs to the executing thread. A moved historical entry
 // must restore it privately and preserve the caller even if execution throws.
 std::fenv_t original;check(std::fegetenv(&original)==0);
 for(int rounding : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
  check(std::fesetround(rounding)==0);
  recomp_context source{};source.f_odd=&source.f0.u32h;
  rr64::prediction::CpuContext captured;check(captured.capture(source));
  auto relocated=captured;
  check(std::fesetround(FE_TONEAREST)==0);
  try {
   rr64::prediction::FloatingPointScope floating(relocated);
   check(floating.valid() && std::fegetround()==rounding);
   check(std::fesetround(FE_UPWARD)==0);
   throw 1;
  }catch(int){}
  check(std::fegetround()==FE_TONEAREST);
 }
 {rr64::prediction::CpuContext invalid;
  rr64::prediction::FloatingPointScope floating(invalid);check(!floating.valid());}
 check(std::fesetenv(&original)==0);
 for(unsigned mode=0;mode<2;++mode){
  rr64::prediction::CpuContext saved;
  recomp_context source{};source.mips3_float_mode=mode;
  source.f_odd=mode?&source.f1.u32l:&source.f0.u32h;
  *source.f_odd=0x12345678;source.r29=0xffffffff807f0000ull;
  check(saved.capture(source));
  auto relocated=saved;source.f_odd=nullptr;source.f0={};source.f1={};
  recomp_context destination{};check(relocated.restore(destination));
  check(destination.f_odd==(mode?&destination.f1.u32l:&destination.f0.u32h));
  check(*destination.f_odd==0x12345678 && destination.r29==0xffffffff807f0000ull);
  *destination.f_odd=42;check(source.f0.u64==0 && source.f1.u64==0);
  rr64::prediction::CpuContext invalid;check(!invalid.capture(source) && !invalid.restore(destination));
 }

 std::vector<unsigned char> m(rr64::engine::kRdramSize),restored(m.size());
 // Omitted entry state is reported by word offset, including block boundaries.
 rr64::engine::write_u32(restored.data(),0x800100fc,1);
 rr64::engine::write_u32(restored.data(),0x80010100,2);
 rr64::engine::write_u32(restored.data(),0x80010864,3);
 auto offsets=rr64::prediction::compare_entry_region(m.data(),restored.data(),0x80010000,0x868,0,"bike");
 check(offsets.valid && offsets.words==3 && offsets.mask[0]==(std::uint64_t(1)<<63) &&
       offsets.mask[1]==1 && offsets.mask[8]==(std::uint64_t(1)<<25));
 check(!rr64::prediction::compare_entry_region(m.data(),restored.data(),0x807ffffc,8,0,"invalid").valid);
 std::fill(restored.begin(),restored.end(),0);
 rr64::authority::NativeTiming timing{};timing.substeps=2;
 for(unsigned i=0;i<timing.bits.size();++i)timing.bits[i]=std::bit_cast<unsigned>(float(i+1)/61.f);
 check(rr64::prediction::restore_timing(m.data(),timing));
 rr64::prediction::UpdateCounters counters{0xffffffffu,0x80000000u,std::bit_cast<unsigned>(0.025f)},captured{};
 check(rr64::prediction::restore_update_counters(m.data(),counters));
 check(rr64::prediction::capture_update_counters(m.data(),captured) && captured==counters);
 check(!rr64::prediction::capture_update_counters(nullptr,captured) && captured==counters);
 const auto before_invalid_clock=m;
 auto invalid_clock=counters;invalid_clock[2]=0x7fc00000;
 check(!rr64::prediction::restore_update_counters(m.data(),invalid_clock) && m==before_invalid_clock);
 // Represent a quiescent resource pool in the historical fixture.
 rr64::engine::write_u16(m.data(),0x8009ce90,32);
 for(unsigned i=0;i<32;++i)rr64::engine::write_u16(m.data(),0x800b1580+2*i,i);
 rr64::engine::write_u32(m.data(),0x800b1970,1);
 rr64::engine::write_u32(m.data(),0x800b1974,0x800b1980);
 recomp_context ctx{};ctx.f_odd=&ctx.f0.u32h;
 rr64::authority::Command c{1,2,0,0,0};
 check(!rr64_prediction_capture_before(m.data(),&c,&ctx));c.sequence=1;
 m[100]=5;check(rr64_prediction_capture_before(m.data(),&c,&ctx));
 check(history->native.size()==1 && history->native.front().rules.win_started==1);
 check(history->native.front().items.state.clock==1 && history->native.front().items.state.riders[13].boost_speed==30);
 check(history->native.front().session.humans==0x2001 && history->native.front().session.is_host);
 m[100]=8;clock_value=2;check(rr64_prediction_capture_after(m.data()));
 check(history->native.back().timing==timing);
 check(history->guest.restore(1,0,restored.data(),restored.size()) && restored[100]==5);
 check(history->guest.restore(1,1,restored.data(),restored.size()) && restored[100]==8);
 check(history->native.back().posts[13].cue==2 && captures==2);
 check(history->native.back().items.state.clock==2 && history->native.back().items.elapsed_us==2000);
 check(!rr64_prediction_capture_after(m.data()) && !rr64_prediction_capture_before(m.data(),&c,&ctx));
 c.sequence=2;check(rr64_prediction_capture_before(m.data(),&c,&ctx));
 check(rr64::prediction::restore_update_counters(m.data(),{0,1}));
 auto later=timing;later.bits[0]=std::bit_cast<unsigned>(1.f/30.f);later.substeps=1;
 check(rr64::prediction::restore_timing(m.data(),later));
 check(!rr64_prediction_capture_before(m.data(),&c,&ctx));check(rr64_prediction_capture_after(m.data()));
 check(history->native.back().timing==timing); // completion must retain entry timing
 rr64::prediction::HistoricalReplay plan;
 const std::array commands{history->native[1].command,history->native[2].command};
 check(rr64::prediction::prepare_history_replay(1,0,commands,0x800d8570,plan));
 check(plan.memory[100]==5 && plan.steps.size()==2 && plan.baseline.rules.win_started==1);
 check(plan.baseline.items.state.clock==1 && plan.baseline.items.elapsed_us==1000);
 check(plan.steps[0].previous_buttons==0 && plan.steps[1].timing==timing);
 check(plan.steps[0].counters==counters && plan.steps[1].counters==counters); // entry, not completion
 check(plan.completed_eject.size()==2 && plan.completed_eject[0].durability==2 && plan.completed_eject[1].durability==2);
 plan.memory[100]=77;
 check(history->guest.restore(1,0,restored.data(),restored.size()) && restored[100]==5);
 auto wrong=commands;wrong[1].buttons^=1;
 check(!rr64::prediction::prepare_history_replay(1,0,wrong,0x800d8570,plan));
 check(plan.memory[100]==77 && plan.steps.size()==2); // failed prepare is atomic
 const auto inventory=history->native.front().resources;
 history->native.front().resources.unrepresented=1;
 rr64::prediction::ReplayAdmission admission;
 check(!rr64::prediction::prepare_history_replay(1,0,commands,0x800d8570,plan,&admission));
 check(admission==rr64::prediction::ReplayAdmission::ResourcePending && plan.memory[100]==77);
 check(!rr64::prediction::prepare_history_replay(1,0,wrong,0x800d8570,plan,&admission));
 check(admission==rr64::prediction::ReplayAdmission::Invalid);
 history->native.front().resources=inventory;
 // Fully acknowledged correction performs no native update, so a held worker
 // must not prevent it. The private image retains the task exactly as captured.
 const auto last_inventory=history->native.back().resources;
 history->native.back().resources.unrepresented=1;
 rr64::prediction::HistoricalReplay no_steps;
 check(rr64::prediction::prepare_history_replay(1,2,{},0x800d8570,no_steps,&admission));
 check(admission==rr64::prediction::ReplayAdmission::Ready && no_steps.steps.empty());
 rr64::prediction::ReplayedHistory no_steps_result;
 check(rr64::prediction::evaluate_history_replay(no_steps,{},no_steps_result,
     [](auto&,const auto&){std::abort();return false;},
     [](auto&,const auto&,auto&){std::abort();return false;}));
 check(no_steps_result.final_memory==no_steps.memory && no_steps_result.native.size()==1);
 history->native.back().resources=last_inventory;


 check(rr64::prediction::prepare_history_replay(1,1,std::span(commands).subspan(1),0x800d8570,plan));
 check(plan.memory[100]==8 && plan.steps.size()==1 && plan.baseline.rules.win_started==2);
 rr64::prediction::ReplayedHistory result;
 const auto execute=[](rr64::prediction::Resources &resources,const rr64::prediction::FrameInput &input,
                      rr64::prediction::FrameOutput &out){
     check(input.rules.win_started==2 && input.items.state.clock==2);resources.memory()[100]+=10;
     out={input.rules,input.posts,input.eject,input.items};out.rules.win_started=3;return true;
 };
 unsigned prepares=0;
 unsigned char *evaluated_image=nullptr;
 const auto prepare=[&](auto &resources,const auto &input){
     evaluated_image=resources.memory();
     ++prepares;check(input.command.sequence==2 && resources.memory()[100]==8);
     resources.memory()[101]=42;return true;
 };
 check(rr64::prediction::evaluate_history_replay(plan,{},result,prepare,execute));
 check(prepares==1 && result.final_memory[101]==42 && m[101]==0);
 check(result.final_memory.data()==evaluated_image && result.final_memory.data()!=plan.memory.data());
 check(result.final_memory[100]==18 && m[100]==8 && plan.memory[100]==8);
 check(!rr64::prediction::evaluate_history_replay(plan,{},result,prepare,
     [](auto &resources,const auto&,auto&){resources.memory()[100]=99;return false;}));
 check(result.final_memory[100]==18 && history->native.size()==3);
 check(!rr64::prediction::evaluate_history_replay(plan,{},result,
     [](auto &resources,const auto&){resources.memory()[100]=99;return false;},execute));
 check(result.final_memory[100]==18 && result.final_memory[101]==42 && plan.memory[100]==8);
 // Initial baseline is already at update entry; only subsequent updates need
 // the between-update stage. A corrected completion needs it even on step one.
 rr64::prediction::HistoricalReplay initial;
 check(rr64::prediction::prepare_history_replay(1,0,commands,0x800d8570,initial));
 rr64::prediction::ReplayedHistory initial_result;
 unsigned initial_prepares=0,initial_executes=0;
 check(rr64::prediction::evaluate_history_replay(initial,{},initial_result,
     [&](auto &resources,const auto &input){
         ++initial_prepares;check(input.command.sequence==2 && initial_executes==1);
         resources.memory()[102]=7;return true;
     },[&](auto &resources,const auto &input,auto &out){
         ++initial_executes;
         check(resources.memory()[102]==(input.command.sequence==1?0:7));
         // Corrected resource ownership changes at each private completion.
         // It must not be reused from the original history or the final image.
         rr64::engine::write_u16(resources.memory(),0x8009ce90,32-input.command.sequence);
         out={input.rules,input.posts,input.eject,input.items};return true;
     }));
 check(initial_prepares==1 && initial_executes==2 && m[102]==0);
 // The network ACK names a completed input; its host time may already lie
 // inside the next interval. Original full replays double-count that prefix.
 auto timed=initial;
 timed.baseline_elapsed_us=50000;
 for(auto &input:timed.steps){
  input.command.duration_us=20000;input.command.actions=rr64::authority::action_eject;
  input.command.buttons=0x8000;input.previous_buttons=0;
  input.timing.bits[0]=std::bit_cast<unsigned>(.02f);
  input.timing.bits[1]=std::bit_cast<unsigned>(.0004f);
  input.timing.bits[2]=std::bit_cast<unsigned>(1.2f);
  input.timing.bits[3]=std::bit_cast<unsigned>(50.f);
 }
 check(!rr64::prediction::align_replay_time(timed,49999) && timed.remaining_us.empty());
 check(rr64::prediction::align_replay_time(timed,60000));
 check(timed.remaining_us==std::vector<unsigned>({10000,20000}));
 unsigned executed=0;
 rr64::prediction::ReplayedHistory timed_result;
 check(rr64::prediction::evaluate_history_replay(timed,{},timed_result,
  [](auto&,const auto&){return true;},[&](auto&,const auto &input,auto &out){
   ++executed;
   check(input.replay_duration_us==(executed==1?10000u:20000u));
   check(input.items.elapsed_us==timed.baseline.items.elapsed_us+(executed==1?0u:10000u));
   if(executed==1){
    check(input.command.actions==0 && input.previous_buttons==input.command.buttons);
    check(std::abs(std::bit_cast<float>(input.timing.bits[0])-.01f)<1e-8f);
    check(std::abs(std::bit_cast<float>(input.timing.bits[1])-.0001f)<1e-8f);
    check(std::abs(std::bit_cast<float>(input.timing.bits[2])-.6f)<1e-6f);
    check(std::bit_cast<float>(input.timing.bits[3])==100.f);
   }else check(input.command.actions==rr64::authority::action_eject && input.timing==timed.steps[1].timing);
   check(input.passes==timed.steps[0].passes && input.timing.bits[4]==timed.steps[0].timing.bits[4]);
   out={input.rules,input.posts,input.eject,input.items};out.items.elapsed_us+=input.replay_duration_us;return true;
  }));
 check(executed==2 && timed_result.guest.frames()==3);
 check(rr64::prediction::align_replay_time(timed,75000));
 check(timed.remaining_us==std::vector<unsigned>({0,15000}));
 executed=0;
 check(rr64::prediction::evaluate_history_replay(timed,{},timed_result,
  [](auto&,const auto&){return true;},[&](auto&,const auto &input,auto &out){
   ++executed;check(input.command.sequence==2 && input.command.actions==0);
   check(std::abs(std::bit_cast<float>(input.timing.bits[0])-.015f)<1e-8f);
   out={input.rules,input.posts,input.eject,input.items};return true;
  }));
 check(executed==1 && timed_result.native.size()==3 && timed_result.resources.size()==3);
 check(rr64::prediction::align_replay_time(timed,95000));
 check(rr64::prediction::evaluate_history_replay(timed,{},timed_result,
  [](auto&,const auto&){std::abort();return false;},[](auto&,const auto&,auto&){std::abort();return false;}));
 check(timed_result.final_memory==timed.memory && timed_result.guest.frames()==3);
 // A partially entered manual-eject edge is suppressed, but its already
 // established native protection must follow the corrected host ejection.
 auto edges=timed;
 constexpr unsigned edge_actor=0x800d8570,edge_bike=0x80100000,edge_rider=0x80300000;
 auto *edge_memory=edges.memory.data();
 rr64::engine::write_u16(edge_memory,edge_actor+0x24,1);
 rr64::engine::write_u32(edge_memory,edge_actor+0xe0,edge_bike);rr64::engine::write_u32(edge_memory,edge_actor+0xe4,edge_rider);
 rr64::engine::write_u32(edge_memory,edge_bike+rr64::engine::bike::rider_pointer,edge_rider);
 rr64::engine::write_u32(edge_memory,edge_rider+rr64::engine::rider::bike_pointer,edge_bike);
 rr64::engine::write_u16(edge_memory,edge_bike+rr64::engine::bike::drive_control_lockout,1);
 rr64::engine::write_u16(edge_memory,edge_rider+rr64::engine::rider::ejected,1);
 rr64::engine::write_float(edge_memory,edge_bike+rr64::engine::bike::durability_current,80);
 rr64::engine::write_float(edge_memory,edge_bike+rr64::engine::bike::durability_capacity,100);
 for(auto &input:edges.steps)input.actor=edge_actor;
 edges.steps[1].command.actions=0;
 edges.completed_eject={{edge_bike,80,true},{edge_bike,80,true}};
 edges.baseline.eject={};edges.baseline.eject[13]={edge_bike+0x868,33,true};
 check(rr64::prediction::align_replay_time(edges,60000));
 rr64::authority::Outcome edge_outcome;edge_outcome.valid=1;
 const auto original_edge_memory=edges.memory;
 auto execute_edge=[](auto &resources,const auto &input,auto &out){
  auto protection=input.eject;
  check(rr64::authority::eject_step(resources.memory(),0,input.command.actions!=0,protection,
   [](unsigned){std::abort();return false;}));
  out={input.rules,input.posts,protection};return true;
 };
 rr64::prediction::ReplayedHistory edge_result;
 check(rr64::prediction::evaluate_history_replay(edges,{},edge_result,[](auto&,const auto&){return true;},execute_edge));
 check(!edge_result.native.back().eject[0].active); // Negative control reproduces the lost edge state.
 check(rr64::prediction::seed_entered_eject(edges,edge_actor,edge_outcome));
 check(edges.memory==original_edge_memory && edges.baseline.eject[0]==edges.completed_eject[0] && edges.baseline.eject[13].durability==33);
 check(rr64::prediction::evaluate_history_replay(edges,{},edge_result,[](auto&,const auto&){return true;},execute_edge));
 check(edge_result.native.back().eject[0].active && edge_result.native.back().eject[0].durability==80);
 auto elapsed_edges=edges;elapsed_edges.baseline.eject[0]={};
 check(rr64::prediction::align_replay_time(elapsed_edges,95000));
 check(rr64::prediction::seed_entered_eject(elapsed_edges,edge_actor,edge_outcome));
 check(rr64::prediction::evaluate_history_replay(elapsed_edges,{},edge_result,
  [](auto&,const auto&){std::abort();return false;},[](auto&,const auto&,auto&){std::abort();return false;}));
 check(edge_result.native.back().eject[0].active && edge_result.final_memory==elapsed_edges.memory);
 for(unsigned rejected=0;rejected<5;++rejected){
  auto rejected_edges=edges;auto outcome=edge_outcome;
  if(rejected==0)rr64::engine::write_u16(rejected_edges.memory.data(),edge_rider+rr64::engine::rider::ejected,0);
  if(rejected==1)rr64::engine::write_u16(rejected_edges.memory.data(),edge_bike+rr64::engine::bike::drive_control_lockout,0);
  if(rejected==2)rr64::engine::write_float(rejected_edges.memory.data(),edge_bike+rr64::engine::bike::durability_current,60);
  if(rejected==3)outcome.finished=1;
  if(rejected==4)rejected_edges.completed_eject[0].bike+=0x868;
  const auto unchanged=rejected_edges.memory;
  check(rr64::prediction::seed_entered_eject(rejected_edges,edge_actor,outcome));
  check(!rejected_edges.baseline.eject[0].active && rejected_edges.baseline.eject[13].durability==33 && rejected_edges.memory==unchanged);
 }
 auto future_edges=edges;future_edges.baseline.eject[0]={};
 check(rr64::prediction::align_replay_time(future_edges,50000));
 check(rr64::prediction::seed_entered_eject(future_edges,edge_actor,edge_outcome) && !future_edges.baseline.eject[0].active);
 // An acknowledged baseline can retain protection from a predicted eject the
 // host rejected. Validate it even with no replay steps or only future inputs.
 for(bool fully_acknowledged:{false,true})for(unsigned rejected=0;rejected<4;++rejected){
  auto baseline_edges=edges;
  if(fully_acknowledged){
   baseline_edges.steps.clear();baseline_edges.completed_eject.clear();baseline_edges.remaining_us.clear();
  }else check(rr64::prediction::align_replay_time(baseline_edges,50000));
  auto *memory=baseline_edges.memory.data();
  if(rejected==1)rr64::engine::write_u16(memory,edge_rider+rr64::engine::rider::ejected,0);
  if(rejected==2)rr64::engine::write_u16(memory,edge_bike+rr64::engine::bike::drive_control_lockout,0);
  if(rejected)rr64::engine::write_float(memory,edge_bike+rr64::engine::bike::durability_current,60);
  const auto unchanged=baseline_edges.memory;
  check(rr64::prediction::seed_entered_eject(baseline_edges,edge_actor,edge_outcome));
  check(baseline_edges.baseline.eject[0].active==(rejected==0));
  check(baseline_edges.baseline.eject[13].durability==33 && baseline_edges.memory==unchanged);
  auto protection=baseline_edges.baseline.eject;
  check(rr64::authority::eject_step(memory,0,false,protection,[](unsigned){std::abort();return false;}));
  float health=0;rr64::engine::read_float(memory,edge_bike+rr64::engine::bike::durability_current,health);
  check(health==(rejected?60:80));
 }
 // Validate time slicing independently of any live clock writes.
 auto slice=timed.steps[0].timing;const auto full=slice;
 check(rr64::authority::duration_us(slice)==20000);
 check(rr64::prediction::slice_timing(slice,20000,20000) && slice==full);
 check(!rr64::prediction::slice_timing(slice,20000,0) && slice==full);
 check(!rr64::prediction::slice_timing(slice,20000,20001) && slice==full);
 slice.bits[0]=std::bit_cast<unsigned>(.5f);check(rr64::authority::duration_us(slice)==0);
 check(initial_result.resources.size()==3);
 for(unsigned sequence=0;sequence<3;++sequence){
  check(initial_result.guest.restore(1,sequence,restored.data(),restored.size()));
  const auto expected=rr64::prediction::ResourceInventory::inspect(restored.data());
  const auto actual=initial_result.resources[sequence];
  check(expected.valid==actual.valid && expected.free==actual.free && expected.queued==actual.queued &&
        expected.priority==actual.priority && expected.unrepresented==actual.unrepresented);
  check(actual.valid && actual.unrepresented==(sequence==0?0u:sequence==1?0x80000000u:0xc0000000u));
 }
 auto stale=plan;stale.epoch+=1;
 unsigned approvals=0;
 const auto approve=[](void *p)noexcept{++*static_cast<unsigned*>(p);return true;};
 const auto inventories=result.resources;
 result.resources.pop_back();
 check(!rr64::prediction::commit_history_replay(plan,result,approve,&approvals) && approvals==0);
 result.resources=inventories;
 rr64::prediction::GuestJournal foreign;
 foreign.reset(2,1);check(foreign.capture(2,1,plan.memory.data(),plan.memory.size()));
 check(foreign.capture(2,2,plan.memory.data(),plan.memory.size()));
 result.guest.swap(foreign);
 check(!rr64::prediction::commit_history_replay(plan,result,approve,&approvals) && approvals==0);
 result.guest.swap(foreign);
 check(!rr64::prediction::commit_history_replay(stale,result,approve,&approvals) && approvals==0);
 check(!rr64::prediction::commit_history_replay(plan,result,[](void*)noexcept{return false;},nullptr));
 check(history->native.size()==3 && result.final_memory[100]==18);
 check(rr64::prediction::commit_history_replay(plan,result,approve,&approvals) && approvals==1);
 check(!rr64::prediction::commit_history_replay(plan,result,approve,&approvals) && approvals==1);
 check(history->native.size()==2 && history->native.back().rules.win_started==3);
 for(unsigned i=0;i<history->native.size();++i){
  const auto actual=history->native[i].resources;
  check(actual.valid==inventories[i].valid && actual.free==inventories[i].free &&
        actual.queued==inventories[i].queued && actual.priority==inventories[i].priority &&
        actual.unrepresented==inventories[i].unrepresented);
 }
 check(!history->guest.restore(1,0,restored.data(),restored.size()));
 check(history->guest.restore(1,2,restored.data(),restored.size()) && restored[100]==18);
 auto invalid=timing;invalid.bits[0]=0;const auto unchanged=m;
 check(!rr64::prediction::restore_timing(m.data(),invalid) && m==unchanged);
 recomp_context replay{};
 check(history->native.back().entry.restore(replay) && replay.f_odd==&replay.f0.u32h);
 rr64_prediction_capture_reset();check(!history);
 // Loading requests cross the rendering/simulation thread boundary in order.
 c.sequence=1;check(rr64_prediction_capture_before(m.data(),&c,&ctx));
 check(rr64_prediction_capture_after(m.data()));
 rr64::engine::write_u32(m.data(),0x800a7720,17);
 compiled_terrain=true;
 rr64_prediction_capture_streaming(m.data(),&ctx);
 rr64::engine::write_u32(m.data(),0x800a7720,23);
 compiled_terrain=false;
 rr64_prediction_capture_streaming(m.data(),&ctx);
 c.sequence=2;check(rr64_prediction_capture_before(m.data(),&c,&ctx));
 check(rr64_prediction_capture_after(m.data()));
 check(rr64::prediction::prepare_history_replay(1,1,std::span(&c,1),0x800d8570,plan));
 check(plan.streaming.size()==1 && plan.streaming[0].size()==2);
 check(!rr64::prediction::evaluate_history_replay(plan,{},result,
     [](auto&,const auto&){return true;},[](auto&,const auto&,auto&){return true;}));
 unsigned streamed=0;
 check(rr64::prediction::evaluate_history_replay(plan,{},result,
     [&](auto&,const auto&){check(streamed==2);return true;},
     [](auto&,const auto& input,auto& out){out={input.rules,input.posts,input.eject,input.items};return true;},
     [&](auto&,const auto& input,const auto& schedule){
         check(schedule.valid && input.words[1]==(streamed?23u:17u));
         check(input.bounded_terrain == (streamed == 0));
         ++streamed;return true;
     }));
 // Overflow cannot silently produce a partial, apparently valid correction.
 for(unsigned i=0;i<65;++i)rr64_prediction_capture_streaming(m.data(),&ctx);
 c.sequence=3;check(rr64_prediction_capture_before(m.data(),&c,&ctx));
 check(rr64_prediction_capture_after(m.data()));
 check(!rr64::prediction::prepare_history_replay(1,2,std::span(&c,1),0x800d8570,plan));
 rr64_prediction_capture_reset();check(!history && !streaming_mailbox.recording);
 std::puts("prediction baseline and completed input state retain separate guest/native histories; duplicate steps rejected");
}
