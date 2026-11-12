#include "rr64_mk64_items.hpp"
#include "rr64_mk64_item_kernel.hpp"
#include "rr64_mk64_item_dimensions.hpp"
#include "rr64_mk64_item_native.hpp"
#include "rr64_mk64_item_replay.hpp"
#include "rr64_mk64_item_audio.hpp"
#include "rr64_mk64_item_render.hpp"
#include "rr64_mk64_item_hud.hpp"
#include "rr64_authoritative_input.hpp"
#include "rr64_course_items.hpp"
#include "rr64_course_walls.hpp"
#include "rr64_course_impact.hpp"
#include "rr64_experimental_course.hpp"
#include "rr64_highlights.hpp"
#include "rr64_netplay.hpp"
#include "rr64_online_flow.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_local_race_options.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>

extern "C" void func_800616BC(unsigned char *, recomp_context *);
extern "C" void func_80037554(unsigned char *, recomp_context *);

namespace rr64::mk64_items {
namespace {
using namespace engine;
using native::half;
using native::word;
using native::scalar;
struct Actor {
    native::Pair owner{};
    native::Geometry bike_geometry{}, rider_geometry{};
    unsigned next_ai_use = 0;
};
struct Runover {
    native::Pair owner{}, victim{};
    unsigned owner_slot = racer_capacity;
};
struct Runtime {
    unsigned char *memory = nullptr;
    const experimental_course::RouteData *course = nullptr;
    Snapshot state{};
    Snapshot physics_state{};
    std::uint64_t physics_elapsed_us = 0;
    std::array<Actor, racer_capacity> actors{};
    std::array<Use, racer_capacity> staged{};
    std::array<Runover, racer_capacity> runovers{};
    unsigned round = 0;
    std::uint64_t tick = 0;
    float elapsed = -1;
    std::array<Vec, 8192> route_points{};
    unsigned route_count = 0;
} live;
std::atomic_bool accepts_input{false};
std::atomic_uint32_t requests{0};
std::mutex published_mutex;
Snapshot published;
struct PrivateRuntime {
    ReplayState frame{};
    unsigned char *memory = nullptr;
    std::uint64_t epoch = 0;
};
thread_local PrivateRuntime private_runtime;
bool private_bound(unsigned char *m) {
    return prediction::active() && private_runtime.epoch == prediction::replay_epoch &&
           private_runtime.memory == m;
}

void publish() {
    std::lock_guard lock(published_mutex);
    published = live.state;
}
bool racing(unsigned char *m) {
    return m && experimental_course::active() && !prediction::active() &&
           !rr64_highlights_presenting() &&
           is_live_race_transition(word(m, globals::main_mode), word(m, globals::pending_mode));
}
unsigned native_slot(unsigned canonical) {
    const auto status = prediction::physics_rules();
    return status.active
               ? online_flow::mapped_slot(canonical, status.local_slot, status.replicated_riders)
               : canonical;
}
unsigned canonical_for_entity(unsigned char *m, unsigned entity) {
    for (unsigned slot = 0; slot < racer_capacity; ++slot) {
        const auto p = native::pair(m, native_slot(slot));
        if (p.actor && (entity == p.actor || entity == p.bike || entity == p.rider ||
                        entity == p.bike + 0x108 || entity == p.rider + 0x28))
            return slot;
    }
    return racer_capacity;
}
struct RenderAnchors {
    Vec bike{}, rider{}, bike_origin{};
    bool attached = false;
};
bool render_anchors(unsigned char *m, unsigned slot, RenderAnchors &anchors) {
    if (!m || prediction::active() || !experimental_course::active() || slot >= racer_capacity)
        return false;
    if (highlights::render_items())
        return highlights::render_rider_anchors(slot, anchors.bike, anchors.rider, anchors.attached,
                                                anchors.bike_origin);
    const auto p = native::pair(m, native_slot(slot));
    anchors.attached = native::mounted(m, p);
    // Native5D9A4/E980/EB50 draw from these authored anchors, which are not
    // interchangeable with the bike/rider physics-body positions.
    return p.actor && native::vector(m, p.bike + 0x53C, anchors.bike) &&
           native::vector(m, p.rider + 0x5DC, anchors.rider) &&
           native::vector(m, p.bike + 0x16C, anchors.bike_origin);
}
void scale_render_root(unsigned char *m, unsigned slot, unsigned type, unsigned matrix,
                       float factor) {
    if (!(factor > 0) || factor > 1000 || !std::isfinite(factor) ||
        !valid_guest_range(matrix, 64) || (matrix & 3))
        return;
    const Snapshot *state = highlights::render_items();
    if (!state)
        state = &live.state;
    if (!state->enabled || state->riders[slot].shrink_until <= state->clock)
        return;
    std::array<float, 16> values{};
    for (unsigned i = 0; i < 16; ++i)
        if (!read_float(m, matrix + i * 4, values[i]) || !std::isfinite(values[i]))
            return;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col)
            values[row * 4 + col] *= .5f;
    // Scale both independently submitted roots about the same physical pivot.
    // Leaving the bike's rear render anchor fixed while shifting only the
    // rider toward the body origin separates their authored attachment points.
    RenderAnchors anchors;
    if (!render_anchors(m, slot, anchors))
        return;
    if (type == 1 || anchors.attached) {
        const Vec anchor = type == 1 ? anchors.bike : anchors.rider;
        for (unsigned axis = 0; axis < 3; ++axis)
            values[12 + axis] += (anchors.bike_origin[axis] - anchor[axis]) * .5f * factor;
    }
    for (unsigned i = 0; i < 16; ++i)
        write_float(m, matrix + i * 4, values[i]);
}
bool owns_simulation() {
    const auto s = netplay::get_physics_rules();
    return !s.active ||
           (s.connected && s.authoritative && s.is_host && s.phase == netplay::Phase::Race);
}
void restore_actor(unsigned char *m, unsigned slot) {
    auto &a = live.actors[slot];
    if (m && a.owner.actor && native::pair(m, native_slot(slot)) == a.owner) {
        a.bike_geometry.restore(m);
        a.rider_geometry.restore(m);
    }
    a = {};
}
float distance_squared(Vec a, Vec b) {
    float value = 0;
    for (unsigned i = 0; i < 3; ++i)
        value += (a[i] - b[i]) * (a[i] - b[i]);
    return value;
}
float big_float(const unsigned char *p) {
    const unsigned v =
        (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) | (unsigned(p[2]) << 8) | p[3];
    return std::bit_cast<float>(v);
}
Vec route_point(const experimental_course::RouteData &d, unsigned segment, float t) {
    const float u = 1 - t;
    Vec p{};
    for (unsigned axis = 0; axis < 2; ++axis) {
        const auto *r = d.records_be + segment * 16 + 8 + axis * 4;
        p[axis] = u * u * big_float(r) + 2 * u * t * big_float(r + 16) + t * t * big_float(r + 32);
    }
    if (d.record_heights)
        p[2] = u * u * d.record_heights[segment] + 2 * u * t * d.record_heights[segment + 1] +
               t * t * d.record_heights[segment + 2];
    return p;
}
SurfaceHit surface(void *, Vec start, Vec motion, float radius) {
    const auto *world = course_walls::surface_world();
    if (!world)
        return {};
    const auto h = course_walls::sweep_sphere(*world, {start, radius}, motion);
    return {h.hit, h.fraction, h.normal, h.point, h.penetration};
}
Vec guide(void *, Vec from, Vec goal) {
    // A rider can be several bike widths away from the route centerline. The
    // old two-unit switch never pursued that rider, so a homing shell simply
    // kept following the route. Take a nearby direct approach only after the
    // actual course surface sweep proves the shell fits through it.
    const auto *world = course_walls::surface_world();
    if (world && distance_squared(from, goal) <= 32.f * 32.f &&
        std::abs(from[2] - goal[2]) <= 2.f) {
        Vec start = from, end = goal, motion{};
        // Grounded shells are tangent to the road. Lift the clearance query
        // slightly, and match the kernel's horizontal steering. A larger shell
        // center can sit above a bike anchor without aiming down into the road.
        start[2] += .02f;
        end[2] = start[2];
        for (unsigned axis = 0; axis < 3; ++axis)
            motion[axis] = end[axis] - start[axis];
        const auto contact =
            course_walls::sweep_sphere(*world, {start, object_radius(Item::RedShell)}, motion);
        if (!contact.hit || contact.fraction >= 1.f)
            return goal;
    }
    if (!live.route_count)
        return from;
    unsigned nearest = 0;
    float best = std::numeric_limits<float>::infinity();
    // The immutable original quadratic route includes height: a different deck
    // at a crossing cannot win solely because its horizontal position is close.
    const unsigned samples = live.route_count;
    for (unsigned i = 0; i < samples; ++i) {
        const auto &p = live.route_points[i];
        const float error = distance_squared(p, from);
        if (error < best) {
            best = error;
            nearest = i;
        }
    }
    const unsigned next = (nearest + 2) % samples;
    return live.route_points[next];
}
void prepare_route() {
    const auto *d = live.course;
    live.route_count = 0;
    if (!d || !d->records_be || !d->record_heights || !d->wrap_segment || d->wrap_segment > 2048 ||
        d->record_count < d->wrap_segment + 3)
        return;
    const unsigned samples = d->wrap_segment / 2 * 8;
    for (unsigned i = 0; i < samples; ++i) {
        const auto p = route_point(*d, i / 8 * 2, (i % 8) / 8.f);
        for (const float value : p)
            if (!std::isfinite(value) || std::abs(value) > 100000)
                return;
        live.route_points[i] = p;
    }
    live.route_count = samples;
}
using native::rated_speed;

Racer read_racer(unsigned char *m, unsigned slot) {
    const auto p = native::pair(m, native_slot(slot));
    auto &a = live.actors[slot];
    if (p != a.owner) {
        restore_actor(m, slot);
        if (owns_simulation())
            retire(live.state, slot);
        a.owner = p;
    }
    Racer r;
    if (!p.actor || !native::vector(m, p.bike + 0x16C, r.position) ||
        !native::vector(m, p.bike + 0x178, r.velocity) ||
        !native::vector(m, p.bike + 0x220, r.forward) || !rated_speed(m, p, r.maximum_speed))
        return r;
    r.active = true;
    r.riding = native::mounted(m, p);
    r.finished = half(m, p.route + 0x4C) || half(m, p.route + 0x4E) || half(m, p.route + 0x50) ||
                 half(m, p.route + 0x52);
    const auto rules = netplay::get_physics_rules();
    r.human = rules.active && rules.authoritative ? (rules.authority_humans & (1u << slot)) != 0
                                                  : half(m, p.actor + 0x26) == 0;
    r.progress = scalar(m, p.route + 0x10) + scalar(m, p.route + 0x20) +
                 scalar(m, p.route + 8) * scalar(m, p.route + 0xC);
    const float radius = scalar(m, p.bike + 0x15C);
    if (std::isfinite(radius) && radius > .01f && radius < 10)
        r.radius = radius;
    native::Contacts contacts;
    if (native::contact_spheres(m, p, contacts)) {
        r.contact_count = contacts.count;
        for (unsigned i = 0; i < contacts.count; ++i) {
            r.contacts[i].radius = contacts.spheres[i].radius;
            for (unsigned axis = 0; axis < 3; ++axis)
                r.contacts[i].offset[axis] = contacts.spheres[i].center[axis] - r.position[axis];
        }
    }
    return r;
}
void native_hit(unsigned char *m, const recomp_context &context, unsigned victim, const Hit &hit) {
    if (!hit.active || victim >= racer_capacity || hit.owner >= racer_capacity ||
        !owns_simulation() || !racing(m))
        return;
    const auto target = native::pair(m, native_slot(victim));
    const auto source = native::pair(m, native_slot(hit.owner));
    if (!native::mounted(m, target) || !source.actor ||
        immune(live.state.riders[victim], live.state.clock))
        return;
    const unsigned stack = unsigned(context.r29);
    if (!valid_guest_range(stack - 0x1000, 0x1000))
        return;
    std::array<unsigned char, 0x1000> saved_stack;
    std::memcpy(saved_stack.data(), m + stack - engine::kRdramBegin - saved_stack.size(),
                saved_stack.size());
    if (shell(hit.item) || hit.item == Item::FakeBox) {
        // Use the same native impulse path as solid traffic. Preserve the
        // actual swept surface point/normal and moving shell speed; a dropped
        // fake box is stationary. Attribution and full rider detachment follow.
        course_impact::Contact contact;
        contact.point = hit.point;
        // The sweep used the victim's pose at impact. Native impulse math uses
        // its current pose, so move the point by the same remaining travel.
        for (unsigned axis = 0; axis < 3; ++axis)
            contact.point[axis] += hit.victim_displacement[axis];
        contact.normal = hit.direction;
        contact.surface_velocity = hit.surface_velocity;
        course_impact::apply(m, context, native_slot(victim), course_impact::Body::Bike, contact);
    }
    // Native attribution uses the collision path, which does not dereference
    // an equipped weapon descriptor. Keep the full detach/physics/recovery path.
    auto call = context;
    call.f_odd = &call.f0.u32h;
    call.r4 = guest_address(source.actor);
    call.r5 = guest_address(target.actor);
    call.r6 = std::bit_cast<unsigned>(scalar(m, target.rider + 0x30C));
    if (source.actor != target.actor)
        func_800616BC(m, &call);
    call = context;
    call.f_odd = &call.f0.u32h;
    call.r4 = guest_address(target.bike);
    call.r5 = guest_address(target.rider);
    func_80037554(m, &call);
    std::memcpy(m + stack - engine::kRdramBegin - saved_stack.size(), saved_stack.data(),
                saved_stack.size());
}
unsigned velocity_mask() {
    const auto rules = prediction::physics_rules();
    if (!rules.active || rules.is_host)
        return (1u << racer_capacity) - 1;
    return rules.connected && rules.authoritative && rules.local_slot < racer_capacity &&
                   (prediction::active() || rules.phase == netplay::Phase::Race)
               ? 1u << rules.local_slot
               : 0;
}
template <class Actors>
void apply_effects(unsigned char *m, const Snapshot &state, Actors &actors, unsigned velocity) {
    for (unsigned slot = 0; slot < racer_capacity; ++slot) {
        auto &a = actors[slot];
        const auto p = native::pair(m, native_slot(slot));
        if (!p.actor || p != a.owner)
            continue;
        const auto &r = state.riders[slot];
        if (r.shrink_until > state.clock) {
            a.bike_geometry.scale(m, p.bike + 0x108, .5f);
            a.rider_geometry.scale(m, p.rider + 0x28, .5f, native::mounted_center_shift(m, p, .5f));
        } else {
            a.bike_geometry.restore(m);
            a.rider_geometry.restore(m);
        }
        if (!(velocity & (1u << slot)))
            continue;
        float rated = 0;
        if (!rated_speed(m, p, rated))
            continue;
        float minimum = 0, maximum = 400;
        if (r.boost_until > state.clock)
            minimum = r.boost_speed;
        if (r.star_until > state.clock)
            minimum = std::max(minimum, std::min(400.f, rated * 1.15f));
        if (r.shrink_until > state.clock) {
            maximum = rated * .55f;
            minimum = std::min(minimum, maximum);
        }
        if (minimum || maximum < 400)
            native::speed(m, p, minimum, maximum);
    }
}
void apply_effects(unsigned char *m) {
    apply_effects(m, owns_simulation() ? live.state : live.physics_state, live.actors,
                  velocity_mask());
}
Snapshot effects_only(const Snapshot &source) {
    if (!source.enabled)
        return {};
    Snapshot out;
    out.enabled = 1;
    out.clock = source.clock;
    out.random = source.random;
    for (unsigned i = 0; i < racer_capacity; ++i) {
        const auto &s = source.riders[i];
        auto &d = out.riders[i];
        d.star_until = s.star_until;
        d.boo_until = s.boo_until;
        d.shrink_until = s.shrink_until;
        d.boost_until = s.boost_until;
        d.boost_speed = s.boost_speed;
        d.hit_until = s.hit_until;
    }
    return out;
}
void expire_effects(Snapshot &state, std::uint64_t elapsed_us) {
    if (!state.enabled)
        return;
    state.clock = unsigned(elapsed_us * ticks_per_second / 1000000);
    for (auto &r : state.riders) {
        for (auto *deadline :
             {&r.star_until, &r.boo_until, &r.shrink_until, &r.boost_until, &r.hit_until})
            if (*deadline <= state.clock)
                *deadline = 0;
        if (!r.boost_until)
            r.boost_speed = 0;
    }
}
const Snapshot *effect_state(unsigned char *m) {
    if (prediction::active())
        return private_bound(m) ? &private_runtime.frame.state : nullptr;
    return racing(m) ? (owns_simulation() ? &live.state : &live.physics_state) : nullptr;
}
bool valid_geometry(const native::Geometry &g, unsigned body) {
    if (!g.active)
        return g == native::Geometry{};
    if (g.body != body || !valid_guest_range(body, 0x200) || !g.count || g.count > 3)
        return false;
    for (unsigned i = 0; i < g.count * 4; ++i) {
        const float value = std::bit_cast<float>(g.original[i]);
        if (!std::isfinite(value) || std::abs(value) > 20 || (i >= g.count * 3 && value <= 0) ||
            !std::isfinite(std::bit_cast<float>(g.written[i])) ||
            std::abs(std::bit_cast<float>(g.written[i])) > 20 ||
            (i >= g.count * 3 && g.written[i] != std::bit_cast<unsigned>(value * .5f)))
            return false;
    }
    return true;
}
bool valid_replay(const ReplayState &s) {
    if (!valid(s.state) || s.elapsed_us > 1000000250000ull ||
        (s.state.enabled && s.elapsed_us * ticks_per_second / 1000000 != s.state.clock))
        return false;
    for (const auto &a : s.actors) {
        const auto &p = a.owner;
        if (!p.actor && p != native::Pair{})
            return false;
        if (p.actor &&
            (p.actor < native::actors || (p.actor - native::actors) % native::actor_stride ||
             (p.actor - native::actors) / native::actor_stride >= racer_capacity ||
             !valid_guest_range(p.bike, bike::stride) ||
             !valid_guest_range(p.rider, rider::stride) || !valid_guest_range(p.route, 0x64)))
            return false;
        if (!valid_geometry(a.bike_geometry, p.bike + 0x108) ||
            !valid_geometry(a.rider_geometry, p.rider + 0x28))
            return false;
    }
    return true;
}
} // namespace

