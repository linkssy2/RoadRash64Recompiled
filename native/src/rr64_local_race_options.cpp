#include "rr64_local_race_options.hpp"
#include "rr64_offline_modifiers.hpp"
#include "rr64_thrash_options.hpp"
#include "rr64_custom_cop.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_local_players.hpp"
#include "rr64_campaign_coop.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_highlights.hpp"
#include "librecomp/addresses.hpp"
#include <atomic>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>


namespace {
using namespace rr64::engine;
namespace layout = rr64::engine::local_race;
std::atomic<unsigned> saved{0}, revision{0};
std::atomic<unsigned> local_traffic{1}, local_police{3};
std::atomic<unsigned> online_saved{0};
std::array<std::atomic<unsigned>, 2> cpu_weapons{};
std::atomic<unsigned> weapon_revision{};
unsigned weapon_written_revision{};
std::filesystem::path weapon_settings_path;
std::atomic<unsigned> solo_saved{10}, solo_revision{0};
std::atomic<unsigned> solo_stock{rr64::local_race_options::unknown_thrash_stock};
std::atomic<bool> items_enabled{true};
std::atomic<unsigned> items_revision{0};
unsigned items_written_revision = 0;
std::filesystem::path items_settings_path;
unsigned item_preference(unsigned bits) {
    constexpr unsigned disabled = rr64::local_race_options::mk64_items_disabled_bit;
    return (bits & ~disabled) | (items_enabled.load(std::memory_order_acquire) ? 0u : disabled);
}
unsigned choices() {
    if (rr64::netplay::get_status().active)
        return online_saved.load(std::memory_order_acquire);
    return item_preference(rr64_thrash_options_active() ? solo_saved.load(std::memory_order_acquire)
                                                       : saved.load(std::memory_order_acquire));
}
unsigned written_revision = 0;
unsigned solo_written_revision = 0;
std::filesystem::path settings_path;
std::filesystem::path solo_settings_path;
bool initialized = false, menu_active = false;
// Session resets arrive from the UI thread; the game thread consumes them.
std::atomic<bool> restore_choices{true};
bool mk64_items_row = false;
// Packed preferences: AI count [0:3], pedestrian density [4:5], bike choice
// [6:8], Custom Cop Mode [9], allow AI cops [10] (off for old presets). Bike choice 0 follows the
// track; 1..5 are normal tiers, 6 is Scooter and 7 is Insanity.
unsigned race_bike_choice = 0;
unsigned char* bike_profile_owner = nullptr;
unsigned bike_profile_copies = 0;
void save(unsigned bits) {
    if (rr64::netplay::get_status().active) {
        online_saved.store(bits, std::memory_order_release);
        return;
    }
    // This preference has its own file; legacy presets retain their format.
    bits &= ~rr64::local_race_options::mk64_items_disabled_bit;
    if (rr64_thrash_options_active()) {
        rr64::local_race_options::set_thrash_options(bits);
        return;
    }
    if (saved.exchange(bits, std::memory_order_acq_rel) != bits)
        revision.fetch_add(1, std::memory_order_release);
}
unsigned table_address = 0;
unsigned char *table_owner = nullptr;
unsigned read(unsigned char *rdram, unsigned a) { return MEM_W(0, guest_address(a)); }
void write(unsigned char *m, unsigned a, unsigned v) {
    auto *rdram = m;
    MEM_W(0, guest_address(a)) = v;
}
// Recomp heap addresses can be outside the original eight-MiB guest arena.
// Only this owned table and bounded stack text use direct guest access.
void text(unsigned char *rdram, unsigned address, const char *value) {
    // The stock menu's string buffer is sp+0x50..0x67; the next local starts
    // at sp+0x68. Do not overwrite it when adding translated labels later.
    for (unsigned i = 0; i < 23; ++i) {
        MEM_B(i, guest_address(address)) = value[i];
        if (!value[i])
            return;
    }
    MEM_B(23, guest_address(address)) = 0;
}
bool local() {
    const auto status = rr64::netplay::get_status();
    if (status.active)
        return status.connected && !status.replicated_riders;
    return rr64_thrash_options_active() ||
           rr64::local_players::active.load(std::memory_order_acquire);
}
bool offline_multiplayer() {
    return rr64::local_players::active.load() && !rr64::prediction::active() &&
           !rr64_thrash_options_active() && !rr64::netplay::get_status().active;
}
unsigned menu_humans(unsigned char *rdram) {
    if (rr64_thrash_options_active())
        return 1;
    const unsigned count = read(rdram, layout::menu_humans);
    return count >= 1 && count <= 4 ? count : read(rdram, layout::humans);
}
unsigned ai_count(unsigned bits, unsigned humans) {
    return std::min(bits & 15u, rr64::local_race_options::max_ai(humans));
}
bool make_table(unsigned char *rdram) {
    if (table_owner != rdram) {
        table_owner = rdram;
        table_address = 0;
    }
    if (!table_address) {
        auto *host = static_cast<unsigned char *>(recomp::alloc(rdram, 14 * 36));
        if (!host)
            return false;
        table_address = 0x80000000u + static_cast<unsigned>(host - rdram);
    }
    // Copy stock records, keeping their colors, font, alignment and animation.
    for (unsigned i = 0; i < 8 * 36; i += 4)
        write(rdram, table_address + i, read(rdram, layout::menu_table + i));
    for (unsigned row : {8u, 9u, 10u, 11u, 12u})
        for (unsigned i = 0; i < 36; i += 4)
            write(rdram, table_address + row * 36 + i,
                  read(rdram, layout::menu_table + 7 * 36 + i));
    for (unsigned i = 0; i < 36; i += 4)
        write(rdram, table_address + 13 * 36 + i, 0);
    // Y is absolute when positive and advances by -Y otherwise. Leave space
    // for the stock rows and optional Custom Cop rows. Compact option rows
    // use twelve-pixel spacing below the three larger header rows.
    write(rdram, table_address + 8, 0x42480000u); // 50.0f, below the title
    for (unsigned row = 0; row <= 2; ++row) {
        write(rdram, table_address + row * 36 + 0x1C, 0x3F400000u); // 0.75f
        if (row)
            write(rdram, table_address + row * 36 + 8, 0xC1800000u); // -16.0f
    }
    for (unsigned row = 3; row <= 12; ++row) {
        write(rdram, table_address + row * 36 + 8,
              row == 3 ? 0xC1900000u : 0xC1400000u);                // heading gap 18, rows 12
        write(rdram, table_address + row * 36 + 0x1C, 0x3F000000u); // 0.5f
    }
    return true;
}
} // namespace

