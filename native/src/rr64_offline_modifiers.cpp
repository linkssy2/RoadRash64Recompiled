#include "rr64_offline_modifiers.hpp"
#include "rr64_offline_modifiers_bikes.hpp"
#include "rr64_offline_modifiers_health.hpp"
#include "rr64_offline_modifiers_weapons.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_highlights.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_replay.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace rr64::offline_modifiers {
namespace {
std::atomic<unsigned> requested{0};
std::atomic<std::uint64_t> weapon_epoch{1};
std::atomic<std::uint64_t> menu_epoch{1};
std::uint64_t applied_menu_epoch = 1;
unsigned char *live_memory = nullptr;
std::array<std::uint64_t, engine::kMaximumRacers> granted{};
constexpr unsigned actors = 0x800D8570u, actor_stride = 0x118u;
constexpr unsigned known_flags = 63;
// Native race-level availability written by 5F420: levels 0-7 and reversed
// copies at 8-14. Nonzero means selectable.
constexpr unsigned race_levels = 0x800A5334u, race_level_count = 15;
unsigned opened_levels = 0;

bool offline() {
    // Reject the entire online session, including lobby, failed connect and
    // disconnect handling. Stored preferences are never part of session rules.
    return !prediction::active() && !netplay::get_physics_rules().active;
}
bool own_session(unsigned char *memory) {
    return memory && memory == live_memory && rr64_player_session_started() && offline();
}
bool live_race(unsigned char *memory) {
    unsigned mode = 0, pending = 0;
    return own_session(memory) && engine::read_u32(memory, engine::globals::main_mode, mode) &&
           engine::read_u32(memory, engine::globals::pending_mode, pending) &&
           engine::is_live_race_transition(mode, pending) && !rr64_highlights_presenting();
}
void grant_once(unsigned char *memory, unsigned actor) {
    if (actor < actors || (actor - actors) % actor_stride)
        return;
    const unsigned slot = (actor - actors) / actor_stride;
    if (slot >= granted.size())
        return;
    const auto epoch = weapon_epoch.load(std::memory_order_acquire);
    if (granted[slot] != epoch && grant_max_weapons(memory, actor, true))
        granted[slot] = epoch;
}

unsigned actor_slot(unsigned actor) {
    return actor >= actors && (actor - actors) % actor_stride == 0
               ? (actor - actors) / actor_stride
               : engine::kMaximumRacers;
}

bool mounted_opponent(unsigned char *memory, unsigned actor, unsigned &bike_address) {
    using namespace engine;
    const auto word = [memory](unsigned address) {
        unsigned value = 0;
        read_u32(memory, address, value);
        return value;
    };
    const auto half = [memory](unsigned address) {
        std::uint16_t value = 0;
        read_u16(memory, address, value);
        return value;
    };
    if (actor_slot(actor) >= kMaximumRacers || !half(actor + 0x24) ||
        half(actor + 0x26) != 1 || word(actor + 8) != 0xFFFFFFFFu)
        return false;
    bike_address = word(actor + 0xE0);
    const unsigned rider_address = word(actor + 0xE4), route = word(actor + 0xE8);
    if ((bike_address | rider_address | route) & 3u ||
        !valid_guest_range(bike_address, bike::stride) ||
        !valid_guest_range(rider_address, rider::stride) || !valid_guest_range(route, 0x64) ||
        word(bike_address + 4) != actor || word(rider_address + 4) != actor ||
        word(bike_address + bike::rider_pointer) != rider_address ||
        word(rider_address + rider::bike_pointer) != bike_address ||
        !half(bike_address + bike::rider_attached) ||
        !half(rider_address + rider::bike_attached) ||
        half(bike_address + bike::drive_control_lockout) || half(rider_address + rider::ejected))
        return false;
    // Busted, wrecked and finished actors must retain native completion/recovery.
    for (const unsigned offset : {0x4Cu, 0x4Eu, 0x50u, 0x52u})
        if (half(route + offset))
            return false;
    return true;
}

void apply_tracks(unsigned char *memory) {
    if (!memory || memory != live_memory)
        return;
    // Open only locked levels and remember them. Switching off, or entering an
    // online session, relocks exactly those; earned levels are never cleared.
    const bool open = enabled(Flag::AllTracks) && rr64_player_session_started();
    for (unsigned level = 0; level < race_level_count; ++level) {
        const unsigned address = race_levels + level * 2, bit = 1u << level;
        std::uint16_t value = 0;
        if (!engine::read_u16(memory, address, value))
            continue;
        if (open && !value) {
            engine::write_u16(memory, address, 1);
            opened_levels |= bit;
        } else if (!open && (opened_levels & bit)) {
            engine::write_u16(memory, address, 0);
            opened_levels &= ~bit;
        }
    }
}
}