bool input_active() noexcept {
    return accepts_input.load(std::memory_order_acquire);
}
void request_use(unsigned controller) noexcept {
    if (controller < 4 && input_active())
        requests.fetch_or(1u << controller, std::memory_order_release);
}
unsigned take_action(unsigned controller) noexcept {
    if (controller >= 4)
        return 0;
    return requests.fetch_and(~(1u << controller), std::memory_order_acq_rel) & (1u << controller)
               ? authority::action_mk64_item
               : 0;
}
void stage_use(unsigned slot, int y) noexcept {
    if (slot < racer_capacity && !prediction::active())
        live.staged[slot] = {true, std::int8_t(std::clamp(y, -128, 127))};
}
void reset_runtime() noexcept {
    if (prediction::active())
        return;
    accepts_input.store(false, std::memory_order_release);
    requests.store(0, std::memory_order_release);
    for (unsigned slot = 0; slot < racer_capacity; ++slot)
        restore_actor(live.memory, slot);
    live = {};
    reset_hud_focus();
    publish();
    reset_audio();
}
Snapshot capture_state() noexcept {
    std::lock_guard lock(published_mutex);
    return published;
}
bool capture_replay(ReplayState &out) noexcept {
    if (prediction::active())
        return false;
    out = {};
    out.state = effects_only(owns_simulation() ? live.state : live.physics_state);
    if (!valid(out.state))
        return false;
    const std::uint64_t minimum_us = (std::uint64_t(out.state.clock) * 1000000 + 29) / 30;
    out.elapsed_us = out.state.enabled ? std::max(minimum_us, live.physics_elapsed_us) : 0;
    for (unsigned slot = 0; slot < racer_capacity; ++slot) {
        const auto &a = live.actors[slot];
        out.actors[slot] = {a.owner, a.bike_geometry, a.rider_geometry};
    }
    return true;
}
bool bind_replay(unsigned char *m, const ReplayState &source) noexcept {
    if (!m || !prediction::active() || !valid_replay(source))
        return false;
    for (unsigned slot = 0; slot < racer_capacity; ++slot)
        if (source.actors[slot].owner.actor &&
            native::pair(m, native_slot(slot)) != source.actors[slot].owner)
            return false;
    private_runtime = {source, m, prediction::replay_epoch};
    private_runtime.frame.state = effects_only(source.state);
    return true;
}
bool replay_state(ReplayState &out) noexcept {
    if (!private_bound(private_runtime.memory))
        return false;
    out = private_runtime.frame;
    return valid(out.state);
}
bool correct_replay(unsigned char *m, ReplayState &frame, const Snapshot &authority) noexcept {
    if (!m || !valid(authority) || !valid_replay(frame))
        return false;
    for (auto &a : frame.actors) {
        if (a.owner.actor && word(m, a.owner.actor + 0xE0) == a.owner.bike &&
            word(m, a.owner.actor + 0xE4) == a.owner.rider) {
            a.bike_geometry.restore(m);
            a.rider_geometry.restore(m);
        } else {
            a = {};
        }
    }
    frame.state = effects_only(authority);
    // Preserve the phase within the authored 30 Hz tick, not a stale elapsed
    // whole tick from the rejected prediction baseline.
    const auto fractional = frame.elapsed_us * ticks_per_second % 1000000;
    frame.elapsed_us =
        (std::uint64_t(authority.clock) * 1000000 + fractional + 29) / ticks_per_second;
    return true;
}
void finish_replay(std::uint32_t duration_us) noexcept {
    if (!private_bound(private_runtime.memory) || duration_us > 250000)
        return;
    auto &frame = private_runtime.frame;
    if (!frame.state.enabled)
        return;
    frame.elapsed_us += duration_us;
    expire_effects(frame.state, frame.elapsed_us);
    apply_effects(private_runtime.memory, frame.state, frame.actors, velocity_mask());
}
bool apply_state(const Snapshot &s, std::uint32_t round, std::uint64_t tick) noexcept {
    if (prediction::active() || !round || !tick || !valid(s))
        return false;
    if (live.round == round && (tick < live.tick || s.clock < live.state.clock))
        return false;
    if (live.round == round && tick == live.tick)
        return s == live.state;
    if (live.round != round)
        reset_runtime();
    live.state = s;
    live.physics_state = effects_only(s);
    live.physics_elapsed_us = (std::uint64_t(s.clock) * 1000000 + 29) / 30;
    live.round = round;
    live.tick = tick;
    publish();
    return true;
}
bool can_grant(unsigned slot) noexcept {
    return slot < racer_capacity && local_race_options::mk64_items_enabled() &&
           live.state.enabled && owns_simulation() && live.state.riders[slot].held == Item::None;
}
bool grant_item(unsigned slot, Item item) noexcept {
    if (!can_grant(slot) || !grant(live.state, slot, item))
        return false;
    publish();
    return true;
}
bool render_effect(unsigned char *m, unsigned node, RiderState &effect, unsigned &clock) noexcept {
    if (!m || prediction::active() || !experimental_course::active() ||
        !valid_guest_range(node, actor_scene::node_minimum_size))
        return false;
    const unsigned type = word(m, node);
    if (type != 1 && type != 2)
        return false;
    const unsigned slot = canonical_for_entity(m, word(m, node + 4));
    if (slot >= racer_capacity)
        return false;
    const Snapshot *state = highlights::render_items();
    if (!state)
        state = &live.state;
    if (!state->enabled)
        return false;
    effect = state->riders[slot];
    clock = state->clock;
    return true;
}
bool render_rider_anchor(unsigned char *m, unsigned slot, Vec &anchor) noexcept {
    RenderAnchors anchors;
    if (!render_anchors(m, slot, anchors))
        return false;
    anchor = anchors.rider;
    const Snapshot *state = highlights::render_items();
    if (!state)
        state = &live.state;
    if (anchors.attached && state->enabled && state->riders[slot].shrink_until > state->clock)
        for (unsigned axis = 0; axis < 3; ++axis)
            anchor[axis] =
                anchors.bike_origin[axis] + (anchor[axis] - anchors.bike_origin[axis]) * .5f;
    return true;
}
void scale_weapon_matrix(unsigned char *m, unsigned node, unsigned record, unsigned matrix,
                         unsigned prepared_source) noexcept {
    if (!m || prediction::active() || !experimental_course::active() ||
        highlights::render_items() || !valid_guest_range(node, actor_scene::node_minimum_size) ||
        word(m, node) != 2)
        return;
    const unsigned rider = word(m, node + 4), slot = canonical_for_entity(m, rider);
    if (slot >= racer_capacity || !record || record != word(m, rider + 0x5BC))
        return; // Only the separately submitted owned root, never its children.
    const unsigned parent = word(m, node + actor_scene::current_model);
    const unsigned source_record = word(m, parent + 0x14);
    const unsigned source = prepared_source <= 2 ? prepared_source : half(m, source_record + 0x12);
    const float factor = source <= 2 ? scalar(m, 0x8009DBAC + source * 4) : 0;
    scale_render_root(m, slot, 2, matrix, factor);
}

} // namespace rr64::mk64_items