namespace rr64::local_race_options {
unsigned cpu_weapon(bool cop) { return cpu_weapons[cop].load(); }
void cycle_cpu_weapon(bool cop, bool increase) {
    if (prediction::active() || netplay::get_status().active) return;
    cpu_weapons[cop].store((cpu_weapon(cop) + (increase ? 1u : 14u)) % 15u);
    weapon_revision.fetch_add(1);
}
const char *cpu_weapon_name(bool cop) {
    static constexpr const char *names[] = {"Default", "Fist", "Spiked Club", "Lead Pipe", "Baseball Bat",
        "Nightstick", "Sledgehammer", "Pool Cue", "Monkey Wrench", "Banana", "Cattle Prod", "Tazer", "Pepper Spray", "Nunchucks", "Chain"};
    return names[cpu_weapon(cop)];
}
void show_mk64_items_row(bool visible) { mk64_items_row = visible; }
bool mk64_items_enabled() {
    if (prediction::active())
        return false; // Historical effects are supplied by the replay binding.
    if (netplay::get_physics_rules().active)
        return (online_saved.load(std::memory_order_acquire) & mk64_items_disabled_bit) == 0;
    return items_enabled.load(std::memory_order_acquire);
}
void toggle_mk64_items(std::string_view course) {
    if (prediction::active() || !course_music_bit(course))
        return;
    const auto status = netplay::get_status();
    if (status.active && !status.is_host)
        return;
    const bool enabled = status.active
        ? (online_saved.load(std::memory_order_acquire) & mk64_items_disabled_bit) != 0
        : !items_enabled.load(std::memory_order_acquire);
    if (items_enabled.exchange(enabled, std::memory_order_acq_rel) != enabled)
        items_revision.fetch_add(1, std::memory_order_release);
    if (status.active) {
        const unsigned bits = online_saved.load(std::memory_order_acquire);
        online_saved.store(enabled ? bits & ~mk64_items_disabled_bit
                                   : bits | mk64_items_disabled_bit,
                           std::memory_order_release);
    }
}
bool course_music_enabled(std::string_view course) {
    return course_music_bit(course) != 0 && (choices() & course_music_mask) != 0;
}
void toggle_course_music(std::string_view course) {
    const auto status = netplay::get_status();
    if (status.active && !status.is_host) return;
    if (!course_music_bit(course)) return;
    const auto bits = choices();
    // Keep the preference across race selection and race teardown. Retain the
    // existing solo/local/session ownership and all unrelated gameplay bits.
    save((bits & course_music_mask) ? bits & ~course_music_mask : bits | course_music_mask);
}
unsigned thrash_options() { return solo_saved.load(std::memory_order_acquire); }
void set_thrash_options(unsigned bits) {
    bits &= ~mk64_items_disabled_bit;
    if (valid_online_options(bits) && solo_saved.exchange(bits, std::memory_order_acq_rel) != bits)
        solo_revision.fetch_add(1, std::memory_order_release);
}
unsigned thrash_stock_options() { return solo_stock.load(std::memory_order_acquire); }
void set_thrash_stock_options(unsigned bits) {
    if (valid_thrash_stock(bits) && solo_stock.exchange(bits, std::memory_order_acq_rel) != bits)
        solo_revision.fetch_add(1, std::memory_order_release);
}
void reset_online() {
    online_saved.store(item_preference(saved.load(std::memory_order_acquire)),
                       std::memory_order_release);
    restore_choices = true;
}
unsigned online_options() { return online_saved.load(std::memory_order_acquire); }
void apply_online_options(unsigned bits) {
    if (valid_online_options(bits))
        online_saved.store(bits, std::memory_order_release);
}
void initialize(const std::filesystem::path &directory) {
    if (initialized)
        return;
    initialized = true;
    settings_path = directory / "local-race-options.cfg";
    solo_settings_path = directory / "thrash-race-options.cfg";
    items_settings_path = directory / "mk64-item-options.cfg";
    {
        unsigned version = 0, enabled = 0;
        std::ifstream items_file(items_settings_path);
        if (items_file >> version >> enabled && version == 1 && enabled <= 1 &&
            (items_file >> std::ws).eof())
            items_enabled.store(enabled != 0, std::memory_order_release);
        online_saved.store(item_preference(online_saved.load(std::memory_order_acquire)),
                           std::memory_order_release);
    }
    {
        unsigned version = 0, bits = 0, stock = unknown_thrash_stock;
        std::ifstream solo_file(solo_settings_path);
        if (solo_file >> version >> bits >> stock && (version == 1 || version == 2) &&
            bits <= (version == 1 ? 2047u : 0x07FFFFFFu) && valid_online_options(bits) &&
            (valid_thrash_stock(stock) || stock == unknown_thrash_stock)) {
            solo_saved.store(bits, std::memory_order_release);
            solo_stock.store(stock, std::memory_order_release);
        }
    }
    weapon_settings_path = directory / "cpu-weapon-options.cfg";
    {
        unsigned version = 0, racer = 0, cop = 0;
        std::ifstream file(weapon_settings_path);
        if (file >> version >> racer >> cop && version == 1 && racer <= 14 && cop <= 14 && (file >> std::ws).eof()) {
            cpu_weapons[0].store(racer); cpu_weapons[1].store(cop);
        }
    }
    unsigned version = 0, bits = 0;
    std::ifstream file(settings_path);
    if (file >> version >> bits) {
        if (version == 1 && bits <= 15) {
            // Preserve the old Off/full-field and Match Track/Level 1 choices.
            saved.store((bits & 1 ? 10u : 0u) | (((bits >> 1) & 3) << 4) | ((bits & 8) ? 64u : 0u));
            revision.fetch_add(1);
        } else if ((version >= 2 && version <= 7) &&
                   bits <= (version == 2   ? 511u
                            : version == 3 ? 1023u
                            : version == 4 ? 2047u : 0x07FFFFFFu) &&
                   (bits & 15) <= 10) {
            saved.store(bits);
            unsigned traffic = 1;
            if (version >= 6 && file >> traffic && traffic <= (version == 6 ? 1u : 3u))
                local_traffic.store(traffic);
            unsigned police = 3;
            if (version == 7 && file >> police && police <= 3)
                local_police.store(police);
        }
    }
}
void flush() {
    if (!initialized)
        return;
    const unsigned weapon_current = weapon_revision.load();
    if (weapon_current != weapon_written_revision) {
        std::ofstream file(weapon_settings_path, std::ios::trunc);
        file << "1 " << cpu_weapon(false) << ' ' << cpu_weapon(true) << '\n';
        file.close();
        if (file) weapon_written_revision = weapon_current;
    }
    const unsigned items_current = items_revision.load(std::memory_order_acquire);
    if (items_current != items_written_revision) {
        std::ofstream file(items_settings_path, std::ios::trunc);
        file << "1 " << unsigned(items_enabled.load(std::memory_order_acquire)) << '\n';
        file.close();
        if (file)
            items_written_revision = items_current;
    }
    const unsigned solo_current = solo_revision.load(std::memory_order_acquire);
    if (solo_current != solo_written_revision) {
        std::ofstream file(solo_settings_path, std::ios::trunc);
        file << "2 " << solo_saved.load(std::memory_order_acquire) << ' '
             << solo_stock.load(std::memory_order_acquire) << '\n';
        file.close();
        if (file)
            solo_written_revision = solo_current;
    }
    const unsigned current = revision.load(std::memory_order_acquire);
    if (!initialized || current == written_revision)
        return;
    std::ofstream file(settings_path, std::ios::trunc);
    file << "7 " << saved.load(std::memory_order_acquire) << ' '
         << local_traffic.load(std::memory_order_acquire) << ' '
         << local_police.load(std::memory_order_acquire) << '\n';
    file.close();
    if (file)
        written_revision = current;
}
} // namespace rr64::local_race_options

