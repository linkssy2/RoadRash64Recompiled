#include "rr64_highlight_render_boundary.hpp"
#include "rr64_shadow_tags.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_camera_diagnostics.hpp"
#include "hle/rt64_rr64_matching_evidence.h"

namespace {
struct DrawScope {
    rr64::highlight_render::Boundary boundary;
    unsigned char *memory = nullptr;
    unsigned marker = 0;
    unsigned replay_key = 0, observed_marker = 0;
    rr64::highlight_render::NormalPhase normal{};
    bool emitted = false;
};
thread_local DrawScope scope;

bool cursor(unsigned char *m, unsigned bytes, unsigned reserve, unsigned &p) {
    using namespace rr64::engine;
    unsigned slot = 0, base = 0, active = 0, count = 0;
    if (!m || !read_u32(m, 0x8009cba4u, slot) || slot > 1 ||
        !read_u32(m, 0x800ac658u + slot * 4, base) ||
        !read_u32(m, 0x8009cb90u, active) || active != base ||
        !read_u32(m, 0x800bc9a0u, count) ||
        (count != 0x4650u && count != 0x36b0u) ||
        !read_u32(m, 0x800ac650u, p)) return false;
    const unsigned size = 0x140u + count * 8u;
    return valid_guest_range(base, size) && !(p & 7u) &&
        p >= base + 0x140u && p <= base + size - bytes - reserve;
}
void command(unsigned char *m, unsigned &p, unsigned a, unsigned b) {
    rr64::engine::write_u32(m, p, a);
    rr64::engine::write_u32(m, p + 4, b);
    p += 8;
}
void begin(unsigned char *m, unsigned replay_key, const rr64::highlight_render::NormalPhase *normal) {
    rr64_shadow_tags_reset();
    unsigned epoch = 0;
    scope.memory = m;
    scope.emitted = false;
    scope.replay_key = replay_key;
    scope.normal = normal ? *normal : rr64::highlight_render::NormalPhase{};
    scope.marker = m && rr64::engine::read_u32(m, 0x800a1830u, epoch)
        ? scope.boundary.observe(m, epoch, replay_key, normal) : 0u;
}
void observe_phase() {
    if (scope.observed_marker == scope.marker) return;
    scope.observed_marker = scope.marker;
    using namespace RT64::RR64MatchingEvidence;
    if (!enabled()) return;
    const int slot = buffer.claim(3);
    if (slot < 0) return;
    Record record;
    record.category = 3;
    record.cameraMarker = scope.marker;
    record.cameraReplayKey = scope.replay_key;
    record.cameraRound = scope.normal.round;
    record.cameraLayout = scope.normal.layout;
    record.cameraViews = scope.normal.views;
    for (unsigned i = 0; i < scope.normal.cameras.size(); ++i) {
        const auto &camera = scope.normal.cameras[i];
        record.cameraValidMask |= unsigned(camera.valid) << i;
        record.cameraModes[i] = camera.mode;
        record.cameraOwners[i] = camera.owner;
        record.cameraFlags[i] = camera.flags;
        record.cameraGenerations[i] = camera.generation;
        record.cameraBikes[i] = camera.bike;
        record.cameraRiders[i] = camera.rider;
    }
    buffer.publish(3, unsigned(slot), record);
}
}

extern "C" void rr64_highlight_render_begin(unsigned char *m, unsigned replay_key) {
    begin(m, replay_key, nullptr);
}
void rr64::highlight_render::begin_normal(unsigned char *m, unsigned round,
                                         const std::array<std::uint32_t, 14> &generations) {
    using namespace rr64::engine;
    NormalPhase normal;
    unsigned mode = 0, pending = 0, players = 0;
    if (!m || !read_u32(m, globals::main_mode, mode) || !is_live_race_mode(mode) ||
        !read_u32(m, globals::pending_mode, pending) || pending != mode ||
        !read_u32(m, 0x800a4f24u, normal.layout) || normal.layout > 2 ||
        !read_u32(m, 0x800a6578u, players) || players < 1 || players > 4) {
        begin(m, 0, nullptr);
        return;
    }
    normal.round = round;
    // Native 15C10's layout tables are {1,1,2} x {1,2,2}. A three-player
    // layout still authors four camera preparations in 6A638.
    normal.views = 1u << normal.layout;
    for (unsigned i = 0; i < normal.views; ++i) {
        const auto captured = rr64::prediction::capture_camera(m, i);
        auto &camera = normal.cameras[i];
        camera.mode = captured.mode;
        // Mode 9 scans for another actor instead of using the viewport map.
        // Unknown branches/owners remain unproved and retain all AUTO guards.
        if (!captured.valid || captured.mode < 1 || captured.mode > 8 ||
            captured.mapped_actor >= generations.size()) continue;
        camera.owner = captured.mapped_actor;
        camera.flags = captured.state_flags;
        camera.generation = generations[camera.owner];
        const unsigned actor = 0x800d8570u + camera.owner * 0x118u;
        camera.valid = read_u32(m, actor + 0xe0u, camera.bike) &&
                       read_u32(m, actor + 0xe4u, camera.rider);
    }
    begin(m, 0, &normal);
    bool known = true;
    for (unsigned i = 0; i < normal.views; ++i) known = known && normal.cameras[i].valid;
    if (known) rr64::shadow_tags::begin(m, round, normal.views, generations);
}
extern "C" void rr64_highlight_render_projection(unsigned char *m) {
    unsigned p = 0;
    if (scope.memory != m || !scope.marker || !cursor(m, 32, 1056, p)) return;
    command(m, p, 0xe0525464u, 0x10000064u);
    command(m, p, 0x6400000cu, scope.marker);
    // Simple camera interpolation, identical to RT64's ordinary AUTO-camera
    // policy. Only the identity changes at a cut. Capture is lazy, so this
    // scope must outlive 16A18 until the geometry using its matrices is read.
    constexpr unsigned components = (1u << 3) | (1u << 5) | (1u << 7) |
                                    (1u << 9) | (1u << 11);
    command(m, p, components | 2u, 0);
    command(m, p, 0xe0525464u, 0x20000000u);
    rr64::engine::write_u32(m, 0x800ac650u, p);
    scope.emitted = true;
    // Only this graphics producer writes category 3, and only once a changed
    // phase has actually emitted its submitted marker. No hot-path I/O.
    observe_phase();
}
extern "C" void rr64_highlight_render_end(unsigned char *m) {
    unsigned p = 0;
    if (scope.memory == m && scope.emitted && cursor(m, 32, 0, p)) {
        command(m, p, 0xe0525464u, 0x10000064u);
        command(m, p, 0x6400000cu, 0xffffffffu); // Restore ordinary AUTO camera.
        command(m, p, 2u, 0);
        command(m, p, 0xe0525464u, 0x20000000u);
        rr64::engine::write_u32(m, 0x800ac650u, p);
    }
    scope.memory = nullptr;
    scope.marker = 0;
    scope.emitted = false;
}