extern "C" void rr64_mk64_items_step(unsigned char *m, void *opaque) {
    using namespace rr64;
    using namespace mk64_items;
    if (!opaque || !racing(m))
        return;
    const auto runovers = live.runovers;
    live.runovers = {};
    if (!local_race_options::mk64_items_enabled()) {
        if (live.state.enabled || live.physics_state.enabled || live.memory)
            reset_runtime();
        accepts_input.store(false, std::memory_order_release);
        requests.store(0, std::memory_order_release);
        live.staged = {};
        return;
    }
    if (half(m, engine::globals::gameplay_pause_state) || !render_asset_available() ||
        !audio_available())
        return;
    const float elapsed = scalar(m, 0x800D7670), delta = scalar(m, engine::globals::physics_delta);
    if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > 1000000 || !std::isfinite(delta) ||
        delta <= 0 || delta > .25f)
        return;
    if (!owns_simulation()) {
        if (!live.physics_state.enabled) {
            live.physics_elapsed_us = 0;
            apply_effects(m);
            return;
        }
        // Native6B5F0 restores the full frame delta after its optional two
        // substeps, before the6B678 item boundary.
        live.physics_elapsed_us += unsigned(std::llround(double(delta) * 1000000));
        expire_effects(live.physics_state, live.physics_elapsed_us);
        apply_effects(m);
        rr64_mk64_item_audio_step(m);
        return;
    }
    if (!live.state.enabled || live.memory != m ||
        live.course != experimental_course::route_data() || elapsed < live.elapsed) {
        const auto admitted = live.staged;
        reset_runtime();
        live.staged = admitted;
        live.memory = m;
        live.course = experimental_course::route_data();
        prepare_route();
        const auto status = netplay::get_status();
        const unsigned seed = (status.active ? status.game_setup.random_seed
                                             : word(m, engine::globals::random_state)) ^
                              0x4D4B3634u;
        initialize(live.state, seed ? seed : 1);
        const auto elapsed_us = std::uint64_t(std::llround(double(elapsed) * 1000000));
        const auto delta_us = unsigned(std::llround(double(delta) * 1000000));
        live.physics_elapsed_us = elapsed_us > delta_us ? elapsed_us - delta_us : 0;
    }
    live.elapsed = elapsed;
    live.physics_elapsed_us += unsigned(std::llround(double(delta) * 1000000));
    const unsigned clock = unsigned(live.physics_elapsed_us * ticks_per_second / 1000000);
    std::array<Racer, racer_capacity> racers{};
    for (unsigned slot = 0; slot < racer_capacity; ++slot) {
        auto &r = racers[slot];
        r = read_racer(m, slot);
        if (!r.active || !r.riding || r.finished)
            continue;
        const unsigned controller = word(m, live.actors[slot].owner.actor + 8);
        if (r.human && !netplay::get_physics_rules().active && controller < 4 &&
            take_action(controller))
            stage_use(
                slot,
                std::int8_t(
                    m[(engine::globals::controller_stick_y - engine::kRdramBegin + controller) ^
                      3u]));
        auto &a = live.actors[slot];
        if (!r.human && live.state.riders[slot].held != Item::None && clock >= a.next_ai_use) {
            live.staged[slot] = {true, 0};
            // Canonical, deterministic stagger avoids every AI firing together.
            a.next_ai_use = clock + 45 + slot * 3;
        }
    }
    auto result = step(live.state, racers, live.staged, clock, delta, {nullptr, surface, guide});
    live.staged = {};
    for (unsigned slot = 0; slot < racer_capacity; ++slot) {
        const auto &contact = runovers[slot];
        if (contact.owner_slot >= racer_capacity || !racers[slot].riding || racers[slot].finished ||
            !racers[contact.owner_slot].riding || racers[contact.owner_slot].finished ||
            native::pair(m, native_slot(slot)) != contact.victim ||
            native::pair(m, native_slot(contact.owner_slot)) != contact.owner)
            continue;
        runover(live.state, result, slot, contact.owner_slot, racers[slot].position,
                racers[contact.owner_slot].velocity);
    }
    const auto &context = *static_cast<recomp_context *>(opaque);
    for (unsigned slot = 0; slot < racer_capacity; ++slot)
        native_hit(m, context, slot, result.hits[slot]);
    apply_effects(m);
    publish();
    rr64_mk64_item_audio_step(m);
}
extern "C" void rr64_mk64_items_before_physics(unsigned char *m) {
    using namespace rr64;
    using namespace mk64_items;
    if (prediction::active()) {
        if (private_bound(m)) {
            auto &frame = private_runtime.frame;
            apply_effects(m, frame.state, frame.actors, velocity_mask());
        }
        return;
    }
    // Native contacts are collected only during this update's physics. Never
    // carry one through a pause, rejected update, actor reset or menu boundary.
    live.runovers = {};
    // Historical private effects above come from ReplayState, never today's
    // menu preference. The live OFF transition restores exact geometry before
    // discarding inventory, projectiles and pending use edges.
    if (!local_race_options::mk64_items_enabled()) {
        if (live.state.enabled || live.physics_state.enabled || live.memory)
            reset_runtime();
        accepts_input.store(false, std::memory_order_release);
        requests.store(0, std::memory_order_release);
        live.staged = {};
        return;
    }
    rr64_mk64_item_audio_step(m);
    const bool ready = racing(m) && render_asset_available() && audio_available();
    accepts_input.store(ready && !half(m, engine::globals::gameplay_pause_state),
                        std::memory_order_release);
    if (!ready || half(m, engine::globals::gameplay_pause_state)) {
        requests.store(0, std::memory_order_release);
        live.staged = {};
        return;
    }
    if (live.state.enabled) {
        if (!live.memory)
            live.memory = m;
        for (unsigned slot = 0; slot < racer_capacity; ++slot)
            read_racer(m, slot);
        apply_effects(m);
    } else if (live.memory == m) {
        apply_effects(m); // A received empty state also restores old geometry.
    }
}
extern "C" void rr64_mk64_items_mode(unsigned char *m, unsigned mode) {
    using namespace rr64;
    if (!prediction::active() && !engine::is_live_race_transition(mode, mode))
        mk64_items::reset_runtime();
    (void)m;
}
extern "C" void rr64_mk64_items_bike_contact(unsigned char *m, unsigned first, unsigned second) {
    using namespace rr64;
    using namespace mk64_items;
    if (!racing(m) || !owns_simulation() || !local_race_options::mk64_items_enabled() ||
        !live.state.enabled || live.memory != m || half(m, engine::globals::gameplay_pause_state))
        return;
    const unsigned a = canonical_for_entity(m, first), b = canonical_for_entity(m, second);
    if (a >= racer_capacity || b >= racer_capacity || a == b)
        return;
    const auto pa = native::pair(m, native_slot(a)), pb = native::pair(m, native_slot(b));
    if (pa.bike != first || pb.bike != second || !native::mounted(m, pa) || !native::mounted(m, pb))
        return;
    // The stock pair test uses a broad bike envelope. Require the current
    // scaled bike spheres too, so a visibly separated small bike cannot count.
    native::Contacts ca, cb;
    if (!native::contact_spheres(m, pa, ca) || !native::contact_spheres(m, pb, cb))
        return;
    bool touching = false;
    for (unsigned i = 0; i < word(m, pa.bike + 0x134); ++i)
        for (unsigned j = 0; j < word(m, pb.bike + 0x134); ++j) {
            const float radius = ca.spheres[i].radius + cb.spheres[j].radius;
            touching |=
                distance_squared(ca.spheres[i].center, cb.spheres[j].center) <= radius * radius;
        }
    if (!touching)
        return;
    for (unsigned victim : {a, b}) {
        const unsigned owner = victim == a ? b : a;
        const auto &small = live.state.riders[victim], &large = live.state.riders[owner];
        if (small.shrink_until > live.state.clock && large.shrink_until <= live.state.clock &&
            !immune(small, live.state.clock) && large.boo_until <= live.state.clock &&
            small.hit_until <= live.state.clock &&
            live.runovers[victim].owner_slot == racer_capacity)
            live.runovers[victim] = {victim == a ? pb : pa, victim == a ? pa : pb, owner};
    }
}
extern "C" int rr64_mk64_items_immune(unsigned char *m, unsigned entity) {
    using namespace rr64::mk64_items;
    const auto *state = effect_state(m);
    if (!state || !state->enabled)
        return 0;
    const unsigned slot = canonical_for_entity(m, entity);
    return slot < racer_capacity && immune(state->riders[slot], state->clock);
}
extern "C" int rr64_mk64_items_ghost(unsigned char *m, unsigned entity) {
    using namespace rr64::mk64_items;
    const auto *state = effect_state(m);
    if (!state || !state->enabled)
        return 0;
    const unsigned slot = canonical_for_entity(m, entity);
    return slot < racer_capacity && state->riders[slot].boo_until > state->clock;
}
extern "C" void rr64_mk64_items_scale_matrix(unsigned char *m, unsigned node, unsigned record,
                                             unsigned matrix) {
    using namespace rr64;
    using namespace engine;
    using namespace mk64_items;
    if (!m || prediction::active() || !experimental_course::active() ||
        !valid_guest_range(node, actor_scene::node_minimum_size) ||
        !valid_guest_range(matrix, 64) || (matrix & 3))
        return;
    const unsigned type = word(m, node), entity = word(m, node + 4);
    if (type != 1 && type != 2)
        return;
    bool root = record && record == word(m, node + actor_scene::current_model);
    for (unsigned tier = 0; !root && tier < 3; ++tier)
        root = record == word(m, node + actor_scene::lod_models + tier * 4);
    if (!root)
        return; // The scaled parent already applies to all skeletal children.
    const unsigned slot = canonical_for_entity(m, entity);
    if (slot >= racer_capacity)
        return;
    const unsigned source_record = word(m, record + 0x14);
    const unsigned source = half(m, source_record + 0x12);
    const float factor = source <= 2 ? scalar(m, 0x8009DBAC + source * 4) : 0;
    scale_render_root(m, slot, type, matrix, factor);
}

extern "C" void rr64_mk64_items_draw(unsigned char *m) {
    using namespace rr64;
    if (!m || prediction::active() || !experimental_course::active())
        return;
    const auto *recorded = highlights::render_items();
    mk64_items::draw_world(m, recorded ? *recorded : mk64_items::capture_state());
}
