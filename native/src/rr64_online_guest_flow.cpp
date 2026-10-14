#include "rr64_online_flow.hpp"
#include "rr64_campaign_coop.hpp"
#include "rr64_online_bike_profile.hpp"
#include "rr64_netplay.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_custom_cop_rules.hpp"
#include "rr64_highlights.hpp"
#include <array>
#include <cstdlib>
#include <cstdio>

namespace {
using namespace rr64;
unsigned read(unsigned char *m, unsigned a) { unsigned v=0; engine::read_u32(m,a,v); return v; }
bool selecting(const netplay::Status &s) {
    return s.active && s.connected && s.game_setup.valid &&
           (s.phase == netplay::Phase::CharacterSelect || s.phase == netplay::Phase::TrackSelect);
}
thread_local unsigned saved_menu_count=0;
thread_local bool private_selector=false;
thread_local std::array<unsigned,3> hud_saved{};
thread_local bool private_hud=false;
thread_local unsigned hud_actor=0;
thread_local online_flow::PostRaceState requested_postrace{};
}

extern "C" void rr64_online_game_setup_restart();

extern "C" int rr64_online_postrace_wait_for_setup() {
    const auto s=netplay::get_status();
    return s.active && s.connected && s.phase==netplay::Phase::Race && s.postrace.stage==2;
}

extern "C" unsigned rr64_online_postrace_route_mode(unsigned char *m,unsigned requested) {
    const auto s=netplay::get_status();
    requested=rr64_coop_mode(m,requested);
    const unsigned stage=online_flow::postrace_stage(read(m,engine::globals::main_mode),requested);
    if(!stage || !s.active || !s.connected) return requested;
    // Guests consume the persistent command below. A sampled host edge, or a
    // guest's local A button, cannot independently advance a results screen.
    if(!s.is_host || !netplay::host_advance_postrace(stage))
        return read(m,engine::globals::pending_mode);
    if(stage==2) rr64_online_game_setup_restart();
    return requested;
}

extern "C" int rr64_online_postrace_update(unsigned char *m,unsigned mode) {
    auto s=netplay::get_status();
    if(!s.active || !s.connected) { requested_postrace={}; return 0; }
    if(!s.postrace.round || !s.postrace.stage) return 0;
    if(mode==0x1f) netplay::acknowledge_postrace({s.postrace.round,1});
    if(mode==0x23 && s.postrace.stage==2) {
        netplay::acknowledge_postrace(s.postrace);
        if(s.is_host) netplay::host_resume_postrace_setup();
        // Do not let the host use the next menu while another peer is still
        // replaying, tearing down, or waiting for the final authority frame.
        return netplay::get_status().phase==netplay::Phase::Race;
    }
    if(s.is_host || rr64_highlights_presenting()) return 0;
    unsigned next=0,stage=0;
    if(mode==0x1e) {next=0x1f;stage=1;}
    if(mode==0x1f && s.postrace.stage==2) {next=0x23;stage=2;}
    if(!next) return 0;
    const online_flow::PostRaceState command{s.postrace.round,stage};
    if(!online_flow::advances(command,requested_postrace) || command==requested_postrace) return 0;
    requested_postrace=command;
    if(std::getenv("RR64_SYNC_LOG"))
        std::fprintf(stderr,"[RR64-FLOW] guest postrace round=%u stage=%u mode=%u target=%u\n",
            command.round,command.stage,mode,next);
    if(stage==2) {
        // func_80071AB0 writes this immediately before its native mode request.
        engine::write_u32(m,engine::globals::multiplayer_stage,1);
        rr64_online_game_setup_restart();
    }
    engine::write_u32(m,engine::globals::pending_mode,next);
    engine::write_u32(m,0x8009CC50u,0);
    return 0;
}

