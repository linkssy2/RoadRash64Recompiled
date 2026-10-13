#include "rr64_quadrant_hud.hpp"

#include "recomp.h"
#include "rr64_engine_layout.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void func_8001EB90(unsigned char *, recomp_context *); // Queue HUD sprite.
extern "C" void func_800795AC(unsigned char *, recomp_context *); // Queue HUD label.
extern "C" void rr64_hud_label_pin_view(unsigned char *, unsigned index, unsigned view);

namespace rr64::quadrant_hud {
namespace {
using namespace engine;

std::atomic<bool> requested{false};

constexpr unsigned layout_global = 0x800A4F24u, view_count = 0x800A6578u;
constexpr unsigned view_slots = 0x800A657Cu, actors = 0x800D8570u, actor_stride = 0x118u;
constexpr unsigned screen_width = 0x800B0808u, screen_height = 0x800B080Cu;
constexpr unsigned sprite_asset = 0x800D6870u, frame_counter = 0x800A1830u;
constexpr unsigned pickup_sprites = 0x8009F3DCu;
constexpr unsigned sprite_buffer = 0x8009CBA4u, sprite_counts = 0x800BC9D0u;
constexpr unsigned sprite_pool = 0x800BCAD8u, sprite_capacity = 96, sprite_stride = 76;
constexpr unsigned label_count = 0x800A76A0u, label_capacity = 64;
constexpr unsigned dl_cursor = 0x800AC650u, dl_bases = 0x800AC658u, dl_commands = 0x800BC9A0u;
// Explicit alignment: extended-GBI enable (8), SetRectAlign (16), scissor push
// (8), view scissor (16) and SetScissorAlign (24); then the SetRectAlign that
// restores no origin (16), the scissor pop (8) and the SetScissorAlign reset (24).
constexpr unsigned align_begin_bytes = 72, align_end_bytes = 48;
// Bar render state, two bars of four commands, closing mode reset, alignment.
constexpr unsigned dl_bytes_per_view = (7 + 2 * 4 + 1) * 8 + align_begin_bytes + align_end_bytes;
// 30220's locals: per-view HUD flags, drawing mask, outgoing call arguments
// and the label text buffer (its sprintf target at sp+48).
constexpr unsigned view_flags = 0x68u, draw_mask = 0xD8u;
constexpr unsigned call_area = 0x10u, call_area_size = 0x50u, text_buffer = 0x48u;

// Upper two-player panel, relative to its anchor (rodata 80004074..80004118),
// in 320x240 units: bars x-16..+20, icon at +32; box spans about x-21..+20.
constexpr float icon_offset = 32.0f, panel_left = 21.0f;
// Each quadrant view is 160x120 units. The panel sits bottom left at about
// 60% size, mirroring the native quadrant speedometer at the bottom right
// (8 units from the edge, centred 105 units down).
constexpr float view_units_x = 160.0f, view_units_y = 120.0f;
constexpr float panel_scale = 0.6f, view_margin = 8.0f, panel_centre_y = 105.0f;
constexpr unsigned no_origin = 0x800;
// Widescreen split-screen columns fill their half of the window, so a panel
// follows its view's left edge: the screen's left edge (origin 0) for the left
// column, the centre line (512) for the right. Matches the stock rank labels.
constexpr unsigned view_origin(unsigned view) { return view % 2 ? 512 : 0; }

// Sprites this frame's HUD queued, given an explicit alignment when flushed.
struct Queued {
    unsigned char *owner = nullptr;
    unsigned count = 0;
    std::array<unsigned, 12> records{};
    std::array<unsigned, 12> views{};
    bool aligned = false;
};
Queued queued;

unsigned word(unsigned char *m, unsigned address) {
    unsigned value = 0;
    read_u32(m, address, value);
    return value;
}
std::uint16_t half(unsigned char *m, unsigned address) {
    std::uint16_t value = 0;
    read_u16(m, address, value);
    return value;
}
float number(unsigned char *m, unsigned address) {
    float value = 0;
    read_float(m, address, value);
    return value;
}

// func_8001A250(value, 0, 1) as used by the stock bars.
float health_ratio(unsigned char *m, unsigned record, unsigned current, unsigned maximum) {
    const float top = number(m, record + maximum), value = number(m, record + current);
    if (!std::isfinite(top) || !std::isfinite(value) || top <= 0)
        return 0;
    return std::clamp(value / top, 0.0f, 1.0f);
}

struct DisplayList {
    unsigned char *m;
    unsigned cursor;
    void put(unsigned w0, unsigned w1) {
        write_u32(m, cursor, w0);
        write_u32(m, cursor + 4, w1);
        cursor += 8;
    }
    // RT64 SetRectAlign, encoded as rr64_hud_widgets does. An explicit origin
    // also exempts the draw from RT64's automatic race-HUD anchoring, which
    // would otherwise move only some of the panel's draws.
    void rect_align(unsigned origin, unsigned width) {
        put(0x64000006u, origin | (origin << 12));
        const int origin_x = origin < no_origin ? int(width) * 4 * int(origin) / 1024 : 0;
        const unsigned offset = unsigned(std::uint16_t(-origin_x)) << 16;
        put(offset, offset);
    }
    // The draw's scissor needs the same origins as its rect: with none, RT64
    // keeps the scissor in the centred 4:3 area and clips a panel anchored to
    // the screen's left edge. Clip to the view, as rr64_hud_widgets does.
    void align_begin(unsigned width, unsigned height, unsigned view) {
        put(0xE0525464u, 0x10000064u); // Standard RT64 F3DEX2 extended-GBI enable.
        rect_align(view_origin(view), width);
        const unsigned col = view % 2, row = view / 2;
        const unsigned left_origin = col ? 512 : 0, right_origin = col ? 1024 : 512;
        const int left = int(col * width / 2) * 4 - int(width) * 4 * int(left_origin) / 1024;
        const int right = int((col + 1) * width / 2) * 4 - int(width) * 4 * int(right_origin) / 1024;
        put(0x64000017u, 0); // Push scissor.
        put(0x64000005u, (left_origin << 2) | (right_origin << 14));
        put((unsigned(std::uint16_t(left)) << 16) | (row * height / 2 * 4),
            (unsigned(std::uint16_t(right)) << 16) | ((row + 1) * height / 2 * 4));
        // The sprite flush can restore its full-screen scissor (ED) inside a
        // record. Give standard scissors the same origins and clamp them to the
        // view; bounds are in origin-moved 10.2 units.
        scissor_align(left_origin, right_origin, col * width / 2 * 4, row * height / 2 * 4,
                      (col + 1) * width / 2 * 4, (row + 1) * height / 2 * 4);
    }
    void align_end() {
        rect_align(no_origin, 0);
        put(0x64000018u, 0); // Pop scissor.
        // RT64's default: no origins and no clamping. The bound fields are
        // unsigned 16-bit, and standard scissor values always lie within them.
        scissor_align(no_origin, no_origin, 0, 0, 0xFFFF, 0xFFFF);
    }
    void scissor_align(unsigned left_origin, unsigned right_origin, unsigned left, unsigned top,
                       unsigned right, unsigned bottom) {
        put(0x64000008u, left_origin | (right_origin << 12));
        put(0, 0); // No offsets.
        put((left << 16) | top, (right << 16) | bottom);
    }
};

unsigned coordinate(float value) {
    const int fixed = int(value);
    return fixed > 0 ? unsigned(fixed) & 0xFFFu : 0;
}

// One stock bar (323F0..326C4): prim colour fades red to green, then a
// texture rectangle in 10.2 coordinates and the E1/F1 halves that follow it.
void bar(DisplayList &dl, float ratio, float scale, float ulx, float uly, float lry,
         float sx, float x) {
    const float green = ratio * scale, red = scale - green;
    dl.put(0xFA000000u, (unsigned(red) << 24) | ((unsigned(green) & 0xFFu) << 16) | 0xFFu);
    const float lrx = x - (16.0f - ratio * 36.0f) * sx;
    dl.put(0xE4000000u | (coordinate(lrx * 4) << 12) | coordinate(lry * 4),
           (coordinate(ulx * 4) << 12) | coordinate(uly * 4));
    dl.put(0xE1000000u, 0);
    dl.put(0xF1000000u, 0);
}

bool display_list_room(unsigned char *m, unsigned cursor, unsigned bytes) {
    const unsigned buffer = word(m, sprite_buffer), commands = word(m, dl_commands);
    if (buffer >= 2 || commands == 0 || commands > 0x10000 || (cursor & 7))
        return false;
    const unsigned base = word(m, dl_bases + buffer * 4);
    const std::uint64_t end = std::uint64_t(base) + 0x140 + std::uint64_t(commands) * 8;
    return base >= kRdramBegin && end <= kRdramEnd && cursor >= base &&
           std::uint64_t(cursor) + bytes <= end;
}

struct Call {
    recomp_context ctx;
    unsigned char *m;
    unsigned stack;
    Call(unsigned char *rdram, const recomp_context &caller)
        : ctx(caller), m(rdram), stack(static_cast<unsigned>(caller.r29)) {
        ctx.f_odd = &ctx.f0.u32h;
        for (unsigned offset = call_area; offset < text_buffer; offset += 4)
            write_u32(m, stack + offset, 0);
    }
    void arg(unsigned offset, float value) { write_float(m, stack + offset, value); }
    void arg(unsigned offset, unsigned value) { write_u32(m, stack + offset, value); }
};

// One sprite exactly as the stock block queues it (32DD4..32F84), remembered
// with its view's origin so its flush can be given an explicit alignment.
void sprite(unsigned char *m, const recomp_context &caller, unsigned view, unsigned id, float x,
            float y, float sx, float sy, unsigned flags, std::initializer_list<unsigned> unit_scales) {
    const unsigned buffer = word(m, sprite_buffer), index = half(m, sprite_counts + buffer * 2);
    Call call(m, caller);
    call.ctx.r4 = guest_address(word(m, sprite_asset));
    call.ctx.r5 = id;
    call.ctx.r6 = std::bit_cast<unsigned>(x);
    call.ctx.r7 = std::bit_cast<unsigned>(y);
    call.arg(0x1C, sx);
    call.arg(0x20, sy);
    for (unsigned offset : unit_scales)
        call.arg(offset, 1.0f);
    call.arg(0x44, flags);
    func_8001EB90(m, &call.ctx);
    if (half(m, sprite_counts + buffer * 2) == index + 1 && queued.count < queued.records.size()) {
        queued.views[queued.count] = view;
        queued.records[queued.count++] = sprite_pool + (buffer * sprite_capacity + index) * sprite_stride;
    }
}

void weapon_box(unsigned char *m, const recomp_context &caller, unsigned view, unsigned rider,
                float x, float y, float sx, float sy) {
    const unsigned stack = static_cast<unsigned>(caller.r29);
    const unsigned weapon = word(m, rider + 0x5B0), inventory = word(m, rider + 0x584);
    std::uint16_t raw = 0;
    const bool counted = weapon < 0x40 && read_u16(m, inventory + weapon * 2 + 0x838, raw);
    const int quantity = std::int16_t(raw);
    const float icon_x = x + icon_offset * sx;
    const unsigned label = word(m, label_count);
    if (counted && quantity >= 2 && label < label_capacity) {
        // sprintf(sp+48, "%d", n) and the stock label call (32D60..32DC8).
        char text[12];
        const int length = std::snprintf(text, sizeof(text), "%d", quantity);
        for (int i = 0; i <= length; ++i)
            write_s8(m, stack + text_buffer + i, std::int8_t(text[i]));
        Call call(m, caller);
        call.ctx.r4 = guest_address(stack + text_buffer);
        call.ctx.r5 = std::bit_cast<unsigned>(icon_x);
        call.ctx.r6 = std::bit_cast<unsigned>(y);
        call.ctx.r7 = 0xFF;
        call.arg(0x10, 0xFFu);
        call.arg(0x14, 0xFFu);
        call.arg(0x18, 0xFFu);
        call.arg(0x2C, 1u);
        call.arg(0x38, 0.5f * panel_scale);
        func_800795AC(m, &call.ctx);
        if (word(m, label_count) == label + 1)
            rr64_hud_label_pin_view(m, label, view);
    }
    sprite(m, caller, view, 0x3D, x, y, sx, sy, 0, {0x24, 0x34, 0x38, 0x3C});
    if (counted)
        sprite(m, caller, view, (weapon + 0xA3) & 0xFFFFu, icon_x, y, sx, sy, 0x400,
               {0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C});
    // Pickup indicator: steady while its timer is above 5, else blinking.
    const unsigned pickup = word(m, rider + 0x5D4);
    if (pickup && pickup < 0x100 &&
        (number(m, rider + 0x5D8) > 5.0f || word(m, frame_counter) % 10 < 5))
        sprite(m, caller, view, half(m, pickup_sprites + pickup * 2), icon_x, y, sx * .5f, sy * .5f, 0,
               {0x24, 0x28, 0x34});
}
} // namespace

void set_enabled(bool value) { requested.store(value, std::memory_order_release); }
bool enabled() { return requested.load(std::memory_order_acquire); }
} // namespace rr64::quadrant_hud

