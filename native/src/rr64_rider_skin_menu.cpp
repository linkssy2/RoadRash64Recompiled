#include "rr64_rider_skin_menu.hpp"
#include "rr64_rider_skins.hpp"
#include "rr64_rider_skin_preferences.hpp"
#include "rr64_custom_cop.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"

#include <cstdio>
#include <algorithm>
#include <bit>
#include <cstring>

extern "C" void func_800796F8(unsigned char *, recomp_context *);

namespace {
using namespace rr64;
constexpr unsigned selected = 0x8009F670u;
constexpr unsigned preview_pool = 0x800D13C8u;
constexpr unsigned actors = 0x800D8570u, actor_stride = 0x118u;
unsigned word(unsigned char *m, unsigned address) {
    unsigned value = 0;
    engine::read_u32(m, address, value);
    return value;
}
unsigned half(unsigned char *m, unsigned address) {
    std::uint16_t value = 0;
    engine::read_u16(m, address, value);
    return value;
}
bool eligible(unsigned char *m) {
    return m && rider_skins::enabled() && !netplay::get_physics_rules().active;
}
bool cop_bike(unsigned char *m, unsigned slot) {
    return rr64_custom_cop_enabled() && rr64_custom_cop_bike_entry(m, slot, 0) == 0x800A684Cu;
}
unsigned preview_slots(unsigned char *m) {
    const unsigned mode = word(m, engine::globals::main_mode);
    if (mode >= engine::kModeRecordCount)
        return 0;
    // The shared menu callbacks are update 72704 and draw 7273C. Draw calls
    // 2F14C, which dispatches on main_mode - 1; selector functions are not
    // stored in the mode record. Require both the native callbacks and its
    // four selector cases, since unrelated menus share these callbacks too.
    const unsigned record = engine::globals::mode_records + mode * engine::kModeRecordSize;
    if (word(m, record + 8) != 0x80072704u || word(m, record + 12) != 0x8007273Cu)
        return 0;
    switch (mode) {
    case 33: // 2F250 -> 256C0
    case 45: // 2F290 -> 2DC30
    case 46: // 2F2A0 -> 2CD60
        return 1;
    case 35: { // 2F270 -> 27A90
        const unsigned count = word(m, engine::local_race::menu_humans);
        return count >= 1 && count <= 4 ? count : 0;
    }
    default:
        return 0;
    }
}
// Return the local controller only for a complete, active native human with
// native donor. Reciprocal links reject stale pools and other character actors.
unsigned local_human(unsigned char *m, unsigned canonical, unsigned *rider_out = nullptr) {
    const unsigned count = word(m, 0x800A656Cu);
    if (!m || count > engine::kMaximumRacers || canonical >= count)
        return 4;
    const unsigned actor = actors + canonical * actor_stride;
    const unsigned controller = word(m, actor + 8);
    const unsigned rider = word(m, actor + 0xE4), bike = word(m, actor + 0xE0);
    if (controller >= 4 || half(m, actor + 0x26) != 0 || !half(m, actor + 0x24) ||
        !engine::valid_guest_range(rider, engine::rider::stride) || (rider & 3u) ||
        !engine::valid_guest_range(bike, engine::bike::stride) || (bike & 3u) ||
        word(m, rider + 4) != actor || word(m, bike + 4) != actor ||
        word(m, rider + engine::rider::bike_pointer) != bike ||
        word(m, bike + engine::bike::rider_pointer) != rider)
        return 4;
    if (rider_out)
        *rider_out = rider;
    return controller;
}
bool selected_skin(unsigned char *m, unsigned slot, unsigned appearance) {
    return appearance && word(m, selected + slot * 4) == rider_skins::native_donor(appearance) &&
           !cop_bike(m, slot);
}
} // namespace

