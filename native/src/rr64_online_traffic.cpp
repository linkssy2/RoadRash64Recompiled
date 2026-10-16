#include "rr64_traffic_roster.hpp"
#include "rr64_prediction_replay.hpp"
#include "recomp.h"
#include "rr64_netplay.hpp"
#include "rr64_traffic_sync_capture.hpp"
#include "rr64_traffic_reconcile.hpp"
#include <bit>

extern "C" {
void func_80046488(unsigned char*,recomp_context*);
void func_80077498(unsigned char*,recomp_context*);
void func_80047668(unsigned char*,recomp_context*);
void func_8006BA88(unsigned char*,recomp_context*);
void func_8006B9E8(unsigned char*,recomp_context*);
}
namespace {
using namespace rr64;
thread_local bool client_world=false;
thread_local world_sync::Snapshot authoritative{};
thread_local const world_sync::Traffic *creating=nullptr;
unsigned word(unsigned char *m,unsigned a) {unsigned v=0;engine::read_u32(m,a,v);return v;}
gpr guest(unsigned v) {return static_cast<gpr>(static_cast<std::int64_t>(static_cast<std::int32_t>(v)));}
bool client_session() {
    const auto s=netplay::get_status();
    return s.active && s.connected && !s.is_host && s.phase==netplay::Phase::Race;
}
unsigned entity_at(unsigned char *m,unsigned index) {return rr64::traffic::entity(m, index);}
void apply_motion(unsigned char *m,unsigned entity,const world_sync::Traffic &v) {
    if(!v.motion_valid || !world_sync::valid(v) || !engine::valid_guest_range(entity,0x360)) return;
    for(unsigned i=0;i<v.motion.size();++i) engine::write_float(m,entity+0xa8+i*4,v.motion[i]);
    for(unsigned i=0;i<v.directions.size();++i) engine::write_float(m,entity+0x310+i*4,v.directions[i]);
    engine::write_float(m,entity+0x24,v.road_distance);
    engine::write_u32(m,entity+0x3c,1);engine::write_u32(m,entity+0x40,1);
    // The local scene owns its allocation/index and uses its own frame clock.
    engine::write_u32(m,entity+0x32c,word(m,0x800a1830));
}
bool observe(unsigned char *m,world_sync::Snapshot &s) {
    return world_sync::capture_traffic(m,authoritative.round,authoritative.tick,s);
}
unsigned spawn(unsigned char *m,recomp_context &parent,const world_sync::Traffic &v) {
    if(word(m,0x800a6528)>=world_sync::capacity || !v.motion_valid) return 0;
    const unsigned sp=static_cast<unsigned>(parent.r29);
    if(sp<1024 || !engine::valid_guest_range(sp-1024,1024)) return 0;
    // Private call context and scratch above the child stack: generated guest
    // callees may change every caller-saved register without damaging the hook.
    recomp_context call=parent;call.r29=guest(sp-64);
    call.r5=guest(sp-16);call.r6=guest(sp-12);call.f12.fl=v.road_distance;
    func_8006B9E8(m,&call);
    if(!call.r2) return 0;
    const unsigned segment=word(m,sp-16),fraction=word(m,sp-12);
    func_80046488(m,&call);
    const unsigned entity=static_cast<unsigned>(call.r2);
    if(!engine::valid_guest_range(entity,0x360)) return 0;
    call.r4=guest(entity);call.r5=guest(v.id);call.r6=v.kind;
    func_80077498(m,&call);
    creating=&v;
    call.r4=guest(entity);call.r5=v.kind;
    func_80047668(m,&call);
    creating=nullptr;
    engine::write_u16(m,entity+0x334,1);engine::write_u16(m,entity+0x336,0);
    call.r4=guest(entity);call.r5=guest(segment);call.r6=guest(fraction);
    func_8006BA88(m,&call);
    apply_motion(m,entity,v);
    return entity;
}
}

// Before the original traffic loop: mark retirements, but let its original
// shared-model ownership transfer and scene unlink/free logic perform cleanup.
extern "C" void rr64_online_traffic_prepare(unsigned char *m) {
    if(rr64::prediction::active())return;
    client_world=false;
    if(!client_session() || !rr64::netplay::get_world_state(authoritative) ||
       !rr64::world_sync::valid(authoritative) ||
       authoritative.round!=rr64::netplay::get_status().game_setup.revision) return;
    for(const auto &v:authoritative.traffic) if(v.active && !v.motion_valid) return;
    rr64::world_sync::Snapshot local{};
    if(!observe(m,local)) return;
    rr64::world_sync::TrafficPlan plan{};
    if(!rr64::world_sync::plan_traffic(local,authoritative,plan)) return;
    client_world=true;
    for(unsigned i=0;i<plan.removes;++i)
        rr64::engine::write_u16(m,entity_at(m,plan.remove[i])+0x334,0);
    for(unsigned i=0;i<plan.updates;++i) {
        const auto &u=plan.update[i];apply_motion(m,entity_at(m,u.local),authoritative.traffic[u.remote]);
    }
}
// Re-observe after compaction; pre-cleanup indices are no longer valid.
extern "C" void rr64_online_traffic_finish(unsigned char *m,void *context) {
    if(rr64::prediction::active())return;
    if(!client_world || !context || !client_session() ||
       authoritative.round!=rr64::netplay::get_status().game_setup.revision) return;
    rr64::world_sync::Snapshot local{};rr64::world_sync::TrafficPlan plan{};
    if(!observe(m,local) || !rr64::world_sync::plan_traffic(local,authoritative,plan)) return;
    auto &ctx=*static_cast<recomp_context*>(context);
    for(unsigned i=0;i<plan.creates;++i) spawn(m,ctx,authoritative.traffic[plan.create[i]]);
}
extern "C" int rr64_online_traffic_owned() {
    if(rr64::prediction::active())return 0;
    // A new race may run a spawn hook before its first traffic prepare pass.
    // Never let a completed race suppress the new round's native initialization.
    return client_world && client_session() &&
        authoritative.round==rr64::netplay::get_status().game_setup.revision;
}
extern "C" void rr64_online_traffic_model(void *context) {
    if(rr64::prediction::active())return;
    if(!creating || !context) return;
    auto &ctx=*static_cast<recomp_context*>(context);
    ctx.r16=creating->model;ctx.r2=creating->kind;
}
