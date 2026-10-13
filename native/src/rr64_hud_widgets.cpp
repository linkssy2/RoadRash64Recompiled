#include "recomp.h"
#include "rr64_native.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
constexpr uint32_t text_base = 0x800D9650, text_stride = 88, text_capacity = 64;
constexpr uint32_t sprite_base = 0x800BCAD8, sprite_stride = 76, sprite_capacity = 96;
constexpr uint16_t no_origin = 0x800;
int64_t guest(uint32_t p) { return static_cast<int32_t>(p); }
uint32_t read(unsigned char *rdram, uint32_t p) { return MEM_W(0, guest(p)); }
void write(unsigned char *rdram, uint32_t p, uint32_t v) { MEM_W(0, guest(p)) = v; }
float number(unsigned char *rdram, uint32_t p) {
    const auto bits = read(rdram, p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
void number(unsigned char *rdram, uint32_t p, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    write(rdram, p, bits);
}
struct Placement {
    uint16_t origin = no_origin;
    int16_t y_offset = 0;
    uint16_t left = 0, top = 0, right = 0, bottom = 0, clip_left_origin = 0,
             clip_right_origin = 1024, width = 0;
};
struct Frame {
    unsigned char *owner = nullptr;
    uint32_t first = 0, end = 0, views = 1;
    bool drawing = false;
    bool paused = false;
    std::array<Placement, text_capacity> labels{};
};
thread_local Frame frame;
struct SpriteSpan {
    unsigned char *owner = nullptr;
    uint32_t buffer = 2, first = 0;
};
thread_local SpriteSpan countdown;

// These are the original HUD producer's layout inputs. Online's scoped HUD
// override is already active when begin() runs, so remote peers remain fullscreen.
uint32_t views(unsigned char *rdram) {
    const auto layout = read(rdram, 0x800A4F24), count = read(rdram, 0x800A6578);
    return layout != 0 && count >= 2 && count <= 4 ? count : 1;
}
bool align(unsigned char *rdram, Placement p, bool enable) {
    auto dl = read(rdram, 0x800AC650); // Native display-list write cursor.
    const auto buffer = read(rdram, 0x8009CBA4), commands = read(rdram, 0x800BC9A0);
    if (buffer >= 2 || commands == 0 || commands > 0x10000)
        return false;
    const auto base = read(rdram, 0x800AC658 + buffer * 4);
    const uint64_t end = uint64_t(base) + 0x140 + uint64_t(commands) * 8;
    // Leave room for a maximum-length native label and its restore command.
    // Near capacity, preserve the original drawing rather than grow past the pool.
    if (base < 0x80000000 || end > 0x80800000 || dl < base + 0x148 ||
        uint64_t(dl) + (enable ? 8192 : 24) > end || (dl & 7))
        return false;
    if (enable) {
        // Standard RT64 F3DEX2 extended-GBI enable, followed by SetRectAlign.
        write(rdram, dl, 0xE0525464);
        write(rdram, dl + 4, 0x10000064);
        dl += 8;
    }
    write(rdram, dl, 0x64000006);
    write(rdram, dl + 4, uint32_t(p.origin) | (uint32_t(p.origin) << 12));
    // Extended GBI positions are relative to their explicit origin. RDP adds
    // that origin back before recording draw/scissor bounds. Feeding absolute
    // positions here doubled right-edge coordinates and widened the framebuffer
    // pair, which also pillarboxed the world. Cancel that addition, not the
    // renderer's final widescreen anchoring.
    const int origin_x = p.origin < no_origin ? int(p.width) * 4 * p.origin / 1024 : 0;
    const uint32_t offset = (uint32_t(uint16_t(-origin_x)) << 16) | uint16_t(p.y_offset);
    write(rdram, dl + 8, offset);
    write(rdram, dl + 12, offset);
    dl += 16;
    if (enable) {
        // Clip the complete widget to its player's view; restore the prior
        // scissor afterwards so menus and other players cannot inherit it.
        write(rdram, dl, 0x64000017);
        write(rdram, dl + 4, 0);
        dl += 8;
        write(rdram, dl, 0x64000005);
        write(rdram, dl + 4,
              (uint32_t(p.clip_left_origin) << 2) | (uint32_t(p.clip_right_origin) << 14));
        const int left = int(p.left) * 4 - int(p.width) * 4 * p.clip_left_origin / 1024;
        const int right = int(p.right) * 4 - int(p.width) * 4 * p.clip_right_origin / 1024;
        write(rdram, dl + 8, (uint32_t(uint16_t(left)) << 16) | uint32_t(p.top * 4));
        write(rdram, dl + 12, (uint32_t(uint16_t(right)) << 16) | uint32_t(p.bottom * 4));
        dl += 16;
    } else {
        write(rdram, dl, 0x64000018);
        write(rdram, dl + 4, 0);
        dl += 8;
    }
    write(rdram, 0x800AC650, dl);
    return true;
}
} // namespace

extern "C" void rr64_hud_widgets_begin(unsigned char *rdram) {
    frame = {};
    frame.owner = rdram;
    frame.views = views(rdram);
    frame.paused = MEM_HU(0, guest(0x800A2192)) != 0;
    frame.first = read(rdram, 0x800A76A0);
}
extern "C" void rr64_hud_widgets_end(unsigned char *rdram) {
    if (frame.owner != rdram || frame.views < 2)
        return;
    frame.end = read(rdram, 0x800A76A0);
    if (frame.first > frame.end || frame.end > text_capacity) {
        frame.end = 0;
        return;
    }
    const float width = float(read(rdram, 0x800B0808)), height = float(read(rdram, 0x800B080C));
    if (width <= 0 || height <= 0) {
        frame.end = 0;
        return;
    }
    const float vw = frame.views > 2 ? width / 2 : width, vh = height / 2;
    for (uint32_t i = frame.first; i < frame.end; ++i) {
        const float x = number(rdram, text_base + i * text_stride + 0x40);
        const float y = number(rdram, text_base + i * text_stride + 0x44);
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= width || y >= height)
            continue;
        const unsigned col = frame.views > 2 && x >= vw ? 1 : 0, row = y >= vh ? 1 : 0;
        if ((frame.views > 2 ? row * 2 + col : row) >= frame.views)
            continue;
        const float local_x = x - col * vw, local_y = y - row * vh;
        // Choose once from the WHOLE label's authored origin, never from its
        // individual glyphs or colored backing. Long names retain one anchor.
        if (local_x <= vw * .36f && (local_y <= vh * .36f || local_y >= vh * .75f)) {
            frame.labels[i] = {uint16_t(col ? 512 : 0),
                               int16_t(row && local_y < vh * .25f ? height * .05f * 4 : 0)};
        } else if (local_x >= vw * .68f && local_y >= vh * .62f) {
            frame.labels[i] = {uint16_t(frame.views > 2 && !col ? 512 : 1024), 0};
        }
        auto &p = frame.labels[i];
        p.left = uint16_t(col * vw);
        p.top = uint16_t(row * vh);
        p.right = uint16_t((col + 1) * vw);
        p.bottom = uint16_t((row + 1) * vh);
        p.clip_left_origin = col ? 512 : 0;
        p.clip_right_origin = frame.views > 2 && !col ? 512 : 1024;
        p.width = uint16_t(width);
    }
}
// A label queued after rr64_hud_widgets_end with a fixed place in a 3/4-player
// view: anchor it to the view's left edge, as the stock rank labels above are,
// and clip it to the view.
extern "C" void rr64_hud_label_pin_view(unsigned char *rdram, unsigned index, unsigned view) {
    if (frame.owner != rdram || frame.views < 3 || view >= frame.views || index < frame.first ||
        index >= text_capacity)
        return;
    const float width = float(read(rdram, 0x800B0808)), height = float(read(rdram, 0x800B080C));
    if (width <= 0 || height <= 0)
        return;
    const float vw = width / 2, vh = height / 2;
    const unsigned col = view % 2, row = view / 2;
    auto &p = frame.labels[index];
    p = {uint16_t(col ? 512 : 0), 0};
    p.left = uint16_t(col * vw);
    p.top = uint16_t(row * vh);
    p.right = uint16_t((col + 1) * vw);
    p.bottom = uint16_t((row + 1) * vh);
    p.clip_left_origin = col ? 512 : 0;
    p.clip_right_origin = col ? 1024 : 512;
    p.width = uint16_t(width);
    if (frame.end <= index)
        frame.end = index + 1;
}
extern "C" void rr64_hud_label_begin(unsigned char *rdram, unsigned record) {
    frame.drawing = false;
    if (frame.owner != rdram || record < text_base || (record - text_base) % text_stride)
        return;
    const unsigned index = (record - text_base) / text_stride;
    if (index >= text_capacity)
        return;
    Placement p;
    if (frame.paused && frame.views > 1 && index < read(rdram, 0x800A76A0)) {
        // Pause is one fullscreen menu, not a lower player's top-left widget.
        // Give all its queued labels a neutral center origin so the automatic
        // race HUD classifier cannot independently move the Options label.
        p.origin = 512;
        p.width = p.right = uint16_t(read(rdram, 0x800B0808));
        p.bottom = uint16_t(read(rdram, 0x800B080C));
    } else {
        if (index < frame.first || index >= frame.end)
            return;
        p = frame.labels[index];
    }
    if (p.origin == no_origin)
        return;
    frame.drawing = align(rdram, p, true);
}
extern "C" void rr64_hud_label_end(unsigned char *rdram) {
    if (frame.owner == rdram && frame.drawing)
        align(rdram, {}, false);
    frame.drawing = false;
}
extern "C" void rr64_hud_widgets_clear(unsigned char *rdram) {
    rr64_hud_label_end(rdram);
    frame = {};
}
extern "C" void rr64_hud_countdown_begin(unsigned char *rdram) {
    countdown = {rdram, read(rdram, 0x8009CBA4), 0};
    if (countdown.buffer < 2)
        countdown.first = MEM_HU(0, guest(0x800BC9D0 + countdown.buffer * 2));
}
extern "C" void rr64_hud_countdown_end(unsigned char *rdram) {
    const auto span = countdown;
    countdown = {};
    if (span.owner != rdram || span.buffer >= 2 || frame.owner != rdram || frame.views < 2 ||
        read(rdram, 0x8009CBA4) != span.buffer)
        return;
    const uint32_t count_address = 0x800BC9D0 + span.buffer * 2;
    const unsigned count = MEM_HU(0, guest(count_address));
    if (count != span.first + 1 || span.first + frame.views > sprite_capacity)
        return;
    const auto source = sprite_base + (span.buffer * sprite_capacity + span.first) * sprite_stride;
    std::array<unsigned char, sprite_stride> original;
    std::memcpy(original.data(), rdram + (source - 0x80000000), sprite_stride);
    const float width = float(read(rdram, 0x800B0808)), height = float(read(rdram, 0x800B080C));
    if (width <= 0 || height <= 0)
        return;
    const float vw = frame.views > 2 ? width / 2 : width, vh = height / 2;
    // Duplicate only the exact sprite appended by native 6AF20. Same asset,
    // countdown phase, tint and timing; no guessing based on texture or position.
    for (unsigned i = 0; i < frame.views; ++i) {
        const auto target = source + i * sprite_stride;
        std::memcpy(rdram + (target - 0x80000000), original.data(), sprite_stride);
        number(rdram, target + 0x14, (frame.views > 2 ? i % 2 : 0) * vw + vw / 2);
        number(rdram, target + 0x18, (frame.views > 2 ? i / 2 : i) * vh + vh / 2);
        number(rdram, target + 0x20, number(rdram, target + 0x20) * .5f);
        number(rdram, target + 0x24, number(rdram, target + 0x24) * .5f);
    }
    MEM_H(0, guest(count_address)) = span.first + frame.views;
}
