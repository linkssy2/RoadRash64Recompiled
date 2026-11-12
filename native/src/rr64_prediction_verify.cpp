#include "rr64_prediction_reconcile.hpp"
#include "rr64_prediction_verify_fields.hpp"
#include "rr64_prediction_verify_inputs.hpp"
#include "rr64_prediction_terrain_probe.hpp"
#include "rr64_prediction_dependency_probe.hpp"
#include "rr64_traffic_comparison.hpp"
#include "rr64_prediction_case.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/rr64_snapshot_gate.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <atomic>
#include <future>

namespace {
// Private comparisons can capture an all-AI demo; online callers retain the
// default requirement for a human roster. No actor flags are rewritten.
bool capture_verification_movement(unsigned char* memory,const rr64::authority::Stamp& stamp,
    unsigned local,unsigned humans,bool mapped,rr64::netplay::AuthorityFrame& out,const char** failure){
    return rr64::prediction::capture_prediction_movement(memory,stamp,local,humans,mapped,out,failure,humans==0 && !mapped);
}
struct RandomCall { unsigned site=0,seed=0; };
struct RandomCalls {
    std::array<RandomCall,256> calls{};
    unsigned count=0;
    void add(unsigned site,unsigned seed){if(count<calls.size())calls[count]={site,seed};++count;}
};
struct Verification {
    rr64::netplay::AuthorityFrame expected;
    rr64::prediction::FrameOutput native;
    unsigned local=0,humans=0,stage=0,mode=0;
    unsigned depth=0,bike=0;
    std::unique_ptr<rr64::prediction::Resources> resources;
    bool mapped=false;
    std::vector<rr64::prediction::EntryDifferences> entry_differences;
    rr64::prediction::TerrainProbe terrain;
    rr64::prediction::DependencyProbe dependencies;
    std::array<unsigned,2> rng_before_order{},rng_after_order{},rng_before_update{};
    RandomCalls live_random,private_random;
    std::unique_ptr<rr64::prediction::ReplayCase> reproducible;
};
std::mutex case_mutex;
std::array<std::future<void>,5> case_writers;
std::atomic_uint case_mask=0;
std::atomic_bool restart_requested=false;
bool case_capture_enabled(){
    static const bool enabled=[] {const char* p=std::getenv("RR64_VERIFY_CASE_DIR");return p && *p;}();
    return enabled;
}
void persist_private_case(std::unique_ptr<rr64::prediction::ReplayCase> c){
    try{
        const std::filesystem::path dir=std::getenv("RR64_VERIFY_CASE_DIR");
        std::filesystem::create_directories(dir);
        const auto path=dir/("case-"+std::to_string(c->header.category)+"-"+std::to_string(c->header.attempt)+".rrcase");
        if(std::filesystem::exists(path))return;
        std::fprintf(stderr,"[RR64-REPLAY-CASE] category=%u attempt=%u saved=%u\n",c->header.category,c->header.attempt,
            rr64::prediction::write_case(path,*c));
    }catch(const std::exception& e){std::fprintf(stderr,"[RR64-REPLAY-CASE] save failed: %s\n",e.what());}
}
thread_local Verification* recording_private_random=nullptr;
thread_local std::unique_ptr<Verification> pending;
// Simulation and rendering use separate native workers. The retained private
// world must be handed between them; thread-local ownership hides it from the
// streaming callback. Serialize ownership and streaming, never live memory.
std::mutex continuation_mutex;
std::unique_ptr<Verification> continuation;
thread_local unsigned attempts=0,completed=0;
// Independent budgets keep menus/ordinary riding from consuming crash coverage.
thread_local unsigned stage_attempts[3]{},stage_completed[3]{};
thread_local std::chrono::steady_clock::time_point last[3]{};
thread_local std::chrono::steady_clock::time_point run_start{},budget_start{};
thread_local bool duration_reported=false;
thread_local bool saw_detached=false,recovery_pending=false;
thread_local unsigned tracked_bike=0,tracked_mode=0;
constexpr const char *stage_names[]{"mounted","detached","remounted"};
std::array<unsigned,2> rng_pair(unsigned char* live,unsigned char* replay){
    std::array<unsigned,2> values{};
    rr64::engine::read_u32(live,0x8009dc30,values[0]);
    rr64::engine::read_u32(replay,0x8009dc30,values[1]);
    return values;
}
bool enabled(){
    static const bool value=[] {const char *p=std::getenv("RR64_VERIFY_NATIVE_REPLAY");return p && *p=='1';}();
    return value;
}
bool consolidated(){
    static const bool value=[] {const char *p=std::getenv("RR64_VERIFY_CONSOLIDATED");return p && *p=='1';}();
    return value;
}
bool ai_verification(){
    static const bool value=[] {const char *p=std::getenv("RR64_VERIFY_AI_REPLAY");return p && *p=='1';}();
    return value;
}
}
// Private diagnostic only, disabled by default. Executes a bounded disposable
extern "C" void rr64_prediction_request_verification_restart(){
    if(enabled())restart_requested.store(true);
}
// Restart only at a fresh simulation boundary. Preserve cumulative sample IDs,
// bounded saved-case slots and all live game state; never splice old replay history.
// Private diagnostic only, disabled by default. Executes a bounded disposable
extern "C" void rr64_prediction_capture_streaming(unsigned char*,void*);
extern "C" void rr64_prediction_verify_streaming(unsigned char *m,void *raw){
    using namespace rr64::prediction;
    rr64_prediction_capture_streaming(m,raw);
    if(active() || !enabled() || !m || !raw)return;
    std::lock_guard lock(continuation_mutex);
    if(!continuation)return;
    StreamingInput input;
    if(!input.capture(m,*static_cast<recomp_context*>(raw))){
        std::fprintf(stderr,"[RR64-REPLAY-VERIFY] streaming capture failed\n");
        continuation.reset();return;
    }
    const auto schedule=ResourceInventory::inspect(m);
    const auto private_schedule=ResourceInventory::inspect(continuation->resources->memory());
    if(!replay_native_streaming(*continuation->resources,input,schedule)){
        std::fprintf(stderr,"[RR64-REPLAY-VERIFY] streaming replay failed reason=%s\n",last_native_replay_error());
        std::fprintf(stderr,"[RR64-REPLAY-STREAM-SCHEDULE] live-valid=%u private-valid=%u free=%08x/%08x queued=%08x/%08x priority=%08x/%08x held=%08x/%08x\n",
            schedule.valid,private_schedule.valid,schedule.free,private_schedule.free,
            schedule.queued,private_schedule.queued,schedule.priority,private_schedule.priority,
            schedule.unrepresented,private_schedule.unrepresented);
        continuation.reset();return;
    }
    std::fprintf(stderr,"[RR64-REPLAY-STREAM] depth=%u completed=1\n",continuation->depth);
}
// Bounded call-site evidence only while a sampled native update is running.
// Never changes the RNG or substitutes a live random result into replay.
extern "C" void rr64_prediction_verify_random(unsigned char* m,unsigned site){
    auto* check=rr64::prediction::active()?recording_private_random:pending.get();
    if(!check)return;
    unsigned seed=0;
    if(!rr64::engine::read_u32(m,0x8009dc30,seed))return;
    (rr64::prediction::active()?check->private_random:check->live_random).add(site,seed);
}
// Private diagnostic only, disabled by default. Executes a bounded disposable
// copy, then compares it with the real native update at the same boundaries.
// It never installs its result, consumes input tickets or writes a memory dump.
extern "C" void rr64_prediction_verify_before(unsigned char *m,void *raw){
    using namespace rr64;
    if(prediction::active() || !enabled() || !m || !raw || pending)return;
    // Move ownership first: any skipped/invalid update discards the sequence,
    // rather than resuming a private world that missed a live simulation step.
    auto previous=[&]{std::lock_guard lock(continuation_mutex);return std::move(continuation);}();
    const auto now=std::chrono::steady_clock::now();
    if(restart_requested.exchange(false)){
        previous.reset();
        run_start=budget_start=now;duration_reported=false;
        saw_detached=recovery_pending=false;tracked_bike=tracked_mode=0;
        for(unsigned i=0;i<3;++i){stage_attempts[i]=stage_completed[i]=0;last[i]={};}
        std::fprintf(stderr,"[RR64-REPLAY-COVERAGE] sampling-restarted=1 after-attempt=%u completed=%u duration-limit-seconds=600\n",attempts,completed);
    }
    unsigned mode=0,count=0;std::uint16_t suspended=0;
    if(!engine::read_u32(m,engine::globals::main_mode,mode) || !engine::is_live_race_mode(mode) ||
       !engine::read_u32(m,0x800a656c,count) || count<1 || count>14 ||
       !engine::read_u16(m,0x800a2192,suspended) || suspended)return;
    const auto status=netplay::get_status();
    if(status.authoritative)return; // don't add verification cost to live reconciliation
    const unsigned local=status.active?status.local_slot:0;
    const bool mapped=status.active && status.replicated_riders;
    const unsigned guest=online_flow::mapped_slot(local,local,mapped);
    if(local>=14 || guest>=4 || guest>=count)return;
    const unsigned actor=0x800d8570+guest*0x118;
    std::uint16_t active=0,ai=0,buttons=0,changed=0;std::uint8_t x=0,y=0;
    if(!engine::read_u16(m,actor+0x24,active) || !active ||
       !engine::read_u16(m,actor+0x26,ai) || (ai!=0)!=ai_verification())return;
    if(ai_verification() && status.active)return;
    unsigned bike=0,rider=0,back=0;
    std::uint16_t attached=0,mounted=0;
    if(!engine::read_u32(m,actor+0xe0,bike) || !engine::valid_guest_range(bike,engine::bike::stride) ||
       !engine::read_u32(m,actor+0xe4,rider) || !engine::valid_guest_range(rider,engine::rider::stride) ||
       !engine::read_u32(m,bike+engine::bike::rider_pointer,back) || back!=rider ||
       !engine::read_u32(m,rider+engine::rider::bike_pointer,back) || back!=bike ||
       !engine::read_u16(m,bike+engine::bike::rider_attached,attached) ||
       !engine::read_u16(m,rider+engine::rider::bike_attached,mounted))return;
    if(tracked_bike!=bike || tracked_mode!=mode){
        tracked_bike=bike;tracked_mode=mode;saw_detached=false;recovery_pending=false;
    }
    // Observe every update, even when a category is exhausted or throttled.
    const bool detached=!attached || !mounted;
    if(detached){saw_detached=true;recovery_pending=false;}
    else if(saw_detached){recovery_pending=true;saw_detached=false;}
    const unsigned stage=detached?1:recovery_pending?2:0;
    // A consolidated run reserves fresh sampling time throughout ten minutes,
    // rather than exhausting every riding sample before traffic appears.
    if(consolidated()){
        if(run_start==decltype(run_start){})run_start=budget_start=now;
        if(now-run_start>=std::chrono::minutes(10)){
            if(!duration_reported){
                std::fprintf(stderr,"[RR64-REPLAY-COVERAGE] sampling-ended=1 duration-limit-seconds=600\n");
                duration_reported=true;
            }
            return;
        }
        if(now-budget_start>=std::chrono::minutes(1)){
            for(unsigned i=0;i<3;++i){stage_attempts[i]=0;stage_completed[i]=0;}
            budget_start=now;
        }
    }
    if(previous && (previous->bike!=bike || previous->mode!=mode ||
                   previous->local!=local || previous->mapped!=mapped))previous.reset();
    if(!previous && (stage_attempts[stage]>=20 || (!consolidated() && stage_completed[stage]>=4) ||
       now-last[stage]<std::chrono::seconds(3)))return;
    if(!previous){last[stage]=now;++stage_attempts[stage];}
    std::fprintf(stderr,"[RR64-REPLAY-VERIFY] sample stage=%s mode=%x bike-attached=%u rider-attached=%u\n",
        stage_names[stage],mode,attached,mounted);
    unsigned humans=0;
    for(unsigned slot=0;slot<count;++slot){
        engine::read_u16(m,0x800d8570+slot*0x118+0x26,ai);
        if(!ai)humans|=1u<<online_flow::mapped_slot(slot,local,mapped);
    }
    if(previous && previous->humans!=humans)return;
    if(previous){
        netplay::AuthorityFrame entry;
        const char *failure=nullptr;
        if(!capture_verification_movement(m,{1,1,{}},local,humans,mapped,entry,&failure)){
            std::fprintf(stderr,"[RR64-REPLAY-VERIFY] entry capture failed reason=%s\n",failure);return;
        }
        // The preceding finish comparison passed. Differences here therefore
        // arose between simulation calls, outside the private update prefix.
        for(unsigned slot=0;slot<14;++slot)
            prediction::report_replay_fields("between-updates",slot,entry,previous->expected);
    }
    if(!engine::read_u16(m,engine::globals::controller_buttons+guest*2,buttons) ||
       !engine::read_u16(m,engine::globals::controller_changed_buttons+guest*2,changed) ||
       !engine::read_u8(m,engine::globals::controller_stick_x+guest,x) ||
       !engine::read_u8(m,engine::globals::controller_stick_y+guest,y))return;
    // AI demo samples retain original AI decisions; controller state is irrelevant.
    if(ai_verification()){buttons=changed=0;x=y=0;}
    ++attempts;
    std::fprintf(stderr,"[RR64-REPLAY-PATH] attempt=%u control=%s\n",attempts,ai_verification()?"ai":"human");
    unsigned traffic_count=0;engine::read_u32(m,0x800a6528,traffic_count);
    std::fprintf(stderr,"[RR64-REPLAY-COVERAGE] attempt=%u stage=%s elapsed-seconds=%lld buttons=%04x changed=%04x traffic-roster=%u mode=%x riders=%u\n",
        attempts,stage_names[stage],static_cast<long long>(consolidated()?std::chrono::duration_cast<std::chrono::seconds>(now-run_start).count():0),
        buttons,changed,traffic_count,mode,count);
    try {
        prediction::FrameInput input;
        input.actor=actor;input.command={1,1,static_cast<std::uint16_t>(buttons&~0x1000u),static_cast<std::int8_t>(x),static_cast<std::int8_t>(y)};
        input.previous_buttons=buttons^changed;
        if(!prediction::capture_visibility(m,input.visibility) || !prediction::capture_terrain_availability(m,input.terrain))return;
        input.session=prediction::capture_session_rules(status);
        if(!input.entry.capture(*static_cast<recomp_context*>(raw)) || !prediction::capture_timing(m,input.timing) ||
           !prediction::capture_update_counters(m,input.counters) ||
           !prediction::capture_cop_rules(input.rules) || !prediction::capture_cop_posts(input.posts) ||
           !prediction::capture_manual_eject(input.eject) || !prediction::capture_item_state(input.items))return;
        input.passes=input.timing.substeps;
        auto check=std::make_unique<Verification>();check->local=local;check->humans=humans;check->mapped=mapped;
        check->stage=previous?previous->stage:stage;check->mode=mode;check->bike=bike;
        check->depth=previous?previous->depth+1:1;
        if(previous){
            check->resources=std::move(previous->resources);
            check->rng_before_order=rng_pair(m,check->resources->memory());
            input.rules=previous->native.rules;input.posts=previous->native.posts;input.eject=previous->native.eject;
            input.items=previous->native.items;
            if(!prediction::replay_native_order(*check->resources,input)){
                std::fprintf(stderr,"[RR64-REPLAY-VERIFY] order replay failed depth=%u reason=%s\n",
                    check->depth,prediction::last_native_replay_error());return;
            }
            netplay::AuthorityFrame live_entry,private_entry;
            const char *failure=nullptr;
            if(!capture_verification_movement(m,{1,1,{}},local,humans,mapped,live_entry,&failure)){
                std::fprintf(stderr,"[RR64-REPLAY-VERIFY] live-order capture failed reason=%s\n",failure);return;
            }
            check->rng_after_order=rng_pair(m,check->resources->memory());
            if(!capture_verification_movement(check->resources->memory(),{1,1,{}},local,humans,mapped,private_entry,&failure)){
                std::fprintf(stderr,"[RR64-REPLAY-VERIFY] private-order capture failed reason=%s\n",failure);return;
            }
            for(unsigned slot=0;slot<14;++slot)
                prediction::report_replay_fields("after-private-order",slot,live_entry,private_entry);
        }else{
            std::vector<unsigned char> image(engine::kRdramSize);
            if(!ultramodern::rr64::copy_guest_snapshot(m,image.data(),image.size()))return;
            const auto inventory=prediction::ResourceInventory::inspect(image.data());
            if(!inventory.can_start_worker()){
                std::fprintf(stderr,"[RR64-REPLAY-VERIFY] attempt=%u resource-baseline-unavailable valid=%u held=%08x\n",attempts,inventory.valid,inventory.unrepresented);return;
            }
            check->resources=std::make_unique<prediction::Resources>(std::move(image),recomp::get_rom());
        }
        // Save the exact command passed to replay, including continuation depth.
        input.command.sequence=check->depth;
        if(case_capture_enabled() && case_mask.load()!=31u){
            auto c=std::make_unique<prediction::ReplayCase>();
            c->header.local=local;c->header.humans=humans;c->header.mapped=mapped;c->header.attempt=attempts;
            static const auto rom_hash=prediction::case_rom_hash(recomp::get_rom());c->header.rom_hash=rom_hash;
            c->input=input;c->before_private=check->resources->image();c->before_live.resize(engine::kRdramSize);
            if(ultramodern::rr64::copy_guest_snapshot(m,c->before_live.data(),c->before_live.size()))check->reproducible=std::move(c);
        }
        check->rng_before_update=rng_pair(m,check->resources->memory());
        check->entry_differences=prediction::compare_actor_entries(m,check->resources->memory(),count);
        check->terrain.capture(m,check->resources->memory());
        prediction::TerrainProbeScope terrain_scope(check->terrain);
        check->dependencies.capture(m,check->resources->memory(),static_cast<unsigned>(static_cast<recomp_context*>(raw)->r29));
        prediction::DependencyProbeScope dependency_scope(check->dependencies);
        struct RandomRecording {
            explicit RandomRecording(Verification* check){recording_private_random=check;}
            ~RandomRecording(){recording_private_random=nullptr;}
        } random_recording(check.get());
        const bool executed=prediction::replay_native_frame(*check->resources,input,check->native);
        const char *capture_failure=nullptr;
        if(!executed || !capture_verification_movement(check->resources->memory(),{1,1,{}},local,humans,mapped,check->expected,&capture_failure)){
            if(!executed){
                // Rejected references are evidence, not permission to overwrite
                // newly allocated objects. Record bounded identities for diagnosis.
                auto* private_memory=check->resources->memory();
                for(unsigned i=0;i<input.visibility.count && i<14;++i){
                    const auto& e=input.visibility.entries[i];unsigned b=0,o=0,d=0;
                    engine::read_u32(private_memory,e.actor+0xe0,b);
                    engine::read_u32(private_memory,b+8,o);
                    engine::read_u32(private_memory,o+0x28,d);
                    if(b!=e.bike || o!=e.object || d!=e.descriptor)
                        std::fprintf(stderr,"[RR64-REPLAY-VISIBILITY] attempt=%u actor=%08x bike=%08x/%08x object=%08x/%08x descriptor=%08x/%08x\n",attempts,e.actor,e.bike,b,e.object,o,e.descriptor,d);
                }
            }
            std::fprintf(stderr,"[RR64-REPLAY-VERIFY] attempt=%u depth=%u private-execution=%u reason=%s\n",attempts,check->depth,executed,
                executed?capture_failure:prediction::last_native_replay_error());return;
        }
        pending=std::move(check);
    }catch(const std::bad_alloc&){std::fprintf(stderr,"[RR64-REPLAY-VERIFY] allocation failed\n");}
     catch(const prediction::Resources::Invalid&){std::fprintf(stderr,"[RR64-REPLAY-VERIFY] invalid private resources\n");}
}
extern "C" void rr64_prediction_verify_after(unsigned char *m){
    using namespace rr64;
    if(prediction::active() || !pending)return;
    auto check=std::move(pending);
    const auto rng_after=rng_pair(m,check->resources->memory());
    const auto &a=check->live_random;const auto &b=check->private_random;
    unsigned first=0,limit=std::min<unsigned>(256,std::min(a.count,b.count));
    while(first<limit && a.calls[first].site==b.calls[first].site && a.calls[first].seed==b.calls[first].seed)++first;
    if(first<limit || a.count!=b.count || a.count>256 || b.count>256){
        std::fprintf(stderr,"[RR64-REPLAY-RNG-CALLS] attempt=%u live-count=%u private-count=%u first=%u truncated=%u\n",attempts,a.count,b.count,first,a.count>256 || b.count>256);
        for(unsigned i=first;i<std::min<unsigned>(256,first+4) && (i<a.count || i<b.count);++i)
            std::fprintf(stderr,"[RR64-REPLAY-RNG-CALL] index=%u live-site=%08x private-site=%08x live-seed=%08x private-seed=%08x\n",
                i,i<a.count?a.calls[i].site:0,i<b.count?b.calls[i].site:0,i<a.count?a.calls[i].seed:0,i<b.count?b.calls[i].seed:0);
    }
    if(rng_after[0]!=rng_after[1] || check->rng_before_update[0]!=check->rng_before_update[1])
        std::fprintf(stderr,"[RR64-REPLAY-RNG] attempt=%u depth=%u before-order=%08x/%08x after-order=%08x/%08x before-update=%08x/%08x after-update=%08x/%08x\n",
            attempts,check->depth,check->rng_before_order[0],check->rng_before_order[1],
            check->rng_after_order[0],check->rng_after_order[1],check->rng_before_update[0],check->rng_before_update[1],rng_after[0],rng_after[1]);
    netplay::AuthorityFrame actual;
    const char *capture_failure=nullptr;
    if(!capture_verification_movement(m,{1,1,{}},check->local,check->humans,check->mapped,actual,&capture_failure)){
        std::fprintf(stderr,"[RR64-REPLAY-VERIFY] actual capture failed reason=%s\n",capture_failure);return;
    }
    unsigned different=0;float error=0;
    for(unsigned slot=0;slot<14;++slot){
        const auto &a=actual.riders[slot],&b=check->expected.riders[slot];
        if(a.active!=b.active || actual.outcomes[slot]!=check->expected.outcomes[slot] ||
           actual.dynamics[slot]!=check->expected.dynamics[slot]){
            ++different;
            prediction::report_replay_fields("after-update",slot,actual,check->expected);
            std::fprintf(stderr,"[RR64-REPLAY-VERIFY] mismatch slot=%u local=%u depth=%u active=%u/%u outcomes-equal=%u dynamics-equal=%u attached=%u/%u\n",
                slot,check->local,check->depth,a.active,b.active,
                actual.outcomes[slot]==check->expected.outcomes[slot],actual.dynamics[slot]==check->expected.dynamics[slot],
                a.root.bike_attached,b.root.bike_attached);
        }
        if(a.active && b.active)error=std::max({error,std::abs(a.position_x-b.position_x),std::abs(a.position_y-b.position_y),std::abs(a.position_z-b.position_z),
            std::abs(a.rider_x-b.rider_x),std::abs(a.rider_y-b.rider_y),std::abs(a.rider_z-b.rider_z)});
    }
    const auto traffic=world_sync::compare_traffic(actual.traffic,check->expected.traffic);
    unsigned different_traffic=traffic.valid?traffic.count:world_sync::capacity*2,active_traffic=0;
    for(const auto &car:actual.traffic)active_traffic+=car.active!=0;
    for(unsigned i=0;i<traffic.count && i<4;++i){
        const auto [slot,replay_slot]=traffic.differences[i];
        const world_sync::Traffic absent{};
        const auto &a=slot<world_sync::capacity?actual.traffic[slot]:absent;
        const auto &b=replay_slot<world_sync::capacity?check->expected.traffic[replay_slot]:absent;
        std::fprintf(stderr,
            "[RR64-REPLAY-TRAFFIC] slot=%u replay-slot=%u active=%u/%u id=%u/%u model=%u/%u motion-equal=%u directions-equal=%u road-distance=%g/%g\n",
            slot,replay_slot,a.active,b.active,a.id,b.id,a.model,b.model,a.motion==b.motion,
            a.directions==b.directions,a.road_distance,b.road_distance);
    }
    prediction::FrameOutput native;
    const bool native_captured=prediction::capture_cop_rules(native.rules) && prediction::capture_cop_posts(native.posts) && prediction::capture_manual_eject(native.eject) && prediction::capture_item_state(native.items);
    const bool native_equal=native_captured && native.rules==check->native.rules && native.posts==check->native.posts && native.eject==check->native.eject && native.items==check->native.items;
    if(!native_equal){
        std::fprintf(stderr,"[RR64-REPLAY-NATIVE] rules-equal=%u\n",native.rules==check->native.rules);
        for(unsigned slot=0;slot<14;++slot){
            if(native.posts[slot]!=check->native.posts[slot] || native.eject[slot]!=check->native.eject[slot])
                std::fprintf(stderr,"[RR64-REPLAY-NATIVE] slot=%u posts-equal=%u eject-equal=%u eject-active=%u/%u eject-durability=%g/%g\n",
                    slot,native.posts[slot]==check->native.posts[slot],native.eject[slot]==check->native.eject[slot],
                    native.eject[slot].active,check->native.eject[slot].active,
                    native.eject[slot].durability,check->native.eject[slot].durability);
        }
    }
    ++completed;
    // Keep one control and the first example of each failure family. Bounded
    // background writers make cases available during play without game-thread I/O.
    unsigned category=4;
    if(different_traffic)category=2;
    else if(different || error!=0)category=1;
    else if(!native_equal)category=3;
    else if(rng_after[0]!=rng_after[1] && check->rng_before_update[0]==check->rng_before_update[1])category=0;
    if(check->reproducible && !(case_mask.load()&(1u<<category))){
        auto &c=*check->reproducible;c.header.category=category;c.header.native_valid=native_captured;c.expected_native=native;c.after_live.resize(engine::kRdramSize);
        if(ultramodern::rr64::copy_guest_snapshot(m,c.after_live.data(),c.after_live.size())){
            std::lock_guard lock(case_mutex);
            if(!(case_mask.load()&(1u<<category))){
                try{
                    case_writers[category]=std::async(std::launch::async,persist_private_case,std::move(check->reproducible));
                    case_mask.fetch_or(1u<<category);
                }catch(const std::exception& e){std::fprintf(stderr,"[RR64-REPLAY-CASE] writer unavailable: %s\n",e.what());}
            }
        }
    }
    const bool end=check->depth>=8 || different || different_traffic || error!=0 || !native_equal;
    if(different || different_traffic || error!=0 || !native_equal){
        prediction::report_actor_entries(check->entry_differences);
        check->terrain.report();
        check->dependencies.report();
    }
    if(end){++stage_completed[check->stage];if(check->stage==2)recovery_pending=false;}
    std::fprintf(stderr,"[RR64-REPLAY-VERIFY] completed=%u attempt=%u stage=%s sequences-ended=%u depth=%u mode=%x different-actors=%u max-root-error=%g native-equal=%u different-traffic=%u active-traffic=%u\n",
        completed,attempts,stage_names[check->stage],stage_completed[check->stage],check->depth,check->mode,different,error,native_equal,different_traffic,active_traffic);
    if(!end){std::lock_guard lock(continuation_mutex);continuation=std::move(check);}
}

