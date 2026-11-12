#include "rr64_course_items.hpp"
#include "rr64_course_item_render.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_experimental_course.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_course_hazards.hpp"
#include "rr64_course_roulette.hpp"
#include "rr64_mk64_items.hpp"
#include "rr64_mk64_item_hud.hpp"
#include "rr64_mk64_item_render.hpp"
#include "rr64_highlights.hpp"
#include "rr64_online_flow.hpp"
#include "rr64_local_race_options.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" void func_80037920(unsigned char *, recomp_context *);
extern "C" void func_80037960(unsigned char *, recomp_context *);
extern "C" void func_8004E754(unsigned char *, recomp_context *);
extern "C" void func_8001EB90(unsigned char *, recomp_context *);

namespace rr64::course_items {
namespace {
using namespace engine;
using Vec = std::array<float, 3>;
constexpr unsigned actors = 0x800D8570, actor_stride = 0x118;
constexpr unsigned first_weapon = 2, last_weapon = 14, inventory = 0x838;
constexpr float clock_hz = 30.0f;
// func_800373F0: three local centers at +58/+64/+70, followed by
// their radii at +7C/+80/+84. World centers are refreshed by 4E754.
constexpr unsigned maximum_contact_points = 3;
struct Pose {
    Vec position{};
    std::array<Vec, maximum_contact_points> contacts{};
    std::array<float, maximum_contact_points> radii{};
    unsigned contact_count = 0;
    unsigned bike = 0, rider = 0, recovery = 0;
    float speed = 0;
    bool valid = false;
};
struct Runtime {
    netplay::CourseItemState boxes{};
    std::array<Pose, kMaximumRacers> previous{};
    std::array<Vec, netplay::kMaximumCourseItems> previous_boxes{};
    std::array<unsigned, netplay::kMaximumCourseItems> parent_generation{};
    std::array<bool, netplay::kMaximumCourseItems> box_valid{};
    std::array<Pose, kMaximumRacers> pending_rewards{};
    std::uint32_t random = 0, round = 0;
    std::uint64_t tick = 0;
    float elapsed = -1;
    bool initialized = false;
} live;
thread_local bool hud_active = false;
thread_local RouletteDisplay hud_weapon{};
thread_local mk64_items::HudDisplay hud_item{};
thread_local unsigned hud_view = 4;
struct CycleWrap {
    unsigned char *mapping = nullptr;
    unsigned rider = 0;
};
thread_local CycleWrap cycle_wrap;

unsigned word(unsigned char *m, unsigned address) {
    unsigned value = 0;
    read_u32(m, address, value);
    return value;
}
unsigned half(unsigned char *m, unsigned address) {
    std::uint16_t value = 0;
    read_u16(m, address, value);
    return value;
}
float square_distance(const Vec &a, const Vec &b) {
    float sum = 0;
    for (unsigned axis = 0; axis < 3; ++axis)
        sum += (a[axis] - b[axis]) * (a[axis] - b[axis]);
    return sum;
}
bool read_pose(unsigned char *m, recomp_context &context, unsigned slot, Pose &out) {
    const unsigned actor = actors + slot * actor_stride;
    if (!half(m, actor + 0x24))
        return false;
    const unsigned bike = word(m, actor + 0xE0), rider = word(m, actor + 0xE4);
    const unsigned route = word(m, actor + 0xE8);
    if (!valid_guest_range(bike, bike::stride) || !valid_guest_range(rider, rider::stride) ||
        !valid_guest_range(route, 0x64) || word(m, bike + bike::rider_pointer) != rider ||
        word(m, rider + rider::bike_pointer) != bike || !half(m, bike + bike::rider_attached) ||
        half(m, rider + rider::ejected) || half(m, bike + bike::drive_control_lockout) ||
        half(m, route + 0x4C) || half(m, route + 0x50) || half(m, route + 0x52))
        return false;
    float speed_squared = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        float velocity = 0;
        if (!read_float(m, bike + bike::body_position + 4 * axis, out.position[axis]) ||
            !std::isfinite(out.position[axis]) ||
            !read_float(m, bike + 0x178 + 4 * axis, velocity) || !std::isfinite(velocity))
            return false;
        speed_squared += velocity * velocity;
    }
    out.speed = std::sqrt(speed_squared);
    if (!std::isfinite(out.speed))
        return false;
    out.contact_count = word(m, rider + 0x54);
    if (!out.contact_count || out.contact_count > maximum_contact_points)
        return false;
    // Native pickup collision lazily refreshes these same spheres. Validate
    // its bounded inputs before calling it; never use last frame's points.
    if (!half(m, rider + 0x1B0)) {
        for (const auto range :
             {std::pair{0x58u, out.contact_count * 3}, std::pair{0x8Cu, 3u}, std::pair{0xB4u, 3u},
              std::pair{0xE8u, 9u}, std::pair{0x140u, 9u}}) {
            for (unsigned i = 0; i < range.second; ++i) {
                float value = 0;
                if (!read_float(m, rider + range.first + 4 * i, value) || !std::isfinite(value))
                    return false;
            }
        }
        const unsigned stack = static_cast<unsigned>(context.r29);
        if (!valid_guest_range(stack - 0x38u, 0x38u))
            return false;
        auto call = context;
        call.f_odd = &call.f0.u32h;
        call.r4 = guest_address(rider + 0x28);
        func_8004E754(m, &call);
    }
    for (unsigned point = 0; point < out.contact_count; ++point) {
        if (!read_float(m, rider + 0x7C + 4 * point, out.radii[point]) ||
            !std::isfinite(out.radii[point]) || out.radii[point] <= 0)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!read_float(m, rider + 0x1B8 + 12 * point + 4 * axis, out.contacts[point][axis]) ||
                !std::isfinite(out.contacts[point][axis]))
                return false;
    }
    out.bike = bike;
    out.rider = rider;
    out.recovery = word(m, route + 0x40);
    out.valid = true;
    return true;
}

