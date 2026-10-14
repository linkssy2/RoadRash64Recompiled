#include "rr64_local_race_options.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_custom_cop_rules.hpp"
#include "rr64_custom_cop.hpp"
#include "rr64_thrash_options.hpp"
#include <cstdio>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include "recomp.h"
#include <vector>
#include <utility>
#include "rr64_engine_layout.hpp"
#include "rr64_local_players.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_replay.hpp"
namespace { bool online = false, online_connected = false, online_host = false; }
namespace { bool highlights = false; }
extern "C" int rr64_highlights_presenting() { return highlights; }
// This pre-existing roster regression exercises the normal cheats-OFF path.
// Dedicated modifier fixtures separately cover enabled and online rejection.
namespace rr64::offline_modifiers { bool enabled(Flag) { return false; } }
extern "C" int rr64_offline_bikes_active(unsigned char*, unsigned) { return 0; }
extern "C" unsigned rr64_offline_bikes_entry(unsigned char*, unsigned, unsigned original) { return original; }
namespace rr64::netplay { Status get_status() { Status result{}; result.active = online; result.connected = online_connected; result.is_host = online_host; return result; } }
namespace rr64::netplay { PhysicsRules get_physics_rules() { PhysicsRules result{}; result.active = online; result.connected = online_connected; result.is_host = online_host; return result; } }
namespace rr64::netplay { bool authority_get_outcome(unsigned,authority::Outcome&,bool&){return false;} }
namespace recomp { void* alloc(unsigned char* memory, size_t size) { static size_t next = 0x700000; const size_t offset = next; next += (size + 15) & ~size_t(15); return memory + offset; } }
namespace { bool placement_ok=false; unsigned voices=0; }
extern "C" void func_8006B740(unsigned char* rdram,recomp_context* ctx) {
 const auto state=MEM_W(0xE8,ctx->r4);
 MEM_W(0,state)=123; // failed stock placement may already modify routing
 MEM_W(0,rr64::engine::guest_address(0x800D77A4))=ctx->r5;
 ctx->r2=placement_ok;
}
extern "C" void func_80056084(unsigned char*,recomp_context*) { ++voices; }
int main() {
    using namespace rr64::local_race_options;
    bool ok = true;
    // Equipment alone never unlocks a role outside Custom Cop Mode.
    for (unsigned player = 0; player < 4; ++player) {
        for (unsigned cop_rider : {40u, 41u}) {
            ok &= rr64::custom_cop::selected_cop(true, 31, cop_rider);
            ok &= !rr64::custom_cop::selected_cop(true, 0, cop_rider);
            ok &= !rr64::custom_cop::selected_cop(false, 31, cop_rider);
        }
        ok &= !rr64::custom_cop::selected_cop(true, 31, player);
    }
    for (unsigned humans = 1; humans <= 4; ++humans) {
        ok &= roster(0, humans, 9) == humans;
        ok &= roster(99, humans, 4) == 11;
    }
    ok &= roster(99, 0, 6) == 6 && roster(99, 5, 6) == 6;
    // A mode hiding all optional stock rows must still reach the new row,
    // wrap in both directions and skip hidden rows without an unbounded loop.
    constexpr unsigned mask = 3 | (1 << 4) | (1 << 7) | (1 << 8);
    ok &= next_row(7, 1, mask) == 8 && next_row(8, 1, mask) == 0;
    ok &= next_row(0, -1, mask) == 8 && next_row(4, -1, mask) == 1;
    ok &= next_row(4, 1, 0) == 0;
    using namespace rr64::engine;
    namespace layout = rr64::engine::local_race;
    std::vector<unsigned char> memory(kRdramSize);
    auto* rdram = memory.data();
    const auto put = [&](unsigned a, unsigned v) { write_u32(rdram, a, v); };
    const auto get = [&](unsigned a) { unsigned v = 0; read_u32(rdram, a, v); return v; };
    put(layout::humans, 4); put(layout::racers, 4); put(layout::ai_choice, 1);
    rr64_local_options_race(rdram); // Default local=false: do not touch another mode.
    ok &= get(layout::racers) == 4;
    rr64::local_players::active.store(true);
    // Every count for every player count, including wrap and a player-count
    // change between setup and race initialization. Stock AI flag stays binary.
    for (unsigned humans = 1; humans <= 4; ++humans) {
        put(layout::humans, humans); put(layout::menu_humans, humans);
        put(layout::menu_cursor, 4); put(layout::menu_buttons, 0);
        rr64_local_options_input(rdram);
        // Previous loop wraps to zero.
        rr64_local_options_race(rdram);
        ok &= get(layout::racers) == humans;
        for (unsigned ai = 1; ai <= max_ai(humans); ++ai) {
            put(layout::menu_buttons, 0x40);
            ok &= rr64_local_options_input(rdram) == 1;
            ok &= get(layout::ai_choice) == 1;
            rr64_local_options_race(rdram);
            ok &= get(layout::racers) == humans + ai;
        }
        rr64_local_options_input(rdram); // Maximum wraps to no AI.
        rr64_local_options_race(rdram);
        ok &= get(layout::racers) == humans && get(layout::ai_choice) == 0;
    }
    put(layout::menu_buttons, 0x20); rr64_local_options_input(rdram);
    rr64_local_options_race(rdram); ok &= get(layout::racers) == 11;
    put(layout::menu_buttons, 0x40); rr64_local_options_input(rdram);
    put(layout::menu_cursor, 8); put(0x800A6418, 4);
    for (unsigned level = 1; level <= 7; ++level) {
        rr64_local_options_input(rdram);
        // Preview uses current options even before the race snapshot is taken.
        for (unsigned map = 0; map < 15; ++map)
            ok &= rr64_local_bike_menu_level(map) == level - 1;
        rr64_local_options_race(rdram);
        ok &= rr64_local_bike_level(4) == level - 1 && get(0x800A6418) == 4;
    }
    rr64_local_options_input(rdram); rr64_local_options_race(rdram);
    ok &= rr64_local_bike_level(3) == 3; // Match Track after Insanity.
    for (unsigned map = 0; map < 15; ++map)
        ok &= rr64_local_bike_menu_level(map) == map;
    put(layout::menu_buttons, 0x20); rr64_local_options_input(rdram);
    rr64_local_options_race(rdram); ok &= rr64_local_bike_level(0) == 6;
    ok &= rr64_local_options_visible(8, 0) == 1;
    const unsigned table = rr64_local_options_table(layout::menu_table);
    ok &= table != layout::menu_table && get(table + 11 * 36) == 0;
    MEM_B(3, guest_address(layout::visibility)) = 1;
    ok &= rr64_local_options_visible(5, 1) == 0;
    put(layout::menu_cursor, 4); put(layout::menu_buttons, 0x10);
    ok &= rr64_local_options_navigation(rdram) == 6;
    put(layout::menu_buttons, 8);
    ok &= rr64_local_options_navigation(rdram) == 4;
    MEM_B(3, guest_address(layout::visibility)) = 0;
    put(layout::menu_cursor, 8);
    put(0x80300018, 0xBADF00D);
    rr64_local_options_text(rdram, 8, 0x80300000);
    ok &= MEM_B(0, guest_address(0x80300000)) == 'B';
    ok &= get(0x80300018) == 0xBADF00D;
    put(layout::menu_buttons, 0x10);
    ok &= rr64_local_options_navigation(rdram) == 9;
    rr64_local_options_finish(rdram);
    // Scooter profile substitution keeps the human rider and original profile
    // intact, and is disabled when Match Track or online is active.
    put(layout::menu_cursor, 8); put(layout::menu_buttons, 0x20);
    rr64_local_options_input(rdram); rr64_local_options_race(rdram); // Insanity -> Scooter
    const unsigned original = 0x800A3460, donor = original + 4 * 16;
    for (unsigned i = 0; i < 16; ++i) MEM_B(i, guest_address(original)) = i;
    MEM_B(8, guest_address(donor)) = 6;
    MEM_B(9, guest_address(donor)) = 1;
    MEM_B(10, guest_address(donor)) = 23;
    MEM_B(13, guest_address(donor)) = 38;
    MEM_B(8, guest_address(donor + 16)) = 12;
    put(0x800D8570 + 0x18, 23); // Native selector commits the chosen Scooter.
    const unsigned profile = rr64_local_bike_profile(rdram, 0x800D8570, original);
    ok &= profile != original && MEM_B(10, guest_address(profile)) == 23;
    ok &= MEM_B(13, guest_address(profile)) == 38;
    ok &= MEM_B(11, guest_address(profile)) == 11 && MEM_B(14, guest_address(profile)) == 14;
    ok &= MEM_B(10, guest_address(original)) == 10 && get(0x800D8588) == 23;
    ok &= rr64_local_bike_profile(rdram, 0x800D8571, original) == original;
    // Donor availability is distinct from track category demand. Insanity has
    // no category 2, Scooter no category 4. Keep total and police count intact;
    // preserve populated families and surrounding native stack data exactly.
    recomp_context pool_context{};
    pool_context.r29 = guest_address(0x80500000);
    const unsigned pool_stack = 0x80500000;
    for (unsigned missing : {2u, 4u}) {
        for (unsigned i = 0; i < 9; ++i) {
            put(pool_stack + 0x48 + i * 4, i == 1 || i == 3 || i == 7 ? 2 : 0);
            put(pool_stack + 0x70 + i * 4, 0);
        }
        put(pool_stack + 0x44, 0xBADF00D);
        put(pool_stack + 0x94, 0xC0FFEE);
        put(pool_stack + 0x70 + missing * 4, 5);
        put(pool_stack + 0x70 + 7 * 4, 2);
        pool_context.r21 = 2;
        rr64_local_bike_ai_pool(rdram, &pool_context);
        ok &= get(pool_stack + 0x70 + missing * 4) == 0;
        ok &= get(pool_stack + 0x70 + (missing - 1) * 4) == 5;
        ok &= get(pool_stack + 0x70 + 7 * 4) == 2 && pool_context.r21 == 2;
        ok &= get(pool_stack + 0x44) == 0xBADF00D && get(pool_stack + 0x94) == 0xC0FFEE;
        ok &= get(pool_stack + 0x48 + missing * 4) == 0; // Do not invent donors.
        rr64_local_bike_ai_pool(rdram, &pool_context);
        ok &= get(pool_stack + 0x70 + (missing - 1) * 4) == 5; // Idempotent.
    }
    // Session options drive the same preview on connected peers, while leaving
    // this machine's saved local Scooter preference alone.
    online = online_connected = true;
    apply_online_options(7u << 6);
    ok &= rr64_local_bike_menu_level(0) == 6;
    online_connected = false;
    ok &= rr64_local_bike_menu_level(2) == 2;
    online = false;
    ok &= rr64_local_bike_menu_level(0) == 5;
    // Exercise the actual guest-memory bridge in every local slot. Snapshot
    // unrelated controller/profile fields and unlocks across role assignment.
    put(layout::menu_cursor, 9); put(layout::menu_buttons, 0x40);
    rr64_local_options_input(rdram);
    ok &= rr64_custom_cop_enabled() && rr64_custom_cop_bike_count(4) == 5;
    ok &= rr64_local_options_visible(10, 0) == 1;
    // New setting defaults off, converts only unreserved AI police slots, and
    // leaves the original pool intact when explicitly enabled.
    put(layout::humans,2); rr64_custom_cop_begin(rdram,1);
    recomp_context ai_context{}; ai_context.r29=guest_address(0x80600000);
    const unsigned ai_pool=0x80600070;
    put(ai_pool+5*4,3);put(ai_pool+7*4,2);ai_context.r21=2;
    rr64_custom_cop_ai_pool(rdram,&ai_context);
    ok &= get(ai_pool+5*4)==5 && get(ai_pool+7*4)==0 && ai_context.r21==0;
    put(layout::menu_cursor,10);put(layout::menu_buttons,0x40);
    ok &= rr64_local_options_input(rdram)==1;
    put(ai_pool+5*4,3);put(ai_pool+7*4,2);ai_context.r21=2;
    rr64_custom_cop_ai_pool(rdram,&ai_context);
    ok &= get(ai_pool+5*4)==3 && get(ai_pool+7*4)==2 && ai_context.r21==2;
    rr64_local_options_input(rdram); // back to default Off
    ok &= valid_online_options(512u|1024u|10u) && valid_online_options(2048u) &&
          valid_online_options(mk64_items_disabled_bit) && !valid_online_options(0x10000000u);
    reset_online();ok &= !(online_options()&1024u);
    apply_online_options(512u|1024u);ok &= online_options()==1536u;
    // Appended cop slot follows the effective bike list, not the map's list.
    put(0x800A6690, 0); put(0x800A66B8, 4); put(0x800A66B8 + 5 * 4, 1);
    for (unsigned i = 0; i < 4; ++i) {
        put(0x8009EF3C + i * 4, 0);
        ok &= rr64_custom_cop_bike_entry(rdram, i, 123) == 123;
        put(0x8009EF3C + i * 4, 1);
        ok &= rr64_custom_cop_bike_entry(rdram, i, 123) == 0x800A684C;
    }
    put(0x800A77D8, 0);
    for (unsigned humans = 1; humans <= 4; ++humans) {
        put(layout::humans, humans); put(0x800A656C, humans + 1);
        for (unsigned mask = 0; mask < (1u << humans); ++mask) {
            rr64_local_options_race(rdram);
            for (unsigned i = 0; i < humans; ++i) {
                const unsigned a = 0x800D8570 + i * 0x118;
                put(a + 0x18, 31); put(a + 0x1C, mask & (1u << i) ? 40 : 0);
                put(a + 4, i); put(a + 8, i); put(a + 0x108, 0x12340000 + i);
            }
            const unsigned ai = 0x800D8570 + humans * 0x118;
            put(ai + 0x20, 1);
            rr64_custom_cop_roles(rdram);
            for (unsigned i = 0; i < humans; ++i) {
                const unsigned a = 0x800D8570 + i * 0x118;
                ok &= (get(a + 0x20) == 7) == bool(mask & (1u << i));
                ok &= get(a + 4) == i && get(a + 8) == i && get(a + 0x108) == 0x12340000 + i;
                ok &= bool(rr64_custom_cop_arrest(rdram, a, ai)) == bool(mask & (1u << i));
                ok &= !rr64_custom_cop_arrest(rdram, a, a);
            }
            put(0x800D764C, 1); ok &= !rr64_custom_cop_finished(rdram);
            put(0x800D764C, 0); ok &= bool(rr64_custom_cop_finished(rdram)) == (mask != 0);
            ok &= get(0x800A77D8) == 0;
        }
    }
    // Only active human slots count toward the required cop. A cop rider
    // and bike on different players must not satisfy the paired selection.
    for (unsigned humans = 1; humans <= 4; ++humans) {
        put(layout::menu_humans, humans);
        for (unsigned i = 0; i < 4; ++i) {
            put(0x8009F660 + i * 4, i < humans ? 0 : 31);
            put(0x8009F400 + i * 4, 40);
            put(0x8009EF2C + i * 4, 2);
        }
        ok &= !rr64_custom_cop_can_start(rdram) && !rr64_custom_cop_confirm(rdram);
        for (unsigned i = 0; i < humans; ++i) ok &= get(0x8009EF2C + i * 4) == 1;
        for (unsigned cop = 0; cop < humans; ++cop) {
            put(0x8009F660 + cop * 4, 31);
            ok &= rr64_custom_cop_can_start(rdram) && rr64_custom_cop_confirm(rdram);
            put(0x8009F660 + cop * 4, 0);
        }
    }
    // All cops with no AI is valid free play, even with no racers remaining.
    put(0x800A656C, 4); rr64_custom_cop_roles(rdram);
    ok &= !rr64_custom_cop_finished(rdram);
    // Every police profile remains selectable on each local controller.
    put(layout::menu_humans, 4); put(0x800A6690, 0); put(0x800A66B8, 4);
    for (unsigned i = 0; i < 4; ++i) {
        put(0x8009EF3C + i * 4, 1); // Cop slot after the single Scooter.
        put(0x8009E238 + i * 4, 8);
        ok &= rr64_custom_cop_rider(rdram, i, 0) == 40;
        for (unsigned variant = 40; variant <= 44; ++variant)
            ok &= rr64_custom_cop_rider(rdram, i, variant) == variant;
        put(0x8009E238 + i * 4, 0x10);
        ok &= rr64_custom_cop_rider(rdram, i, 41) == 41;
        ok &= rr64_custom_cop_rider(rdram, i, 39) == 44;
    }
    const unsigned cop = 0x800D8570, target = cop + 0x118;
    put(cop + 0x20, 7); put(target + 0x20, 5);
    put(cop + 0xE0, 0x80200000); put(cop + 0xE4, 0x80201000);
    MEM_H(0x83C,guest_address(0x80200000))=1;
    rr64_custom_cop_equipment(rdram, cop);
    ok &= MEM_HU(0x83C,guest_address(0x80200000))==0;
    ok &= MEM_HU(0x842, guest_address(0x80200000)) == 1;
    ok &= get(0x802015B0) == 5;
    put(0x80300000, 0); put(0x80300004, 0);
    rr64_custom_cop_spawn(rdram, 0, 0x80300000);
    ok &= get(0x80300000) == 0x41600000 && get(0x80300004) == 0x40F00000;
    rr64_custom_cop_spawn(rdram, 1, 0x80300000);
    ok &= get(0x80300000) == 0x41600000; // Racer's offsets untouched.
    put(target + 0xE8, 0x80202000);
    MEM_H(0x4C, guest_address(0x80202000)) = 1;
    MEM_H(0x88, guest_address(cop)) = 1;
    MEM_H(0x88, guest_address(target)) = 1;
    MEM_B(0xC, guest_address(target)) = 'T'; MEM_B(0xD, guest_address(target)) = 0;
    rr64_custom_cop_bust_message(rdram, cop, target);
    ok &= MEM_B(0x92, guest_address(cop)) == 'B';
    MEM_B(0x92, guest_address(cop)) = 'K';
    rr64_custom_cop_notification(rdram, cop + 0x2C, 3, target + 0xC);
    ok &= MEM_B(0x92, guest_address(cop)) == 'B';
    // Failed roadside attempts must restore route state and ranking; successful
    // posting triggers only after a non-cop passes and throttles voice playback.
    put(cop+0xE8,0x80203000);put(target+0xE8,0x80203100);
    put(0x80203000,77);put(0x80203040,0);put(0x800D77A4,0);
    put(0x800D762C,0x447A0000); // length 1000
    recomp_context context{};context.r29=guest_address(0x80700000);
    rr64_custom_cop_post(rdram,&context,0);
    ok &= get(0x80203000)==77 && get(0x800D77A4)==0;
    placement_ok=true;rr64_custom_cop_post(rdram,&context,0);
    ok &= rr64_custom_cop_pursuit(rdram,0x80200000)==0;
    context.r4=guest_address(cop);context.r6=0x2000;context.r5=0x2000;
    context.r7=0x3F800000;MEM_W(16,context.r29)=0x3F800000;
    rr64_custom_cop_control(rdram,&context);
    ok &= context.r6==0x10 && context.r7==0 && MEM_W(16,context.r29)==0;
    ok &= MEM_HU(0x81C,guest_address(0x80200000))==1;
    MEM_H(0x48,guest_address(0x80203100))=1;
    put(0x80203120,0x43960000); // racer at 300, cop posted at 200
    ok &= rr64_custom_cop_pursuit(rdram,0x80200000)==1;
    ok &= rr64_custom_cop_siren(rdram,0x80200000)==0;
    put(0x800A1820,0);context.r6=0x20;context.r5=0x20;
    rr64_custom_cop_control(rdram,&context);
    ok &= !(context.r6&0x20) && rr64_custom_cop_siren(rdram,0x80200000)==0;
    put(0x800A1820,0x3E800000);context.r6=0;context.r5=0; // quarter-second tap
    rr64_custom_cop_control(rdram,&context);
    ok &= (context.r6&context.r5&0x20)!=0;
    context.r6=0x20;context.r5=0x20;rr64_custom_cop_control(rdram,&context);
    put(0x800A1820,0x3FA00000);context.r6=0x20;context.r5=0;
    rr64_custom_cop_control(rdram,&context);
    ok &= rr64_custom_cop_siren(rdram,0x80200000)==1 && !(context.r6&0x20);
    put(0x800A1820,0x40000000);context.r6=0x20; // continuing to hold must not toggle twice
    rr64_custom_cop_control(rdram,&context);
    ok &= rr64_custom_cop_siren(rdram,0x80200000)==1;
    context.r6=0;rr64_custom_cop_control(rdram,&context);
    ok &= rr64_custom_cop_siren(rdram,0x80200000)==1 && !(context.r6&0x20);
    context.r6=0x20;context.r5=0x20;rr64_custom_cop_control(rdram,&context);
    put(0x800A1820,0x40400000);context.r6=0x20;context.r5=0;
    rr64_custom_cop_control(rdram,&context);
    ok &= rr64_custom_cop_siren(rdram,0x80200000)==0;
    context.r6=0;rr64_custom_cop_control(rdram,&context);
    ok &= !(context.r6&0x20);
    context.r6=4;context.r5=4;rr64_custom_cop_control(rdram,&context);
    ok &= !rr64_custom_cop_trick(rdram,cop) && (context.r6&4) && (context.r5&4) && !(context.r6&0x20);
    context.r6=rr64_cop_weapon_trick_button;context.r5=rr64_cop_weapon_trick_button;rr64_custom_cop_control(rdram,&context);
    ok &= !(context.r6&rr64_cop_weapon_trick_button) && !(context.r5&rr64_cop_weapon_trick_button);
    ok &= rr64_custom_cop_trick(rdram,cop) && (context.r6&0x20) && !(context.r6&4);
    context.r6=0;context.r5=0;rr64_custom_cop_control(rdram,&context);
    ok &= !rr64_custom_cop_trick(rdram,cop);
    // Spoke jam must survive the shared cop input adapter, including edges.
    context.r6=5;context.r5=5;rr64_custom_cop_control(rdram,&context);
    ok &= (context.r6&5)==5 && (context.r5&5)==5 && !rr64_custom_cop_trick(rdram,cop);
    put(0x800D7670,0x3F800000);
    // The original eligible-racer flag stays zero for cops, without blocking recovery.
    put(0x8020304C,0);MEM_H(0x48,guest_address(0x80203000))=0;
    put(0x802004F8,0x42C80000);
    put(0x800048EC,0x40A00000); // Fixture stock delay: five simulation seconds.
    for (unsigned age : {0u,0x3F800000u,0x409FFFFFu,0x40A00000u}) {
        put(0x802004CC,age);
        ok &= !rr64_custom_cop_can_recover(rdram,cop);
    }
    put(0x802004CC,0x40A00001); // Stock comparison is strictly greater-than.
    ok &= rr64_custom_cop_can_recover(rdram,cop) && get(0x80203048)==0;
    put(0x800048EC,0x40C00000); // Follow game data, not a hard-coded five.
    ok &= !rr64_custom_cop_can_recover(rdram,cop);
    put(0x800048EC,0x40A00000);
    put(0x802004F8,0xBF800000);ok &= !rr64_custom_cop_can_recover(rdram,cop);
    put(0x802004F8,0x42C80000);put(0x8020304C,1);
    ok &= !rr64_custom_cop_can_recover(rdram,cop);
    put(0x8020304C,0);
    MEM_H(0x57C,guest_address(0x80201000))=1;MEM_H(0x7F6,guest_address(0x80200000))=0;
    ok &= rr64_custom_cop_roaming(rdram,cop) && !rr64_custom_cop_roaming(rdram,target);
    MEM_H(0x7F6,guest_address(0x80200000))=1;
    ok &= !rr64_custom_cop_roaming(rdram,cop);
    MEM_H(0x7F6,guest_address(0x80200000))=0;MEM_H(0x57C,guest_address(0x80201000))=0;
    ok &= !rr64_custom_cop_roaming(rdram,cop);
    // Victory waits for all opponents, then allows the stock results after 3s.
    put(0x800A656C,2);put(0x800A6578,1);rr64_custom_cop_roles(rdram);
    put(0x800D764C,0);MEM_H(0x4C,guest_address(0x80203100))=0;
    ok &= rr64_custom_cop_win_age(rdram)<0 && rr64_custom_cop_finished(rdram);
    MEM_H(0x4C,guest_address(0x80203100))=1;
    ok &= rr64_custom_cop_win_age(rdram)==0 && !rr64_custom_cop_finished(rdram);
    put(0x800D7670,0x40800000); // 4 seconds, announcement began at 1
    ok &= rr64_custom_cop_finished(rdram);
    // AI police are allies, not an impossible extra arrest requirement.
    const unsigned ai_cop=0x800D8570+2*0x118;
    put(ai_cop+0x20,7);put(0x800A656C,3);
    rr64_custom_cop_begin(rdram,1);rr64_custom_cop_roles(rdram);
    MEM_H(0x4C,guest_address(0x80203100))=0;
    put(0x800D764C,1);
    ok &= rr64_custom_cop_win_age(rdram)<0 && !rr64_custom_cop_finished(rdram);
    MEM_H(0x4C,guest_address(0x80203100))=1;
    ok &= rr64_custom_cop_win_age(rdram)==0 && !rr64_custom_cop_finished(rdram);
    put(0x800D7670,0x40E00000); // three seconds after the announcement at four
    ok &= rr64_custom_cop_finished(rdram);
    rr64_custom_cop_reset(); ok &= !rr64_custom_cop_active();
    ok &= rr64_custom_cop_pursuit(rdram,0x80200000)==-1;
    // Each populated local slot can roam, including non-cops. Exercise the
    // negative recovery paths independently of the selection/menu settings.
    for (unsigned humans = 1; humans <= 4; ++humans) {
        put(layout::humans, humans);
        for (unsigned slot = 0; slot < 4; ++slot) {
            const unsigned actor = 0x800D8570 + slot * 0x118;
            const unsigned bike = 0x80300000 + slot * 0x1000;
            const unsigned rider = 0x80310000 + slot * 0x1000;
            const unsigned state = 0x80320000 + slot * 0x100;
            put(actor + 0xE0, bike); put(actor + 0xE4, rider); put(actor + 0xE8, state);
            put(bike + 0x4F8, 0x42C80000); put(bike + 0x858, 0x41200000);
            MEM_H(0x57C, guest_address(rider)) = 1;
            MEM_H(0x7F6, guest_address(bike)) = 0;
            MEM_H(0x4C, guest_address(state)) = 0;
            ok &= bool(rr64_local_player_roaming(rdram, actor)) == (slot < humans);
            ok &= get(bike + 0x858) == (slot < humans ? 0u : 0x41200000u);
            MEM_H(0x7F6, guest_address(bike)) = 1;
            ok &= !rr64_local_player_roaming(rdram, actor);
            MEM_H(0x7F6, guest_address(bike)) = 0;
            MEM_H(0x57C, guest_address(rider)) = 0;
            ok &= !rr64_local_player_roaming(rdram, actor);
            MEM_H(0x57C, guest_address(rider)) = 1;
            MEM_H(0x4C, guest_address(state)) = 1;
            ok &= !rr64_local_player_roaming(rdram, actor);
            MEM_H(0x4C, guest_address(state)) = 0;
            for (unsigned health : {0xBF800000u, 0x7FC00000u, 0x7F800000u}) {
                put(bike + 0x4F8, health);
                ok &= !rr64_local_player_roaming(rdram, actor);
            }
            put(bike + 0x4F8, 0x42C80000);
            ok &= !rr64_local_player_roaming(rdram, actor + 1);
        }
        ok &= !rr64_local_player_roaming(rdram, 0x800D8570 + humans * 0x118);
    }
    rr64::local_players::active.store(false);
    ok &= !rr64_local_player_roaming(rdram, 0x800D8570);
    rr64::local_players::active.store(true);
    for (unsigned humans : {0u, 5u, 0xFFFFFFFFu}) {
        put(layout::humans, humans);
        ok &= !rr64_local_player_roaming(rdram, 0x800D8570);
    }
    put(layout::humans, 4);
    online = true;
    ok &= !rr64_local_player_roaming(rdram, 0x800D8570);
    ok &= rr64_local_bike_profile(rdram, 0x800D8570, original) == original;
    put(layout::racers, 6); rr64_local_options_race(rdram);
    ok &= get(layout::racers) == 6 && rr64_local_bike_level(4) == 4;
    rr64_local_options_input(rdram);
    ok &= rr64_local_options_navigation(rdram) == -1;
    ok &= rr64_local_options_table(layout::menu_table) == layout::menu_table;
    // Transport is disconnected above, but historical authority rules must
    // still govern replay. One camera does not limit the human actor pool.
    put(layout::humans, 1);
    for (bool host : {false, true}) for (unsigned slot = 0; slot < 14; ++slot) {
        rr64::prediction::SessionRules rules{true,true,true,host,true,0,1u << slot};
        rr64::prediction::ReplayScope replay(rules);
        const unsigned actor = 0x800D8570 + slot * 0x118;
        const unsigned bike = 0x80400000, rider = 0x80401000, state = 0x80402000;
        put(actor + 0xE0, bike); put(actor + 0xE4, rider); put(actor + 0xE8, state);
        put(bike + 0x4F8, 0x42C80000); put(bike + 0x858, 0x41200000);
        MEM_H(0x57C, guest_address(rider)) = 1;
        MEM_H(0x7F6, guest_address(bike)) = 0;
        MEM_H(0x4C, guest_address(state)) = 0;
        ok &= rr64_local_player_roaming(rdram, actor) == 1 && get(bike + 0x858) == 0;
        ok &= !rr64_local_player_roaming(rdram, 0x800D8570 + ((slot + 1) % 14) * 0x118);
        MEM_H(0x7F6, guest_address(bike)) = 1;
        ok &= !rr64_local_player_roaming(rdram, actor);
        MEM_H(0x7F6, guest_address(bike)) = 0;
        MEM_H(0x57C, guest_address(rider)) = 0;
        ok &= !rr64_local_player_roaming(rdram, actor);
        MEM_H(0x57C, guest_address(rider)) = 1;
        MEM_H(0x4C, guest_address(state)) = 1;
        ok &= !rr64_local_player_roaming(rdram, actor);
    }
    // The single-player preset and selector have their own ownership. Exercise
    // these bridges with local multiplayer explicitly disabled and its menu
    // arrays left populated, as after returning from a four-player race.
    unsigned solo_checks = 0;
    const auto check_solo = [&](bool condition, const char* description) {
        ++solo_checks;
        if (!condition) {
            std::printf("FAIL Thrash: %s\n", description);
            ok = false;
        }
    };
    online = online_connected = false;
    rr64::local_players::active.store(false);
    rr64_thrash_options_mode(32);
    rr64_local_options_reset_race();
    reset_online();
    const unsigned multiplayer_preset = online_options();
    const auto enter_thrash = [&] {
        rr64_thrash_options_mode(33);
        put(globals::main_mode, 33);
        rr64_thrash_options_begin(rdram);
    };
    check_solo(thrash_options() == 10, "first solo preset retains ten stock AI racers");
    check_solo(thrash_stock_options() == unknown_thrash_stock,
               "first visit has no saved stock options");
    check_solo(!rr64_thrash_options_active(), "main menu clears solo ownership");
    enter_thrash();
    check_solo(rr64_thrash_options_active(), "native mode 33 enters offline Thrash");
    check_solo(!rr64::local_players::active.load(), "solo context does not enable multiplayer");
    put(0x8009ECA0, 0); // Native options stage, before character selection.
    put(layout::menu_cursor, 4); put(layout::menu_buttons, 0);
    put(0x8009EABC, 2); put(0x8009EAC4, 3); put(0x8009EAC8, 1);
    rr64_thrash_options_input(rdram);
    check_solo(get(0x8009EABC) == 2 && get(0x8009EAC4) == 3 && get(0x8009EAC8) == 1,
               "first visit preserves unsaved native difficulty/traffic/police");
    put(layout::menu_humans, 4); put(layout::humans, 1);
    put(0x8009EAC0, 3); // Original density is not the new 0..10 count.
    put(layout::menu_buttons, 0x40);
    for (unsigned ai = 0; ai <= 10; ++ai) {
        check_solo(rr64_thrash_options_input(rdram) == 1 && (thrash_options() & 15u) == ai,
                   "solo AI row cycles all eleven counts despite stale four-player menu");
        check_solo(get(0x8009EAC0) == 3, "AI row never writes extended count into native density");
    }
    const unsigned solo_table = rr64_thrash_options_table(0x8009EB4C);
    check_solo(solo_table != 0x8009EB4C && get(solo_table + 11 * 36) == 0,
               "solo menu uses an extended table with bounded terminator");
    check_solo(get(solo_table + 10 * 36) == 0, "AI-cop row stays hidden when custom cop is off");
    put(layout::menu_cursor, 9); put(layout::menu_buttons, 0x10);
    check_solo(rr64_thrash_options_navigation(rdram) == 0,
               "navigation wraps past hidden AI-cop row");
    put(layout::menu_cursor, 9); put(layout::menu_buttons, 0x40);
    rr64_thrash_options_input(rdram);
    check_solo((thrash_options() & 512u) && get(solo_table + 10 * 36) != 0,
               "enabling custom cop exposes its AI-cop row immediately");
    put(layout::menu_buttons, 0x10);
    check_solo(rr64_thrash_options_navigation(rdram) == 10,
               "navigation reaches enabled AI-cop row");
    put(0x80300018, 0xBADF00D);
    rr64_thrash_options_text(rdram, 8, 0x80300000);
    check_solo(MEM_B(0, guest_address(0x80300000)) == 'B' && get(0x80300018) == 0xBADF00D,
               "solo label fits the native stack buffer");
    put(0x8009ECA0, 1); put(layout::menu_buttons, 0x40);
    const unsigned before_selector_options = thrash_options();
    const auto before_selector_memory = memory;
    check_solo(!rr64_thrash_options_input(rdram) &&
                   rr64_thrash_options_navigation(rdram) == -1 &&
                   rr64_thrash_options_table(0x8009EB4C) == 0x8009EB4C,
               "options adapter is dormant during native character selection");
    check_solo(memory == before_selector_memory && thrash_options() == before_selector_options,
               "character selection input cannot mutate race options");
    for (unsigned mode : {17u, 18u, 19u, 20u, 21u}) {
        rr64_thrash_options_mode(mode);
        check_solo(rr64_thrash_options_active(), "Thrash selector/race/results retain ownership");
    }
    // These are distinct preferences even while the solo selector is active.
    set_thrash_options(512u | (6u << 6) | 10u);
    const unsigned solo_preset = thrash_options();
    // Bit 27 is a valid online rule; the remembered item preference has its
    // own ownership and is intentionally stripped from the solo preset.
    const bool items_before_solo_preset = mk64_items_enabled();
    set_thrash_options(solo_preset | mk64_items_disabled_bit);
    check_solo(thrash_options() == solo_preset, "online item rule is stripped from solo preset");
    check_solo(mk64_items_enabled() == items_before_solo_preset,
               "solo preset cannot change remembered item preference");
    for (unsigned invalid : {11u, 15u, 0x10000000u, 0xFFFFFFFFu}) {
        set_thrash_options(invalid);
        check_solo(thrash_options() == solo_preset, "invalid solo bits do not replace preset");
    }
    set_thrash_stock_options(4u | (3u << 3) | (2u << 5));
    const unsigned solo_stock_preset = thrash_stock_options();
    for (unsigned invalid : {5u, 7u, 128u, 0xFFFFFFFFu}) {
        set_thrash_stock_options(invalid);
        check_solo(thrash_stock_options() == solo_stock_preset,
                   "invalid stock bits do not replace preset");
    }
    enter_thrash();
    put(0x8009ECA0, 0); put(layout::menu_cursor, 3); put(layout::menu_buttons, 0);
    put(0x8009EABC, 0); put(0x8009EAC4, 0); put(0x8009EAC8, 0);
    rr64_thrash_options_input(rdram);
    check_solo(get(0x8009EABC) == 4 && get(0x8009EAC4) == 3 && get(0x8009EAC8) == 2,
               "reentering Thrash restores its independent stock settings");
    put(0x8009EABC, 1); put(0x8009EAC4, 2); put(0x8009EAC8, 3); put(0x8009EACC, 1);
    rr64_thrash_options_finish(rdram);
    check_solo(thrash_stock_options() == (1u | (2u << 3) | (3u << 5)) &&
                   ((thrash_options() >> 4) & 3u) == 1,
               "finishing menu captures native stock choices in solo preset");
    reset_online();
    check_solo(online_options() == multiplayer_preset, "solo settings preserve multiplayer preset");
    apply_online_options(3u | (2u << 6));
    check_solo(thrash_options() == (solo_preset | 16u), "session settings preserve solo preset");

    put(layout::humans, 1);
    put(layout::menu_humans, 4); // Stale local multiplayer must not own this roster.
    for (unsigned ai = 0; ai <= 10; ++ai) {
        set_thrash_options(ai);
        put(layout::racers, 99);
        rr64_local_options_race(rdram);
        check_solo(get(layout::racers) == ai + 1, "solo race uses exactly one human plus chosen AI");
        check_solo(get(layout::humans) == 1 && get(layout::menu_humans) == 4,
                   "solo race leaves human/controller ownership intact");
        set_thrash_options(512u | ai);
        put(layout::racers, 99);
        rr64_local_options_race(rdram);
        check_solo(get(layout::racers) == ai + 1 && rr64_custom_cop_active(),
                   "custom cop supports every solo AI count");
    }
    for (unsigned choice = 0; choice <= 7; ++choice) {
        set_thrash_options(choice << 6);
        const unsigned expected_level = choice ? choice - 1 : 3;
        check_solo(rr64_local_bike_menu_level(3) == expected_level,
                   "every solo bike preset drives native selection preview");
        rr64_local_options_race(rdram);
        check_solo(rr64_local_bike_level(3) == expected_level,
                   "every solo bike preset survives race initialization");
    }
    set_thrash_options(6u << 6);
    rr64_local_options_race(rdram);
    put(cop + 0x18, 23);
    const unsigned solo_profile = rr64_local_bike_profile(rdram, cop, original);
    check_solo(solo_profile != original && MEM_B(10, guest_address(solo_profile)) == 23 &&
                   MEM_B(13, guest_address(solo_profile)) == 38,
               "solo Scooter uses the stock donor bike and physics");
    check_solo(MEM_B(11, guest_address(solo_profile)) == 11 &&
                   MEM_B(14, guest_address(solo_profile)) == 14 &&
                   MEM_B(10, guest_address(original)) == 10,
               "solo bike substitution preserves rider stats and original profile");
    put(cop + 0x18, 13); // Unmatched committed model must not turn into Scooter.
    check_solo(rr64_local_bike_profile(rdram, cop, original) == original &&
                   get(cop + 0x18) == 13, "no unrelated donor fallback");
    set_thrash_options(512u | (6u << 6));
    rr64_local_options_race(rdram);
    put(cop + 0x18, 31);
    const auto before_cop_profile = memory;
    check_solo(rr64_local_bike_profile(rdram, cop, original) == original &&
                   memory == before_cop_profile,
               "special bike preset cannot replace committed solo police equipment");
    for (unsigned allow_ai_cops : {0u, 1024u}) {
        set_thrash_options(512u | 10u | allow_ai_cops);
        rr64_local_options_race(rdram);
        put(ai_pool + 5 * 4, 3); put(ai_pool + 7 * 4, 2); ai_context.r21 = 2;
        rr64_custom_cop_ai_pool(rdram, &ai_context);
        check_solo(get(ai_pool + 5 * 4) == (allow_ai_cops ? 3u : 5u) &&
                       get(ai_pool + 7 * 4) == (allow_ai_cops ? 2u : 0u) &&
                       ai_context.r21 == (allow_ai_cops ? 2u : 0u),
                   "solo AI-cop preference governs only unreserved AI profiles");
    }
    // The appended cop choice follows the native single selector A66B0, even
    // when every multiplayer cursor points to the opposite kind of bike.
    set_thrash_options(512u | (6u << 6));
    put(0x800A6690, 0);
    put(0x800A66B8, 4);
    put(0x800A66B8 + 5 * 4, 1); // Scooter effective tier has one stock bike.
    put(0x800A77D8, 0);
    for (unsigned count = 1; count <= 4; ++count) {
        put(0x800A66B8 + 5 * 4, count);
        for (unsigned slot = 0; slot < 4; ++slot)
            put(0x8009EF3C + slot * 4, count);
        put(0x800A66B0, 0);
        check_solo(rr64_custom_cop_bike_entry(rdram, 0, 123) == 123,
                   "stale multiplayer cop cursor cannot select solo cop bike");
        for (unsigned slot = 0; slot < 4; ++slot)
            put(0x8009EF3C + slot * 4, 0);
        put(0x800A66B0, count);
        check_solo(rr64_custom_cop_bike_entry(rdram, 0, 123) == 0x800A684C,
                   "native solo cursor appends cop after effective tier");
        check_solo(rr64_custom_cop_bike_count(count) == count + 1,
                   "solo cop extends each valid bike-list length");
        for (unsigned slot = 1; slot <= 4; ++slot)
            check_solo(rr64_custom_cop_bike_entry(rdram, slot, 123) == 123,
                       "solo cop entry is owned only by player zero");
    }
    for (unsigned invalid : {0u, 5u, 0xFFFFFFFFu}) {
        put(0x800A66B8 + 5 * 4, invalid);
        put(0x800A66B0, invalid);
        check_solo(rr64_custom_cop_bike_entry(rdram, 0, 123) == 123,
                   "invalid effective bike-list bounds reject appended entry");
    }
    put(0x800A66B8 + 5 * 4, 1);
    put(0x800A66B0, 1);
    check_solo(rr64_custom_cop_rider(rdram, 0, 0) == 40 &&
                   rr64_custom_cop_rider(rdram, 0, 39) == 44,
               "cop selection wraps both ends of native five-rider list");
    for (unsigned slot = 0; slot < 4; ++slot) {
        put(0x8009F660 + slot * 4, 31);
        put(0x8009F400 + slot * 4, 44);
        put(0x8009EF2C + slot * 4, 2);
    }
    // At 26F1C the native selector has committed F660/F400. The pending menu
    // F670 value and stale multiplayer count/ready rows are not confirmation.
    for (unsigned stale_humans : {0u, 1u, 4u, 0xFFFFFFFFu}) {
        put(layout::menu_humans, stale_humans);
        for (unsigned variant = 40; variant <= 44; ++variant) {
            put(0x8009F660, 31); put(0x8009F400, variant); put(0x8009F670, 0);
            const auto before = memory;
            check_solo(rr64_custom_cop_rider(rdram, 0, variant) == variant,
                       "all five native solo police profiles remain selectable");
            check_solo(rr64_custom_cop_can_start(rdram) && rr64_custom_cop_confirm(rdram),
                       "committed solo cop starts independently of multiplayer rows");
            check_solo(memory == before, "successful solo selection checks never write guest memory");
        }
        for (const auto selection : {std::pair{0u, 40u}, std::pair{31u, 0u},
                                    std::pair{31u, 39u}, std::pair{31u, 45u}}) {
            put(0x8009F660, selection.first); put(0x8009F400, selection.second);
            put(0x8009F670, 40); // A pending cop does not repair a non-cop commit.
            const auto before = memory;
            check_solo(!rr64_custom_cop_can_start(rdram) && !rr64_custom_cop_confirm(rdram),
                       "cop bike and rider must both belong to the sole human");
            check_solo(memory == before, "rejected solo start leaves multiplayer ready rows untouched");
        }
    }
    put(0x800A66B0, 0);
    check_solo(rr64_custom_cop_rider(rdram, 0, 44) == 0,
               "locked police profile normalizes on an ordinary solo bike");
    MEM_H(0, guest_address(0x800A77D8)) = 1;
    check_solo(rr64_custom_cop_rider(rdram, 0, 44) == 44,
               "existing native police unlock still applies on an ordinary bike");
    check_solo(MEM_HU(0, guest_address(0x800A77D8)) == 1,
               "selection preserves native unlock value");
    put(0x800A77D8, 0);

    // Role setup remains functional with local multiplayer disabled. A cop
    // alone is free play, while a cop plus AI has the original arrest objective.
    for (unsigned ai : {0u, 1u, 10u}) {
        set_thrash_options(512u | ai);
        put(layout::humans, 1); put(0x800A656C, ai + 1);
        put(cop + 0x18, 31); put(cop + 0x1C, 40); put(cop + 4, 0);
        put(cop + 8, 0); put(cop + 0x108, 0x12345678);
        for (unsigned slot = 1; slot <= ai; ++slot)
            put(cop + slot * 0x118 + 0x20, 5);
        rr64_local_options_race(rdram);
        rr64_custom_cop_roles(rdram);
        put(0x800D764C, ai);
        check_solo(rr64_custom_cop_active() && get(cop + 0x20) == 7,
                   "solo selection becomes a native police role");
        check_solo(get(cop + 4) == 0 && get(cop + 8) == 0 && get(cop + 0x108) == 0x12345678,
                   "role setup preserves sole controller, viewport and profile");
        check_solo(!rr64_custom_cop_finished(rdram), "zero-AI free play or unbusted AI does not finish");
        check_solo(bool(rr64_custom_cop_arrest(rdram, cop, target)) == (ai != 0),
                   "solo arrest requires an existing opposing racer");
    }

    // Leaving Thrash drops all custom ownership before any shared native race
    // helper can see Big Game, demos, or another menu. No new unlocks, roster,
    // selections, or profile changes are permitted in those contexts.
    for (unsigned mode : {0u, 9u, 10u, 22u, 23u, 24u, 25u, 28u, 29u, 30u, 32u, 34u}) {
        enter_thrash();
        set_thrash_options(512u | (7u << 6) | 10u);
        rr64_thrash_options_mode(mode);
        put(globals::main_mode, mode);
        put(layout::humans, 1); put(layout::racers, 6);
        const auto before = memory;
        check_solo(!rr64_thrash_options_active() && !rr64_custom_cop_enabled(),
                   "campaign/demo/non-Thrash mode clears custom ownership");
        rr64_local_options_race(rdram);
        check_solo(!rr64_custom_cop_active() && rr64_custom_cop_can_start(rdram),
                   "campaign/demo never require custom cop equipment");
        check_solo(rr64_local_bike_menu_level(3) == 3 && rr64_local_bike_level(3) == 3 &&
                       rr64_local_bike_profile(rdram, cop, original) == original,
                   "campaign/demo preserve stock bike lists and profiles");
        check_solo(rr64_custom_cop_bike_entry(rdram, 0, 123) == 123 &&
                       rr64_custom_cop_rider(rdram, 0, 44) == 44,
                   "campaign/demo preserve stock selection behavior");
        check_solo(memory == before, "campaign/demo shared helpers do not write guest memory");
    }
    enter_thrash();
    online = online_connected = true;
    check_solo(!rr64_thrash_options_active(), "online session suppresses solo context");
    rr64_thrash_options_mode(33);
    online = online_connected = false;
    check_solo(!rr64_thrash_options_active(), "online mode entry cannot arm a later offline context");
    reset_online();
    check_solo(online_options() == multiplayer_preset, "solo session leaves multiplayer preset unchanged");
    // The music preference follows MK64 course changes, never pedestrian
    // density. Retain historical bit assignments and host-only online edits.
    unsigned all_music = 0;
    for (auto id : music_courses) {
        const unsigned bit = course_music_bit(id);
        check_solo(bit && !(all_music & bit), "unique stable music preference bit");
        all_music |= bit;
        check_solo(valid_online_options(bit | 10u), "music option survives setup validator");
    }
    check_solo(all_music == 0x07FFF800u && !course_music_bit("stock"),
               "all16 courses covered, unknown course rejected");
    online = online_connected = online_host = true;
    apply_online_options(512u | 1024u | 10u);
    toggle_course_music("rainbow_road");
    check_solo(course_music_enabled("rainbow_road") && course_music_enabled("mario_raceway"),
               "music preference follows another course");
    check_solo((online_options() & 2047u) == 1546u, "music preserves gameplay options");
    const auto host_choices = online_options();
    online_host = false;
    toggle_course_music("rainbow_road");
    check_solo(online_options() == host_choices, "guest cannot toggle host race music");
    apply_online_options((host_choices & ~all_music) | course_music_bit("mario_raceway"));
    check_solo(course_music_enabled("rainbow_road"), "legacy host music choice follows another course");
    apply_online_options(0x10000000u);
    check_solo(course_music_enabled("mario_raceway"), "invalid setup ignored");
    online = online_connected = online_host = false;
    enter_thrash();
    const auto solo_before_music = thrash_options();
    toggle_course_music("bowsers_castle");
    check_solo(thrash_options() == (solo_before_music | course_music_mask),
               "Thrash has independent music preferences");
    rr64_thrash_options_mode(0);
    reset_online();
    check_solo(online_options() == multiplayer_preset, "solo and online music preserve local preset");
    rr64::local_players::active.store(true);
    put(layout::humans, 4); put(layout::menu_humans, 4);
    put(layout::menu_cursor, 10); put(layout::menu_buttons, 0);
    rr64_local_options_input(rdram);
    check_solo(rr64_local_options_visible(10, 0) == 1, "AI cops available without Custom Cop");
    for (unsigned toggle = 0; toggle < 2; ++toggle) {
        put(layout::menu_buttons, 0x40); rr64_local_options_input(rdram);
        reset_online();
        rr64_local_options_race(rdram);
        check_solo(get(0x800A6570) == ((online_options() & 1024u) ? 3u : 0u), "police allocated independently");
        check_solo(get(layout::racers) + get(0x800A6570) <= 14, "police fit native actor capacity");
    }
    put(layout::menu_cursor, 6); rr64_local_options_input(rdram);
    const auto traffic = get(0x8009EAD8);
    put(layout::menu_cursor, 7); rr64_local_options_input(rdram);
    check_solo(get(0x8009EAD8) == traffic, "pedestrian edit preserves traffic");
    const auto peds = get(layout::pedestrian_choice);
    put(layout::menu_cursor, 6); rr64_local_options_input(rdram);
    check_solo(get(layout::pedestrian_choice) == peds && get(0x8009EAD8) != traffic, "traffic edit preserves pedestrians");
    put(globals::main_mode, 0x1C); put(globals::pending_mode, 0x1C);
    put(0x800A656C, 7);
    for (unsigned i = 0; i < 4; ++i) {
        put(0x800D8570 + i * 0x118 + 0xE8, 0x80310000 + i * 0x100);
        write_u16(rdram, 0x80310048 + i * 0x100, 1);
        write_u16(rdram, 0x8031004C + i * 0x100, 0);
        write_u16(rdram, 0x8031004E + i * 0x100, 0);
        write_u16(rdram, 0x80310050 + i * 0x100, 0);
        put(0x800A657C + i * 4, i); put(0x800A4FA0 + i * 4, 3);
    }
    check_solo(!rr64_local_spectator_begin(rdram, 0), "active player keeps own camera");
    write_u16(rdram, 0x8031004C, 1); write_u16(rdram, 0x80310048, 0);
    write_u16(rdram, 0x80310150, 1); // Player 2 finished; watch Player 3.
    const auto spectator_before = memory;
    auto token = rr64_local_spectator_begin(rdram, 0);
    check_solo(token && get(0x800A657C) == 2 && get(0x800A4FA0) == 5, "busted player follows unfinished survivor");
    rr64_local_spectator_end(rdram, 0, token);
    check_solo(memory == spectator_before, "camera scope restores all identity and mode fields");
    highlights = true;
    check_solo(!rr64_local_spectator_begin(rdram, 0), "highlights own their camera");
    highlights = false; online = true;
    check_solo(!rr64_local_spectator_begin(rdram, 0), "online view ownership unchanged");
    online = false;
    put(globals::pending_mode, 0x1E);
    check_solo(!rr64_local_spectator_begin(rdram, 0), "results transition preserves original identity");
    put(globals::pending_mode, 0x1C);
    write_u16(rdram, 0x8031024C, 1); write_u16(rdram, 0x8031034E, 1);
    check_solo(!rr64_local_spectator_begin(rdram, 0), "no spectator target when everyone is done");
    for (unsigned humans = 2; humans <= 4; ++humans) {
        put(layout::humans, humans);
        for (unsigned view = 0; view < humans; ++view) {
            for (unsigned i = 0; i < humans; ++i) {
                write_u16(rdram, 0x80310048 + i * 0x100, i != view);
                write_u16(rdram, 0x8031004C + i * 0x100, i == view);
                write_u16(rdram, 0x8031004E + i * 0x100, 0);
                write_u16(rdram, 0x80310050 + i * 0x100, 0);
            }
            const auto before = memory;
            token = rr64_local_spectator_begin(rdram, view);
            check_solo(token && get(0x800A657C + view * 4) == (view + 1) % humans,
                       "every split-screen slot can spectate a survivor");
            rr64_local_spectator_end(rdram, view, token);
            check_solo(memory == before, "every spectator scope restores player ownership");
        }
    }
    std::printf("Thrash regression checks: %u\n", solo_checks);
    std::puts(ok ? "PASS local roster/menu, solo Thrash and online replay roaming" : "FAIL local race options");
    return ok ? 0 : 1;
}
