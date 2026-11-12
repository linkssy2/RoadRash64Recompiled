#include "rr64_prediction_replay.hpp"
#include "rr64_prediction_controls.hpp"
#include "rr64_authoritative_native.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_mk64_items.hpp"
#endif
#include "rr64_online_terrain.hpp"
#include "rr64_prediction_timing.hpp"

extern "C" int rr64_online_authority_capture(unsigned char*,const void*);
extern "C" void rr64_prediction_capture_reset();
extern "C" int rr64_prediction_capture_before(unsigned char*,const void*,const void*);
extern "C" int rr64_prediction_capture_after(unsigned char*);
extern "C" void rr64_prediction_reconcile_step(unsigned char*);
extern "C" void rr64_prediction_verify_before(unsigned char*,void*);
extern "C" void rr64_prediction_verify_after(unsigned char*);
extern "C" unsigned rr64_authority_take_actions(unsigned);
extern "C" void rr64_authority_reset_eject();
extern "C" int rr64_authority_eject_step(unsigned char*,void*,unsigned,unsigned);
namespace {
thread_local bool client_pending=false;
thread_local rr64::authority::Step staged{};
thread_local std::array<std::uint16_t,14> previous{};
thread_local unsigned step_round=0,humans=0;
thread_local unsigned required=0,translated=0;
thread_local bool running=false,translating=false,failed=false;
unsigned slot_of(unsigned actor) {
    constexpr unsigned base=0x800d8570u,stride=0x118u;
    return actor>=base && (actor-base)%stride==0 ? (actor-base)/stride : 14;
}
bool owns(unsigned actor) {
    const unsigned slot=slot_of(actor);
    return running && slot<14 && (humans&(1u<<slot));
}
}

// Hooks remain dormant until the integrated authority lifecycle enables the
// mode. Never infer authority merely from an online session being present.
extern "C" int rr64_authority_step_begin(unsigned char *m,void *context) {
    // Reserve collision scratch before any prediction snapshot. A missing
    // graphics cell must never become missing physical ground for a remote racer.
    if (!rr64_online_terrain_prepare(m, context)) {
        rr64::netplay::authority_fail("online collision terrain unavailable");
        return 0;
    }
    if(rr64::prediction::active())return 1;
    rr64_prediction_verify_before(m,context);
    using namespace rr64;
    netplay::authority_pin_frame();
    const auto status=netplay::get_status();
    running=false;failed=false;client_pending=false;
    if(!status.authoritative || status.host_disconnected || status.phase!=netplay::Phase::Race){
        rr64_prediction_capture_reset();
        step_round=0;previous={};staged={};humans=0;return 1;
    }
    // The race routine can be reached by stock transition paths as well as
    // the dispatcher. Gate the native routine itself before sampling input.
    // Waiting for peers is normal loading, not an input-admission failure.
    if(!netplay::authority_race_gate(false))return 0;
    //6B098 skips the entire simulation pass while this native flag is set.
    // Do not enqueue/acknowledge an input for a pass that cannot consume it.
    // Keep existing history and button edges intact until simulation resumes.
    std::uint16_t suspended=0;
    if(!engine::read_u16(m,0x800A2192,suspended)){
        netplay::authority_fail("native update flag unavailable");return 1;
    }
    if(suspended)return 1;
    if(step_round!=status.game_setup.revision){step_round=status.game_setup.revision;previous={};rr64_authority_reset_eject();}
    humans=status.authority_humans;
    required=translated=0;
    unsigned actor_count=0;
    if(status.is_host && (!engine::read_u32(m,0x800a656cu,actor_count) || !actor_count ||
       actor_count>14 || (humans>>actor_count))){netplay::authority_fail("invalid native roster");return 1;}
    if(status.is_host)for(unsigned slot=0;slot<14;++slot)if(humans&(1u<<slot)) {
        std::uint16_t active=0;
        if(!engine::read_u16(m,0x800d8570u+slot*0x118u+0x24,active))return 1;
        if(active)required|=1u<<slot;
    }
    const unsigned controller=status.replicated_riders?0:status.local_slot;
    if(controller>=4)return 1;
    std::uint16_t buttons=0;std::uint8_t x=0,y=0;
    if(!engine::read_u16(m,engine::globals::controller_buttons+controller*2,buttons) ||
       !engine::read_u8(m,engine::globals::controller_stick_x+controller,x) ||
       !engine::read_u8(m,engine::globals::controller_stick_y+controller,y))return 1;
    authority::Command accepted{};
    authority::NativeTiming timing{};
    if(!prediction::capture_timing(m,timing) || !authority::duration_us(timing)){
        netplay::authority_fail("native input duration unavailable");return 0;
    }
    const unsigned duration_us=authority::duration_us(timing);
    const auto actions=static_cast<std::uint8_t>(rr64_authority_take_actions(controller));
    if(!netplay::authority_queue_input_recorded(buttons,static_cast<std::int8_t>(x),static_cast<std::int8_t>(y),accepted,actions,duration_us)){netplay::authority_fail("input admission failed");return 1;}
    if(!status.is_host){
        client_pending=rr64_prediction_capture_before(m,&accepted,context)!=0;
        if(!client_pending)netplay::authority_fail("prediction baseline capture failed");
        else if(!rr64_authority_eject_step(m,context,status.replicated_riders?0:status.local_slot,accepted.actions)){
            client_pending=false;netplay::authority_fail("prediction eject state invalid");
        }
    }
    if(status.is_host){
        running=netplay::authority_begin_step(staged,duration_us);
        if(!running)netplay::authority_fail("native step could not begin");
        else {
            for(unsigned slot=0;slot<14;++slot)if(required&(1u<<slot))
                if(!rr64_authority_eject_step(m,context,slot,staged.inputs[slot].actions)){
                    failed=true;netplay::authority_fail("host eject state invalid");break;
                }
#ifdef RR64_EXPERIMENTAL_COURSE
            // Only the authenticated host command stages gameplay. Guest live
            // prediction and private replay must never launch or consume items.
            if(!failed)for(unsigned slot=0;slot<14;++slot)if((required&(1u<<slot)) &&
                (staged.inputs[slot].actions&authority::action_mk64_item))
                mk64_items::stage_use(slot,staged.inputs[slot].y);
#endif
        }
    }
    return !failed;
}