// Swept sphere entry (not closest-point order) makes simultaneous collection
// deterministic: earliest intersection, then canonical racer slot. This uses a
// actual native rider collision spheres: the bike origin is near the ground
// and otherwise passes below a correctly positioned, settled item box.
float entry_fraction(const Vec &a, const Vec &b, const ItemBoxDefinition &box, float radius) {
    const float combined_radius = box.radius + radius;
    float aa = 0, bb = 0, cc = -combined_radius * combined_radius;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float d = b[axis] - a[axis], p = a[axis] - box.position[axis];
        aa += d * d;
        bb += p * d;
        cc += p * p;
    }
    if (cc <= 0)
        return 0;
    if (aa <= 1e-12f)
        return std::numeric_limits<float>::infinity();
    const float discriminant = bb * bb - aa * cc;
    if (discriminant < 0)
        return std::numeric_limits<float>::infinity();
    const float t = (-bb - std::sqrt(discriminant)) / aa;
    return t >= 0 && t <= 1 ? t : std::numeric_limits<float>::infinity();
}
unsigned random_word() {
    // Independent stream: item rewards must not perturb native AI/traffic RNG.
    auto x = live.random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    live.random = x;
    return x;
}
unsigned choose_reward(unsigned char *m, const Pose &pose, unsigned slot) {
    std::array<unsigned, last_weapon - first_weapon + 3 + 14> pool{};
    unsigned size = 0;
    for (unsigned weapon = first_weapon; weapon <= last_weapon; ++weapon) {
        const auto quantity =
            static_cast<std::int16_t>(half(m, pose.bike + inventory + 2 * weapon));
        if (quantity >= 0 && quantity < 4)
            pool[size++] = weapon;
    }
    // Native power-ups replace/refresh the current effect. They remain useful
    // when all weapon slots are full and do not occupy an inventory slot.
    pool[size++] = netplay::kCourseRewardAttackX2;
    pool[size++] = netplay::kCourseRewardAttackX4;
    // A held MK64 item is never replaced by a second box. Double mushroom is
    // an intermediate triple-stack icon, not an additional roulette reward.
    if (mk64_items::can_grant(slot))
        for (unsigned item = 1; item <= 15; ++item)
            if (item != unsigned(mk64_items::Item::DoubleMushroom))
                pool[size++] = mk64_items::reward_id(mk64_items::Item(item));
    // Rejection sampling avoids modulo bias without changing the guest RNG.
    const unsigned threshold = (0u - size) % size;
    unsigned value = random_word();
    while (value < threshold)
        value = random_word();
    return pool[value % size];
}
bool grant(unsigned char *m, recomp_context &context, const Pose &pose, unsigned reward,
           unsigned slot) {
    if (mk64_items::is_reward(reward))
        return mk64_items::grant_item(slot, mk64_items::reward_item(reward));
    auto call = context;
    call.f_odd = &call.f0.u32h;
    call.r4 = guest_address(pose.rider);
    if (const unsigned effect = netplay::course_reward_effect(reward)) {
        // The original pickup dispatcher owns the 25-second timer, replacement
        // behavior, sound and combat effect. It leaves the equipped weapon alone.
        call.r5 = effect;
        func_80037960(m, &call);
    } else if (netplay::is_course_weapon_reward(reward)) {
        call.r5 = reward;
        func_80037920(m, &call);
    } else {
        return false;
    }
    return call.r2 != 0;
}
void finish_rewards(unsigned char *m, recomp_context &context, unsigned count) {
    for (unsigned slot = 0; slot < kMaximumRacers; ++slot) {
        auto &roll = live.boxes.roulette[slot];
        if (!roll.phase)
            continue;
        const auto age = live.boxes.clock - roll.start_clock;
        auto retire = [&] { roll = {roll.generation, 0, 0, 0}; };
        if (slot >= count) {
            retire();
            continue;
        }
        if (roll.phase == 2) {
            if (age >= netplay::kCourseRouletteTicks + netplay::kCourseRewardBlinkTicks)
                retire();
            continue;
        }
        const auto &pose = live.pending_rewards[slot];
        const unsigned actor = actors + slot * actor_stride;
        // Crashing does not discard a collected box, but a retired/replaced
        // actor must never receive an old rider's delayed inventory grant.
        if (!half(m, actor + 0x24) || word(m, actor + 0xE0) != pose.bike ||
            word(m, actor + 0xE4) != pose.rider ||
            word(m, pose.rider + rider::bike_pointer) != pose.bike) {
            retire();
            continue;
        }
        if (age < netplay::kCourseRouletteTicks)
            continue;
        if (netplay::is_course_weapon_reward(roll.weapon)) {
            const auto quantity =
                static_cast<std::int16_t>(half(m, pose.bike + inventory + 2 * roll.weapon));
            if (quantity < 0) {
                retire();
                continue;
            }
            // A weapon stolen during the spin may already have filled this slot.
            // Keep the native cap and choose it without awarding a fifth copy.
            if (quantity >= 4)
                write_u32(m, pose.rider + rider::selected_weapon, roll.weapon);
            else if (!grant(m, context, pose, roll.weapon, slot)) {
                retire();
                continue;
            }
        } else if (!grant(m, context, pose, roll.weapon, slot)) {
            retire();
            continue;
        }
        roll.phase = 2;
    }
}
} // namespace