// The stock recovery routine also runs a route-boundary timer (+858).
// Local humans may roam while mounted; actual crashes, ejects, busts and
// exhausted durability still need its original recovery/death decisions.
extern "C" int rr64_local_player_roaming(unsigned char *rdram, unsigned actor) {
    constexpr unsigned first_actor = 0x800D8570, actor_stride = 0x118;
    if (actor < first_actor || (actor - first_actor) % actor_stride != 0 ||
        (actor - first_actor) / actor_stride >= kMaximumRacers)
        return 0;
    const unsigned slot = (actor - first_actor) / actor_stride;
    // Authority uses stable actor-slot bits, not four local camera/controller
    // slots. Replay must see its captured rules, not today's transport state.
    const auto status = rr64::prediction::status_for_rules();
    if (status.active && status.authoritative) {
        if (!status.connected || (status.authority_humans & (1u << slot)) == 0)
            return 0;
    } else {
        const bool enabled = status.active ? status.connected && !status.replicated_riders
                                           : rr64::local_players::active.load();
        const unsigned humans = read(rdram, layout::humans);
        if (!enabled || humans < 1 || humans > 4 || slot >= humans)
            return 0;
    }
    const unsigned bike = read(rdram, actor + 0xE0);
    const unsigned rider = read(rdram, actor + 0xE4);
    const unsigned state = read(rdram, actor + 0xE8);
    if (!valid_guest_range(bike, 0x868) || !valid_guest_range(rider, 0x5F0) ||
        !valid_guest_range(state, 0x64))
        return 0;
    std::uint16_t attached = 0, crashing = 0, busted = 0;
    float health = -1;
    read_u16(rdram, rider + 0x57C, attached);
    read_u16(rdram, bike + 0x7F6, crashing);
    read_u16(rdram, state + 0x4C, busted);
    read_float(rdram, bike + 0x4F8, health);
    if (!attached || crashing || busted || !std::isfinite(health) || health < 0)
        return 0;
    write(rdram, bike + 0x858, 0); // Also clear any pending HUD countdown.
    return 1;
}