void set_requested(Flag flag, bool value) {
    const unsigned bit = static_cast<unsigned>(flag);
    if (!(bit & known_flags) || (bit & (bit - 1)))
        return;
    const unsigned previous = value ? requested.fetch_or(bit, std::memory_order_acq_rel)
                                    : requested.fetch_and(~bit, std::memory_order_acq_rel);
    if (flag == Flag::AllWeapons && value && !(previous & bit))
        weapon_epoch.fetch_add(1, std::memory_order_release);
    if ((flag == Flag::AllBikes || flag == Flag::AllTracks) && ((previous & bit) != 0) != value)
        menu_epoch.fetch_add(1, std::memory_order_release);
}
bool enabled(Flag flag) {
    return (requested.load(std::memory_order_acquire) & static_cast<unsigned>(flag)) && offline();
}
bool enabled_any() {
    return requested.load(std::memory_order_acquire) && offline();
}
void reset(unsigned char *memory) {
    live_memory = memory;
    granted.fill(0);
    opened_levels = 0;
    applied_menu_epoch = menu_epoch.load(std::memory_order_acquire);
    rr64_offline_bikes_reset();
}
}

extern "C" void rr64_offline_modifiers_race_begin(unsigned char *memory) {
    using namespace rr64::offline_modifiers;
    if (memory == live_memory && !rr64::prediction::active())
        granted.fill(0);
}
extern "C" void rr64_offline_modifiers_equipment(unsigned char *memory, unsigned actor) {
    using namespace rr64::offline_modifiers;
    // This hook follows native/Custom Cop equipment and reciprocal rider-bike
    // link construction. Loading is not a live-race mode.
    if (enabled(Flag::AllWeapons) && own_session(memory))
        grant_once(memory, actor);
}
extern "C" void rr64_offline_modifiers_frame(unsigned char *memory) {
    using namespace rr64::offline_modifiers;
    apply_tracks(memory);
    const auto epoch = menu_epoch.load(std::memory_order_acquire);
    if (applied_menu_epoch != epoch && own_session(memory)) {
        unsigned mode = 0;
        rr64::engine::read_u32(memory, rr64::engine::globals::main_mode, mode);
        // Native 2F150 dispatches the selectors at 21/23. Their idle path
        // skips cursor clamping unless dirty; shops 2D/2E clamp every tick.
        if (mode == 0x21 || mode == 0x23) {
            rr64::engine::write_u32(memory, rr64::engine::local_race::menu_dirty, 1);
            applied_menu_epoch = epoch;
        }
    }
    // A toggle enabled in the overlay grants once on the next safe game tick.
    // Leaving it on grants again for a new race, never continuously refilling.
    if (!enabled(Flag::AllWeapons) || !live_race(memory))
        return;
    for (unsigned slot = 0; slot < rr64::engine::kMaximumRacers; ++slot)
        grant_once(memory, actors + slot * actor_stride);
}
extern "C" int rr64_offline_modifiers_freeze_actor(unsigned char *memory, unsigned actor) {
    using namespace rr64::offline_modifiers;
    if (!enabled(Flag::FreezeOpponents) || !live_race(memory))
        return 0;
    unsigned bike = 0;
    if (!mounted_opponent(memory, actor, bike))
        return 0;
    // Cancel forward momentum before holding BOTH native control and paired
    // bike/rider integration. No saved position, route progress, AI profile or
    // drive lock is written; disabling the option resumes from this position.
    // A hit that detaches the rider makes mounted_opponent false on the next
    // pass, so native crash/recovery can run instead of trapping a wreck.
    for (unsigned axis = 0; axis < 3; ++axis) {
        rr64::engine::write_float(memory, bike + 0x178u + axis * 4u, 0.0f);
        rr64::engine::write_float(memory, bike + 0x1A0u + axis * 4u, 0.0f);
    }
    rr64::engine::write_float(memory, bike + 0x184u, 0.0f);
    return 1;
}
extern "C" int rr64_offline_rider_protected(unsigned char *memory, unsigned rider) {
    using namespace rr64::offline_modifiers;
    return rider_damage_protected(memory, rider, enabled(Flag::RiderHealth) && live_race(memory));
}
extern "C" int rr64_offline_bike_protected(unsigned char *memory, unsigned bike) {
    using namespace rr64::offline_modifiers;
    return bike_damage_protected(memory, bike, enabled(Flag::BikeDurability) && live_race(memory));
}