void reset_runtime() noexcept {
    if (!prediction::active()) {
        mk64_items::reset_runtime();
        live = {};
        hud_active = false;
        hud_weapon = {};
    }
}
netplay::CourseItemState capture_state() noexcept {
    return experimental_course::active() ? live.boxes : netplay::CourseItemState{};
}
bool apply_state(const netplay::CourseItemState &state, std::uint32_t round,
                 std::uint64_t tick) noexcept {
    if (prediction::active() || !round || !tick || !netplay::valid_course_item_state(state) ||
        state.count != definitions().size())
        return false;
    if (live.round == round && (tick < live.tick || state.clock < live.boxes.clock))
        return false;
    if (live.round == round && tick == live.tick)
        return state == live.boxes;
    if (live.round != round)
        mk64_items::reset_hud_focus();
    live.boxes = state;
    live.round = round;
    live.tick = tick;
    live.initialized = true;
    return true;
}
} // namespace rr64::course_items

extern "C" void rr64_course_items_step(unsigned char *m, void *opaque) {
    using namespace rr64;
    using namespace course_items;
    if (!m || !opaque || prediction::active() || rr64_highlights_presenting() ||
        !experimental_course::active())
        return;
    // Publish after every live item update, including depletion and grants,
    // rather than waiting for another HUD draw before the next input poll.
    struct PublishCycleBindings {
        unsigned char *memory;
        ~PublishCycleBindings() {
            rr64_course_items_cycle_publish(memory);
        }
    } publish_cycle_bindings{m};
    // The enclosing race function still reaches this hook on paused frames,
    // after skipping native physics. Retain visible boxes but award nothing.
    if (half(m, engine::globals::gameplay_pause_state))
        return;
    const auto boxes = definitions();
    if (boxes.empty() || boxes.size() > netplay::kMaximumCourseItems) {
        rr64_mk64_items_step(m, opaque);
        return;
    }
    const auto status = netplay::get_status();
    if (status.active && (!status.connected || !status.authoritative || !status.is_host ||
                          status.phase != netplay::Phase::Race)) {
        rr64_mk64_items_step(m, opaque);
        return;
    }
    float elapsed = 0, delta = 0;
    if (!engine::read_float(m, 0x800D7670, elapsed) || !std::isfinite(elapsed) || elapsed < 0 ||
        elapsed > 1000000 || !engine::read_float(m, engine::globals::physics_delta, delta) ||
        !std::isfinite(delta) || delta <= 0 || delta > 0.25f)
        return;
    if (!live.initialized || elapsed < live.elapsed || live.boxes.count != boxes.size()) {
        reset_runtime();
        live.initialized = true;
        live.boxes.count = static_cast<std::uint16_t>(boxes.size());
        live.random = (status.active ? status.game_setup.random_seed
                                     : word(m, engine::globals::random_state)) ^
                      0x4D4B4954u;
        if (!live.random)
            live.random = 1;
    }
    // Parent reset also clears the MK runtime. Initialize/advance it only
    // afterwards, before choosing or delivering this frame's box rewards.
    rr64_mk64_items_step(m, opaque);
    const auto clock = static_cast<std::uint32_t>(std::floor(double(elapsed) * clock_hz));
    const unsigned advance = clock >= live.boxes.clock ? clock - live.boxes.clock : 0;
    live.boxes.clock = clock;
    live.elapsed = elapsed;
    for (unsigned i = 0; i < boxes.size(); ++i)
        live.boxes.cooldown[i] -=
            static_cast<std::uint16_t>(std::min<unsigned>(advance, live.boxes.cooldown[i]));

    const unsigned count = word(m, 0x800A656C);
    if (!count || count > engine::kMaximumRacers)
        return;
    std::array<Pose, engine::kMaximumRacers> current{};
    std::array<bool, engine::kMaximumRacers> sweep{};
    auto &context = *static_cast<recomp_context *>(opaque);
    finish_rewards(m, context, count);
    for (unsigned slot = 0; slot < count; ++slot) {
        if (!read_pose(m, context, slot, current[slot]))
            continue;
        const auto &p = current[slot], &previous = live.previous[slot];
        // Never sweep across a recovery warp or a reallocated bike. The small
        // tolerance allows contact correction; motion beyond the integrator's
        // plausible displacement uses only the new endpoint, not the path.
        const float max_motion = (std::max(p.speed, previous.speed) * delta + 0.25f) * 2;
        if (previous.valid && previous.bike == p.bike && previous.rider == p.rider &&
            previous.recovery == p.recovery && previous.contact_count == p.contact_count &&
            square_distance(previous.position, p.position) <= max_motion * max_motion)
            sweep[slot] = true;
    }
    const auto hazards = course_hazards::capture_state();
    for (unsigned i = 0; i < boxes.size(); ++i) {
        ItemBoxDefinition posed;
        const bool available = posed_definition(boxes[i], hazards, posed);
        const unsigned generation = available && boxes[i].parent_hazard != ~0u
                                        ? hazards.poses[boxes[i].parent_hazard].generation
                                        : 0;
        const auto old_center = live.previous_boxes[i];
        const bool continuous =
            available && live.box_valid[i] && generation == live.parent_generation[i];
        live.previous_boxes[i] = posed.position;
        live.box_valid[i] = available;
        live.parent_generation[i] = generation;
        if (!available || live.boxes.cooldown[i])
            continue;
        std::array<std::pair<float, unsigned>, engine::kMaximumRacers> contacts;
        for (unsigned slot = 0; slot < count; ++slot) {
            float first = std::numeric_limits<float>::infinity();
            const auto &pose = current[slot], &previous = live.previous[slot];
            if (pose.valid && live.boxes.roulette[slot].phase != 1)
                for (unsigned point = 0; point < pose.contact_count; ++point) {
                    auto start =
                        sweep[slot] && continuous ? previous.contacts[point] : pose.contacts[point];
                    if (sweep[slot] && continuous)
                        for (unsigned axis = 0; axis < 3; ++axis)
                            start[axis] += posed.position[axis] - old_center[axis];
                    first = std::min(first, entry_fraction(start, pose.contacts[point], posed,
                                                           pose.radii[point]));
                }
            contacts[slot] = {first, slot};
        }
        std::sort(contacts.begin(), contacts.begin() + count);
        for (unsigned contact = 0; contact < count && std::isfinite(contacts[contact].first);
             ++contact) {
            const unsigned slot = contacts[contact].second;
            const unsigned reward = choose_reward(m, current[slot], slot);
            auto &roll = live.boxes.roulette[slot];
            const auto generation = roll.generation + 1u;
            roll = {generation ? generation : 1u, live.boxes.clock,
                    static_cast<std::uint16_t>(reward), 1};
            live.pending_rewards[slot] = current[slot];
            live.boxes.cooldown[i] = netplay::kCourseItemCooldown;
            ++live.boxes.generation[i];
            break;
        }
    }
    live.previous = current;
}