extern "C" void rr64_local_options_menu_begin(unsigned char *rdram) {
    menu_active = false;
    mk64_items_row = false;
    if (read(rdram, globals::multiplayer_stage) != 1) {
        restore_choices = true;
        if (offline_multiplayer())
            write(rdram, 0x8009EAD8, local_traffic.load() != 0);
    }
}
extern "C" int rr64_custom_cop_enabled() {
    return local() && (choices() & 512u);
}
// Native 516B8 has already reserved human profiles at this point. Its remaining
// category counts at sp+70 drive AI profile selection, including police (7).
// Replace that category with regular racers before models/physics are built;
// never edit already-spawned actors or the human controller/selection records.
extern "C" void rr64_custom_cop_ai_pool(unsigned char *rdram, void *context) {
    if (!context || !rr64_custom_cop_active() || (choices() & 1024u))
        return;
    auto &c = *static_cast<recomp_context *>(context);
    const unsigned pool = static_cast<unsigned>(c.r29) + 0x70;
    if (!valid_guest_range(pool, 9 * 4))
        return;
    const unsigned police = read(rdram, pool + 7 * 4);
    if (!police || police > 14)
        return;
    unsigned regular = 5;
    for (unsigned category : {5u, 6u, 8u})
        if (read(rdram, pool + category * 4)) {
            regular = category;
            break;
        }
    const unsigned racers = read(rdram, pool + regular * 4);
    if (racers > 14 - police)
        return;
    write(rdram, pool + regular * 4, racers + police);
    write(rdram, pool + 7 * 4, 0);
    c.r21 = 0; // native police speed/profile partition, now empty
}
extern "C" void rr64_local_options_reset_race() {
    race_bike_choice = 0;
    rr64_custom_cop_reset();
}
// Temporarily redirect camera preparation only. Results, HUD identity and
// controller ownership still belong to the busted player after the call.
extern "C" unsigned rr64_local_spectator_begin(unsigned char *m, unsigned view) {
    if (!m || view >= 4 || (!rr64::local_players::active.load() && !rr64_coop_spectating()) || rr64::prediction::active())
        return 0;
    const unsigned mode = read(m, globals::main_mode);
    if (!is_live_race_mode(mode) || read(m, globals::pending_mode) != mode)
        return 0;
    const unsigned humans = read(m, layout::humans), total = read(m, 0x800A656C);
    if (humans < 2 || humans > 4 || view >= humans || total < humans || total > 14)
        return 0;
    const unsigned original = read(m, 0x800A657C + view * 4);
    if (original >= humans)
        return 0;
    const auto flag = [&](unsigned actor, unsigned offset) {
        const unsigned state = read(m, 0x800D8570 + actor * 0x118 + 0xE8);
        std::uint16_t value = 0;
        return valid_guest_range(state, 0x64) && read_u16(m, state + offset, value) && value != 0;
    };
    // Avoid a transport/status snapshot for the ordinary, non-busted camera.
    if ((!flag(original, 0x4C) && !(rr64_coop_spectating() && flag(original, 0x4E))) ||
        (!offline_multiplayer() && !rr64_coop_spectating()) || rr64_highlights_presenting())
        return 0;
    for (unsigned step = 1; step < humans; ++step) {
        const unsigned target = (original + step) % humans;
        if (!flag(target, 0x48) || flag(target, 0x4C) || flag(target, 0x4E) || flag(target, 0x50))
            continue;
        const unsigned camera = read(m, 0x800A4FA0 + view * 4);
        if (camera > 255)
            return 0;
        write(m, 0x800A657C + view * 4, target);
        write(m, 0x800A4FA0 + view * 4, 5); // Native chase camera.
        return (original + 1) | (camera << 8);
    }
    return 0;
}
extern "C" void rr64_local_spectator_end(unsigned char *m, unsigned view, unsigned saved) {
    if (!m || view >= 4 || !(saved & 255u))
        return;
    write(m, 0x800A657C + view * 4, (saved & 255u) - 1);
    write(m, 0x800A4FA0 + view * 4, saved >> 8);
}
extern "C" void rr64_local_bike_profiles_reset() {
    // Called once at ROM initialization, including a reused RDRAM mapping.
    // The runtime owns heap teardown; no pointer from the old heap survives.
    bike_profile_owner = nullptr;
    bike_profile_copies = 0;
}
extern "C" int rr64_local_options_input(unsigned char *rdram) {
    menu_active = local() && make_table(rdram);
    if (!menu_active) {
        restore_choices = true;
        return 0;
    }
    unsigned bits = choices();
    if (restore_choices.exchange(false, std::memory_order_acq_rel)) {
        write(rdram, layout::pedestrian_choice, (bits >> 4) & 3);
        write(rdram, layout::menu_dirty, 1);
    }
    const unsigned row = read(rdram, layout::menu_cursor);
    const unsigned buttons = read(rdram, layout::menu_buttons) & 0x60;
    if (offline_multiplayer() && (row == 11 || row == 12)) {
        if (buttons) {
            rr64::local_race_options::cycle_cpu_weapon(row == 12, buttons & 0x40);
            write(rdram, layout::menu_dirty, 1);
        }
        return 1;
    }
    // Skip the stock mutually-exclusive traffic/pedestrian input handlers.
    if (offline_multiplayer() && (row == 6 || row == 7) && buttons) {
        if (row == 6) {
            const unsigned value = (local_traffic.load() + ((buttons & 0x40) ? 1u : 3u)) & 3u;
            local_traffic.store(value);
            revision.fetch_add(1);
            write(rdram, 0x8009EAD8, value != 0);
        } else {
            const unsigned value = (((bits >> 4) & 3u) + ((buttons & 0x40) ? 1u : 3u)) & 3u;
            bits = (bits & ~48u) | (value << 4);
            save(bits);
            write(rdram, layout::pedestrian_choice, value);
        }
        write(rdram, layout::menu_dirty, 1);
    }
    if ((row == 4 || row == 8 || row == 9 || (row == 10 && ((bits & 512u) || offline_multiplayer()))) && buttons) {
        const bool increase = (buttons & 0x40) != 0;
        const unsigned maximum = row == 4   ? rr64::local_race_options::max_ai(menu_humans(rdram))
                                 : row == 8 ? 7
                                 : row == 10 && offline_multiplayer() ? 3 : 1;
        unsigned value = row == 4   ? ai_count(bits, menu_humans(rdram))
                         : row == 8 ? (bits >> 6) & 7
                         : row == 10 && offline_multiplayer() ? ((bits & 1024u) ? local_police.load() : 0u)
                                    : (bits >> (row == 10 ? 10 : 9)) & 1;
        value = increase ? (value < maximum ? value + 1 : 0) : (value ? value - 1 : maximum);
        if (row == 10 && offline_multiplayer()) {
            local_police.store(value);
            revision.fetch_add(1);
        }
        bits = row == 4    ? (bits & ~15u) | value
               : row == 8  ? (bits & ~448u) | (value << 6)
               : row == 10 ? (bits & ~1024u) | (unsigned(value != 0) << 10)
                           : (bits & ~512u) | (value << 9);
        save(bits);
        write(rdram, layout::menu_dirty, 1);
    }
    // Stock code still expects a boolean, never the new AI count. Skip its
    // row-4 input handler so it cannot increment this flag a second time.
    const unsigned enabled = ai_count(bits, menu_humans(rdram)) != 0;
    if (read(rdram, layout::ai_choice) != enabled)
        write(rdram, layout::menu_dirty, 1);
    write(rdram, layout::ai_choice, enabled);
    return row == 4 || row == 8 || row == 9 || row == 10 ||
           (offline_multiplayer() && (row == 6 || row == 7));
}