extern "C" int rr64_authority_translate(unsigned char *m,void *context) {
    if(rr64::prediction::active())return rr64::prediction::Controls::translate(m,context);
    if(translating || !context)return 0;
    auto &ctx=*static_cast<recomp_context*>(context);
    const unsigned actor=static_cast<unsigned>(ctx.r4);
    if(!owns(actor))return 0;
    const unsigned slot=slot_of(actor);
    auto command=staged.inputs[slot];
    // A held/neutral command has sequence zero in the coordinator and is never
    // acknowledged. The native translator only needs its control values.
    command.round=step_round;if(!command.sequence)command.sequence=1;
    translating=true;
    const bool ok=rr64::authority::native_controls(m,ctx,command,previous[slot],step_round,staged.presses[slot]);
    translating=false;
    if(ok){previous[slot]=command.buttons;staged.presses[slot]=0;translated|=1u<<slot;}else failed=true;
    return 1;
}

extern "C" void rr64_authority_actor_route(void *context,unsigned stage) {
    if(rr64::prediction::active())return;
    if(!context)return;
    auto &ctx=*static_cast<recomp_context*>(context);
    if(!owns(static_cast<unsigned>(ctx.r16)))return;
    const auto guest=[](unsigned p){return static_cast<gpr>(static_cast<std::int64_t>(static_cast<std::int32_t>(p)));};
    if(stage==0) {
        // Native paired selectors:6CC94/6CCC4 choose524CC/4EB6C;
        // 6ECC0/6ECB8 choose5264C/4EAD0. Redirect this call only, so
        // race-state transitions retain their canonical callback fields.
        switch(static_cast<unsigned>(ctx.r3)) {
        case 0x8004eb6c:ctx.r3=guest(0x800524cc);break;
        case 0x8004ead0:ctx.r3=guest(0x8005264c);break;
        case 0x800524cc:case 0x8005264c:break;
        default:failed=true;break;
        }
    } else if(stage==1 && !failed) {
        // Enter the existing40664 -> override -> reaction ->4090C pipeline.
        // Do not change actor+26 globally: other systems still have four slots.
        ctx.r2=0;
    } else if(stage==2 && static_cast<unsigned>(ctx.r2)==0x800515c4) {
        // The same initializer pairs the AI start override with human40500.
        // Other overrides, including crash/race transitions, stay untouched.
        ctx.r2=guest(0x80040500);
    }
}

extern "C" void rr64_authority_step_finish(unsigned char *m) {
    if(rr64::prediction::active())return;
    rr64_prediction_verify_after(m);
    if(client_pending){
        client_pending=false;
        if(!rr64_prediction_capture_after(m))rr64::netplay::authority_fail("prediction history capture failed");
        else rr64_prediction_reconcile_step(m);
        return;
    }
    if(!running)return;
    running=false;
    // Native AI branches skip40664. Never acknowledge those human commands as
    // simulated merely because the surrounding frame reached its end.
    if(failed || (translated&required)!=required){
        rr64::netplay::authority_fail("native controls incomplete");return;
    }
    rr64::authority::Stamp completed{};
    if(!rr64::netplay::authority_finish_step(staged.tick,completed))
        rr64::netplay::authority_fail("native step acknowledgement failed");
    else if(!rr64_online_authority_capture(m,&completed))
        rr64::netplay::authority_fail("native state capture failed");
}
