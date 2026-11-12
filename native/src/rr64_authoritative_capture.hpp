#pragma once
#include "rr64_netplay.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_integrator.hpp"
#include "rr64_traffic_sync_capture.hpp"

namespace rr64::authority {
// Host roster indices are canonical network indices. Capture through actor
// links, never assume a pool allocation index equals the player's identity.
// The supplied native capture reads the existing rider/bike state schema.
template<class Capture>
bool capture_frame(unsigned char *m,const Stamp &stamp,unsigned humans,
                   std::uint64_t sample_us,netplay::AuthorityFrame &out,Capture capture,
                   const char **failure=nullptr,bool diagnostic_ai_roster=false) {
    const auto stage=[&](const char *reason){if(failure)*failure=reason;};
    stage("rider-roster");
    if(!m || !stamp.round || !stamp.tick || (!humans && !diagnostic_ai_roster) || humans>>maximum_players)return false;
    unsigned count=0;
    if(!engine::read_u32(m,0x800a656cu,count) || !count || count>maximum_players || (humans>>count))return false;
    netplay::AuthorityFrame candidate{};candidate.stamp=stamp;
    stage("timing");
    constexpr std::array<unsigned,6> timing_addresses{
        0x8009cba8,0x8009cbac,0x8009cbb0,0x8009cbb4,0x800a1820,0x800d7670};
    for(unsigned i=0;i<timing_addresses.size();++i)
        if(!engine::read_u32(m,timing_addresses[i],candidate.timing.bits[i]))return false;
    std::uint16_t split=0;
    if(!engine::read_u16(m,0x800a659a,split))return false;
    candidate.timing.substeps=split?2:1;
    if(!valid_timing(candidate.timing))return false;
    std::array<unsigned,maximum_players> bikes{},riders{};
    for(unsigned slot=0;slot<count;++slot) {
        stage("rider-route");
        const unsigned actor=0x800d8570u+slot*0x118u;
        std::uint16_t active=0;unsigned bike=0,rider=0,linked=0,model=0,character=0;
        if(!engine::read_u16(m,actor+0x24,active))return false;
        unsigned route=0;
        auto &outcome=candidate.outcomes[slot];
        if(!engine::read_u32(m,actor+0xe8,route) || !engine::valid_guest_range(route,0x64)) {
            if(active)return false;
            continue;
        }
        stage("rider-outcome");
        if(!engine::read_u32(m,actor+0x20,outcome.role) ||
           !engine::read_u32(m,route+0x58,outcome.busts) ||
           !engine::read_u16(m,route+0x48,outcome.eligible) ||
           !engine::read_u16(m,route+0x4c,outcome.busted) ||
           !engine::read_u32(m,route+0x40,outcome.recovery_count) ||
           !engine::read_u16(m,route+0x50,outcome.recovery_flag) ||
           !engine::read_u16(m,route+0x52,outcome.finished) ||
           !engine::read_u16(m,route+0x4e,outcome.progress_gate) ||
           !engine::read_float(m,route+8,outcome.progress[0]) ||
           !engine::read_float(m,route+0xc,outcome.progress[1]) ||
           !engine::read_float(m,route+0x20,outcome.progress[2]) || !valid_outcome(outcome))return false;
        outcome.valid=1;
        // Retiring a rider must not discard the final bust/recovery result.
        if(!active)continue;
        stage("rider-links");
        if(!engine::read_u32(m,actor+0xe0,bike) || !engine::read_u32(m,actor+0xe4,rider) ||
           !engine::valid_guest_range(bike,engine::bike::stride) ||
           !engine::valid_guest_range(rider,engine::rider::stride) ||
           !engine::read_u32(m,bike+engine::bike::rider_pointer,linked) || linked!=rider ||
           !engine::read_u32(m,rider+engine::rider::bike_pointer,linked) || linked!=bike)return false;
        stage("rider-model");
        if(!engine::read_u32(m,actor+0x18,model) || model>31)return false;
        stage("rider-character");
        if(!engine::read_u32(m,actor+0x1c,character) || character>44)return false;
        stage("rider-duplicate");
        for(unsigned prior=0;prior<slot;++prior)
            if(bikes[prior]==bike || riders[prior]==rider)return false;
        bikes[slot]=bike;riders[slot]=rider;
        stage("rider-integrator");
        // 37CDC embeds bike physics at+108;36B88 embeds rider physics at+28.
        if(!prediction::capture_integrator(m,bike+0x108,candidate.dynamics[slot].bike_physics) ||
           !prediction::capture_integrator(m,rider+0x28,candidate.dynamics[slot].rider_physics))return false;
        stage("rider-dynamics");
        for(unsigned vector=0;vector<rider_dynamics_offsets.size();++vector)
            for(unsigned axis=0;axis<3;++axis)
                if(!engine::read_float(m,rider+rider_dynamics_offsets[vector]+axis*4,
                    candidate.dynamics[slot].values[vector*3+axis]))return false;
        if(!engine::read_u32(m,rider+0x20,candidate.dynamics[slot].damping_mode) ||
           !engine::read_u32(m,rider+0x5d4,candidate.dynamics[slot].effect) ||
           !engine::read_float(m,rider+0x5d8,candidate.dynamics[slot].effect_remaining) ||
           !engine::read_float(m,bike+0x4cc,candidate.dynamics[slot].recovery_age) ||
           !valid_dynamics(candidate.dynamics[slot]))return false;
        auto &state=candidate.riders[slot];
        stage("rider-presentation");
        if(!capture(m,bike,state) || !state.root.valid || !state.rider_position_valid)return false;
        state.active=true;state.host_ai=!(humans&(1u<<slot));
        state.character=character;state.bike=model;
        state.tick=static_cast<std::uint32_t>(stamp.tick);state.sample_time_us=sample_us;
    }
    world_sync::Snapshot world;
    if(!world_sync::capture_traffic(m,stamp.round,stamp.tick,world,failure))return false;
    candidate.traffic=world.traffic;
    // A failed actor must not publish a mixture of old/new identities.
    out=candidate;stage(nullptr);return true;
}
}
