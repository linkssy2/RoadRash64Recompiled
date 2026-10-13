#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_prediction_replay.hpp"

#include <cstdio>
#include <vector>

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
    if (failures) return 1;
    std::puts("[OFFLINE-OPPONENTS] all checks passed");
    return 0;
}