extern "C" int rr64_rider_skin_cycle(unsigned char *m, unsigned slot, int raw) {
    if (!m || slot >= 4)
        return raw;
    if (!eligible(m) || cop_bike(m, slot)) {
        rider_skins::set_menu_selection(slot, 0);
        return raw;
    }
    const int maximum = half(m, 0x800A77D8u) ? 44 : 39;
    const unsigned appearance = rider_skins::menu_selection(slot), count = rider_skins::count();
    if (selected_skin(m, slot, appearance)) {
        const int donor = int(rider_skins::native_donor(appearance));
        if (raw == donor)
            return donor;
        if (raw < donor) {
            rider_skins::set_menu_selection(slot, appearance - 1);
            return appearance == 1 ? maximum : int(rider_skins::native_donor(appearance - 1));
        }
        rider_skins::set_menu_selection(slot, appearance < count ? appearance + 1 : 0);
        return appearance < count ? int(rider_skins::native_donor(appearance + 1)) : 0;
    }
    rider_skins::set_menu_selection(slot, 0);
    // The native code has already added +/-1; only its two exact wrap points
    // enter the ordered catalog. Every ordinary stock step remains native.
    if (raw == -1 || raw == maximum + 1) {
        rider_skins::set_menu_selection(slot, raw < 0 ? count : 1);
        return int(rider_skins::native_donor(raw < 0 ? count : 1));
    }
    return raw;
}
extern "C" void rr64_rider_skin_clear_menu(unsigned char *) {
    rider_skins::clear_menu();
}
extern "C" void rr64_rider_skin_race_begin(unsigned char *) {
    rider_skins::clear_race();
}
extern "C" void rr64_rider_skin_spawn(unsigned char *m, unsigned canonical) {
    if (canonical >= engine::kMaximumRacers)
        return;
    const unsigned slot = eligible(m) ? local_human(m, canonical) : 4;
    const unsigned appearance = slot < 4 ? rider_skins::menu_selection(slot) : 0;
    const unsigned native = word(m, actors + canonical * actor_stride + 0x1c);
    rider_skins::set_race_selection(canonical,
                                    slot < 4 && selected_skin(m, slot, appearance) &&
                                            native == rider_skins::native_donor(appearance)
                                        ? appearance
                                        : 0);
}
extern "C" unsigned rr64_rider_skin_actor_selection(unsigned char *m, unsigned node) {
    if (!m || !rider_skins::enabled() || (node & 3u) || !engine::valid_guest_range(node, 0x44) || word(m, node) != 2)
        return 0;
    const unsigned entity = word(m, node + 4);
    if ((entity & 3u) || !engine::valid_guest_range(entity, engine::rider::stride))
        return 0;
    const unsigned slots = preview_slots(m);
    if (slots) {
        if (!eligible(m)) return 0;
        const unsigned slot = word(m, node + 0x40), pool = word(m, preview_pool);
        return slot < slots && engine::valid_guest_range(pool, slots * engine::rider::stride) &&
                       entity == pool + slot * engine::rider::stride &&
                       selected_skin(m, slot, rider_skins::menu_selection(slot))
                   ? rider_skins::menu_selection(slot)
                   : 0;
    }
    const unsigned mode = word(m, engine::globals::main_mode);
    if (!engine::is_live_race_mode(mode) && !engine::is_race_results_mode(mode))
        return 0;
    const unsigned actor = word(m, entity + 4);
    if (actor < actors || (actor - actors) % actor_stride)
        return 0;
    const unsigned canonical = (actor - actors) / actor_stride;
    const unsigned count = word(m, 0x800A656Cu), native = word(m, actor + 0x1c);
    const unsigned bike = word(m, actor + 0xe0);
    if (count > engine::kMaximumRacers || canonical >= count || native >= 40 ||
        !half(m, actor + 0x24) || word(m, actor + 0xe4) != entity ||
        !engine::valid_guest_range(bike, engine::bike::stride) || (bike & 3u) ||
        word(m, bike + 4) != actor || word(m, entity + engine::rider::bike_pointer) != bike ||
        word(m, bike + engine::bike::rider_pointer) != entity)
        return 0;
    const auto rules = netplay::get_physics_rules();
    bool cpu = half(m, actor + 0x26) != 0;
    if (rules.active) {
        if (!rules.connected || !rules.authoritative || !rules.authority_humans ||
            (rules.authority_humans >> engine::kMaximumRacers) || rules.local_slot >= 14)
            return 0;
        for (unsigned slot = 0; slot < 14; ++slot)
            if ((rules.authority_humans & (1u << slot)) &&
                (rules.is_host ? slot : online_flow::mapped_slot(slot, rules.local_slot,
                    rules.replicated_riders)) == canonical)
                return 0; // Remote humans may use native AI control on the guest.
        cpu = true;
    }
    if (cpu) {
        const unsigned gender = half(m, 0x800a1574 + native * 8);
        if (gender > 1) return 0;
        return rider_skins::random_cpu_selection(canonical, gender ? 0 : 10);
    }
    unsigned actual_rider = 0;
    const unsigned appearance = rider_skins::race_selection(canonical);
    return !rules.active && local_human(m, canonical, &actual_rider) < 4 && actual_rider == entity &&
                   native == rider_skins::native_donor(appearance) ? appearance : 0;
}
extern "C" void rr64_rider_skin_selection_hint(unsigned char *m, void *context) {
    if (!context || !eligible(m) || !rr64_custom_cop_can_start(m))
        return;
    const unsigned slots = preview_slots(m);
    if (!slots)
        return;
    char label[48] = "More Characters: past the last rider";
    unsigned chosen = 0;
    for (unsigned slot = 0; slot < slots; ++slot)
        if (selected_skin(m, slot, rider_skins::menu_selection(slot)))
            chosen |= 1u << slot;
    auto draw = *static_cast<recomp_context *>(context);
    // The original menu font consumes a guest string synchronously. Bound a
    // temporary frame before writing any guest memory, and keep ctx untouched.
    const unsigned stack = static_cast<unsigned>(draw.r29) - 112u;
    if ((stack & 3u) || !engine::valid_guest_range(stack, 112))
        return;
    draw.r29 = engine::guest_address(stack);
    const unsigned labels = chosen ? slots : 1;
    for (unsigned slot = 0; slot < labels; ++slot) {
        if (chosen && !(chosen & (1u << slot)))
            continue;
        if (chosen) {
            const auto appearance = rider_skins::menu_selection(slot);
            if (slots == 1)
                std::snprintf(label, sizeof(label), "%s", rider_skins::name(appearance));
            else
                std::snprintf(label, sizeof(label), "P%u: %s", slot + 1,
                              rider_skins::name(appearance));
        }
        // Separate local players' labels occupy equal-width footer regions.
        // Scale bounded names to their region rather than truncating identity.
        const float width = chosen ? 320.f / static_cast<float>(slots) : 320.f;
        const float scale =
            std::min(0.625f, (width - 8.f) / (8.f * static_cast<float>(std::strlen(label))));
        const float x = chosen ? width * slot + 4.f : 48.f;
        for (unsigned i = 0; i < sizeof(label); ++i)
            engine::write_s8(m, stack + 32 + i, static_cast<std::int8_t>(label[i]));
        draw.r4 = engine::guest_address(stack + 32);
        draw.r5 = std::bit_cast<unsigned>(x);
        draw.r6 = 0x43620000; // y=226, below the selection frame
        draw.r7 = engine::guest_address(0x8009E20Cu);
        engine::write_u32(m, stack + 16, 3);
        engine::write_u32(m, stack + 20, std::bit_cast<unsigned>(scale));
        func_800796F8(m, &draw);
    }
}