extern "C" int rr64_local_options_navigation(unsigned char *rdram) {
    if (!menu_active)
        return -1;
    unsigned mask = 3u | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 9);
    if (offline_multiplayer()) mask |= (1u << 11) | (1u << 12);
    if (mk64_items_row || offline_multiplayer())
        mask |= 1u << 6;
    if ((choices() & 512u) || offline_multiplayer())
        mask |= 1u << 10;
    for (unsigned row = 2; row < 8; ++row)
        if (MEM_B(row - 2, guest_address(layout::visibility)))
            mask |= 1u << row;
    // The old police-density row has no local label; AI Cops replaces it at row 10.
    if (offline_multiplayer()) mask &= ~(1u << 5);
    unsigned row = std::min(read(rdram, layout::menu_cursor), 12u);
    if (row == 10 && !(choices() & 512u) && !offline_multiplayer())
        row = 9;
    const unsigned buttons = read(rdram, layout::menu_buttons);
    if (buttons & 0x18)
        row = rr64::local_race_options::next_row(row, buttons & 8 ? -1 : 1, mask);
    // Match the stock path's dirty/cursor state without touching controller input.
    write(rdram, layout::menu_cursor, row);
    return static_cast<int>(row);
}
extern "C" unsigned rr64_local_options_table(unsigned stock) {
    return menu_active ? table_address : stock;
}
extern "C" unsigned rr64_local_options_visible(unsigned row, unsigned stock) {
    if (menu_active && (row == 11 || row == 12)) return offline_multiplayer();
    if (menu_active && row == 5 && offline_multiplayer())
        return 0; // Skip drawing AND vertical advance for the retired police row.
    if (menu_active && row == 10)
        return ((choices() & 512u) || offline_multiplayer()) ? 1u : 0u;
    return menu_active && (row == 4 || (row == 6 && (mk64_items_row || offline_multiplayer())) ||
                           row == 7 || row == 8 || row == 9) ? 1u : stock;
}
extern "C" void rr64_local_options_text(unsigned char *rdram, unsigned row, unsigned buffer) {
    if (!menu_active || !valid_guest_range(buffer, 24))
        return;
    char label[24];
    const unsigned bits = choices();
    if ((row == 11 || row == 12) && offline_multiplayer()) {
        std::snprintf(label, sizeof(label), "%s Wpn: %s", row == 12 ? "Cop" : "Racer",
                      rr64::local_race_options::cpu_weapon_name(row == 12));
        text(rdram, buffer, label);
    }
    if (row == 4) {
        std::snprintf(label, sizeof(label), "AI Racers: %u", ai_count(bits, menu_humans(rdram)));
        text(rdram, buffer, label);
    }
    if (row == 10) {
        static constexpr const char *labels[] = {"No Cops", "A Few Cops", "Some Cops", "Too Many Cops"};
        text(rdram, buffer, offline_multiplayer() ? labels[(bits & 1024u) ? local_police.load() : 0u]
                                                : (bits & 1024u ? "AI Cops: On" : "AI Cops: Off"));
    }
    if (row == 6 && offline_multiplayer() && !mk64_items_row) {
        static constexpr const char *labels[] = {"No Traffic", "Light Traffic", "Some Traffic", "Heavy Traffic"};
        text(rdram, buffer, labels[local_traffic.load()]);
    }
    if (row == 9)
        text(rdram, buffer, bits & 512u ? "Custom Cop Mode: On" : "Custom Cop Mode: Off");
    if (row == 8) {
        const unsigned level = (bits >> 6) & 7;
        if (level == 6)
            text(rdram, buffer, "Bike Level: Scooter");
        else if (level == 7)
            text(rdram, buffer, "Bike Level: Insanity");
        else if (level) {
            std::snprintf(label, sizeof(label), "Bike Level: %u", level);
            text(rdram, buffer, label);
        } else
            text(rdram, buffer, "Bike Level: Match Track");
    }
}
extern "C" void rr64_local_options_finish(unsigned char *rdram) {
    if (!menu_active)
        return;
    const unsigned peds = read(rdram, layout::pedestrian_choice);
    if (peds <= 3)
        save((choices() & ~48u) | (peds << 4));
    menu_active = false;
}
extern "C" void rr64_local_options_race(unsigned char *rdram) {
    race_bike_choice = 0;
    rr64_custom_cop_begin(rdram, rr64_custom_cop_enabled());
    if (!local())
        return;
    const unsigned humans = read(rdram, layout::humans);
    if (humans < 1 || humans > 4)
        return;
    const unsigned bits = choices();
    write(rdram, layout::racers,
          rr64::local_race_options::roster(bits & 15u, humans, read(rdram, layout::racers)));
    // The stock multiplayer menu clears this count. Reserve cops before the
    // native constructor allocates profiles; racers + cops never exceed 14.
    if (offline_multiplayer()) {
        const unsigned police = (bits & 1024u) ? local_police.load() : 0u;
        write(rdram, 0x800A6570, police);
        // Allocation alone leaves multiplayer cops dormant. Native road events
        // also require Thrash's encounter density (80026124..80026154).
        constexpr float police_density[] = {0.f, .2f, .25f, 1.f};
        // Match solo Thrash's native hardware profiles (80026268..800262C4).
        unsigned short reduced_profile = 0;
        read_u16(rdram, 0x800A4F28, reduced_profile);
        constexpr float traffic_density[2][4] = {{0.f, 1.5f, 5.f, 9.f}, {0.f, 1.5f, 2.5f, 4.f}};
        write_float(rdram, 0x800D855C, police_density[police]);
        write_float(rdram, 0x800D8560, traffic_density[reduced_profile != 0][local_traffic.load()]);
        // Pedestrians use the adjoining solo profile table (800261C0..8002621C).
        constexpr float pedestrian_density[2][4] = {{0.f, 1.f, 2.5f, 3.f}, {0.f, 1.f, 1.5f, 2.25f}};
        write_float(rdram, 0x800D8564, pedestrian_density[reduced_profile != 0][(bits >> 4) & 3u]);
    }
    race_bike_choice = (bits >> 6) & 7;
    restore_choices = true;
}
// Thrash's legacy density menu can select twelve racers. Replace that count
// before the native police-capacity calculation, and again before allocation.
// At most eleven race entrants plus three AI police fit the fourteen actors.
extern "C" void rr64_local_options_thrash_roster(unsigned char *rdram) {
    if (!rdram || !rr64_thrash_options_active())
        return;
    const unsigned bits = rr64::local_race_options::thrash_options();
    const unsigned racers = 1 + ai_count(bits, 1);
    unsigned police = std::min(read(rdram, 0x8009EAC8), 3u);
    if ((bits & 512u) && !(bits & 1024u))
        police = 0;
    write(rdram, layout::racers, racers);
    write(rdram, 0x800A6570, police);
}
extern "C" void rr64_local_options_thrash_race(unsigned char *rdram) {
    if (!rdram || !rr64_thrash_options_active())
        return;
    // 6C414 reads the roster before it writes its own human count. Do not let
    // a prior split-screen session choose this race's human mask or allocation.
    write(rdram, layout::humans, 1);
    rr64_local_options_race(rdram);
    rr64_local_options_thrash_roster(rdram);
}
extern "C" unsigned rr64_local_bike_level(unsigned original) {
    return race_bike_choice && local() ? race_bike_choice - 1 : original;
}