extern "C" void rr64_online_private_selection_begin(unsigned char *m) {
    const auto s=rr64::netplay::get_status();
    private_selector=false;
    if (!selecting(s) || read(m,engine::globals::multiplayer_stage)<2) return;
    saved_menu_count=read(m,0x8009EF5C);
    // Only the local selector exists on this machine's menu. The race roster
    // is restored at the transition, using confirmed network choices.
    engine::write_u32(m,0x8009EF5C,1);
    private_selector=true;
}

extern "C" int rr64_online_selection_commit(unsigned char *m) {
    auto s=rr64::netplay::get_status();
    if (!selecting(s)) return 1;
    // Content validation belongs to this revision. A stale confirmed selection
    // must never advance the native carrier track after validation is revoked.
    if (s.local_slot>=s.players.size() ||
        s.players[s.local_slot].course_compatibility!=race_pack::Compatibility::Ready) return 0;
    if (std::getenv("RR64_SYNC_LOG")) {
        static std::array<unsigned,5> last{};
        unsigned confirmed=0,loaded=0;
        for (unsigned i=0;i<s.players.size();++i) if(s.players[i].connected) {
            if(s.players[i].selection.confirmed) confirmed|=1u<<i;
            if(s.players[i].selection.loaded) loaded|=1u<<i;
        }
        const std::array<unsigned,5> current{s.game_setup.revision,unsigned(s.phase),
            read(m,engine::globals::multiplayer_stage),confirmed,loaded};
        if (current!=last) {
            last=current;
            std::fprintf(stderr,"[RR64-SELECT] round=%u phase=%u stage=%u confirmed=%x loaded=%x start=%u local=%u\n",
                current[0],current[1],current[2],confirmed,loaded,s.game_setup.start_requested,unsigned(s.local_slot));
        }
    }
    if (!private_selector) return 0;
    if (s.phase==netplay::Phase::CharacterSelect) {
        online_flow::Selection choice{s.game_setup.revision,read(m,0x8009F670),
            read(m,0x8009F660),read(m,0x8009EF2C)==2 ? 1u : 0u,0};
        netplay::set_selection(choice);
        bool cop=false;
        s=netplay::get_status();
        for (const auto &p:s.players)
            if (p.connected && p.selection.confirmed && p.selection.bike==31 &&
                p.selection.rider>=40 && p.selection.rider<=44) cop=true;
        if (!(s.game_setup.race_options&512u) || cop) netplay::host_release_selection();
        s=netplay::get_status();
    }
    if (s.phase!=netplay::Phase::TrackSelect) return 0;
    for (const auto &player:s.players)
        if (player.connected && player.course_compatibility!=race_pack::Compatibility::Ready) return 0;
    const unsigned humans=s.replicated_riders ? 1u : s.connected_players;
    if (humans<1 || humans>4) return 0;
    for (unsigned guest=0;guest<humans;++guest) {
        const unsigned slot=s.replicated_riders ? s.local_slot : guest;
        const auto &choice=s.players[slot].selection;
        if (!choice.confirmed || choice.round!=s.game_setup.revision) return 0;
        engine::write_u32(m,0x8009F670+guest*4,choice.rider);
        engine::write_u32(m,0x8009F660+guest*4,choice.bike);
        // The stock selector maintains separate race-load choices. func_8006CB8C
        // consumes these, not the menu arrays above. Commit both atomically.
        engine::write_u32(m,0x8009F400+guest*4,choice.rider);
        engine::write_u32(m,0x8009F5D8+guest*4,choice.bike);
        engine::write_u32(m,0x8009EF2C+guest*4,2);
    }
    engine::write_u32(m,0x8009EF5C,humans);
    engine::write_u32(m,engine::globals::random_state,s.game_setup.random_seed);
    saved_menu_count=humans;
    return 1;
}

extern "C" void rr64_online_private_selection_end(unsigned char *m) {
    if (private_selector) engine::write_u32(m,0x8009EF5C,saved_menu_count);
    private_selector=false;
}

