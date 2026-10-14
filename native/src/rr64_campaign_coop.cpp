#include "rr64_campaign_coop.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_local_players.hpp"
#include "rr64_netplay.hpp"
#include <algorithm>
#include <array>

namespace {
using namespace rr64;
using namespace engine;
constexpr unsigned profile = 0x800D6A40, descriptor = 0x800D8520;
constexpr unsigned actors = 0x800D8570, stride = 0x118;
bool selecting = false, racing = false;
std::array<unsigned, 18> saved_descriptor{};
std::array<unsigned, 7> remembered_bikes{};
unsigned saved_track = 0, saved_level = 0, bike_tier = 0;
unsigned partner_bike = 0, partner_rider = 0, leader_rider = 0;
unsigned word(unsigned char *m, unsigned a) { unsigned v=0; read_u32(m,a,v); return v; }
unsigned half(unsigned char *m, unsigned a) { std::uint16_t v=0; read_u16(m,a,v); return v; }
bool active() { return campaign_coop::enabled.load() && !netplay::get_status().active; }
void restore_course(unsigned char *m) {
    for (unsigned i=0; i<saved_descriptor.size(); ++i)
        write_u32(m, descriptor+i*4, saved_descriptor[i]);
    write_u32(m, 0x800A6680, saved_track);
    write_u32(m, 0x800A6690, saved_level);
}
unsigned result_actor(unsigned char *m) {
    unsigned best=actors, best_rank=14;
    for (unsigned slot=0; slot<2; ++slot) {
        const unsigned actor=actors+slot*stride, state=word(m,actor+0xE8);
        if (!valid_guest_range(state,0x64) || !half(m,state+0x50) ||
            half(m,state+0x4C) || half(m,state+0x4E)) continue;
        const unsigned rank=word(m,state+0x44);
        if (rank<best_rank) {best=actor; best_rank=rank;}
    }
    return best;
}
}

extern "C" void rr64_coop_main_menu(unsigned char *m) {
    if (!m || !(word(m,local_race::menu_buttons)&5)) return;
    const unsigned selection=word(m,local_race::menu_cursor);
    if (selection!=5) return;
    if (netplay::get_status().active) return;
    campaign_coop::enabled.store(true);
    // Let the original Big Game dispatcher own New/Load, shop and save menus.
    write_u32(m,local_race::menu_cursor,2);
}

extern "C" unsigned rr64_coop_mode(unsigned char *m, unsigned requested) {
    if (!m) return requested;
    const unsigned current=word(m,globals::main_mode);
    if (requested==0x20 || netplay::get_status().active) {
        if (campaign_coop::enabled.exchange(false)) campaign_coop::input_request.store(0);
        selecting=racing=false;
        return requested;
    }
    if (!active()) return requested;
    // Intercept before the campaign loading screen, not after its world loads.
    if (requested==0x16 && current==0x2F && !selecting) {
        const unsigned level=word(m,profile+0x3C);
        if (level>5) return requested;
        saved_track=word(m,0x800A6680); saved_level=word(m,0x800A6690);
        bike_tier=level==5 ? 6 : level;
        for (unsigned i=0;i<saved_descriptor.size();++i) saved_descriptor[i]=word(m,descriptor+i*4);
        selecting=true; racing=false;
        campaign_coop::input_request.store(1);
        write_u32(m,local_race::menu_humans,2);
        write_u32(m,globals::multiplayer_stage,2);
        write_u32(m,0x800A6690,bike_tier);
        write_u32(m,local_race::menu_dirty,1);
        // Same preview-rotation reset as the native options-to-selector step.
        write_u32(m,0x8009E210,word(m,0x80003C74));
        for (unsigned slot=0;slot<4;++slot) {
            write_u32(m,0x8009EF2C+slot*4,slot<2 ? 1 : 0);
            write_u16(m,0x800A7520+slot*2,slot<2 ? 1 : 0);
            write_u16(m,0x8009EAE8+slot*2,slot==0 ? 1 : 0);
        }
        write_u32(m,0x8009F670,word(m,profile+0x18));
        write_u32(m,0x8009F400,word(m,profile+0x18));
        write_u32(m,0x8009EF3C,0); // Native per-player bike-list cursors.
        write_u32(m,0x8009EF40,remembered_bikes[bike_tier]);
        return 0x23;
    }
    if (selecting && current==0x23 && requested==0x1B) {
        partner_bike=word(m,0x8009F5DC); partner_rider=word(m,0x8009F404);
        leader_rider=word(m,0x8009F400);
        if (partner_bike>30 || partner_rider>=45 || leader_rider>=45) return current;
        remembered_bikes[bike_tier]=word(m,0x8009EF40);
        write_u32(m,profile+0x18,leader_rider);
        restore_course(m);
        selecting=false; racing=true;
        return 0x16;
    }
    if (selecting && requested==0x2F) {
        restore_course(m); selecting=false;
        campaign_coop::input_request.store(0);
    }
    // Native campaign bust/repair menus assume there is only one human.
    if (racing && current==0x17 && (requested==0x30 || requested==0x32))
        return word(m,0x800D7648) ? current : 0x19;
    if (racing && current==0x19 && requested==0x1A) {
        const unsigned state=word(m,result_actor(m)+0xE8);
        if (!valid_guest_range(state,0x64) || !half(m,state+0x50))
            return valid_guest_range(state,0x64) && half(m,state+0x4C) ? 0x32 : 0x30;
    }
    if (requested==0x2F) {
        racing=false;
        campaign_coop::input_request.store(0);
    }
    return requested;
}