namespace {
// Native 1EB90 allocates 76-byte records in two pools of 96 sprites.
constexpr unsigned kHudSpriteCapacity = 96, kHudSpriteRecordBytes = 76;
constexpr unsigned kHudSpriteCounts = 0x800BC9D0, kHudSpritePool = 0x800BCAD8;
constexpr unsigned kHiddenHudSprite = 0xFFFF;

struct HudSpriteDimensions {
    unsigned width = 0, height = 0;
};
bool hud_sprite_dimensions(unsigned char *m, unsigned asset, unsigned sprite,
                           HudSpriteDimensions &out) {
    using namespace rr64::course_items;
    // 1E898 resolves the 24-byte descriptor; 1CFB8 reads its +8/+A dimensions.
    const std::uint64_t descriptor = std::uint64_t(asset) + word(m, asset) + sprite * 24u;
    if (descriptor > std::numeric_limits<unsigned>::max() ||
        !rr64::engine::valid_guest_range(static_cast<unsigned>(descriptor), 24))
        return false;
    out = {half(m, static_cast<unsigned>(descriptor) + 8),
           half(m, static_cast<unsigned>(descriptor) + 10)};
    return out.width && out.height && out.width <= 4096 && out.height <= 4096;
}

struct ItemHudSpriteFit {
    unsigned record = 0, buffer = 0, expected_count = 0, asset = 0, sprite = 0;
    float scale = 1;
};
thread_local ItemHudSpriteFit item_hud_fit;
}

