#include "rr64_highlight_camera.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_replay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>
#include <vector>

using namespace rr64;
using namespace rr64::engine;
extern "C" void func_800146C8(unsigned char *, recomp_context *);
extern "C" void highlight_missing_floor_native(unsigned char *, recomp_context *);

namespace {
unsigned checks = 0;
constexpr unsigned grid = 0x80200000, root = 0x80300000, query = 0x80400000;
constexpr unsigned stack = 0x80500000, width = 70, units = 100;
constexpr unsigned cell = grid + (35 * width + 35) * 16;
constexpr unsigned eye = 0x800d6a28, target = 0x800d69f8;
void check(bool ok, const char *message,
           const std::source_location at = std::source_location::current()) {
    ++checks;
    if (!ok) {
        std::cerr << "FAIL " << message << " (line " << at.line() << ")\n";
        std::exit(1);
    }
}
float real(unsigned char *m, unsigned address) {
    float value = 0;
    check(read_float(m, address, value), "valid fixture float");
    return value;
}
void vector(unsigned char *m, unsigned address, const highlight_camera::Vec3 &v) {
    for (unsigned i = 0; i < 3; ++i) write_float(m, address + i * 4, v[i]);
}
highlight_camera::View ordinary(unsigned view) {
    const float x = float(1 + 10 * view);
    return {{x, -8, 8}, {x, 2, 3}, {0, 0, 1}};
}
void put(unsigned char *m, const highlight_camera::View &v) {
    vector(m, eye, v.eye); vector(m, target, v.target);
    vector(m, 0x800d6a08, v.up); vector(m, 0x800d6a18, v.up);
}
bool matches(unsigned char *m, const highlight_camera::View &v) {
    bool same = true;
    for (unsigned i = 0; i < 3; ++i)
        same &= real(m, eye + 4 * i) == v.eye[i] && real(m, target + 4 * i) == v.target[i];
    return same;
}
struct Scene {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(kRdramSize);
    unsigned char *m() { return bytes.data(); }
    Scene(unsigned views = 1) {
        write_u32(m(), globals::main_mode, 0x19); write_u32(m(), globals::pending_mode, 0x19);
        write_u32(m(), globals::terrain_cell_grid, grid);
        write_u32(m(), globals::terrain_map_width, width); write_u32(m(), 0x800dacc0 + 0x10, units);
        write_u32(m(), 0x800a4f24, views == 1 ? 0 : views - 1);
        write_u32(m(), 0x8009db88, views); write_u32(m(), 0x800a6578, views);
        write_u16(m(), 0x800a65c4, 1);
        write_u32(m(), cell, root); write_u32(m(), cell + 4, 0x00100000);
        write_u32(m(), cell + 8, 100);
        write_u32(m(), root + 0x10, 0x40); write_u16(m(), cell + 14, 1);
        write_float(m(), 0x80005f44, 1);
        write_float(m(), 0x800a52cc, 1); write_float(m(), 0x80005f74, 2);
        write_float(m(), 0x800a52c8, 1);
        for (unsigned v = 0; v < 4; ++v) {
            write_float(m(), 0x800a52b8 + v * 4, 10);
            write_u32(m(), 0x800a657c + v * 4, v);
        }
        write_u32(m(), 0x800a1830, 100);
        state(5);
    }
    void state(unsigned value, bool resource = true) {
        write_s8(m(), cell + 0xc, static_cast<std::int8_t>(value));
        write_u32(m(), cell, resource ? root : 0);
    }
};
void prepare(Scene &s, unsigned view) {
    write_u32(s.m(), globals::active_viewport, view);
    recomp_context c{}; c.r4 = view;
    rr64_highlight_camera_prepare(s.m(), &c);
}
unsigned lookup(Scene &s, float x = 0, float y = 0) {
    vector(s.m(), query, {x, y, 0});
    recomp_context c{}; c.r4 = guest_address(query);
    func_800146C8(s.m(), &c);
    return unsigned(c.r2);
}
// This is the original branch taken after 14DE4 rejects an unavailable cell.
// Synthetic direction/coefficient values make its shrinking chase arm visible.
void collapse(Scene &s, unsigned view) {
    recomp_context c{};
    c.r16 = guest_address(0x800a52b8 + view * 4);
    c.r17 = guest_address(eye); c.r19 = guest_address(target);
    c.r21 = guest_address(0x800d0000); c.r29 = guest_address(stack);
    c.f22.fl = 1;
    vector(s.m(), stack + 0xa0, {0, -1, .5f});
    highlight_missing_floor_native(s.m(), &c);
}
void cache_view(Scene &s, unsigned view) {
    put(s.m(), ordinary(view)); prepare(s, view);
    check(lookup(s) == 1, "native state5 resource lookup succeeds");
    rr64_highlight_camera_apply(s.m());
}
void replay(Scene &s) {
    const highlight_camera::View alternate{{100, 101, 20}, {100, 110, 10}, {0, 0, 1}};
    // The real replay draw occurs on the graphics worker; ordinary preparation
    // and its next missing-data camera calculation occur on the game thread.
    std::thread worker([&] {
        check(highlight_camera::begin(s.m(), alternate), "replay scope begins on worker");
        prepare(s, 0); rr64_highlight_camera_apply(s.m());
        check(matches(s.m(), alternate), "worker renders recorded camera");
        highlight_camera::end(s.m());
    });
    worker.join();
}
void native_lookup_contract() {
    highlight_camera::reset(nullptr); Scene s;
    for (unsigned state : {0u, 1u, 2u, 3u, 4u, 5u}) {
        s.state(state); prepare(s, 0);
        check(lookup(s) == (state == 5 ? 1u : 0u), "original lookup requires loaded state5");
        rr64_highlight_camera_apply(s.m());
    }
    s.state(5, false); prepare(s, 0);
    check(lookup(s) == 0, "state5 without a resource is not a valid native floor");
    rr64_highlight_camera_apply(s.m());
    put(s.m(), ordinary(0)); collapse(s, 0);
    check(real(s.m(), 0x800a52b8) == 8, "actual native missing-floor branch shortens arm");
    check(real(s.m(), eye + 4) == -6 && real(s.m(), eye + 8) == 7,
          "actual native missing-floor branch moves eye toward rider");
}
void return_lifecycle(unsigned views) {
    highlight_camera::reset(nullptr); Scene s(views);
    for (unsigned v = 0; v < views; ++v) cache_view(s, v);
    replay(s);
    for (unsigned v = 0; v < views; ++v) {
        put(s.m(), ordinary(v));
        for (unsigned frame = 0; frame < 40; ++frame) {
            // Native7B8D4 stamps selected pending cells even while async
            // completion is outstanding. A stale/unselected record is tested
            // separately; advancing time alone must never make a cell ready.
            write_u32(s.m(), cell + 8, 100 + 40 * v + frame);
            write_u32(s.m(), 0x800a1830, 101 + 40 * v + frame);
            s.state(frame % 3 == 0 ? 1 : frame % 3 == 1 ? 3 : 4);
            prepare(s, v);
            check(lookup(s) == 0, "native floor remains unavailable while streaming");
            collapse(s, v);
            check(!matches(s.m(), ordinary(v)), "native branch would visibly alter return camera");
            rr64_highlight_camera_apply(s.m());
            check(matches(s.m(), ordinary(v)), "return camera stays valid until real cell becomes ready");
            check(real(s.m(), 0x800a52b8 + 4 * v) == 10, "all four view arms retain pre-query distance");
        }
        s.state(5); prepare(s, v); check(lookup(s) == 1, "native streamer completion is observed");
        auto next = ordinary(v); next.eye[0] += 2;
        put(s.m(), next); rr64_highlight_camera_apply(s.m());
        check(matches(s.m(), next), "ready floor releases ordinary authored camera without a timer");
        s.state(3); prepare(s, v); lookup(s); collapse(s, v);
        rr64_highlight_camera_apply(s.m());
        check(!matches(s.m(), next), "completed handoff does not permanently freeze future cameras");
    }
}
void genuine_void() {
    for (unsigned state : {0u, 2u, 5u}) {
        highlight_camera::reset(nullptr); Scene s;
        cache_view(s, 0); replay(s);
        s.state(state, false); prepare(s, 0);
        check(lookup(s) == 0, "native empty/absent resource query really fails");
        collapse(s, 0); rr64_highlight_camera_apply(s.m());
        check(!matches(s.m(), ordinary(0)), "genuine void is not confused with pending streaming");
    }
    highlight_camera::reset(nullptr); Scene s;
    cache_view(s, 0); replay(s); s.state(3, false);
    write_u32(s.m(), cell + 4, 0);
    prepare(s, 0); check(lookup(s) == 0, "cell with no ROM source remains unavailable");
    collapse(s, 0); rr64_highlight_camera_apply(s.m());
    check(!matches(s.m(), ordinary(0)), "unbacked cell never freezes camera as if a load were pending");
}
void neighboring_queries() {
    highlight_camera::reset(nullptr); Scene s;
    const unsigned next_cell = cell + width * 16;
    write_u32(s.m(), next_cell, root); write_u32(s.m(), next_cell + 4, 0x00100100);
    write_u16(s.m(), next_cell + 14, 1);
    write_u32(s.m(), next_cell + 8, 100);
    write_s8(s.m(), next_cell + 0xc, 3);
    cache_view(s, 0); replay(s); prepare(s, 0);
    check(lookup(s) == 1, "actor's original neighborhood can be loaded");
    check(lookup(s, 100) == 0, "camera eye can probe a different still-unloaded neighboring cell");
    collapse(s, 0); rr64_highlight_camera_apply(s.m());
    check(matches(s.m(), ordinary(0)), "actual missing neighboring probe protects return camera");
    // A later successful adjustment must not erase the earlier missing-cell
    // evidence. The next clean preparation can safely resume native motion.
    prepare(s, 0); check(lookup(s, 100) == 0, "first eye candidate is still pending");
    check(lookup(s) == 1, "native closer candidate succeeds in the same preparation");
    collapse(s, 0); rr64_highlight_camera_apply(s.m());
    check(matches(s.m(), ordinary(0)), "mixed candidate probes retain stable pre-collapse camera");
    write_s8(s.m(), next_cell + 0xc, 5);
    prepare(s, 0); check(lookup(s, 100) == 1 && lookup(s) == 1, "both candidate cells are truly ready");
    auto next = ordinary(0); next.eye[0] += 1;
    put(s.m(), next); rr64_highlight_camera_apply(s.m());
    check(matches(s.m(), next), "missing-probe state is cleared each preparation");
}
void invalidation() {
    // A new scene must never inherit a return camera from an earlier race.
    for (unsigned kind = 0; kind < 9; ++kind) {
        highlight_camera::reset(nullptr); Scene s, different;
        cache_view(s, 0); replay(s);
        Scene *working = &s;
        if (kind == 0) write_u32(s.m(), globals::main_mode, 0x27);
        if (kind == 1) write_u32(s.m(), globals::pending_mode, 0x27);
        if (kind == 2) {
            const unsigned other_grid = grid + 0x20000;
            std::copy_n(s.bytes.data() + grid - kRdramBegin, width * width * 16,
                        s.bytes.data() + other_grid - kRdramBegin);
            write_u32(s.m(), globals::terrain_cell_grid, other_grid);
            write_s8(s.m(), other_grid + cell - grid + 0xc, 3);
        }
        if (kind == 3) { working = &different; put(different.m(), ordinary(0)); }
        if (kind == 4) highlight_camera::reset(nullptr);
        if (kind == 5) { write_u32(s.m(), 0x800a4f24, 1); write_u32(s.m(), 0x8009db88, 2); }
        if (kind == 6) write_u32(s.m(), 0x800a657c, 5);
        if (kind == 7) write_u32(s.m(), 0x800a1830, 1);
        if (kind == 8) write_u32(s.m(), globals::terrain_map_width, 71);
        working->state(3); prepare(*working, 0);
        check(lookup(*working) == 0, "invalidated scene still follows real unavailable-cell lookup");
        collapse(*working, 0); rr64_highlight_camera_apply(working->m());
        check(!matches(working->m(), ordinary(0)), "scene identity change invalidates handoff");
    }
}
void no_false_hold() {
    for (unsigned kind = 0; kind < 5; ++kind) {
        highlight_camera::reset(nullptr); Scene s;
        cache_view(s, 0); replay(s); s.state(3);
        prepare(s, 0); check(lookup(s) == 0, "pending cell exercises native failure");
        collapse(s, 0); rr64_highlight_camera_apply(s.m());
        check(matches(s.m(), ordinary(0)), "handoff first holds real unavailable resource");
        if (kind == 0) { write_u32(s.m(), cell + 8, 99); write_u32(s.m(), 0x800a1830, 101); }
        if (kind == 1) write_u16(s.m(), cell + 14, 0);
        prepare(s, 0);
        if (kind != 2) lookup(s); // A preparation with no camera query must not stay armed.
        collapse(s, 0);
        if (kind == 3) write_float(s.m(), target, 1000);
        if (kind == 4) highlight_camera::reset(s.m());
        rr64_highlight_camera_apply(s.m());
        check(!matches(s.m(), ordinary(0)), "unselected cell, no metadata/query, target change or reset releases hold");
    }
}
void replay_restore_lifetime() {
    highlight_camera::reset(nullptr); Scene s(4);
    for (unsigned v = 0; v < 4; ++v) cache_view(s, v);
    const highlight_camera::View alternate{{100, 101, 20}, {100, 110, 10}, {0, 0, 1}};
    check(highlight_camera::begin(s.m(), alternate), "lifetime replay begins");
    for (unsigned v = 0; v < 4; ++v) write_float(s.m(), 0x800a52b8 + v * 4, 1);
    write_u32(s.m(), 0x800b6568, 0x11223344);
    write_u32(s.m(), 0x800b6de8, 0x55667788);
    highlight_camera::end(s.m());
    for (unsigned v = 0; v < 4; ++v)
        check(real(s.m(), 0x800a52b8 + v * 4) == 10, "replay restores all four native camera-control arms");
    unsigned value = 0;
    read_u32(s.m(), 0x800b6568, value); check(value == 0x11223344, "submitted projection matrix is not rewound");
    read_u32(s.m(), 0x800b6de8, value); check(value == 0x55667788, "submitted view matrix is not rewound");
    highlight_camera::reset(nullptr);
    prediction::ReplayScope predicted;
    check(!highlight_camera::begin(s.m(), alternate), "prediction cannot arm presentation handoff");
    prepare(s, 0); lookup(s); collapse(s, 0); rr64_highlight_camera_apply(s.m());
    check(!matches(s.m(), ordinary(3)), "prediction leaves native authored camera untouched");
}
}

int main() {
    native_lookup_contract();
    for (unsigned views = 1; views <= 4; ++views) return_lifecycle(views);
    genuine_void(); neighboring_queries(); invalidation(); no_false_hold(); replay_restore_lifetime();
    std::cout << "Highlight return camera: " << checks << " checks passed (native cell lookup, native collapse, worker handoff).\n";
}