extern "C" unsigned rr64_local_bike_menu_level(unsigned original) {
    // Selection happens before race initialization takes its settings snapshot.
    // Change only the bike-list index, never the map level or permanent unlocks.
    const unsigned choice = (choices() >> 6) & 7;
    return choice && local() ? choice - 1 : original;
}

extern "C" void rr64_local_bike_ai_pool(unsigned char *rdram, void *context) {
    if (!rdram || !context)
        return;
    // Mode 18's native initializer is 73728 (Big Game). The optional Insanity
    // chapter borrows Level 5's population requests, but its actual donor
    // pool lacks some requested families. Normalize those requests here too,
    // without changing Thrash, local multiplayer, or online ownership rules.
    const bool campaign_bonus = read(rdram, 0x800D6A7C) == 5 &&
        read(rdram, 0x800D8548) == 7 &&
        (read(rdram, globals::main_mode) == 0x18 ||
         read(rdram, globals::pending_mode) == 0x18) &&
        !rr64::netplay::get_status().active;
    if (!campaign_bonus && (!local() || (!race_bike_choice && !rr64_custom_cop_active())))
        return;
    auto &c = *static_cast<recomp_context *>(context);
    const unsigned stack = static_cast<unsigned>(c.r29);
    if (!valid_guest_range(stack + 0x48, 0x50))
        return;

    // Native 516B8 has now built the actual tier's donor lists. Track templates
    // can still request an absent family: Insanity has no family 2, Scooter no
    // family 4. Custom Cop's general groups (5/6/8) also need concrete donors.
    // A positive demand with zero donors otherwise selects [0, -1] and hands
    // native 51E24 a null profile. Redirect only unsupported demands, before
    // stock RNG, speed ordering and actor construction use the lists.
    std::array<unsigned, 9> available{}, demand{};
    unsigned total = 0;
    for (unsigned family = 0; family < available.size(); ++family) {
        available[family] = read(rdram, stack + 0x48 + family * 4);
        demand[family] = read(rdram, stack + 0x70 + family * 4);
        if (available[family] > 15 || demand[family] > kMaximumRacers)
            return;
        total += demand[family];
    }
    if (total > kMaximumRacers)
        return;
    unsigned fallback = 0;
    for (unsigned family : {1u, 2u, 3u, 4u})
        if (available[family]) {
            fallback = family;
            break;
        }
    if (!fallback)
        return;
    for (unsigned family = 1; family < demand.size(); ++family) {
        if (!demand[family] || available[family])
            continue;
        // Keep light/heavy family preferences used by the native human
        // reservation code. Never turn a regular racer into an AI cop.
        const unsigned first = family == 3 || family == 4 || family == 6 ? 3 : 1;
        const unsigned recipient = available[first] ? first
                                   : available[first + 1] ? first + 1 : fallback;
        demand[recipient] += demand[family];
        if (family == 7)
            c.r21 = 0; // No police donors: exclude them from the speed partition.
        demand[family] = 0;
    }
    for (unsigned family = 1; family < demand.size(); ++family)
        write(rdram, stack + 0x70 + family * 4, demand[family]);
}