extern "C" void rr64_course_items_hud_begin() {
    item_hud_fit = {};
    rr64::mk64_items::reset_hud_queue();
    rr64::course_items::hud_item = {};
    rr64::course_items::hud_view = 4;
    rr64::course_items::hud_active = rr64::experimental_course::active();
    rr64::course_items::hud_weapon = {};
}
extern "C" void rr64_course_items_hud_end() {
    item_hud_fit = {};
    rr64::course_items::hud_item = {};
    rr64::course_items::hud_view = 4;
    rr64::course_items::hud_active = false;
    rr64::course_items::hud_weapon = {};
}
extern "C" unsigned rr64_course_items_hud_weapon(unsigned char *m, unsigned rider,
                                                 unsigned original) {
    using namespace rr64::course_items;
    hud_weapon = {original};
    hud_item = {};
    hud_view = 4;
    if (!m || !hud_active || rr64::prediction::active())
        return original;
    for (unsigned slot = 0; slot < rr64::engine::kMaximumRacers; ++slot) {
        const unsigned actor = actors + slot * actor_stride;
        if (half(m, actor + 0x24) && word(m, actor + 0xE4) == rider) {
            const auto status = rr64::netplay::get_physics_rules();
            const unsigned canonical =
                status.active ? rr64::online_flow::mapped_slot(slot, status.local_slot,
                                                               status.replicated_riders)
                              : slot;
            if (canonical < live.boxes.roulette.size()) {
                hud_weapon = roulette_display(live.boxes.roulette[canonical], live.boxes.clock,
                                              canonical, original, word(m, rider + 0x5D4),
                                              rr64::local_race_options::mk64_items_enabled());
                const auto items = rr64::mk64_items::capture_state();
                if (items.enabled && rr64::mk64_items::render_asset_available())
                    hud_item = rr64::mk64_items::hud_display_for_rider(
                        m, canonical, rider, items.riders[canonical],
                        live.boxes.roulette[canonical], live.boxes.clock, original,
                        rr64::local_race_options::mk64_items_enabled());
                const unsigned views = word(m, 0x800A6578);
                for (unsigned view = 0; view < std::min(views, 4u); ++view)
                    if (word(m, 0x800A657C + view * 4) == slot) {
                        hud_view = view;
                        break;
                    }
            }
            break;
        }
    }
    return hud_weapon.weapon;
}
extern "C" void rr64_course_items_cycle_publish(unsigned char *m) {
    using namespace rr64;
    using namespace course_items;
    if (prediction::active())
        return;
    std::array<mk64_items::CycleBinding, 4> bindings{};
    const auto publish_empty = [&] { mk64_items::publish_cycle_bindings(bindings); };
    if (!m || rr64_highlights_presenting() || !experimental_course::active() ||
        !local_race_options::mk64_items_enabled() ||
        !engine::is_live_race_transition(word(m, engine::globals::main_mode),
                                         word(m, engine::globals::pending_mode))) {
        publish_empty();
        return;
    }
    const auto items = mk64_items::capture_state();
    const auto rules = netplay::get_physics_rules();
    const unsigned mask = word(m, 0x800A54D8);
    const unsigned views = word(m, 0x800A6578);
    if (!items.enabled || !mk64_items::render_asset_available() || !mask || mask > 0xffff ||
        views < 1 || views > bindings.size() ||
        (rules.active && (!rules.connected || rules.phase != netplay::Phase::Race))) {
        publish_empty();
        return;
    }
    std::array<bool, 4> duplicate{};
    for (unsigned view = 0; view < views; ++view) {
        const unsigned slot = word(m, 0x800A657C + view * 4);
        if (slot >= engine::kMaximumRacers)
            continue;
        const unsigned actor = actors + slot * actor_stride;
        const unsigned canonical =
            rules.active ? online_flow::mapped_slot(slot, rules.local_slot, rules.replicated_riders)
                         : slot;
        if (canonical >= items.riders.size() || (rules.active && canonical != rules.local_slot))
            continue;
        const unsigned profile = rules.active ? 0 : word(m, actor + 8);
        const unsigned rider = word(m, actor + 0xE4), bike = word(m, actor + 0xE0);
        if (profile >= bindings.size() || !half(m, actor + 0x24) ||
            !engine::valid_guest_range(rider, engine::rider::stride) ||
            !engine::valid_guest_range(bike, engine::bike::stride) ||
            word(m, bike + engine::bike::rider_pointer) != rider ||
            word(m, rider + engine::rider::bike_pointer) != bike)
            continue;
        if (bindings[profile].mapping) {
            duplicate[profile] = true;
            continue;
        }
        bindings[profile] = {m, canonical, rider, mask, items.riders[canonical]};
    }
    for (unsigned profile = 0; profile < bindings.size(); ++profile)
        if (duplicate[profile])
            bindings[profile] = {};
    mk64_items::publish_cycle_bindings(bindings);
}
extern "C" void rr64_course_items_weapon_wrapped(unsigned char *m, unsigned rider) {
    using namespace rr64::course_items;
    if (!rr64::prediction::active())
        cycle_wrap = {m, rider};
}
extern "C" void rr64_course_items_weapon_switched(unsigned char *m, unsigned rider) {
    using namespace rr64;
    using namespace course_items;
    const bool wrapped = cycle_wrap.mapping == m && cycle_wrap.rider == rider;
    cycle_wrap = {};
    // These callbacks follow the original inventory search. Never filter its
    // input or put an imported item ID into a native weapon/inventory field.
    if (!m || prediction::active() || rr64_highlights_presenting() ||
        !experimental_course::active() || !local_race_options::mk64_items_enabled() ||
        !engine::valid_guest_range(rider, engine::rider::stride))
        return;
    const auto items = mk64_items::capture_state();
    const unsigned equipped = word(m, rider + engine::rider::selected_weapon);
    if (!items.enabled || equipped > 14)
        return;
    const auto status = netplay::get_physics_rules();
    for (unsigned slot = 0; slot < engine::kMaximumRacers; ++slot) {
        const unsigned actor = actors + slot * actor_stride;
        if (!half(m, actor + 0x24) || word(m, actor + 0xE4) != rider)
            continue;
        const unsigned canonical =
            status.active
                ? online_flow::mapped_slot(slot, status.local_slot, status.replicated_riders)
                : slot;
        if (canonical < items.riders.size())
            mk64_items::focus_native_weapon(m, canonical, rider, equipped, items.riders[canonical],
                                            wrapped && equipped == 1);
        return;
    }
}
extern "C" unsigned rr64_course_items_hud_quantity(unsigned original) {
    using namespace rr64::course_items;
    const bool effect_icon = rr64::netplay::course_reward_effect(hud_weapon.reward) != 0;
    return hud_active && (hud_weapon.rolling || effect_icon || hud_item.owns) ? 0u : original;
}
extern "C" unsigned rr64_course_items_hud_sprite(unsigned char *m, unsigned original) {
    using namespace rr64::course_items;
    item_hud_fit = {};
    if (hud_active && hud_item.owns && original == 0xA3u + hud_weapon.weapon) {
        rr64::mk64_items::arm_hud_sprite(m, hud_view, original, hud_item);
        return original;
    }
    if (!hud_active || original != 0xA3u + hud_weapon.weapon ||
        (!hud_weapon.rolling && !hud_weapon.reward_visible))
        return original;
    // The single-player caller masks the sprite to u16 in its delay slot.
    // Keep this marker confined to the sprite argument, never the weapon ID.
    if (hud_weapon.blink_off)
        return kHiddenHudSprite;
    const unsigned sprite = roulette_sprite(hud_weapon);
    if (!rr64::netplay::course_reward_effect(hud_weapon.reward))
        return sprite;
    // Native BF/C0 are 64x64; weapon icons are 24x24. Read the actual atlas
    // descriptors so a texture mod retains the same weapon-box footprint.
    if (!m)
        return original;
    const unsigned buffer = word(m, 0x8009CBA4), asset = word(m, 0x800D6870);
    if (buffer >= 2 || !rr64::engine::valid_guest_range(asset, 0x1C))
        return original;
    const unsigned count = half(m, kHudSpriteCounts + buffer * 2);
    if (count >= kHudSpriteCapacity)
        return original;
    HudSpriteDimensions source, replacement;
    if (!hud_sprite_dimensions(m, asset, original, source) ||
        !hud_sprite_dimensions(m, asset, sprite, replacement))
        return original;
    item_hud_fit = {kHudSpritePool + (buffer * kHudSpriteCapacity + count) * kHudSpriteRecordBytes,
                    buffer,
                    count + 1,
                    asset,
                    sprite,
                    std::min(float(source.width) / replacement.width,
                             float(source.height) / replacement.height)};
    return sprite;
}
extern "C" void rr64_course_items_hud_sprite_end(unsigned char *m) {
    using namespace rr64::course_items;
    rr64::mk64_items::commit_hud_sprite(m);
    const auto fit = item_hud_fit;
    item_hud_fit = {};
    // The native allocator can decline a full pool. Authenticate this exact
    // allocation before fitting it; never resize the previous player's sprite.
    if (!m || !hud_active || !fit.record || word(m, 0x8009CBA4) != fit.buffer ||
        half(m, kHudSpriteCounts + fit.buffer * 2) != fit.expected_count ||
        word(m, fit.record) != fit.asset || half(m, fit.record + 0x10) != fit.sprite)
        return;
    float sx = 0, sy = 0;
    if (rr64::engine::read_float(m, fit.record + 0x20, sx) &&
        rr64::engine::read_float(m, fit.record + 0x24, sy) && std::isfinite(sx) &&
        std::isfinite(sy)) {
        rr64::engine::write_float(m, fit.record + 0x20, sx * fit.scale);
        rr64::engine::write_float(m, fit.record + 0x24, sy * fit.scale);
    }
}
extern "C" int rr64_course_items_hud_hide(unsigned sprite) {
    using namespace rr64::course_items;
    return hud_active && sprite == kHiddenHudSprite;
}

