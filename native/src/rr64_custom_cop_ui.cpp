#include "rr64_custom_cop.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "recomp.h"
#include <cmath>
#include <cstring>
#include <cstdio>

extern "C" void func_800796F8(unsigned char *, recomp_context *);

// Original selection-screen hint; separate from the online popup/lobby UI.
extern "C" void rr64_custom_cop_selection_hint(unsigned char *rdram, void *context) {
    if (rr64_custom_cop_can_start(rdram))
        return;
    recomp_context draw = *static_cast<recomp_context *>(context);
    draw.r29 -= 96;
    const char label[] = "Choose a cop rider + cop bike";
    for (unsigned i = 0; i < sizeof(label); ++i)
        MEM_B(32 + i, draw.r29) = label[i];
    draw.r4 = draw.r29 + 32;
    draw.r5 = 0x42A00000; // x=80
    // Shared reminder below the selection frame (ends near y=224), outside
    // every player's bike/rider panel on the native 320x240 menu canvas.
    draw.r6 = 0x43620000; // y=226
    draw.r7 = rr64::engine::guest_address(0x8009E20Cu);
    MEM_W(16, draw.r29) = 3;
    MEM_W(20, draw.r29) = 0x3F200000; // 0.625, same font size as options
    func_800796F8(rdram, &draw);
}

// Draw after the original HUD has committed its display list. Text uses the
// original font and logical 320x240 canvas; each local view gets its own region.
extern "C" void rr64_custom_cop_hud(unsigned char *rdram, void *context) {
    using namespace rr64::engine;
    if (!context || MEM_HU(0, guest_address(0x800A2192)))
        return;
    const auto online = rr64::netplay::get_status();
    const bool authority_view =
        online.active && online.connected && online.authoritative && !online.is_host;
    rr64::netplay::AuthorityFrame host_frame;
    if (authority_view) {
        if (online.local_slot >= rr64::netplay::kMaximumPlayers ||
            !rr64::netplay::authority_get_frame(host_frame) || !host_frame.cop_mode)
            return;
    } else if (!rr64_custom_cop_active())
        return;
    unsigned humans = 0;
    read_u32(rdram, 0x800A6578, humans);
    if (authority_view)
        humans = 1;
    if (humans < 1 || humans > 4)
        return;
    const auto original = *static_cast<recomp_context *>(context);
    auto text = [&](const char *label, float x, float y, float scale) {
        auto c = original;
        c.r29 -= 128;
        if (!valid_guest_range(unsigned(c.r29), 128))
            return;
        for (unsigned j = 0; j < 48; ++j) {
            MEM_B(48 + j, c.r29) = label[j];
            if (!label[j])
                break;
        }
        MEM_B(32, c.r29) = -1;
        MEM_B(33, c.r29) = -1;
        MEM_B(34, c.r29) = 0;
        MEM_B(35, c.r29) = -1; // opaque yellow RGBA
        c.r4 = c.r29 + 48;
        std::memcpy(&c.r5, &x, 4);
        std::memcpy(&c.r6, &y, 4);
        c.r7 = c.r29 + 32;
        MEM_W(16, c.r29) = 3;
        unsigned bits;
        std::memcpy(&bits, &scale, 4);
        MEM_W(20, c.r29) = bits;
        func_800796F8(rdram, &c);
    };
    const float win_age = authority_view ? host_frame.cop_win_age : rr64_custom_cop_win_age(rdram);
    if (win_age >= 0) {
        const float scale = 1.0f + 0.12f * std::sin(win_age * 9.f);
        text("COPS WIN!", 160.f - 32.f * scale, 110.f, scale);
        return;
    }
    const bool peer_view = online.active && online.connected;
    for (unsigned slot = 0; slot < humans; ++slot) {
        if (!authority_view && peer_view &&
            slot != (online.replicated_riders ? 0u : online.local_slot))
            continue;
        const unsigned a = 0x800D8570 + slot * 0x118;
        unsigned role = 0, state = 0, view = 0, count = 0;
        if (authority_view) {
            if (!host_frame.riders[online.local_slot].active)
                continue;
            role = host_frame.outcomes[online.local_slot].role;
            count = host_frame.outcomes[online.local_slot].busts;
        } else
            read_u32(rdram, a + 0x20, role);
        if (role != 7)
            continue;
        if (!authority_view)
            read_u32(rdram, a + 8, view);
        if (view >= humans)
            continue;
        if (!authority_view) {
            read_u32(rdram, a + 0xE8, state);
            if (!valid_guest_range(state, 0x64))
                continue;
            read_u32(rdram, state + 0x58, count);
        }
        const float w = !peer_view && humans > 2 ? 160.f : 320.f,
                    h = !peer_view && humans > 1 ? 120.f : 240.f;
        const float x = !peer_view && humans > 2 ? (view % 2) * w : 0.f,
                    y = peer_view ? 0.f : (humans > 2 ? (view / 2) * h : view * h);
        char label[32];
        std::snprintf(label, sizeof(label), "Busts: %u", count);
        text(label, x + w - 70, y + 8, 0.5f);
        const float age = authority_view ? host_frame.outcomes[online.local_slot].cue_age
                                         : rr64_custom_cop_cue(rdram, slot);
        if (age >= 0 && int(age * 5) % 2 == 0) {
            const float scale = 0.7f + 0.12f * std::sin(age * 9.f);
            text("Bust' em!", x + w * 0.5f - 26.f * scale, y + h * 0.5f - 10.f, scale);
        }
    }
}