extern "C" void rr64_online_seed_race(unsigned char *m) {
    const auto s=netplay::get_status();
    if (s.active && s.connected && s.game_setup.valid && s.phase==netplay::Phase::TrackSelect) {
        engine::write_u32(m,engine::globals::random_state,s.game_setup.random_seed);
        if (std::getenv("RR64_SYNC_LOG")) {
            for (unsigned guest=0;guest<(s.replicated_riders ? 1u : s.connected_players) && guest<4;++guest)
                std::fprintf(stderr,"[RR64-ROSTER] round=%u local=%u guest=%u rider=%u bike=%u\n",
                    s.game_setup.revision,unsigned(s.local_slot),guest,
                    read(m,0x8009F400+guest*4),read(m,0x8009F5D8+guest*4));
        }
    }
}

extern "C" void rr64_online_hud_begin(unsigned char *m) {
    const auto s=netplay::get_status();
    private_hud=false;
    if (!s.active || !s.connected || s.phase!=netplay::Phase::Race) return;
    const unsigned guest=s.replicated_riders ? 0 : s.local_slot;
    if (guest>=4) return;
    hud_saved={read(m,0x800A4F24),read(m,0x800A6578),read(m,0x800A657C)};
    const unsigned rider=read(m,0x800A657C+guest*4);
    if (rider>=14) return;
    hud_actor=rider;
    // These are presentation inputs to func_80030220, restored at its sole
    // epilogue. Simulation and the world/sky render layout are untouched.
    engine::write_u32(m,0x800A4F24,0);
    engine::write_u32(m,0x800A6578,1);
    engine::write_u32(m,0x800A657C,rider);
    private_hud=true;
}
extern "C" void rr64_online_hud_end(unsigned char *m) {
    if (!private_hud) return;
    engine::write_u32(m,0x800A4F24,hud_saved[0]);
    engine::write_u32(m,0x800A6578,hud_saved[1]);
    engine::write_u32(m,0x800A657C,hud_saved[2]);
    private_hud=false;
}

extern "C" void rr64_online_pause_owner(unsigned char *m) {
    const auto s=netplay::get_status();
    if (s.active && s.connected && s.phase==netplay::Phase::Race)
        engine::write_u32(m,0x800A74A4,0); // pause reads host stream in virtual port 0
}

extern "C" unsigned rr64_online_player_count_label(unsigned original) {
    const auto s=netplay::get_status();
    return s.active && s.connected ? s.connected_players : original;
}
extern "C" unsigned rr64_online_race_choice(unsigned guest, unsigned original, unsigned bike) {
    const auto s=netplay::get_status();
    if (!s.active || !s.connected || !s.game_setup.valid || guest>=netplay::kMaximumPlayers)
        return original;
    const unsigned slot=online_flow::mapped_slot(guest,s.local_slot,s.replicated_riders);
    if (slot>=s.players.size()) return original;
    const auto &p=s.players[slot];
    if (!p.connected || !p.selection.confirmed || p.selection.round!=s.game_setup.revision)
        return original;
    // Inject at native actor creation, rather than extending four-controller
    // selection arrays. All fourteen network riders get their own identity.
    return bike ? p.selection.bike : p.selection.rider;
}

extern "C" unsigned rr64_online_bike_profile(unsigned char *m, unsigned actor, unsigned profile) {
    return rr64::online_flow::bike_profile(m, actor, profile, rr64::netplay::get_status());
}
extern "C" void rr64_online_bike_profiles_reset() {
    rr64::online_flow::reset_bike_profiles();
}

// Full-screen native HUD has a few direct actor-zero loads in addition to its
// mapped player loops. Redirect only those loads, not the shared roster base.
extern "C" unsigned rr64_online_hud_actor_pointer(unsigned char *m,unsigned original,unsigned field) {
    if (!private_hud || hud_actor>=14 || (field!=0xE0 && field!=0xE4 && field!=0xE8)) return original;
    return read(m,0x800D8570+hud_actor*0x118+field);
}
