#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_prediction_replay.hpp"

#include <cstdio>
#include <cmath>
#include <vector>
extern "C" void test_combat_damage_left(unsigned char *, recomp_context *);
extern "C" void test_combat_damage_right(unsigned char *, recomp_context *);
extern "C" void test_combat_force(unsigned char *, recomp_context *);
extern "C" void test_gravity_rider_special(unsigned char *, recomp_context *);
extern "C" void test_gravity_rider(unsigned char *, recomp_context *);
extern "C" void test_gravity_bike_special(unsigned char *, recomp_context *);
extern "C" void test_gravity_bike_blend(unsigned char *, recomp_context *);
extern "C" void test_gravity_bike(unsigned char *, recomp_context *);

namespace {
bool session = true, highlights = false;
rr64::netplay::PhysicsRules rules{};
constexpr unsigned actor = 0x800D8570u + 0x118u;
constexpr unsigned bike_address = 0x80100000u;
constexpr unsigned rider_address = 0x80101000u;
constexpr unsigned route = 0x80102000u;
int failures = 0;
void check(bool value, const char *label) {
    if (!value) {
        std::fprintf(stderr, "[OFFLINE-OPPONENTS] FAILED: %s\n", label);
        ++failures;
    }
}
void seed(unsigned char *memory) {
    using namespace rr64::engine;
    write_u32(memory, globals::main_mode, 0x09);
    write_u32(memory, globals::pending_mode, 0x09);
    write_u16(memory, actor + 0x24, 1);
    write_u16(memory, actor + 0x26, 1);
    write_u32(memory, actor + 8, 0xFFFFFFFFu);
    write_u32(memory, actor + 0xE0, bike_address);
    write_u32(memory, actor + 0xE4, rider_address);
    write_u32(memory, actor + 0xE8, route);
    write_u32(memory, bike_address + 4, actor);
    write_u32(memory, rider_address + 4, actor);
    write_u32(memory, bike_address + bike::rider_pointer, rider_address);
    write_u32(memory, rider_address + rider::bike_pointer, bike_address);
    write_u16(memory, bike_address + bike::rider_attached, 1);
    write_u16(memory, rider_address + rider::bike_attached, 1);
    for (unsigned offset : {0x178u, 0x17Cu, 0x180u, 0x184u, 0x1A0u, 0x1A4u, 0x1A8u})
        write_float(memory, bike_address + offset, 23.5f);
    write_float(memory, bike_address + bike::body_position, 314.5f);
    write_float(memory, route + 0x20, 123.5f);
}
}

// Only external session and unrelated cheat services are stubbed. The fixture
// executes the production preference, memory-ownership and opponent guards.
namespace rr64::netplay {
PhysicsRules get_physics_rules() { return rules; }
}
namespace rr64::offline_modifiers {
bool grant_max_weapons(unsigned char *, unsigned, bool) { return false; }
bool rider_damage_protected(unsigned char *, std::uint32_t, bool) noexcept { return false; }
bool bike_damage_protected(unsigned char *, std::uint32_t, bool) noexcept { return false; }
}
extern "C" int rr64_player_session_started() { return session; }
extern "C" int rr64_highlights_presenting() { return highlights; }
extern "C" void rr64_offline_bikes_reset() {}