extern "C" void rr64_course_items_hud_quadrants(unsigned char *m, void *context) {
    using namespace rr64;
    using namespace course_items;
    // 30220 branches past its entire weapon block for layouts >= 2. Do not
    // change those layout globals to force it through a different HUD: doing
    // so also changes world/view placement. Online's scoped HUD is layout 0.
    if (!m || !context || !hud_active || prediction::active() || rr64_highlights_presenting() ||
        half(m, engine::globals::gameplay_pause_state))
        return;
    const unsigned views = word(m, 0x800A6578), layout = word(m, 0x800A4F24);
    if (layout < 2 || views < 3 || views > 4)
        return;
    auto &caller = *static_cast<recomp_context *>(context);
    const unsigned stack = static_cast<unsigned>(caller.r29);
    if (!engine::valid_guest_range(stack - 0x18u, 0xF4u) ||
        !(word(m, stack + 0xD8) & 1)) // Same 30220 weapon/HUD drawing mask.
        return;
    const unsigned buffer = word(m, 0x8009CBA4), asset = word(m, 0x800D6870);
    const unsigned width = word(m, 0x800B0808), height = word(m, 0x800B080C);
    if (buffer >= 2 || !width || !height || width > 4096 || height > 4096 ||
        !engine::valid_guest_range(asset, 0x1Cu))
        return;
    const auto rules = netplay::get_physics_rules();
    // Preserve the caller's outgoing argument area and the sprite producer's
    // small stack frame. Texture preparation may use ordinary free stack below
    // that; no caller-owned locals, registers or gameplay state are borrowed.
    std::array<unsigned char, 0x60> scratch;
    std::memcpy(scratch.data(), m + stack - 0x80000000u - 0x18u, scratch.size());
    const auto previous = hud_weapon;
    const auto previous_item = hud_item;
    const auto previous_view = hud_view;
    const auto items = mk64_items::capture_state();
    for (unsigned view = 0; view < views; ++view) {
        const unsigned slot = word(m, 0x800A657C + view * 4);
        if (slot >= engine::kMaximumRacers)
            continue;
        const unsigned canonical =
            rules.active ? online_flow::mapped_slot(slot, rules.local_slot, rules.replicated_riders)
                         : slot;
        if (canonical >= live.boxes.roulette.size())
            continue;
        const auto &roll = live.boxes.roulette[canonical];
        const unsigned actor = actors + slot * actor_stride;
        const unsigned rider = word(m, actor + 0xE4), route = word(m, actor + 0xE8);
        if (!half(m, actor + 0x24) || !engine::valid_guest_range(rider, engine::rider::stride) ||
            !engine::valid_guest_range(route, 0x54) || half(m, route + 0x4E) ||
            half(m, route + 0x50))
            continue;
        const unsigned equipped = word(m, rider + engine::rider::selected_weapon);
        hud_weapon =
            roulette_display(roll, live.boxes.clock, canonical, equipped, word(m, rider + 0x5D4),
                             local_race_options::mk64_items_enabled());
        hud_view = view;
        hud_item = items.enabled && mk64_items::render_asset_available()
                       ? mk64_items::hud_display_for_rider(
                             m, canonical, rider, items.riders[canonical], roll, live.boxes.clock,
                             equipped, local_race_options::mk64_items_enabled())
                       : mk64_items::HudDisplay{};
        // The original quadrant HUD has no permanent weapon icon. Keep this
        // shared square visible when switching away from a held imported item.
        const bool held_item = items.enabled && local_race_options::mk64_items_enabled() &&
                               items.riders[canonical].held != mk64_items::Item::None;
        if ((!hud_item.owns &&
             (hud_weapon.blink_off ||
              (!hud_weapon.rolling && !hud_weapon.reward_visible && !held_item))) ||
            half(m, kHudSpriteCounts + buffer * 2) >= kHudSpriteCapacity)
            continue;
        const unsigned sprite = roulette_sprite(hud_weapon);
        // Fit even a texture-mod replacement in a bounded 16x16 rectangle.
        HudSpriteDimensions icon;
        if (!hud_sprite_dimensions(m, asset, sprite, icon))
            continue;
        auto call = caller;
        call.f_odd = &call.f0.u32h;
        call.r4 = engine::guest_address(asset);
        call.r5 = sprite;
        // Native quadrant rank/status occupies the upper band (y18/23),
        // speed the bottom (y100/210). The 16px transient fits between those
        // bands at local(36,64); reusing the two-player icon position would
        // overlap quadrant rank text. It intentionally uses normal centered
        // sprite placement, without borrowing a corner anchor or scissor.
        const float fit = 16.0f / std::max(icon.width, icon.height);
        const float sx = fit * float(width) / 320.0f, sy = fit * float(height) / 240.0f;
        call.r6 = std::bit_cast<unsigned>((view % 2) * (width * .5f) + 36.0f * width / 320.0f);
        call.r7 = std::bit_cast<unsigned>((view / 2) * (height * .5f) + 64.0f * height / 240.0f);
        for (unsigned offset = 0x10; offset <= 0x44; offset += 4)
            engine::write_u32(m, stack + offset, 0);
        engine::write_float(m, stack + 0x1C, sx);
        engine::write_float(m, stack + 0x20, sy);
        for (unsigned offset = 0x24; offset <= 0x3C; offset += 4)
            engine::write_float(m, stack + offset, 1.0f);
        engine::write_u32(m, stack + 0x44, 0x400); // Original weapon sprite flags.
        if (hud_item.owns)
            mk64_items::arm_hud_sprite(m, view, sprite, hud_item);
        func_8001EB90(m, &call);
        mk64_items::commit_hud_sprite(m);
    }
    hud_weapon = previous;
    hud_item = previous_item;
    hud_view = previous_view;
    std::memcpy(m + stack - 0x80000000u - 0x18u, scratch.data(), scratch.size());
}

