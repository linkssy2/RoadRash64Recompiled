#include "recomp.h"
#include "rr64_engine_layout.hpp"
#include "rr64_quadrant_hud.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace rr64::engine;
int failures = 0;
void check(bool value, const char *label) {
    if (!value) {
        std::fprintf(stderr, "[QUADRANT-HUD] FAILED: %s\n", label);
        ++failures;
    }
}
constexpr unsigned stack = 0x807F0000u, actors = 0x800D8570u, slots = 0x800A657Cu;
constexpr unsigned dl_base = 0x80300000u, dl_cursor = 0x800AC650u, asset = 0x80310000u;
constexpr unsigned riders = 0x80320000u, bikes = 0x80330000u, inventory = 0x80340000u;
gpr guest(unsigned address) { return static_cast<gpr>(static_cast<std::int32_t>(address)); }
unsigned word(unsigned char *m, unsigned a) {
    unsigned v = 0;
    read_u32(m, a, v);
    return v;
}
float number(unsigned char *m, unsigned a) {
    float v = 0;
    read_float(m, a, v);
    return v;
}
float bits(gpr value) { return std::bit_cast<float>(static_cast<std::uint32_t>(value)); }

struct Sprite {
    unsigned id;
    float x, y, sx, sy;
    unsigned flags;
};
struct Label {
    std::string text;
    float x, y, scale;
};
std::vector<Sprite> sprites;
std::vector<Label> labels;
std::vector<std::pair<unsigned, unsigned>> pins;

void seed(unsigned char *m, recomp_context &ctx, unsigned views) {
    std::memset(m, 0, kRdramSize);
    write_u32(m, 0x800A4F24u, 2); // Quadrant layout.
    write_u32(m, 0x800A6578u, views);
    for (unsigned view = 0; view < 4; ++view) {
        write_u32(m, slots + view * 4, view);
        write_u16(m, stack + 0x68 + view * 2, 1);
        const unsigned actor = actors + view * 0x118u, rider = riders + view * 0x1000u;
        const unsigned bike = bikes + view * 0x1000u;
        write_u32(m, actor + 0xE4, rider);
        write_u32(m, actor + 0xE0, bike);
        write_float(m, rider + 0x30C, 100.f);
        write_float(m, rider + 0x310, 50.f);
        write_float(m, bike + 0x4FC, 100.f);
        write_float(m, bike + 0x4F8, 100.f);
        write_u32(m, rider + 0x5B0, view);       // Selected weapon.
        write_u32(m, rider + 0x584, inventory);
        write_u16(m, inventory + view * 2 + 0x838, std::uint16_t(view + 1));
    }
    write_u32(m, stack + 0xD8, 1); // Native weapon/HUD drawing mask.
    write_u32(m, 0x800B0808u, 320);
    write_u32(m, 0x800B080Cu, 240);
    write_u32(m, 0x800D6870u, asset);
    write_u32(m, 0x8009CBA4u, 0);
    write_u32(m, 0x800AC658u, dl_base);
    write_u32(m, 0x800BC9A0u, 0x2000);
    write_u32(m, dl_cursor, dl_base + 0x400);
    ctx = {};
    ctx.r29 = guest(stack);
    ctx.r30 = guest(actors);
    write_u16(m, 0x800BC9D0u, 5); // Earlier HUD sprites in this frame's buffer.
    sprites.clear();
    labels.clear();
    pins.clear();
}
} // namespace

extern "C" void func_8001EB90(unsigned char *m, recomp_context *ctx) {
    const unsigned sp = static_cast<unsigned>(ctx->r29);
    check(static_cast<unsigned>(ctx->r4) == asset, "sprites use the HUD sprite asset");
    sprites.push_back({static_cast<unsigned>(ctx->r5), bits(ctx->r6), bits(ctx->r7),
                       number(m, sp + 0x1C), number(m, sp + 0x20), word(m, sp + 0x44)});
    write_u16(m, 0x800BC9D0u, std::uint16_t(sprites.size() + 5));
}
extern "C" void rr64_hud_label_pin_view(unsigned char *, unsigned index, unsigned view) {
    pins.emplace_back(index, view);
}
extern "C" void func_800795AC(unsigned char *m, recomp_context *ctx) {
    const unsigned sp = static_cast<unsigned>(ctx->r29);
    std::string text;
    for (unsigned a = static_cast<unsigned>(ctx->r4); m[(a - kRdramBegin) ^ 3]; ++a)
        text.push_back(char(m[(a - kRdramBegin) ^ 3]));
    labels.push_back({text, bits(ctx->r5), bits(ctx->r6), number(m, sp + 0x38)});
    write_u32(m, 0x800A76A0u, word(m, 0x800A76A0u) + 1);
}