int main() {
    using namespace rr64::engine;
    using namespace rr64::offline_modifiers;
    std::vector<unsigned char> memory(kRdramSize), shadow(kRdramSize);
    auto *m = memory.data();
    reset(m);
    seed(m);
    auto before = memory;
    check(!rr64_offline_modifiers_freeze_actor(m, actor) && memory == before,
          "disabled option is a complete no-op");
    set_requested(Flag::FreezeOpponents, true);
    check(enabled_any(), "new cheat participates in existing cheat-session eligibility");
    check(rr64_offline_modifiers_freeze_actor(m, actor), "mounted AI control and physics may hold");
    auto expected = before;
    for (unsigned offset : {0x178u, 0x17Cu, 0x180u, 0x184u, 0x1A0u, 0x1A4u, 0x1A8u})
        write_float(expected.data(), bike_address + offset, 0.f);
    check(memory == expected, "only speed changes; position, route, rank and locks preserved");

    const auto rejected = [&](const char *label) {
        const auto snapshot = memory;
        check(!rr64_offline_modifiers_freeze_actor(m, actor) && memory == snapshot, label);
    };
    set_requested(Flag::FreezeOpponents, false);
    rejected("toggle off immediately resumes native passes with no stale restoration");
    check(!enabled_any(), "no stale requested flag remains");
    set_requested(Flag::FreezeOpponents, true);
    write_u16(m, actor + 0x26, 0);
    rejected("local human not frozen");
    write_u16(m, actor + 0x26, 1);
    write_u32(m, actor + 8, 1);
    rejected("human controller ownership not frozen even with stale AI flag");
    write_u32(m, actor + 8, 0xFFFFFFFFu);
    for (unsigned field : {bike_address + bike::drive_control_lockout,
                           rider_address + rider::ejected, route + 0x4C,
                           route + 0x4E, route + 0x50, route + 0x52}) {
        write_u16(m, field, 1);
        rejected("crash, recovery and terminal state retain native simulation");
        write_u16(m, field, 0);
    }
    write_u16(m, rider_address + rider::bike_attached, 0);
    rejected("detached rider physics remains active");
    write_u16(m, rider_address + rider::bike_attached, 1);
    write_u32(m, bike_address + bike::rider_pointer, rider_address + 4);
    rejected("invalid reciprocal entity links rejected");
    write_u32(m, bike_address + bike::rider_pointer, rider_address);
    write_u16(m, actor + 0x24, 0);
    rejected("inactive actor untouched");
    write_u16(m, actor + 0x24, 1);
    check(!rr64_offline_modifiers_freeze_actor(m, actor + 4), "misaligned actor rejected");
    check(!rr64_offline_modifiers_freeze_actor(m, 0xFFFFFFF0u), "out-of-range actor rejected");

    rules.active = true;
    rejected("online lobby, race and disconnect session gated");
    rules.active = false;
    {
        rr64::prediction::ReplayScope replay;
        rejected("prediction replay cannot mutate live cheat state");
    }
    highlights = true;
    rejected("highlight presentation untouched");
    highlights = false;
    session = false;
    rejected("attract demo untouched");
    session = true;
    write_u32(m, globals::pending_mode, 0x0B);
    rejected("race-results transition untouched");
    write_u32(m, globals::pending_mode, 0x09);
    shadow = memory;
    const auto shadow_before = shadow;
    check(!rr64_offline_modifiers_freeze_actor(shadow.data(), actor) && shadow == shadow_before,
          "private renderer and snapshot memory untouched");
    for (const unsigned mode : {0x09u, 0x0Au, 0x12u, 0x13u, 0x17u, 0x18u, 0x1Cu, 0x1Du}) {
        write_u32(m, globals::main_mode, mode);
        write_u32(m, globals::pending_mode, mode);
        check(rr64_offline_modifiers_freeze_actor(m, actor), "all live offline race modes supported");
    }
    set_requested(Flag::FreezeOpponents, false);

    // All Tracks opens locked race levels and relocks only those it opened.
    constexpr unsigned levels = 0x800A5334u;
    const auto level = [&](unsigned index) {
        std::uint16_t value = 0;
        read_u16(m, levels + index * 2, value);
        return value;
    };
    write_u32(m, globals::main_mode, 0x21);
    write_u32(m, globals::pending_mode, 0x21);
    write_u16(m, levels + 0 * 2, 1); // Earned before the cheat.
    write_u16(m, levels + 8 * 2, 1);
    const auto table = [&] {
        std::vector<std::uint16_t> values;
        for (unsigned i = 0; i < 16; ++i)
            values.push_back(level(i));
        return values;
    };
    const auto earned = table();
    before = memory;
    rr64_offline_modifiers_frame(m);
    check(memory == before, "disabled All Tracks leaves race levels untouched");
    set_requested(Flag::AllTracks, true);
    check(!enabled(Flag::AllBikes), "All Tracks is independent of All Bikes");
    rules.active = true;
    rr64_offline_modifiers_frame(m);
    check(table() == earned, "online session does not open race levels");
    rules.active = false;
    rr64_offline_modifiers_frame(m);
    unsigned dirty = 0;
    read_u32(m, local_race::menu_dirty, dirty);
    check(dirty == 1, "open selector refreshes after toggling All Tracks");
    bool all = true;
    for (unsigned i = 0; i < 15; ++i)
        all = all && level(i) == 1;
    check(all, "every race level and reversed copy is open");
    check(level(15) == 0, "no entry beyond the native table is written");
    rules.active = true;
    rr64_offline_modifiers_frame(m);
    check(level(0) == 1 && level(8) == 1 && level(3) == 0 && level(14) == 0,
          "going online relocks opened levels but keeps earned ones");
    rules.active = false;
    rr64_offline_modifiers_frame(m);
    set_requested(Flag::AllTracks, false);
    rr64_offline_modifiers_frame(m);
    check(table() == earned, "switching off restores the exact earned level table");
    set_requested(Flag::AllTracks, true);
    set_requested(Flag::AllBikes, true);
    rr64_offline_modifiers_frame(m);
    check(enabled(Flag::AllBikes) && enabled(Flag::AllTracks) && level(5) == 1,
          "All Bikes and All Tracks can be enabled together");
    set_requested(Flag::AllTracks, false);
    rr64_offline_modifiers_frame(m);
    check(enabled(Flag::AllBikes) && level(5) == 0, "All Bikes alone leaves race levels native");
    set_requested(Flag::AllBikes, false);
    seed(m);
    check(!rr64_offline_combat_multiplier(m, rider_address), "combat defaults to native");
    set_requested(Flag::Combat4x, true);
    check(enabled_any(), "combat cheat disables achievements");
    for (unsigned cpu : {0u, 1u}) {
        write_u16(m, actor + 0x26, cpu);
        check(rr64_offline_combat_multiplier(m, rider_address) == 4, "human and CPU combat 4x");
    }
    set_requested(Flag::Combat100x, true);
    check(rr64_offline_combat_multiplier(m, rider_address) == 100, "100x wins without stacking");
    rules.active = true;
    check(!rr64_offline_combat_multiplier(m, rider_address), "online combat unchanged");
    rules.active = false;
    {
        rr64::prediction::ReplayScope replay;
        check(!rr64_offline_combat_multiplier(m, rider_address), "prediction combat unchanged");
    }
    highlights = true;
    check(!rr64_offline_combat_multiplier(m, rider_address), "highlights not amplified twice");
    highlights = false;
    check(!rr64_offline_combat_multiplier(shadow.data(), rider_address), "private memory rejected");
    write_u32(m, rider_address + 4, actor + 1);
    check(!rr64_offline_combat_multiplier(m, rider_address), "invalid combat ownership rejected");
    seed(m);
    for (unsigned mode : {0x09u,0x0Au,0x12u,0x13u,0x17u,0x18u,0x1Cu,0x1Du}) {
        write_u32(m, globals::main_mode, mode);
        write_u32(m, globals::pending_mode, mode);
        check(rr64_offline_combat_multiplier(m, rider_address) == 100, "all offline race modes");
    }
    set_requested(Flag::Combat100x, false);
    check(rr64_offline_combat_multiplier(m, rider_address) == 4, "disabling 100x restores 4x");
    set_requested(Flag::Combat4x, false);
    check(!enabled_any() && !rr64_offline_combat_multiplier(m, rider_address), "combat off restores native");
    seed(m);
    // Actual generated branches: every pickup state, both attack directions,
    // and force after the original mass cap. No ROM assets are required.
    write_float(m, 0x8000619C, 2.f); write_float(m, 0x800061A0, 4.f); write_float(m, 0x800061A4, 1.f);
    write_float(m, 0x800061AC, 2.f); write_float(m, 0x800061B0, 4.f); write_float(m, 0x800061B4, 1.f);
    write_float(m, 0x800067A8, 20.f); write_float(m, 0x800067AC, 3.f); write_float(m, 0x800067B0, 6.f);
    write_float(m, rider_address + 0x530, 12.f);
    write_float(m, bike_address, 0.25f);
    for (unsigned multiplier : {0u,4u,100u}) {
        set_requested(Flag::Combat4x, multiplier == 4);
        set_requested(Flag::Combat100x, multiplier == 100);
        for (unsigned pickup : {0u,1u,2u}) {
            write_u32(m, rider_address + 0x5D4, pickup);
            const float expected = multiplier ? float(multiplier) : pickup == 1 ? 2.f : pickup == 2 ? 4.f : 1.f;
            for (auto fn : {test_combat_damage_left, test_combat_damage_right}) {
                recomp_context ctx{}; ctx.r17=ctx.r18=static_cast<int32_t>(rider_address);
                fn(m,&ctx);
                check(ctx.f2.fl == expected, "native damage coefficient and pickup replacement");
            }
            recomp_context ctx{}; ctx.r16=static_cast<int32_t>(rider_address); ctx.r18=static_cast<int32_t>(bike_address);
            test_combat_force(m,&ctx);
            const float force = multiplier ? float(multiplier) : pickup == 1 ? 3.f : pickup == 2 ? 6.f : 1.f;
            check(ctx.f20.fl == 5.f*force, "native mass-capped impulse amplified exactly once");
        }
    }
    set_requested(Flag::Combat100x, false);
    if (failures) return 1;
    seed(m);
    check(rr64_offline_gravity(m,bike_address,0,20.f)==20.f, "gravity defaults to native");
    set_requested(Flag::MoonGravity,true);
    write_u32(m,bike_address+0x100,8);
    check(enabled_any(), "moon gravity participates in achievement suppression");
    for (unsigned mode : {0x09u,0x0Au,0x12u,0x13u,0x17u,0x18u,0x1Cu,0x1Du}) {
        write_u32(m,globals::main_mode,mode); write_u32(m,globals::pending_mode,mode);
        check(std::abs(rr64_offline_gravity(m,bike_address,0,20.f)-20.f/6.f)<0.00001f,"moon gravity in every offline race mode");
    }
    rules.active=true;
    check(rr64_offline_gravity(m,bike_address,0,20.f)==20.f,"online gravity unchanged");
    rules.active=false;
    { rr64::prediction::ReplayScope replay;
      check(rr64_offline_gravity(m,bike_address,0,20.f)==20.f,"prediction gravity unchanged"); }
    highlights=true;
    check(rr64_offline_gravity(m,bike_address,0,20.f)==20.f,"recorded highlights are not simulated with gravity again");
    highlights=false;
    check(rr64_offline_gravity(shadow.data(),bike_address,0,20.f)==20.f,"private memory gravity unchanged");
    write_u32(m,globals::pending_mode,0x20);
    check(rr64_offline_gravity(m,bike_address,0,20.f)==20.f,"transition to menu restores gravity immediately");
    seed(m);
    write_float(m,0x8009F260,20.f); write_float(m,0x8009F264,14.f);
    write_float(m,rider_address+0x28,2.f); write_float(m,rider_address+0x124,100.f);
    write_float(m,bike_address+0x108,2.f); write_float(m,bike_address+0x204,100.f);
    write_float(m,bike_address+0x9C,1.5f); write_float(m,bike_address+0xA0,1.5f);
    write_u32(m,bike_address+0x100,8);
    using Branch=void(*)(unsigned char*,recomp_context*);
    const Branch branches[]={test_gravity_rider_special,test_gravity_rider,test_gravity_bike_special,test_gravity_bike_blend,test_gravity_bike};
    const float native_weight[]={28.f,40.f,42.f,40.5f,60.f};
    for(unsigned branch=0;branch<5;++branch) for(bool moon:{false,true}) for(unsigned state:{2u,8u,9u,10u}) {
        write_u32(m,bike_address+0x100,state);
        const auto gravity_memory=memory;
        set_requested(Flag::MoonGravity,moon);
        recomp_context ctx{};
        ctx.r2=ctx.r3=ctx.r16=S32(0x800A0000);
        ctx.r18=S32(rider_address); ctx.r19=S32(bike_address);
        ctx.r17=S32(branch<2?rider_address+0x28:bike_address+0x108);
        ctx.f0.fl=1.f; ctx.f6.fl=0.25f; ctx.f4.fl=2.f;
        branches[branch](m,&ctx);
        const float result=branch==1?ctx.f4.fl:ctx.f0.fl;
        check(std::abs(result-(100.f-native_weight[branch]/(moon && state==8?6.f:1.f)))<0.0001f,
              "native gravity branch scales weight exactly once, preserves other accumulated forces");
        check(memory==gravity_memory,"gravity never changes shared constants or saved guest state");
    }
    for (unsigned state : {0u,1u,2u,8u,9u,10u}) {
        write_u32(m,bike_address+0x100,state);
        const float expected=state==8?20.f/6.f:20.f;
        check(rr64_offline_gravity(m,bike_address,0,20.f)==expected,"only airborne bike state reduces weight");
        check(rr64_offline_gravity(m,rider_address,1,20.f)==expected,"mounted rider follows bike contact state");
    }
    write_u16(m,rider_address+rider::bike_attached,0);
    for (float front : {-1.f,0.f,1.f}) for (float rear : {-1.f,0.f,1.f}) {
        write_float(m,rider_address+rider::front_contact_gap,front);
        write_float(m,rider_address+rider::rear_contact_gap,rear);
        check(rr64_offline_gravity(m,rider_address,1,20.f)==(front<0&&rear<0?20.f/6.f:20.f),
              "ejected rider floats only until either end lands");
    }
    check(rr64_offline_gravity(m,0,0,20.f)==20.f,"invalid body keeps native gravity");
    set_requested(Flag::MoonGravity,false);
    check(!enabled_any() && rr64_offline_gravity(m,bike_address,0,20.f)==20.f,"switching moon gravity off restores native behavior");
    memory.assign(kRdramSize, 0);
    m = memory.data(); reset(m); seed(m);
    write_u16(m, actor + 0x26, 0);
    write_u32(m, actor + 8, 0);
    write_u16(m, rider_address + rider::bike_attached, 0);
    check(!rr64_offline_hold_recovery(m, actor), "manual recovery defaults off");
    set_requested(Flag::ManualRecovery, true);
    check(enabled_any(), "manual recovery suppresses cheat achievements");
    for (unsigned player = 0; player < 4; ++player) {
        write_u32(m, actor + 8, player);
        for (unsigned input = 0; input < 4; ++input) {
            write_u16(m, globals::controller_pressed_buttons + input * 2, 0x8000);
            check(rr64_offline_hold_recovery(m, actor) == (input != player), "only the owner's A accepts recovery");
            write_u16(m, globals::controller_pressed_buttons + input * 2, 0);
        }
        write_u16(m, globals::controller_buttons + player * 2, 0x8000);
        check(rr64_offline_hold_recovery(m, actor), "holding accelerator before a crash does not recover");
    }
    before = memory;
    check(rr64_offline_hold_recovery(m, actor) && memory == before, "holding recovery never freezes physics or writes positions");
    for (unsigned offset : {0x4Cu, 0x4Eu, 0x50u, 0x52u}) {
        write_u16(m, route + offset, 1);
        check(!rr64_offline_hold_recovery(m, actor), "terminal racers retain elimination and finish handling");
        write_u16(m, route + offset, 0);
    }
    write_u16(m, rider_address + rider::bike_attached, 1);
    check(!rr64_offline_hold_recovery(m, actor), "ordinary mounted riding remains unchanged");
    write_u16(m, rider_address + rider::ejected, 1);
    check(rr64_offline_hold_recovery(m, actor), "manual ejection can wait for A");
    write_u16(m, actor + 0x26, 1);
    check(!rr64_offline_hold_recovery(m, actor), "AI recovery remains automatic");
    write_u16(m, actor + 0x26, 0);
    rules.active = true;
    check(!rr64_offline_hold_recovery(m, actor), "online recovery remains automatic");
    rules.active = false;
    highlights = true;
    check(!rr64_offline_hold_recovery(m, actor), "highlights cannot hold recovery");
    highlights = false;
    { rr64::prediction::ReplayScope replay;
      check(!rr64_offline_hold_recovery(m, actor), "prediction ignores manual recovery"); }
    session = false;
    check(!rr64_offline_hold_recovery(m, actor), "attract demo ignores manual recovery");
    session = true;
    write_u32(m, globals::pending_mode, 0x0B);
    check(!rr64_offline_hold_recovery(m, actor), "results transition cannot be held");
    write_u32(m, globals::pending_mode, 0x09);
    check(!rr64_offline_hold_recovery(m, actor + 4), "invalid actor rejected");
    write_u32(m, actor + 0xE4, 0xFFFFFFFCu);
    check(!rr64_offline_hold_recovery(m, actor), "invalid rider rejected");
    write_u32(m, actor + 0xE4, rider_address);
    set_requested(Flag::ManualRecovery, false);
    check(!enabled_any() && !rr64_offline_hold_recovery(m, actor), "switching off releases recovery immediately");
    if (failures) return 1;
    std::puts("[OFFLINE-OPPONENTS] all checks passed");
    return 0;
}