extern "C" void rr64_course_items_draw(unsigned char *m) {
    using namespace rr64;
    using namespace course_items;
    if (!m || prediction::active() || !experimental_course::active())
        return;
    const auto boxes = definitions();
    if (boxes.size() > netplay::kMaximumCourseItems)
        return;
    std::array<ItemBoxDrawState, netplay::kMaximumCourseItems> draw{};
    const auto hazards = course_hazards::capture_state();
    unsigned count = 0;
    for (unsigned i = 0; i < boxes.size(); ++i) {
        ItemBoxDefinition posed;
        if (!posed_definition(boxes[i], hazards, posed))
            continue;
        const unsigned cooldown = live.boxes.cooldown[i];
        const unsigned age = netplay::kCourseItemCooldown - cooldown;
        if (cooldown && age >= 20)
            continue;
        auto &out = draw[count++];
        out.id = boxes[i].id;
        out.position = posed.position;
        out.source_to_world_scale = experimental_course::source_to_world_scale();
        out.state = cooldown ? 3 : boxes[i].kind;
        out.break_age = cooldown ? float(age) : 0;
        const unsigned normal_ticks = live.boxes.clock - (cooldown ? age : 0);
        out.rotation = {
            static_cast<std::uint16_t>(182u * normal_ticks + (cooldown ? 1092u * age : 0)),
            static_cast<std::uint16_t>(-364u * normal_ticks - (cooldown ? 728u * age : 0)),
            static_cast<std::uint16_t>(182u * normal_ticks + (cooldown ? 364u * age : 0))};
    }
    draw_boxes(m, std::span(draw.data(), count));
}