extern "C" void rr64_quadrant_hud_draw(unsigned char *m, void *context) {
    using namespace rr64::quadrant_hud;
    using namespace rr64::engine;
    queued = {};
    if (!m || !context || !enabled())
        return;
    const auto &caller = *static_cast<recomp_context *>(context);
    const unsigned stack = static_cast<unsigned>(caller.r29);
    // A paused frame jumps straight to 342C4 before the view flags are built.
    const unsigned views = word(m, view_count);
    if (half(m, globals::gameplay_pause_state) || word(m, layout_global) < 2 || views < 3 ||
        views > 4 || !valid_guest_range(stack, 0xDC) || !(word(m, stack + draw_mask) & 1))
        return;
    const unsigned width = word(m, screen_width), height = word(m, screen_height);
    if (!width || !height || width > 4096 || height > 4096 ||
        !valid_guest_range(word(m, sprite_asset), 0x1C))
        return;
    queued.owner = m;
    const float sx = width / 320.0f * panel_scale, sy = height / 240.0f * panel_scale;
    // The sprite and label producers use the outgoing argument area and text
    // buffer of 30220's frame; keep them as the stock exit left them.
    std::array<unsigned char, call_area_size> saved;
    std::memcpy(saved.data(), m + (stack + call_area - kRdramBegin), saved.size());
    for (unsigned view = 0; view < views; ++view) {
        const unsigned slot = word(m, view_slots + view * 4);
        // Finished or busted riders have no stock HUD either.
        if (!half(m, stack + view_flags + view * 2) || slot >= kMaximumRacers)
            continue;
        const unsigned actor = actors + slot * actor_stride;
        const unsigned rider = word(m, actor + 0xE4), bike = word(m, actor + 0xE0);
        if (!valid_guest_range(rider, 0x600) || !valid_guest_range(bike, 0x500))
            continue;
        const float x = ((view % 2) * view_units_x + view_margin) * width / 320.0f + panel_left * sx;
        const float y = ((view / 2) * view_units_y + panel_centre_y) * height / 240.0f;
        DisplayList dl{m, word(m, dl_cursor)};
        if (display_list_room(m, dl.cursor, dl_bytes_per_view)) {
            dl.align_begin(width, height, view);
            // Stock bar render state (323F0..324A4), without the world depth
            // compare/write bits (0x30), as the 1P/2P bar hooks clear them.
            dl.put(0xE7000000u, 0);
            dl.put(0xE3000A01u, 0);
            dl.put(0xFCFFFFFFu, 0xFFFDF6FBu);
            dl.put(0xD7000000u, 0x80008000u);
            dl.put(0xE200001Cu, 0x00552200u);
            dl.put(0xE2001D00u, 4);
            dl.put(0xEE000000u, 0x01000000u);
            bar(dl, health_ratio(m, rider, 0x310, 0x30C), 200.0f, x - 16 * sx, y - 8 * sy,
                y - 4 * sy, sx, x);
            bar(dl, health_ratio(m, bike, 0x4F8, 0x4FC), 255.0f, x - 16 * sx, y + 3 * sy,
                y + 8 * sy, sx, x);
            dl.put(0xE2001D00u, 0);
            dl.align_end();
            write_u32(m, dl_cursor, dl.cursor);
        }
        const unsigned buffer = word(m, sprite_buffer);
        if (buffer < 2 && half(m, sprite_counts + buffer * 2) + 3 <= sprite_capacity)
            weapon_box(m, caller, view, rider, x, y, sx, sy);
    }
    std::memcpy(m + (stack + call_area - kRdramBegin), saved.data(), saved.size());
}

extern "C" void rr64_quadrant_hud_sprite_begin(unsigned char *m, unsigned record) {
    using namespace rr64::quadrant_hud;
    const auto end = queued.records.begin() + queued.count;
    const auto found = std::find(queued.records.begin(), end, record);
    if (queued.owner != m || queued.aligned || found == end)
        return;
    DisplayList dl{m, word(m, dl_cursor)};
    if (!display_list_room(m, dl.cursor, align_begin_bytes + align_end_bytes))
        return;
    dl.align_begin(word(m, screen_width), word(m, screen_height),
                   queued.views[found - queued.records.begin()]);
    write_u32(m, dl_cursor, dl.cursor);
    queued.aligned = true;
}

extern "C" void rr64_quadrant_hud_sprite_end(unsigned char *m) {
    using namespace rr64::quadrant_hud;
    if (queued.owner != m || !queued.aligned)
        return;
    queued.aligned = false;
    DisplayList dl{m, word(m, dl_cursor)};
    dl.align_end();
    write_u32(m, dl_cursor, dl.cursor);
}