extern "C" void rr64_coop_selection_begin(unsigned char *m) {
    if (!m || !selecting || !active()) return;
    // Shared menu state may retain a previous multiplayer course or cop tier.
    write_u32(m,local_race::menu_humans,2);
    write_u32(m,0x800A6690,bike_tier);
    // Reuse the native bike shop's temporary entry, never its immutable table.
    write_u32(m,0x800D8510,0);
    write_u32(m,0x800D8514,word(m,profile+0x14));
}
extern "C" void rr64_coop_selection_end(unsigned char *m) {
    if (m && selecting && active() && word(m,globals::multiplayer_stage)!=2) {
        restore_course(m); selecting=false;
        campaign_coop::input_request.store(0);
        write_u32(m,globals::pending_mode,0x2F);
    }
}
extern "C" unsigned rr64_coop_bike_count(unsigned slot,unsigned original) {
    return selecting && active() && slot==0 ? 1 : original;
}
extern "C" unsigned rr64_coop_bike_entry(unsigned char *,unsigned slot,unsigned original) {
    return selecting && active() && slot==0 ? 0x800D8510 : original;
}
extern "C" void rr64_coop_roster(unsigned char *m) {
    if (!m || !racing || !active()) return;
    // Two humans replace one AI when the original campaign roster is full.
    write_u16(m,descriptor+0x2C,2);
    unsigned total=0;
    for(unsigned i=0;i<8;++i) total+=half(m,descriptor+0x2C+i*2);
    for(unsigned i=1;i<8 && total>14;++i) {
        const unsigned amount=half(m,descriptor+0x2C+i*2), remove=std::min(amount,total-14);
        write_u16(m,descriptor+0x2C+i*2,amount-remove); total-=remove;
    }
    write_u32(m,local_race::humans,2);
}
extern "C" void rr64_coop_riders(unsigned char *m) {
    if (!m || !racing || !active()) return;
    write_u32(m,actors+0x1C,leader_rider);
    const unsigned partner=actors+stride;
    write_u32(m,partner+0x18,partner_bike);
    write_u32(m,partner+0x1C,partner_rider);
    write_u32(m,partner+0x20,partner_bike<12 ? 5 : partner_bike<24 ? 6 : 8);
    const auto names=local_players::snapshot();
    for(unsigned i=0;i<12;++i) write_s8(m,partner+0xC+i,i<names[1].size() ? names[1][i] : 0);
}
extern "C" unsigned rr64_coop_result_actor(unsigned char *m,unsigned original) {
    return m && racing && active() ? result_actor(m) : original;
}
extern "C" unsigned rr64_coop_result_state(unsigned char *m,unsigned original) {
    return m && racing && active() ? word(m,result_actor(m)+0xE8) : original;
}
extern "C" int rr64_coop_spectating() { return racing && active(); }
