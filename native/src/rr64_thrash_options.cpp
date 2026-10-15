#include "rr64_thrash_options.hpp"

#include "rr64_engine_layout.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_netplay.hpp"
#include "librecomp/addresses.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
using namespace rr64::engine;
namespace shared = rr64::local_race_options;
namespace menu = rr64::engine::local_race;

// USA v1.0 func_800256C0: stage 0 selects a route/options; stage 1 selects
// the solo character and bike. These are not the multiplayer EF28/ED94 fields.
constexpr unsigned kStage = 0x8009ECA0u;
constexpr unsigned kStockTable = 0x8009EB4Cu;
constexpr unsigned kDifficulty = 0x8009EABCu;
constexpr unsigned kTraffic = 0x8009EAC4u;
constexpr unsigned kCops = 0x8009EAC8u;
constexpr unsigned kPedestrians = 0x8009EACCu;
constexpr unsigned kRecordBytes = 36;
constexpr unsigned kRecordCount = 14;

std::atomic<bool> thrash_active{false};
bool menu_active = false;
bool restore_choices = true;
unsigned table_address = 0;
unsigned char *table_owner = nullptr;

unsigned read(unsigned char *rdram, unsigned address) {
    return MEM_W(0, guest_address(address));
}
void write(unsigned char *rdram, unsigned address, unsigned value) {
    MEM_W(0, guest_address(address)) = value;
}
bool offline() {
    return !rr64::netplay::get_status().active;
}
unsigned options() {
    return shared::thrash_options();
}
unsigned advance(unsigned value, unsigned maximum, bool increase) {
    value = std::min(value, maximum);
    return increase ? (value < maximum ? value + 1 : 0)
                    : (value ? value - 1 : maximum);
}
void copy_text(unsigned char *rdram, unsigned buffer, const char *label) {
    // Native text is sp+50..67. The next stack local begins at sp+68.
    for (unsigned i = 0; i < 23; ++i) {
        MEM_B(i, guest_address(buffer)) = label[i];
        if (!label[i])
            return;
    }
    MEM_B(23, guest_address(buffer)) = 0;
}

bool make_table(unsigned char *rdram) {
    if (!table_address) {
        auto *host = static_cast<unsigned char *>(
            recomp::alloc(rdram, kRecordCount * kRecordBytes));
        if (!host)
            return false;
        table_address = 0x80000000u + static_cast<unsigned>(host - rdram);
    }
    // Use the game's own colors/font/animation and keep its first three rows.
    for (unsigned i = 0; i < 8 * kRecordBytes; i += 4)
        write(rdram, table_address + i, read(rdram, kStockTable + i));
    for (unsigned row : {8u, 9u, 10u, 11u, 12u})
        for (unsigned i = 0; i < kRecordBytes; i += 4)
            write(rdram, table_address + row * kRecordBytes + i,
                  read(rdram, kStockTable + 7 * kRecordBytes + i));
    for (unsigned i = 0; i < kRecordBytes; i += 4)
        write(rdram, table_address + 13 * kRecordBytes + i, 0);

    // A non-null pointer to an empty label keeps the native loop alive while
    // our stack text supplies the entire row. Row 13 remains its null sentinel.
    const unsigned empty_label = table_address + 13 * kRecordBytes + 4;
    for (unsigned row : {4u, 8u, 9u, 10u, 11u, 12u})
        write(rdram, table_address + row * kRecordBytes, empty_label);


    // Fit all options above the native footer. Positive Y starts a group;
    // negative Y advances relative to the previous row.
    write(rdram, table_address + 8, 0x42480000u); // 50.0f
    for (unsigned row = 0; row <= 2; ++row) {
        write(rdram, table_address + row * kRecordBytes + 0x1C, 0x3F400000u);
        if (row)
            write(rdram, table_address + row * kRecordBytes + 8, 0xC1800000u);
    }
    for (unsigned row = 3; row <= 12; ++row) {
        write(rdram, table_address + row * kRecordBytes + 8,
              row == 3 ? 0xC1900000u : 0xC1400000u); // -18 / -12
        write(rdram, table_address + row * kRecordBytes + 0x1C, 0x3F000000u);
    }
    if (!(options() & 512u))
        write(rdram, table_address + 10 * kRecordBytes + 8, 0); // Hidden row adds no vertical spacing.
    return true;
}

void restore(unsigned char *rdram) {
    const unsigned stock = shared::thrash_stock_options();
    if (stock <= 0x7Fu && (stock & 7u) <= 4) {
        write(rdram, kDifficulty, stock & 7u);
        write(rdram, kTraffic, (stock >> 3) & 3u);
        write(rdram, kCops, (stock >> 5) & 3u);
    }
    write(rdram, kPedestrians, (options() >> 4) & 3u);
    write(rdram, menu::menu_dirty, 1);
    restore_choices = false;
}
} // namespace

extern "C" int rr64_thrash_options_active() {
    return thrash_active.load(std::memory_order_acquire) && offline();
}

extern "C" void rr64_thrash_options_mode(unsigned requested_mode) {
    menu_active = false;
    if (requested_mode == 0x21u) {
        // Entering mode 33 alone does not opt another context in; 256C0 must
        // actually execute. Restore the independent preset on the next menu.
        restore_choices = true;
    } else if (requested_mode < 0x11u || requested_mode > 0x15u) {
        thrash_active.store(false, std::memory_order_release);
        restore_choices = true;
    }
    if (!offline())
        thrash_active.store(false, std::memory_order_release);
}