extern "C" unsigned rr64_rider_skin_preview_selection(unsigned char *m, unsigned graph) {
    if (!eligible(m) || !preview_slots(m) || !engine::valid_guest_range(graph, 0x18))
        return 0;
    unsigned node = word(m, 0x800a1454);
    // Native showroom actors share a linked list with bikes and held weapons.
    // Only the selected rider's authenticated graph may receive a skin.
    for (unsigned i = 0; node && i < 64; ++i) {
        if ((node & 3) || !engine::valid_guest_range(node, engine::actor_scene::node_minimum_size))
            return 0;
        if (word(m, node) == 2 && word(m, node + 0x28) == graph)
            return rr64_rider_skin_actor_selection(m, node);
        node = word(m, node + 0x3c);
    }
    return 0;
}

extern "C" void rr64_rider_skin_restore(unsigned char *m, unsigned slot, unsigned new_campaign) {
    if (!eligible(m) || slot >= 4 || cop_bike(m, slot))
        return;
    // Never replace a loaded native campaign rider or fresh manual choice.
    if (!new_campaign && half(m, 0x8009eae8 + slot * 2))
        return;
    const unsigned appearance = rider_skins::find(rider_skin_preferences::last(slot));
    if (!appearance)
        return;
    rider_skins::set_menu_selection(slot, appearance);
    engine::write_u32(m, selected + slot * 4, rider_skins::native_donor(appearance));
    engine::write_u16(m, 0x8009eae8 + slot * 2, 1);
}
extern "C" void rr64_rider_skin_remember(unsigned char *m, unsigned slot) {
    if (!eligible(m) || slot >= 4)
        return;
    const unsigned appearance = rider_skins::menu_selection(slot);
    const auto *a =
        selected_skin(m, slot, appearance) ? rider_skins::appearance(appearance) : nullptr;
    rider_skin_preferences::remember(slot, a ? std::string_view(a->id) : std::string_view{});
}
extern "C" void rr64_rider_skin_remember_multiplayer(unsigned char *m) {
    if (!eligible(m))
        return;
    const unsigned count = word(m, engine::local_race::menu_humans);
    if (count < 1 || count > 4)
        return;
    for (unsigned slot = 0; slot < count; ++slot)
        if (word(m, 0x8009ef2c + slot * 4) == 2)
            rr64_rider_skin_remember(m, slot);
}
extern "C" void rr64_rider_skin_campaign_load(unsigned char *m, unsigned record) {
    rider_skins::clear_menu();
    if (!eligible(m) || record != 0x800d6a40)
        return;
    const unsigned appearance = rider_skins::find(rider_skin_preferences::last(0));
    // Cosmetics follow this profile only when the saved native donor matches.
    // No additional data is written into original Controller Pak save records.
    if (appearance && word(m, record + 0x18) == rider_skins::native_donor(appearance))
        rider_skins::set_menu_selection(0, appearance);
}