int main() {
    std::vector<unsigned char> memory(kRdramSize);
    auto *m = memory.data();
    recomp_context ctx{};

    seed(m, ctx, 4);
    auto before = memory;
    rr64_quadrant_hud_draw(m, &ctx);
    check(memory == before && sprites.empty(), "disabled option is a no-op");
    rr64::quadrant_hud::set_enabled(true);

    for (const auto &[layout, views, mask, paused, label] :
         {std::tuple{1u, 2u, 1u, 0, "stock two-player HUD untouched"},
          std::tuple{0u, 1u, 1u, 0, "single view untouched"},
          std::tuple{2u, 2u, 1u, 0, "two quadrant views untouched"},
          std::tuple{2u, 4u, 0u, 0, "disabled native HUD mask untouched"},
          std::tuple{2u, 4u, 1u, 1, "paused frame untouched"}}) {
        seed(m, ctx, views);
        write_u32(m, 0x800A4F24u, layout);
        write_u32(m, stack + 0xD8, mask);
        write_u16(m, globals::gameplay_pause_state, std::uint16_t(paused));
        before = memory;
        rr64_quadrant_hud_draw(m, &ctx);
        check(memory == before && sprites.empty() && labels.empty(), label);
    }

    // Four players, view 2 finished: three panels at 60% size, bottom left.
    seed(m, ctx, 4);
    write_u16(m, stack + 0x6C, 0);
    write_u32(m, stack + 0x10, 0xA5A5A5A5u); // Caller's argument area must survive.
    rr64_quadrant_hud_draw(m, &ctx);
    check(word(m, stack + 0x10) == 0xA5A5A5A5u, "caller argument area restored");
    const unsigned dl_end = word(m, dl_cursor);
    check(dl_end == dl_base + 0x400 + 3 * 31 * 8, "three views of aligned bar commands appended");
    const auto near = [](unsigned value, float expected) {
        return std::fabs(float(value) - expected * 4) <= 1.0f;
    };
    unsigned rects = 0, aligns = 0, scissors = 0, scissor_aligns = 0;
    for (unsigned a = dl_base + 0x400; a < dl_end; a += 8) {
        if (word(m, a) == 0x64000006u) {
            // The view's origin while drawing (left column 0, right column
            // 512), no origin afterwards. Drawn views are 0, 1 and 3.
            const bool right = aligns / 2 != 0;
            check(word(m, a + 4) == (aligns % 2 ? 0x800800u : right ? 0x200200u : 0u),
                  "bar alignment origin");
            check(word(m, a + 8) == (aligns % 2 || !right ? 0u : 0xFD800000u),
                  "bar alignment offset");
            ++aligns;
            a += 8;
            continue;
        }
        if (word(m, a) == 0x64000008u) {
            // Standard scissors take the view's origins while drawing (bounds
            // clamp to the view), then the default.
            const unsigned view = scissor_aligns / 2 == 2 ? 3 : scissor_aligns / 2;
            const unsigned col = view % 2, row = view / 2;
            const bool reset = scissor_aligns % 2;
            check(word(m, a + 4) == (reset ? 0x800800u : col ? 0x400200u : 0x200000u) &&
                      word(m, a + 8) == 0u && word(m, a + 12) == 0u &&
                      word(m, a + 16) == (reset ? 0u : (col * 640u) << 16 | row * 480u) &&
                      word(m, a + 20) == (reset ? 0xFFFFFFFFu
                                                : (col * 640u + 640u) << 16 | (row * 480u + 480u)),
                  "bar scissor alignment");
            ++scissor_aligns;
            a += 16;
            continue;
        }
        if (word(m, a) == 0x64000005u) {
            // View scissor with matching origins: views 0, 1 and 3.
            const unsigned view = scissors == 2 ? 3 : scissors, col = view % 2, row = view / 2;
            check(word(m, a + 4) == (col ? (512u << 2) | (1024u << 14) : 512u << 14) &&
                      // Each view edge is its origin, so the x offsets are zero.
                      word(m, a + 8) == row * 480u && word(m, a + 12) == row * 480u + 480u,
                  "bar scissor clipped to its view");
            ++scissors;
            a += 8;
            continue;
        }
        if ((word(m, a) >> 24) != 0xE4u)
            continue;
        const unsigned w0 = word(m, a), w1 = word(m, a + 4);
        const unsigned view = rects / 2 == 2 ? 3 : rects / 2, bike = rects % 2;
        const float ax = (view % 2) * 160.f + 8.f + 21.f * .6f, ay = (view / 2) * 120.f + 105.f;
        const float ratio = bike ? 1.f : .5f;
        check(near((w1 >> 12) & 0xFFF, ax - 16.f * .6f), "bar left edge");
        check(near((w0 >> 12) & 0xFFF, ax + (-16.f + 36.f * ratio) * .6f), "bar fill length");
        check(near(w1 & 0xFFF, ay + (bike ? 3.f : -8.f) * .6f), "bar top");
        check(near(w0 & 0xFFF, ay + (bike ? 8.f : -4.f) * .6f), "bar bottom");
        const unsigned colour = word(m, a - 4);
        check(colour == (bike ? 0x00FF00FFu : 0x646400FFu), "bar fades red to green");
        ++rects;
    }
    check(rects == 6 && aligns == 6 && scissors == 3 && scissor_aligns == 6,
          "rider and bike bar per drawn view, each aligned and clipped");

    unsigned boxes = 0;
    for (const auto &s : sprites) {
        if (s.id != 0x3D)
            continue;
        const unsigned view = boxes == 2 ? 3 : boxes;
        check(std::fabs(s.x - ((view % 2) * 160.f + 8.f + 21.f * .6f)) < .01f &&
                  s.y == (view / 2) * 120.f + 105.f,
              "weapon box at the view's bottom left");
        check(std::fabs(s.sx - .6f) < 1e-6f && std::fabs(s.sy - .6f) < 1e-6f && s.flags == 0,
              "weapon box at 60% size");
        ++boxes;
    }
    check(boxes == 3, "weapon box per drawn view");
    check(sprites.size() == 6 && sprites[1].id == 0xA3 && sprites[3].id == 0xA4 &&
              sprites[5].id == 0xA6 && sprites[5].flags == 0x400,
          "weapon icons follow each rider's selection");
    check(labels.size() == 2 && labels[0].text == "2" && labels[1].text == "4" &&
              std::fabs(labels[0].x - (160.f + 8.f + 53.f * .6f)) < .01f &&
              labels[1].y == 225.f && std::fabs(labels[0].scale - .3f) < 1e-6f,
          "quantity labels only for two or more, beside the icon");
    check(pins.size() == 2 && pins[0] == std::pair{0u, 1u} && pins[1] == std::pair{1u, 3u},
          "quantity labels pinned to their views");

    // The sprite flush aligns exactly the queued panel sprites.
    const unsigned buffer0 = 0x800BCAD8u;
    write_u32(m, dl_cursor, dl_base + 0x800);
    rr64_quadrant_hud_sprite_begin(m, buffer0 + 4 * 76); // Earlier HUD sprite.
    rr64_quadrant_hud_sprite_end(m);
    check(word(m, dl_cursor) == dl_base + 0x800, "other sprites untouched");
    rr64_quadrant_hud_sprite_begin(m, buffer0 + 5 * 76); // View 0's weapon box.
    check(word(m, dl_cursor) == dl_base + 0x800 + 72 && word(m, dl_base + 0x808) == 0x64000006u &&
              word(m, dl_base + 0x80C) == 0u && word(m, dl_base + 0x810) == 0u &&
              word(m, dl_base + 0x818) == 0x64000017u && word(m, dl_base + 0x820) == 0x64000005u &&
              word(m, dl_base + 0x824) == 512u << 14 && word(m, dl_base + 0x828) == 0u &&
              word(m, dl_base + 0x82C) == 480u && word(m, dl_base + 0x830) == 0x64000008u &&
              word(m, dl_base + 0x834) == 0x200000u && word(m, dl_base + 0x840) == 0u &&
              word(m, dl_base + 0x844) == (640u << 16 | 480u),
          "left-column panel sprite aligned and clipped to the screen's left edge");
    rr64_quadrant_hud_sprite_end(m);
    check(word(m, dl_cursor) == dl_base + 0x800 + 120 && word(m, dl_base + 0x84C) == 0x800800u &&
              word(m, dl_base + 0x858) == 0x64000018u && word(m, dl_base + 0x860) == 0x64000008u &&
              word(m, dl_base + 0x864) == 0x800800u && word(m, dl_base + 0x874) == 0xFFFFFFFFu,
          "alignment and scissor restored after the panel sprite");
    rr64_quadrant_hud_sprite_end(m);
    check(word(m, dl_cursor) == dl_base + 0x800 + 120, "unmatched end is a no-op");
    rr64_quadrant_hud_sprite_begin(m, buffer0 + 7 * 76); // View 1's weapon box.
    check(word(m, dl_base + 0x880) == 0x64000006u && word(m, dl_base + 0x884) == 0x200200u &&
              word(m, dl_base + 0x888) == 0xFD800000u &&
              word(m, dl_base + 0x89C) == (512u << 2 | 1024u << 14) &&
              word(m, dl_base + 0x8AC) == 0x400200u && word(m, dl_base + 0x8B8) == 640u << 16 &&
              word(m, dl_base + 0x8BC) == (1280u << 16 | 480u),
          "right-column panel sprite aligned and clipped to the centre line");
    rr64_quadrant_hud_sprite_end(m);

    // Pickup indicator follows the stock steady/blink rule.
    seed(m, ctx, 3);
    write_u32(m, riders + 0x5D4, 2);
    write_float(m, riders + 0x5D8, 9.f);
    write_u16(m, 0x8009F3DCu + 4, 0x77);
    rr64_quadrant_hud_draw(m, &ctx);
    check(sprites.size() == 7 && sprites[2].id == 0x77 && std::fabs(sprites[2].sx - .3f) < 1e-6f,
          "pickup indicator drawn at half panel size");

    // A nearly full display list skips the bars but keeps the queued box.
    seed(m, ctx, 3);
    write_u32(m, dl_cursor, dl_base + 0x140 + 0x2000 * 8 - 16);
    rr64_quadrant_hud_draw(m, &ctx);
    check(word(m, dl_cursor) == dl_base + 0x140 + 0x2000 * 8 - 16, "capacity guard");
    check(sprites.size() == 6, "sprites still queued when the display list is full");

    if (failures)
        return 1;
    std::puts("[QUADRANT-HUD] all checks passed");
    return 0;
}