// The first four profile records are mutable human selections. Keep their
// rider identity and rider statistics; replace only bike identity and its
// physics profile with a stock entry from the selected bike tier.
extern "C" unsigned rr64_local_bike_profile(unsigned char *rdram, unsigned racer,
                                            unsigned profile) {
    if (!race_bike_choice || !local() || racer < 0x800D8570u)
        return profile;
    const unsigned offset = racer - 0x800D8570u;
    if (offset % 0x118 || offset / 0x118 >= 4 || !valid_guest_range(profile, 16))
        return profile;
    if (rr64::offline_modifiers::enabled(rr64::offline_modifiers::Flag::AllBikes) &&
        MEM_HU(0x26, guest_address(racer)) == 0 && read(rdram, racer + 8) < 4)
        return profile; // Retain the human's selected model and native physics profile.
    if (rr64_custom_cop_active() && read(rdram, racer + 0x18) == 31)
        return profile;
    // 6CB8C has already committed the menu/network choice to actor+18.
    // 6CF60 copies that choice to the mutable profile only AFTER this hook
    // returns (6D0AC..6D0B0). Its old byte10 can name a previous race's bike;
    // matching against that byte silently replaces the current selection with
    // the first donor in the chosen tier. This ordering applies to stock and
    // imported tracks alike. Never substitute an unrelated model as fallback.
    const unsigned selected_bike = read(rdram, racer + 0x18);
    unsigned donor = 0;
    for (unsigned i = 4; i < 160; ++i) {
        const unsigned entry = 0x800A3460u + i * 16;
        const unsigned tier = static_cast<unsigned char>(MEM_B(8, guest_address(entry)));
        if (tier == 12)
            break;
        if (tier != race_bike_choice || MEM_B(9, guest_address(entry)) == 7)
            continue;
        if (static_cast<unsigned char>(MEM_B(10, guest_address(entry))) == selected_bike) {
            donor = entry;
            break;
        }
    }
    if (!donor)
        return profile;
    if (profile < 0x800A3460u || profile >= 0x800A34A0u)
        return profile;
    // Keep the original selection intact for Match Track and later races.
    if (bike_profile_owner != rdram) {
        bike_profile_owner = rdram;
        bike_profile_copies = 0;
    }
    if (!bike_profile_copies) {
        auto *host = static_cast<unsigned char *>(recomp::alloc(rdram, 4 * 16));
        if (!host)
            return profile;
        bike_profile_copies = 0x80000000u + static_cast<unsigned>(host - rdram);
    }
    const unsigned copy = bike_profile_copies + offset / 0x118 * 16;
    for (unsigned i = 0; i < 16; i += 4)
        write(rdram, copy + i, read(rdram, profile + i));
    MEM_B(10, guest_address(copy)) = MEM_B(10, guest_address(donor));
    MEM_B(13, guest_address(copy)) = MEM_B(13, guest_address(donor));
    write(rdram, racer + 0x18, static_cast<unsigned char>(MEM_B(10, guest_address(donor))));
    return copy;
}

extern "C" void rr64_cpu_starting_weapon(unsigned char *rdram, unsigned slot) {
    if (!rdram || slot >= 14 || rr64::prediction::active() || rr64::netplay::get_status().active ||
        (!offline_multiplayer() && !rr64_thrash_options_active())) return;
    const unsigned count = read(rdram, 0x800A656C), actor = 0x800D8570 + slot * 0x118;
    if (count > 14 || slot >= count || !MEM_HU(0x26, guest_address(actor))) return;
    const unsigned weapon = rr64::local_race_options::cpu_weapon(read(rdram, actor + 0x20) == 7);
    const unsigned bike = read(rdram, actor + 0xE0), rider = read(rdram, actor + 0xE4);
    if (!weapon || weapon > 14 || !valid_guest_range(bike, 0x868) || !valid_guest_range(rider, 0x5F0) ||
        read(rdram, rider + 0x584) != bike) return;
    // Match native starting equipment; later pickups and disarms remain native.
    for (unsigned w = 1; w <= 14; ++w) write_u16(rdram, bike + 0x838 + w * 2, w == 1 || w == weapon ? 1 : 0);
    write(rdram, rider + 0x5B0, weapon);
}