extern "C" void rr64_thrash_options_begin(unsigned char *rdram) {
    menu_active = false;
    if (!rdram || !offline()) {
        thrash_active.store(false, std::memory_order_release);
        restore_choices = true;
        return;
    }
    if (table_owner != rdram) {
        table_owner = rdram;
        table_address = 0;
        restore_choices = true;
    }
    if (!thrash_active.exchange(true, std::memory_order_acq_rel))
        restore_choices = true;
}

extern "C" int rr64_thrash_options_input(unsigned char *rdram) {
    menu_active = rdram && rr64_thrash_options_active() && read(rdram, kStage) == 0 &&
                  make_table(rdram);
    if (!menu_active)
        return 0;
    if (restore_choices)
        restore(rdram);

    const unsigned row = read(rdram, menu::menu_cursor);
    const unsigned buttons = read(rdram, menu::menu_buttons) & 0x60u;
    unsigned bits = options();
    if (buttons && row >= 3 && row <= 12) {
        const bool increase = (buttons & 0x40u) != 0;
        if (row == 11 || row == 12) {
            shared::cycle_cpu_weapon(row == 12, increase);
        } else if (row == 3 || row == 5 || row == 6 || row == 7) {
            const unsigned field = row == 3 ? kDifficulty : row == 5 ? kCops
                                                      : row == 6 ? kTraffic : kPedestrians;
            const unsigned maximum = row == 3 ? 4u : 3u;
            write(rdram, field, advance(read(rdram, field), maximum, increase));
        } else if (row != 10 || (bits & 512u)) {
            const unsigned shift = row == 4 ? 0u : row == 8 ? 6u : row == 9 ? 9u : 10u;
            const unsigned mask = row == 4 ? 15u : row == 8 ? 7u : 1u;
            const unsigned maximum = row == 4 ? shared::max_ai(1) : mask;
            const unsigned value = advance((bits >> shift) & mask, maximum, increase);
            bits = (bits & ~(mask << shift)) | (value << shift);
            shared::set_thrash_options(bits);
            // Keep later weapon rows reachable when Custom Cop is switched off.
            write(rdram, table_address + 10 * kRecordBytes,
                  table_address + 13 * kRecordBytes + 4);
        }
        write(rdram, table_address + 10 * kRecordBytes + 8, bits & 512u ? 0xC1400000u : 0u);
        write(rdram, menu::menu_dirty, 1);
    }
    // Skip stock input for option rows, including the obsolete density pairs
    // that used to lower cops when raising opponents (and peds vs traffic).
    // Native normalization, resource scales and route selection still run.
    return row >= 3 && row <= 12;
}

extern "C" int rr64_thrash_options_navigation(unsigned char *rdram) {
    if (!menu_active)
        return -1;
    unsigned row = std::min(read(rdram, menu::menu_cursor), 12u);
    const unsigned mask = options() & 512u ? 0x1FFFu : 0x1BFFu;
    if (row == 10 && !(mask & (1u << 10)))
        row = 9;
    const unsigned buttons = read(rdram, menu::menu_buttons);
    if (buttons & 0x18u)
        row = shared::next_row(row, buttons & 8u ? -1 : 1, mask);
    write(rdram, menu::menu_cursor, row);
    return static_cast<int>(row);
}

extern "C" unsigned rr64_thrash_options_table(unsigned stock) {
    return menu_active ? table_address : stock;
}

extern "C" void rr64_thrash_options_text(unsigned char *rdram, unsigned row,
                                          unsigned buffer) {
    if (!menu_active || !valid_guest_range(buffer, 24))
        return;
    const unsigned bits = options();
    char label[24];
    if (row == 11 || row == 12) {
        std::snprintf(label, sizeof(label), "%s Wpn: %s", row == 12 ? "Cop" : "Racer", shared::cpu_weapon_name(row == 12));
        copy_text(rdram, buffer, label);
    } else if (row == 4) {
        std::snprintf(label, sizeof(label), "AI Racers: %u",
                      std::min(bits & 15u, shared::max_ai(1)));
        copy_text(rdram, buffer, label);
    } else if (row == 8) {
        const unsigned level = (bits >> 6) & 7u;
        if (level == 6)
            copy_text(rdram, buffer, "Bike Level: Scooter");
        else if (level == 7)
            copy_text(rdram, buffer, "Bike Level: Insanity");
        else if (level) {
            std::snprintf(label, sizeof(label), "Bike Level: %u", level);
            copy_text(rdram, buffer, label);
        } else
            copy_text(rdram, buffer, "Bike Level: Match Track");
    } else if (row == 9) {
        copy_text(rdram, buffer, bits & 512u ? "Custom Cop Mode: On" : "Custom Cop Mode: Off");
    } else if (row == 10) {
        copy_text(rdram, buffer, !(bits & 512u) ? "" : bits & 1024u ? "AI Cops: On" : "AI Cops: Off");
    }
}

extern "C" void rr64_thrash_options_finish(unsigned char *rdram) {
    if (!menu_active)
        return;
    const unsigned difficulty = read(rdram, kDifficulty);
    const unsigned traffic = read(rdram, kTraffic);
    const unsigned cops = read(rdram, kCops);
    const unsigned pedestrians = read(rdram, kPedestrians);
    if (difficulty <= 4 && traffic <= 3 && cops <= 3)
        shared::set_thrash_stock_options(difficulty | (traffic << 3) | (cops << 5));
    if (pedestrians <= 3)
        shared::set_thrash_options((options() & ~48u) | (pedestrians << 4));
    menu_active = false;
}