extern "C" void rr64_prediction_flush_cases(){
    if(!case_capture_enabled())return;
    std::lock_guard lock(case_mutex);
    for(auto &writer:case_writers)if(writer.valid())writer.get();
}

// Runs before SDL, ROM boot, renderer or network initialization. This is an
// offline evaluator of a private case, not another interactive game session.
extern "C" int rr64_prediction_run_case(const char* path,const char* rom_path,int live_baseline){
    using namespace rr64;
    try{
        prediction::ReplayCase c;
        if(!prediction::read_case(path,c)){std::fprintf(stderr,"Invalid or incompatible replay case\n");return 1;}
        const auto size=std::filesystem::file_size(rom_path);
        if(!size || size>64u*1024*1024)return 1;
        std::vector<unsigned char> rom(size);std::ifstream in(rom_path,std::ios::binary);
        in.read(reinterpret_cast<char*>(rom.data()),rom.size());
        if(!in || prediction::case_rom_hash(rom)!=c.header.rom_hash){std::fprintf(stderr,"ROM identity mismatch\n");return 1;}
        auto diagnostic_live=c.before_live;
        prediction::Resources resources(live_baseline?std::move(c.before_live):std::move(c.before_private),rom);
        Verification trace;
        recomp_context diagnostic_context{};if(!c.input.entry.restore(diagnostic_context))return 1;
        trace.dependencies.capture(diagnostic_live.data(),resources.memory(),static_cast<unsigned>(diagnostic_context.r29));
        prediction::DependencyProbeScope dependency_scope(trace.dependencies);
        struct Recording {Recording(Verification* t){recording_private_random=t;}~Recording(){recording_private_random=nullptr;}} recording(&trace);
        prediction::FrameOutput native;
        if(!prediction::replay_native_frame(resources,c.input,native)){
            std::fprintf(stderr,"Offline replay rejected: %s\n",prediction::last_native_replay_error());return 1;
        }
        netplay::AuthorityFrame expected,actual;const char* reason=nullptr;
        if(!capture_verification_movement(c.after_live.data(),{1,1,{}},c.header.local,c.header.humans,c.header.mapped,expected,&reason) ||
           !capture_verification_movement(resources.memory(),{1,1,{}},c.header.local,c.header.humans,c.header.mapped,actual,&reason)){
            std::fprintf(stderr,"Offline capture rejected: %s\n",reason?reason:"unknown");return 1;
        }
        unsigned differences=0;
        for(unsigned slot=0;slot<14;++slot){
            const auto &a=expected.riders[slot],&b=actual.riders[slot];
            if(a.active!=b.active || expected.outcomes[slot]!=actual.outcomes[slot] || expected.dynamics[slot]!=actual.dynamics[slot] ||
               (a.active && b.active && (a.position_x!=b.position_x || a.position_y!=b.position_y || a.position_z!=b.position_z ||
                a.rider_x!=b.rider_x || a.rider_y!=b.rider_y || a.rider_z!=b.rider_z))){
                ++differences;prediction::report_replay_fields("offline",slot,expected,actual);
            }
        }
        const auto cars=world_sync::compare_traffic(expected.traffic,actual.traffic);
        if(differences || cars.count)trace.dependencies.report();
        const auto rng=rng_pair(c.after_live.data(),resources.memory());
        const bool native_equal=c.header.native_valid && native.rules==c.expected_native.rules && native.posts==c.expected_native.posts && native.eject==c.expected_native.eject && native.items==c.expected_native.items;
        std::fprintf(stderr,"[RR64-OFFLINE-NATIVE] valid=%u equal=%u\n",c.header.native_valid,native_equal);
        std::fprintf(stderr,"[RR64-OFFLINE] attempt=%u baseline=%s actors=%u traffic=%u rng=%08x/%08x random-calls=%u\n",
            c.header.attempt,live_baseline?"live":"private",differences,cars.count,rng[0],rng[1],trace.private_random.count);
        for(unsigned i=0;i<std::min<unsigned>(256,trace.private_random.count);++i)
            std::fprintf(stderr,"[RR64-OFFLINE-RNG] index=%u site=%08x seed=%08x\n",i,trace.private_random.calls[i].site,trace.private_random.calls[i].seed);
        return differences || !cars.valid || cars.count || rng[0]!=rng[1] || !native_equal?2:0;
    }catch(const std::exception& e){std::fprintf(stderr,"Offline case error: %s\n",e.what());return 1;}
}
