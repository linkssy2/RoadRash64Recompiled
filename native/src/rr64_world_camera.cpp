#include "rr64_world_camera.hpp"
#include "rr64_world_render.hpp"
#include "rr64_actor_render_snapshot.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#include "rr64_prediction_replay.hpp"
#endif
#include <cmath>

namespace {
struct Stamp {
    unsigned char *mapping = nullptr;
    unsigned epoch = 0;
    bool valid = false;
};
thread_local Stamp stamps[4][3][2];
thread_local Stamp native_stamps[4][3][2];
struct Producer {
    Stamp stamp;
    float near = 0, far = 0;
    bool extended = false;
};
thread_local Producer producers[4][3][2];
bool camera(unsigned char *m, const recomp_context *c, unsigned &view, unsigned &source,
            unsigned &slot) {
    if (!c || !rr64::world::static_scene(m))
        return false;
    source = unsigned(c->r18);
    slot = unsigned(c->r22);
    return source < 3u && slot < 2u && rr64::engine::read_u32(m, 0x8009db84u, view) && view < 4u;
}
}
extern "C" int rr64_world_camera_open_gap(unsigned char *m, void *context) {
#ifdef RR64_EXPERIMENTAL_COURSE
    auto *c = static_cast<recomp_context *>(context);
    if (!m || !c || !rr64::experimental_course::active() || rr64::prediction::active())
        return 0;
    const auto *route = rr64::experimental_course::route_data();
    if (!route || !route->delayed_fall_recovery)
        return 0;
    // 5D9A4 reaches 5E2A0 only when its camera-floor query found no surface.
    // On stock roads that means an invalid camera location, so native code
    // shortens the chase arm. Imported open gaps are legitimate empty space.
    // Use the native unobstructed branch instead: it gradually restores an
    // already shortened arm, and still rechecks real terrain before extending.
    // Its secondary failed query (5E184) must likewise skip the fallback
    // sea-level floor; otherwise a camera below height zero shortens again.
    const unsigned view_offset = unsigned(c->r23);
    const unsigned arm = unsigned(c->r16);
    float fraction = 0, terrain_scale = 0;
    if (c->r2 != 0 || view_offset >= 16 || (view_offset & 3) != 0 ||
        arm != 0x800a52b8u + view_offset ||
        !rr64::engine::read_float(m, arm, fraction) ||
        !std::isfinite(fraction) || fraction <= 0 || fraction > 1 ||
        !rr64::engine::read_float(m, 0x80005f48u, terrain_scale) || terrain_scale != .25f)
        return 0;
    c->f2.fl = fraction; // input normally loaded at the unobstructed branch
    // The first failed query skipped 5DE70. Its scale is nevertheless required
    // if the secondary extension probe hits terrain (5E178). Recreate that
    // original register input before entering the shared native branch.
    c->f26.fl = terrain_scale;
    return 1;
#else
    (void)m;
    (void)context;
    return 0;
#endif
}
extern "C" void rr64_world_camera_far(unsigned char *m, void *context) {
    auto *c = static_cast<recomp_context *>(context);
    unsigned view = 0, source = 0, slot = 0;
    if (!camera(m, c, view, source, slot))
        return;
    stamps[view][source][slot].valid = false;
    native_stamps[view][source][slot].valid = false;
    auto &producer = producers[view][source][slot];
    producer.stamp.valid = false;
    constexpr float scales[]{4.0f, 100.0f, 10.0f};
    // Hook 16888 follows the legacy 32767 clamp, while f4 still holds the
    // source scale. All three banks use the same world near/far convention.
    if (c->f4.fl != scales[source] || !std::isfinite(c->f8.fl) || c->f8.fl <= 0.0f ||
        c->f8.fl >= rr64::world::far_distance * scales[source])
        return;
    const bool extended = rr64_world_distance_enabled() != 0;
    if (extended)
        c->f6.fl = rr64::world::far_distance * scales[source];
    // Record the actual native producer without extending it when the world
    // option is off. Replay traffic needs this freshness, not a longer range.
    unsigned epoch = 0;
    if (!std::isfinite(c->f6.fl) || c->f6.fl <= c->f8.fl ||
        !rr64::engine::read_u32(m, 0x800a1830u, epoch))
        return;
    producer = {{m, epoch, true}, c->f8.fl, c->f6.fl, extended};
}
extern "C" void rr64_world_camera_normalization(unsigned char *m, void *context) {
    auto *c = static_cast<recomp_context *>(context);
    unsigned view = 0, source = 0, slot = 0;
    if (!camera(m, c, view, source, slot))
        return;
    auto &producer = producers[view][source][slot];
    const auto preceding = producer;
    producer.stamp.valid = false;
    float near = 0.0f, far = 0.0f;
    unsigned epoch = 0;
    if (!preceding.stamp.valid || preceding.stamp.mapping != m ||
        !rr64::engine::read_u32(m, 0x800a1830u, epoch) || preceding.stamp.epoch != epoch ||
        !rr64::engine::read_float(m, unsigned(c->r29) + 0x10u, near) || near != preceding.near ||
        !rr64::engine::read_float(m, unsigned(c->r29) + 0x14u, far) || far != preceding.far)
        return;
    const unsigned address = 0x800b73e8u + view * 12u + source * 4u + slot * 2u;
    std::uint16_t norm = 0;
    if (!rr64::engine::read_u16(m, address, norm))
        return;
    // Only the extended projection needs the existing truncation repair.
    // Native projection readiness observes its normalization without writes.
    if (preceding.extended && norm == 0u) {
        rr64::engine::write_u16(m, address, 1u);
        norm = 1u;
    }
    if (!norm)
        return;
    native_stamps[view][source][slot] = {m, epoch, true};
    if (preceding.extended)
        stamps[view][source][slot] = {m, epoch, true};
}
extern "C" int rr64_world_camera_native_ready(unsigned char *m, unsigned source, unsigned slot) {
    unsigned epoch = 0, view = 0;
    if (!rr64::engine::read_u32(m, 0x8009db84u, view) || view >= 4u || source >= 3u || slot >= 2u ||
        !rr64::world::static_scene(m) || !rr64::engine::read_u32(m, 0x800a1830u, epoch))
        return 0;
    const auto &stamp = native_stamps[view][source][slot];
    return stamp.valid && stamp.mapping == m && stamp.epoch == epoch;
}
extern "C" int rr64_world_camera_ready(unsigned char *m, unsigned source, unsigned slot) {
    unsigned epoch = 0, view = 0;
    if (!rr64::engine::read_u32(m, 0x8009db84u, view) || view >= 4u || source >= 3u || slot >= 2u ||
        !rr64_world_distance_enabled() || !rr64::world::static_scene(m) ||
        !rr64::engine::read_u32(m, 0x800a1830u, epoch))
        return 0;
    const auto &stamp = stamps[view][source][slot];
    return stamp.valid && stamp.mapping == m && stamp.epoch == epoch;
}
