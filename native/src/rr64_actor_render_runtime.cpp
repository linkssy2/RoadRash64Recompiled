#include "rr64_prediction_replay.hpp"
#include "rr64_presentation_options.hpp"
#include "rr64_actor_render_snapshot.hpp"
#include "rr64_actor_render_diagnostics.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_actor_held_pose.hpp"
#include "rr64_native.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "recomp.h"

extern "C" {
void func_80012CE0(unsigned char *, recomp_context *);
void func_80012DBC(unsigned char *, recomp_context *);
void func_8005E980(unsigned char *, recomp_context *);
void func_8005EB50(unsigned char *, recomp_context *);
void func_8005AEE0(unsigned char *, recomp_context *);
void func_8005B63C(unsigned char *, recomp_context *);
void func_8005B948(unsigned char *, recomp_context *);
void func_8005BEEC(unsigned char *, recomp_context *);
}

namespace {
using namespace rr64;

using SuffixVectors = std::array<std::array<std::uint32_t, 3>, 3>;
constexpr std::uint32_t suffix_camera = 0x800D69F8u;

std::array<std::uint32_t, 3> suffix_addresses(std::uint32_t bike_entity, std::uint32_t owner) {
    return {suffix_camera, bike_entity + 0x53cu, owner + 0x5dcu};
}

bool read_suffix_vectors(unsigned char *rdram, const std::array<std::uint32_t, 3> &addresses,
                         SuffixVectors &vectors) {
    for (std::size_t vector = 0; vector < addresses.size(); ++vector) {
        for (std::uint32_t word = 0; word < 3u; ++word) {
            if (!engine::read_u32(rdram, addresses[vector] + word * 4u, vectors[vector][word])) {
                return false;
            }
        }
    }
    return true;
}

void write_suffix_vectors(unsigned char *rdram, const std::array<std::uint32_t, 3> &addresses,
                          const SuffixVectors &vectors) {
    for (std::size_t vector = 0; vector < addresses.size(); ++vector) {
        for (std::uint32_t word = 0; word < 3u; ++word) {
            engine::write_u32(rdram, addresses[vector] + word * 4u, vectors[vector][word]);
        }
    }
}

struct PreparationInput {
    recomp_context context{};
    SuffixVectors suffix_vectors{};
    std::uint32_t bike_node = 0;
    std::uint32_t rider_node = 0;
    std::uint32_t stage_mask = 0;
    std::array<lod::RootRenderPlan, 2> root_plans{};
    bool eligible = false;
    lod::PreparationPurpose purpose = lod::PreparationPurpose::LiveDraw;
};

struct RiderRangeProof {
    lod::RootRenderPlan plan{};
    std::uint64_t generation = 0;
    std::uint32_t node = 0, entity = 0, model_state = 0, viewport = 0;
    std::uint16_t original_flags = 0;
};

struct Runtime {
    lod::SnapshotStore store;
    std::array<PreparationInput, lod::maximum_pairs> inputs{};
    std::array<RiderRangeProof, lod::maximum_pairs> rider_ranges{};
    std::size_t input_count = 0;
    unsigned char *live = nullptr;
    std::vector<unsigned char> scratch;
    std::mutex mutex;
};

Runtime &runtime() {
    // Heap allocation avoids a large stack frame in guest threads.
    static auto instance = std::make_unique<Runtime>();
    return *instance;
}

thread_local bool draw_active = false;
thread_local unsigned char *shadow_mapping = nullptr;
thread_local std::uint32_t pending_animation_node = 0;
struct FullWeightAnimation {
    std::uint32_t node = 0, graph = 0;
    std::uint64_t topology_hash = 0;
    bool previous_children = false;
    bool seeded = false;
};
thread_local FullWeightAnimation pending_full_weight;
thread_local lod::PoseBinding pose_binding;
thread_local lod::RootRenderPlan bound_root_plan;
thread_local unsigned char *bound_mapping = nullptr;
thread_local std::uint32_t bound_node = 0;
thread_local bool root_source_selected = false;
thread_local bool visibility_prebound = false;
thread_local std::uint16_t bound_stock_lod = 0;
std::atomic_uint64_t preparation_count{0};
std::atomic_uint64_t published_pair_count{0};
std::atomic_uint64_t consumed_actor_count{0};
std::atomic_uint64_t fallback_actor_count{0};
std::array<std::atomic_uint64_t, static_cast<std::size_t>(lod::ActivityCounter::Count)> activity{};
std::array<std::atomic_uint64_t, static_cast<std::size_t>(lod::FindFailure::Count)> find_failures{};
std::array<std::atomic_uint64_t, 4> view_published{}, view_detailed{}, view_fallback{};
bool view_diagnostics_enabled() { return rr64::diagnostics::detailed_enabled(); }
std::array<std::atomic_uint32_t, 6> last_scene{};
std::array<std::atomic_uint32_t, 2> last_camera_planes{};
struct ActorDetailCounters {
    std::atomic_uint64_t detailed{0}, fallback{0};
    std::array<std::atomic_uint64_t, 3> fallback_by_lod{};
    std::atomic_uint32_t rider_style{0};
    std::atomic_uint32_t node{0}, stock_lod{0}, detail_list{0}, fallback_list{0};
    std::atomic_uint32_t detailed_distance_bits{0}, fallback_distance_bits{0}, last_failure{0};
    std::atomic_uint32_t fallback_state_valid{0}, fallback_attached{0}, fallback_ejected{0};
    std::atomic_uint32_t fallback_contact_phase{0}, fallback_bike_state{0};
    std::atomic_uint32_t fallback_speed_bits{0}, fallback_transition_bits{0};
};
std::array<ActorDetailCounters, lod::maximum_actors> actor_details{};

void end_binding() {
    visibility_prebound = false;
    bound_stock_lod = 0;
    bound_root_plan = {};
    bound_mapping = nullptr;
    bound_node = 0;
    root_source_selected = false;
    pose_binding.end();
}

void count_activity(lod::ActivityCounter counter) {
    activity[static_cast<std::size_t>(counter)].fetch_add(1u, std::memory_order_relaxed);
}

bool observe_scene(unsigned char *rdram) {
    std::uint32_t mode = 0, pending = 0, views = 0, players = 0, setup = 0;
    std::uint16_t compiled = 0;
    engine::read_u32(rdram, engine::globals::main_mode, mode);
    engine::read_u32(rdram, engine::globals::pending_mode, pending);
    engine::read_u32(rdram, 0x8009DB88u, views);
    engine::read_u32(rdram, 0x800A6578u, players);
    engine::read_u32(rdram, engine::globals::active_racer_count, setup);
    engine::read_u16(rdram, 0x800A65C4u, compiled);
    const std::array<std::uint32_t, 6> values{mode, pending, views, players, setup, compiled};
    for (std::size_t i = 0; i < values.size(); ++i) {
        last_scene[i].store(values[i], std::memory_order_relaxed);
    }
    // These world-space camera limits explain the different per-bank packed
    // projection clamps. They are observation only, never selection inputs.
    std::uint32_t near_bits = 0, far_bits = 0;
    engine::read_u32(rdram, 0x8009DBC8u, near_bits);
    engine::read_u32(rdram, 0x8009DBCCu, far_bits);
    last_camera_planes[0].store(near_bits, std::memory_order_relaxed);
    last_camera_planes[1].store(far_bits, std::memory_order_relaxed);
    const bool supported = lod::supported_scene(rdram);
    if (!supported) {
        count_activity(lod::ActivityCounter::SceneRejected);
    }
    return supported;
}

void mapping(Runtime &state, unsigned char *rdram) {
    if (state.live != rdram) {
        state.store.reset(rdram);
        state.input_count = 0;
        state.rider_ranges = {};
        state.live = rdram;
    }
}

bool viewport(unsigned char *rdram, std::uint32_t &view, std::uint32_t &slot) {
    std::uint32_t views = 0;
    return engine::read_u32(rdram, 0x8009DB88u, views) && views >= 1u && views <= 4u &&
           engine::read_u32(rdram, engine::globals::active_viewport, view) &&
           engine::read_u32(rdram, engine::globals::actor_render_buffer_slot, slot) &&
           view < views && slot < 2u;
}

void bind_private_float_registers(recomp_context &context) noexcept {
    // f_odd aliases storage inside its owning context. Rebind after the last
    // copy, in the exact local object passed to generated helpers; copying an
    // already-bound context would retain a pointer into the previous object.
    context.f_odd = context.mips3_float_mode ? &context.f1.u32l : &context.f0.u32h;
}

// Exact data flow of 0x8005E6D8..0x8005E734. All four helpers' transitive
// callees are generated arithmetic/vector functions. The enclosing function's
// later HUD/DMA/scheduler tail at 0x8005E77C is deliberately never replayed.
bool replay_pair_suffix(unsigned char *scratch, const PreparationInput &input) {
    auto ctx = input.context;
    bind_private_float_registers(ctx);
    std::uint32_t graph = 0, source = 0, scale = 0, model_state = 0, id = 0;
    std::uint32_t current_bike = 0, current_owner = 0, linked_owner = 0;
    std::uint16_t index = 0;
    const auto bike_entity = static_cast<std::uint32_t>(ctx.r22);
    const auto owner = static_cast<std::uint32_t>(ctx.r18);
    if (!engine::read_u32(scratch, input.bike_node + 4u, current_bike) ||
        current_bike != bike_entity ||
        !engine::read_u32(scratch, input.rider_node + 4u, current_owner) ||
        current_owner != owner || !engine::read_u32(scratch, input.bike_node + 0x28u, graph) ||
        !engine::read_u32(scratch, graph + 0x14u, source) ||
        !engine::read_u16(scratch, source + 0x12u, index) || index > 2u ||
        !engine::read_u32(scratch, 0x8009DBACu + index * 4u, scale) ||
        !engine::read_u32(scratch, bike_entity + 4u, model_state) ||
        !engine::read_u32(scratch, model_state + 0xe4u, linked_owner) || linked_owner != owner ||
        !engine::read_u32(scratch, model_state, id) || id >= engine::kMaximumRacers ||
        static_cast<std::uint32_t>(ctx.r17) != 0x800D6880u + id * 12u ||
        static_cast<std::uint32_t>(ctx.r16) != 0x800D6940u + id * 12u ||
        static_cast<std::uint32_t>(ctx.r30) != suffix_camera ||
        !engine::valid_guest_range(owner, 0x5e8u) ||
        !engine::valid_guest_range(static_cast<std::uint32_t>(ctx.r29) - 0x400u, 0x400u)) {
        return false;
    }
    const auto addresses = suffix_addresses(bike_entity, owner);
    SuffixVectors late_vectors{};
    if (!read_suffix_vectors(scratch, addresses, late_vectors)) {
        return false;
    }
    struct RestoreSuffixVectors {
        unsigned char *scratch;
        const std::array<std::uint32_t, 3> &addresses;
        const SuffixVectors &vectors;
        ~RestoreSuffixVectors() {
            write_suffix_vectors(scratch, addresses, vectors);
        }
    } restore{scratch, addresses, late_vectors};
    // The original suffix runs before 7B50C rebases D69F8 by the sector
    // origin. Replaying against that later camera would shift every root by
    // the sector offset. Use the exact observed subtraction inputs only for
    // these four helpers, then restore the clone's later inputs for all other
    // visual passes. The same rule also preserves any later anchor changes.
    write_suffix_vectors(scratch, addresses, input.suffix_vectors);
    ctx.r4 = static_cast<std::int32_t>(bike_entity + 0x53cu);
    ctx.r5 = ctx.r30;
    ctx.r6 = ctx.r17;
    ctx.f20.u32l = scale;
    func_80012CE0(scratch, &ctx);
    ctx.r4 = static_cast<std::int32_t>(owner + 0x5dcu);
    ctx.r5 = ctx.r30;
    ctx.r6 = ctx.r16;
    func_80012CE0(scratch, &ctx);
    ctx.r4 = ctx.r17;
    ctx.r5 = ctx.r4;
    ctx.r6 = static_cast<std::int32_t>(ctx.f20.u32l);
    func_80012DBC(scratch, &ctx);
    ctx.r4 = ctx.r16;
    ctx.r5 = ctx.r4;
    ctx.r6 = 0x42c80000u;
    func_80012DBC(scratch, &ctx);
    return true;
}

bool promote_private_node(unsigned char *scratch, std::uint32_t node, bool prepare_hidden_peer) {
    std::uint32_t graph = 0, stock_graph = 0;
    std::uint16_t stock_lod = 0, stock_flags = 0, detailed_flags = 0;
    if (!engine::read_u32(scratch, node + engine::actor_scene::lod_models, graph) ||
        !engine::read_u16(scratch, node + engine::actor_scene::selected_lod, stock_lod) ||
        !engine::read_u32(scratch, node + engine::actor_scene::current_model, stock_graph) ||
        !engine::read_u16(scratch, stock_graph + 0xau, stock_flags) ||
        !engine::read_u16(scratch, graph + 0xau, detailed_flags)) {
        return false;
    }
    // Force a new detailed visual preparation in the clone. In particular the
    // original AEE0 1/2 and 1/4 cadence shortcuts require selected==previous;
    // a nonzero previous tier prevents stale children from passing by chance.
    // A far visible bike can have a hidden lower-range rider peer. Prepare
    // both private poses so the pair can be certified atomically; the live
    // renderer still checks each stock graph's visibility before selection.
    return engine::write_u16(scratch, graph + 0xau,
                             (detailed_flags & ~1u) |
                                 (prepare_hidden_peer ? 0u : (stock_flags & 1u))) &&
           engine::write_u32(scratch, node + engine::actor_scene::current_model, graph) &&
           engine::write_u16(scratch, node + engine::actor_scene::selected_lod, 0u) &&
           engine::write_u16(scratch, node + engine::actor_scene::previous_lod,
                             stock_lod == 0u ? 1u : stock_lod);
}

void record_actor_detail(unsigned char *rdram, std::uint32_t node, std::uint32_t stock_lod,
                         bool detailed, lod::FindFailure reason = lod::FindFailure::None) {
    // Fixed per-racer counters, reported only by the existing ten-second log.
    // The shared selector also sees scenery/traffic; never attribute those
    // missing snapshots to an NPC bike or rider.
    std::uint32_t type = 0, entity = 0, model_state = 0, id = 0, view = 0, distance = 0, list = 0;
    if (!engine::read_u32(rdram, node, type) || (type != 1u && type != 2u) || stock_lod >= 3u ||
        !engine::read_u32(rdram, node + 4u, entity) ||
        !engine::read_u32(rdram, entity + 4u, model_state) ||
        !engine::read_u32(rdram, model_state, id) || id >= engine::kMaximumRacers ||
        !engine::read_u32(rdram, engine::globals::active_viewport, view) || view >= 4u ||
        !engine::read_u32(rdram, node + 8u + view * 4u, distance) ||
        !std::isfinite(std::bit_cast<float>(distance)) || std::bit_cast<float>(distance) < 0.0f ||
        !engine::read_u32(
            rdram, node + engine::actor_scene::display_lists + (detailed ? 0u : stock_lod * 4u),
            list)) {
        return;
    }
    if (view_diagnostics_enabled()) {
        (detailed ? view_detailed[view] : view_fallback[view])
            .fetch_add(1u, std::memory_order_relaxed);
    }
    auto &counters = actor_details[id * 2u + (type - 1u)];
    counters.node.store(node, std::memory_order_relaxed);
    std::uint32_t owner = type == 2u ? entity : 0u, style = 0;
    if ((type == 2u || engine::read_u32(rdram, entity + engine::bike::rider_pointer, owner)) &&
        engine::valid_guest_range(owner, engine::rider::stride) &&
        engine::read_u32(rdram, owner + 0xcu, style))
        counters.rider_style.store(style, std::memory_order_relaxed);
    if (detailed) {
        counters.detailed_distance_bits.store(distance, std::memory_order_relaxed);
        counters.detail_list.store(list, std::memory_order_relaxed);
        counters.detailed.fetch_add(1u, std::memory_order_relaxed);
    } else {
        // Preserve brief lower-tier selections even if the last sample in
        // this logging interval returns to stock tier zero. Fixed counters
        // only: no per-frame log lines, hooks or growing event buffers.
        counters.fallback_by_lod[stock_lod].fetch_add(1u, std::memory_order_relaxed);
        counters.stock_lod.store(stock_lod, std::memory_order_relaxed);
        counters.fallback_distance_bits.store(distance, std::memory_order_relaxed);
        counters.fallback_list.store(list, std::memory_order_relaxed);
        counters.last_failure.store(static_cast<std::uint32_t>(reason), std::memory_order_relaxed);
        std::uint32_t bike = type == 1u ? entity : 0u, rider = type == 2u ? entity : 0u;
        std::uint32_t speed = 0, transition = 0, bike_state = 0;
        std::uint16_t attached = 0, ejected = 0, contact = 0;
        const bool state_valid =
            (type == 1u ? engine::read_u32(rdram, bike + engine::bike::rider_pointer, rider)
                        : engine::read_u32(rdram, rider + engine::rider::bike_pointer, bike)) &&
            engine::valid_guest_range(bike, engine::bike::stride) &&
            engine::valid_guest_range(rider, engine::rider::stride) &&
            engine::read_u16(rdram, bike + engine::bike::rider_attached, attached) &&
            engine::read_u16(rdram, rider + engine::rider::ejected, ejected) &&
            engine::read_u16(rdram, bike + 0x818u, contact) &&
            engine::read_u32(rdram, bike + 0x100u, bike_state) &&
            engine::read_u32(rdram, bike + 0x184u, speed) &&
            engine::read_u32(rdram, bike + 0x4d4u, transition) &&
            std::isfinite(std::bit_cast<float>(speed)) &&
            std::isfinite(std::bit_cast<float>(transition));
        counters.fallback_state_valid.store(state_valid, std::memory_order_relaxed);
        if (state_valid) {
            counters.fallback_attached.store(attached, std::memory_order_relaxed);
            counters.fallback_ejected.store(ejected, std::memory_order_relaxed);
            counters.fallback_contact_phase.store(contact, std::memory_order_relaxed);
            counters.fallback_bike_state.store(bike_state, std::memory_order_relaxed);
            counters.fallback_speed_bits.store(speed, std::memory_order_relaxed);
            counters.fallback_transition_bits.store(transition, std::memory_order_relaxed);
        }
        counters.fallback.fetch_add(1u, std::memory_order_relaxed);
    }
}

bool bind_actor(unsigned char *rdram, const lod::ActorSnapshot &actor, std::uint32_t slot) {
    if (!pose_binding.begin(rdram, actor)) {
        count_activity(lod::ActivityCounter::BindingRejected);
        fallback_actor_count.fetch_add(1u, std::memory_order_relaxed);
        record_actor_detail(rdram, actor.node, actor.stock_lod, false, lod::FindFailure::Isolation);
        return false;
    }
    bound_root_plan = actor.root_plan;
    bound_mapping = rdram;
    bound_node = actor.node;
    bound_stock_lod = actor.stock_lod;
    if (actor.root_plan.normalized) {
        count_activity(actor.type == 1u ? lod::ActivityCounter::BikeFarNormalized
                                        : lod::ActivityCounter::RiderFarNormalized);
    }
    consumed_actor_count.fetch_add(1u, std::memory_order_relaxed);
    record_actor_detail(rdram, actor.node, actor.stock_lod, true);
    count_activity(slot == 0u ? lod::ActivityCounter::ConsumedSlot0
                              : lod::ActivityCounter::ConsumedSlot1);
    if (actor.stock_lod == 1u) {
        count_activity(actor.type == 1u ? lod::ActivityCounter::BikeTier1ToMax
                                        : lod::ActivityCounter::RiderTier1ToMax);
    } else if (actor.stock_lod == 2u) {
        count_activity(actor.type == 1u ? lod::ActivityCounter::BikeTier2ToMax
                                        : lod::ActivityCounter::RiderTier2ToMax);
    }
    return true;
}

void count_far_fallback(unsigned char *rdram, std::uint32_t node, std::uint32_t view) {
    std::uint32_t type = 0;
    float distance_squared = 0.0f;
    if (view < 4u && engine::read_u32(rdram, node, type) && (type == 1u || type == 2u) &&
        engine::read_float(rdram, node + 8u + view * 4u, distance_squared) &&
        std::isfinite(distance_squared) && distance_squared >= 26843.5f) {
        count_activity(type == 1u ? lod::ActivityCounter::FarBikeFallback
                                  : lod::ActivityCounter::FarRiderFallback);
    }
}
} // namespace

rr64::lod::ActivitySnapshot rr64::lod::read_activity() noexcept {
    ActivitySnapshot result;
    for (unsigned view = 0; view < 4; ++view) {
        result.view_published[view] = view_published[view].load(std::memory_order_relaxed);
        result.view_detailed[view] = view_detailed[view].load(std::memory_order_relaxed);
        result.view_fallback[view] = view_fallback[view].load(std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < result.counts.size(); ++i) {
        result.counts[i] = activity[i].load(std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < result.find_failures.size(); ++i) {
        result.find_failures[i] = find_failures[i].load(std::memory_order_relaxed);
    }
    result.mode = last_scene[0].load(std::memory_order_relaxed);
    result.pending = last_scene[1].load(std::memory_order_relaxed);
    result.view_count = last_scene[2].load(std::memory_order_relaxed);
    result.race_players = last_scene[3].load(std::memory_order_relaxed);
    result.setup_players = last_scene[4].load(std::memory_order_relaxed);
    result.compiled_renderer = last_scene[5].load(std::memory_order_relaxed);
    result.camera_near =
        std::bit_cast<float>(last_camera_planes[0].load(std::memory_order_relaxed));
    result.camera_far = std::bit_cast<float>(last_camera_planes[1].load(std::memory_order_relaxed));
    for (std::size_t index = 0; index < actor_details.size(); ++index) {
        const auto &counters = actor_details[index];
        auto &sample = result.actor_details[index];
        sample.detailed = counters.detailed.load(std::memory_order_relaxed);
        sample.fallback = counters.fallback.load(std::memory_order_relaxed);
        for (std::size_t tier = 0; tier < 3u; ++tier)
            sample.fallback_by_lod[tier] =
                counters.fallback_by_lod[tier].load(std::memory_order_relaxed);
        sample.rider_style = counters.rider_style.load(std::memory_order_relaxed);
        sample.node = counters.node.load(std::memory_order_relaxed);
        sample.stock_lod = counters.stock_lod.load(std::memory_order_relaxed);
        sample.detail_list = counters.detail_list.load(std::memory_order_relaxed);
        sample.fallback_list = counters.fallback_list.load(std::memory_order_relaxed);
        sample.detailed_distance = std::sqrt(
            std::bit_cast<float>(counters.detailed_distance_bits.load(std::memory_order_relaxed)));
        sample.fallback_distance = std::sqrt(
            std::bit_cast<float>(counters.fallback_distance_bits.load(std::memory_order_relaxed)));
        sample.last_failure =
            static_cast<lod::FindFailure>(counters.last_failure.load(std::memory_order_relaxed));
        sample.fallback_state_valid = counters.fallback_state_valid.load(std::memory_order_relaxed);
        sample.fallback_attached = counters.fallback_attached.load(std::memory_order_relaxed);
        sample.fallback_ejected = counters.fallback_ejected.load(std::memory_order_relaxed);
        sample.fallback_contact_phase =
            counters.fallback_contact_phase.load(std::memory_order_relaxed);
        sample.fallback_bike_state = counters.fallback_bike_state.load(std::memory_order_relaxed);
        sample.fallback_speed =
            std::bit_cast<float>(counters.fallback_speed_bits.load(std::memory_order_relaxed));
        sample.fallback_transition =
            std::bit_cast<float>(counters.fallback_transition_bits.load(std::memory_order_relaxed));
    }
    return result;
}

extern "C" int rr64_render_only_max_lod_enabled() {
    static const bool fallback =
        rr64::presentation_options::environment_enabled("RR64_RENDER_ONLY_MAX_LOD");
    return rr64::presentation_options::enabled(rr64::presentation_options::max_lod, fallback);
}
extern "C" void rr64_lod_read_stats(unsigned long long *preparations,
                                    unsigned long long *published_pairs,
                                    unsigned long long *consumed_actors,
                                    unsigned long long *fallback_actors) {
    if (preparations) {
        *preparations = preparation_count.load(std::memory_order_relaxed);
    }
    if (published_pairs) {
        *published_pairs = published_pair_count.load(std::memory_order_relaxed);
    }
    if (consumed_actors) {
        *consumed_actors = consumed_actor_count.load(std::memory_order_relaxed);
    }
    if (fallback_actors) {
        *fallback_actors = fallback_actor_count.load(std::memory_order_relaxed);
    }
}

extern "C" void rr64_lod_observe_allocation(unsigned char *rdram, unsigned int node,
                                            unsigned int view, unsigned int bytes) {
    // Replay allocations belong to a disposable image. Registering that image
    // here resets the live Max LOD allocation store and loses distant poses.
    if (rr64::prediction::active()) return;
    // Keep bounded allocation certificates even when live Max LOD is off:
    // recording starts after these native allocations have already happened.
    if (shadow_mapping) {
        return;
    }
    count_activity(lod::ActivityCounter::Allocation);
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    mapping(state, rdram);
    if (state.store.observe_allocation(rdram, node, view, bytes)) {
        count_activity(lod::ActivityCounter::AllocationAccepted);
    }
}

extern "C" void rr64_lod_invalidate(unsigned char *rdram) {
    if (rr64::prediction::active()) return;
    end_binding();
    if (shadow_mapping) {
        return;
    }
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    mapping(state, rdram);
    state.store.invalidate();
    state.input_count = 0;
    state.rider_ranges = {};
}

extern "C" void rr64_lod_release_node(unsigned char *rdram, unsigned int node) {
    if(rr64::prediction::active())return;
    if (shadow_mapping)
        return;
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    mapping(state, rdram);
    if (state.store.forget_allocation(rdram, node)) {
        end_binding();
        state.input_count = 0;
        state.rider_ranges = {};
    }
}

extern "C" void rr64_lod_reset_actor_pool(unsigned char *rdram) {
    if (rr64::prediction::active()) return;
    if (shadow_mapping)
        return;
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    end_binding();
    mapping(state, rdram);
    state.store.reset(rdram);
    state.input_count = 0;
    state.rider_ranges = {};
}

extern "C" void rr64_lod_begin_preparation(unsigned char *rdram) {
    if (rr64::prediction::active()) return;
    rr64_lod_invalidate(rdram);
    if ((!rr64_render_only_max_lod_enabled() && !rr64_highlights_needs_detail(rdram)) || shadow_mapping) {
        return;
    }
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    state.store.begin_pose_epoch(rdram);
}

extern "C" void rr64_lod_observe_rider_range(unsigned char *rdram, unsigned int node,
                                             int in_range) {
    // Exact live EC28 branch provenance: the rider entered visible and active.
    // The shared ED2C hide target also handles inactivity and cannot certify it.
    if (!rr64_render_only_max_lod_enabled() || shadow_mapping || !lod::supported_scene(rdram)) {
        return;
    }
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    mapping(state, rdram);
    std::uint32_t type = 0, entity = 0, model_state = 0, id = 0, view = 0, slot = 0;
    if (!viewport(rdram, view, slot) || !engine::read_u32(rdram, node, type) || type != 2u ||
        !engine::read_u32(rdram, node + 4u, entity) || !engine::valid_guest_range(entity, 0x5e8u) ||
        !engine::read_u32(rdram, entity + 4u, model_state) ||
        !engine::read_u32(rdram, model_state, id) || id >= state.rider_ranges.size()) {
        return;
    }
    auto &proof = state.rider_ranges[id];
    proof = {};
    if (in_range) {
        return;
    }
    lod::RootRenderPlan plan{};
    std::uint16_t active = 0, flags = 0;
    if (!lod::capture_root_render_plan(rdram, node, view, plan) || !plan.normalized ||
        plan.original_source != 1u || plan.stock_source_index != 1u ||
        !engine::read_u16(rdram, 0x800D8570u + id * 0x118u + 0x24u, active) || active == 0u ||
        !engine::read_u16(rdram, plan.stock_model + 0xau, flags) || (flags & 1u) != 0u ||
        10000.0f * std::bit_cast<float>(plan.distance_squared_bits) < 268435000.0f) {
        return;
    }
    proof = {plan, state.store.generation(), node, entity, model_state, view, flags};
    count_activity(lod::ActivityCounter::RiderRangeObserved);
}

extern "C" void rr64_lod_observe_pair(unsigned char *rdram, void *context) {
    if ((!rr64_render_only_max_lod_enabled() && !rr64_highlights_needs_detail(rdram)) || shadow_mapping || !context) {
        return;
    }
    count_activity(lod::ActivityCounter::PairObserved);
    if (!observe_scene(rdram)) {
        return;
    }
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    mapping(state, rdram);
    auto &ctx = *static_cast<recomp_context *>(context);
    const auto bike = static_cast<std::uint32_t>(ctx.r20);
    const auto rider = static_cast<std::uint32_t>(ctx.r19);
    std::uint32_t view = 0, slot = 0;
    if (!viewport(rdram, view, slot))
        return;
    const auto purpose = view == 0 && rr64_highlights_needs_detail(rdram)
                             ? lod::PreparationPurpose::HighlightRecording
                             : lod::PreparationPurpose::LiveDraw;
    if (!state.store.can_prepare(rdram, bike, rider, view, slot, purpose) ||
        state.input_count == state.inputs.size()) {
        count_activity(lod::ActivityCounter::PreflightRejected);
        return;
    }
    for (std::size_t i = 0; i < state.input_count; ++i) {
        if (state.inputs[i].bike_node == bike || state.inputs[i].rider_node == rider) {
            return;
        }
    }
    PreparationInput input{};
    input.context = ctx;
    // This is a movable register image, never an executable context. The
    // replay creates and binds its own local alias before calling helpers.
    input.context.f_odd = nullptr;
    input.bike_node = bike;
    input.rider_node = rider;
    input.purpose = purpose;
    if (!read_suffix_vectors(rdram,
                             suffix_addresses(static_cast<std::uint32_t>(ctx.r22),
                                              static_cast<std::uint32_t>(ctx.r18)),
                             input.suffix_vectors)) {
        count_activity(lod::ActivityCounter::PreflightRejected);
        return;
    }
    state.inputs[state.input_count++] = input;
    count_activity(lod::ActivityCounter::PairQueued);
}

extern "C" void rr64_lod_observe_recording_pairs(unsigned char *rdram, void *context) {
    // D9A4 skips hidden bikes before its ordinary pair observer. Enroll active
    // recording subjects before that branch, while the camera is still in the
    // original world basis. Only viewport zero adds off-camera recording work.
    if (!context || shadow_mapping || !rr64_highlights_needs_detail(rdram))
        return;
    std::uint32_t view = 0, slot = 0, count = 0;
    if (!viewport(rdram, view, slot) || view != 0 ||
        !engine::read_u32(rdram, 0x800A656Cu, count) || // Native race roster, not the menu player count.
        count == 0 || count > engine::kMaximumRacers)
        return;
    for (unsigned i = 0; i < count; ++i) {
        const auto actor = 0x800D8570u + i * 0x118u;
        std::uint16_t active = 0;
        std::uint32_t bike = 0, rider = 0, bike_node = 0, rider_node = 0, owner = 0, id = 0;
        if (!engine::read_u16(rdram, actor + 0x24u, active) || !active ||
            !engine::read_u32(rdram, actor + 0xe0u, bike) ||
            !engine::read_u32(rdram, actor + 0xe4u, rider) ||
            !engine::read_u32(rdram, bike + 8u, bike_node) ||
            !engine::read_u32(rdram, rider + 8u, rider_node) ||
            !engine::read_u32(rdram, bike + 4u, owner) ||
            !engine::read_u32(rdram, owner, id) || id != i)
            continue;
        auto input = *static_cast<const recomp_context *>(context);
        input.r20 = static_cast<std::int32_t>(bike_node);
        input.r19 = static_cast<std::int32_t>(rider_node);
        input.r22 = static_cast<std::int32_t>(bike);
        input.r18 = static_cast<std::int32_t>(rider);
        input.r17 = static_cast<std::int32_t>(0x800D6880u + id * 12u);
        input.r16 = static_cast<std::int32_t>(0x800D6940u + id * 12u);
        input.r30 = static_cast<std::int32_t>(suffix_camera);
        rr64_lod_observe_pair(rdram, &input);
    }
}

extern "C" int rr64_lod_shadow_rider(unsigned char *rdram, unsigned int node) {
    if (!shadow_mapping || rdram != shadow_mapping) {
        return 0;
    }
    auto &state = runtime();
    for (std::size_t i = 0; i < state.input_count; ++i) {
        if (state.inputs[i].eligible && state.inputs[i].rider_node == node) {
            return 1;
        }
    }
    return 0;
}

extern "C" void rr64_lod_shadow_stage(unsigned char *rdram, unsigned int node, unsigned int stage) {
    if (!shadow_mapping || rdram != shadow_mapping) {
        return;
    }
    // B5EC is also a stock skip target. Only the path which entered the final
    // 18114 call at B5E4 may certify its return here; a skip cannot reuse a
    // previous node's completion. Other call sites mark their direct returns.
    if (stage == 8u) {
        pending_animation_node = rr64_lod_shadow_rider(rdram, node) ? node : 0u;
        return;
    }
    if (stage == 16u) {
        const bool completed = pending_animation_node != 0u && pending_animation_node == node;
        pending_animation_node = 0;
        if (!completed) {
            return;
        }
        stage = 4u;
    }
    auto &state = runtime();
    for (std::size_t i = 0; i < state.input_count; ++i) {
        auto &input = state.inputs[i];
        if (input.eligible && (((stage == 1u || stage == 128u) && input.bike_node == node) ||
                               ((stage == 2u || stage == 4u) && input.rider_node == node))) {
            input.stage_mask |= stage;
        }
    }
}

extern "C" void rr64_lod_shadow_finish_hold(unsigned char *rdram, void *context) {
    if (!shadow_mapping || rdram != shadow_mapping || !context)
        return;
    const auto &ctx = *static_cast<const recomp_context *>(context);
    // Called only on the original B290 taken branch: this finish animation
    // intentionally holds all rider children while speed is at least ten.
    if (!std::isfinite(ctx.f2.fl) || ctx.f0.fl != 10.0f || ctx.f2.fl < 10.0f)
        return;
    const auto node = static_cast<std::uint32_t>(ctx.r19);
    auto &state = runtime();
    std::uint32_t view = 0, slot = 0;
    if (!viewport(state.live, view, slot))
        return;
    for (std::size_t i = 0; i < state.input_count; ++i) {
        auto &input = state.inputs[i];
        if (input.eligible && input.rider_node == node && (input.stage_mask & 3u) == 3u &&
            !(input.stage_mask & 4u) &&
            state.store.seed_previous_rider_children(state.live, rdram, node, view, slot, false)) {
            input.stage_mask |= 4u;
            count_activity(lod::ActivityCounter::FinishBlendPrepared);
        }
    }
}

extern "C" void rr64_lod_shadow_full_weight(unsigned char *rdram, void *context, int completed) {
    if (!shadow_mapping || rdram != shadow_mapping || !context) {
        return;
    }
    const auto &ctx = *static_cast<const recomp_context *>(context);
    const auto node = static_cast<std::uint32_t>(ctx.r19);
    const auto pending = pending_full_weight;
    pending_full_weight = {};
    if (!rr64_lod_shadow_rider(rdram, node)) {
        return;
    }
    std::uint32_t graph = 0;
    if (!engine::read_u32(rdram, node + engine::actor_scene::current_model, graph)) {
        return;
    }
    engine::ModelGraphTopologySnapshot topology{};
    if (!engine::capture_model_graph_topology(rdram, graph, topology)) {
        return;
    }
    if (completed) {
        if (pending.node == node && pending.graph == graph &&
            pending.topology_hash == topology.topology_hash) {
            rr64_lod_shadow_stage(rdram, node, 4u);
            count_activity(pending.previous_children
                               ? lod::ActivityCounter::FinishBlendPrepared
                               : lod::ActivityCounter::FullWeightRiderPrepared);
            if (pending.seeded)
                count_activity(lod::ActivityCounter::FinishSeedPrepared);
        }
        return;
    }
    std::uint32_t weight = 0, offset = 0;
    const auto stack = static_cast<std::uint32_t>(ctx.r29);
    const auto phase = std::bit_cast<float>(static_cast<std::uint32_t>(ctx.r7));
    if (static_cast<std::uint32_t>(ctx.r4) != graph || ctx.r6 != 0x100u || !std::isfinite(phase) ||
        phase < 0.0f || !engine::read_u32(rdram, stack + 0x14u, weight) ||
        !engine::read_u32(rdram, stack + 0x10u, offset) || offset != 0u) {
        return;
    }
    const bool full_weight = weight == 0x3f800000u;
    auto &state = runtime();
    if (!full_weight) {
        // The original low-speed transition has no preceding base writer:
        // B2F4/B4F8 enter B480 with phase=1-speed*.1, weight=1-speed*.05.
        // It needs the last detailed child pose, then the real current blend.
        // Never treat a generic partial/masked animation as a complete pose.
        const PreparationInput *input = nullptr;
        for (std::size_t i = 0; i < state.input_count; ++i) {
            if (state.inputs[i].eligible && state.inputs[i].rider_node == node) {
                input = &state.inputs[i];
                break;
            }
        }
        std::uint32_t bike = 0, bike_state = 0, phase_factor = 0, weight_factor = 0;
        std::uint16_t attached = 0, special = 0;
        float speed = 0.0f, transition = 0.0f;
        if (!input || (input->stage_mask & 4u) ||
            !engine::read_u32(rdram, input->bike_node + 4u, bike) ||
            !engine::read_u32(rdram, bike + 0x100u, bike_state) || bike_state != 2u ||
            !engine::read_u16(rdram, bike + engine::bike::rider_attached, attached) ||
            attached == 0u || !engine::read_u16(rdram, bike + 0x818u, special) || special != 0u ||
            !engine::read_float(rdram, bike + 0x4d4u, transition) || transition != 0.0f ||
            !engine::read_float(rdram, bike + 0x184u, speed) || !(speed > 1.5f && speed < 10.0f) ||
            !engine::read_u32(rdram, 0x80005d78u, phase_factor) || phase_factor != 0x3dcccccdu ||
            !engine::read_u32(rdram, 0x80005d7cu, weight_factor) || weight_factor != 0x3d4ccccdu ||
            phase != 1.0f - speed * 0.1f ||
            weight != std::bit_cast<std::uint32_t>(1.0f - speed * 0.05f)) {
            return;
        }
    }

    // 18BD8 uses +2 animation links, while the renderer's certified topology
    // uses +8 links. A full-weight call is a fresh base only if it reaches
    // every rendered child exactly once and leaves the separately prepared
    // root alone. Mask flags 1/2 must stay clear (the exact 0x100 above).
    std::array<bool, engine::actor_scene::maximum_model_records> visited{};
    std::uint32_t record = graph;
    for (std::size_t count = 0; record != 0u; ++count) {
        if (count >= topology.record_count) {
            return;
        }
        std::size_t index = 0;
        while (index < topology.record_count && topology.records[index].address != record) {
            ++index;
        }
        if (index == topology.record_count || visited[index]) {
            return;
        }
        visited[index] = true;
        const auto type = topology.records[index].type;
        if (type == engine::actor_scene::model_record_triple_transform && record != graph) {
            return;
        }
        if (type == engine::actor_scene::model_record_single_transform) {
            std::uint32_t pose = 0;
            if (!engine::read_u32(rdram, record + 0xcu, pose) ||
                !engine::valid_guest_range(pose, 28u)) {
                return;
            }
            float norm = 0.0f;
            for (std::uint32_t word = 0; word < 7u; ++word) {
                float value = 0.0f;
                if (!engine::read_float(rdram, pose + word * 4u, value) || !std::isfinite(value)) {
                    return;
                }
                if (word >= 3u) {
                    norm += value * value;
                }
            }
            if (!(norm > 0.0f && norm <= 16.0f)) {
                return;
            }
        }
        std::uint16_t delta = 0;
        if (!engine::read_u16(rdram, record + 2u, delta)) {
            return;
        }
        if (delta == 0u) {
            record = 0u;
        } else {
            const auto next = std::int64_t(record) + std::int16_t(delta) * 8;
            if (next < engine::kRdramBegin || next > engine::kRdramEnd - 0x18u) {
                return;
            }
            record = static_cast<std::uint32_t>(next);
        }
    }
    for (std::size_t i = 0; i < topology.record_count; ++i) {
        if (topology.records[i].type == engine::actor_scene::model_record_single_transform &&
            !visited[i]) {
            return;
        }
    }
    bool seeded = false;
    if (!full_weight) {
        std::uint32_t view = 0, slot = 0;
        bool missing_history = false;
        if (!viewport(state.live, view, slot))
            return;
        if (!state.store.seed_previous_rider_children(state.live, rdram, node, view, slot, false,
                                                      &missing_history)) {
            if (!missing_history)
                return;
            // Recover only this certified low-speed whole-body animation.
            // Its original full-weight endpoint initializes current children
            // in the private mapping without reusing a stale pose or root.
            // Resource, ownership and chain failures above still refuse.
            engine::write_u32(rdram, stack + 0x14u, 0x3f800000u);
            seeded = true;
        }
    }
    pending_full_weight = {node, graph, topology.topology_hash, !full_weight, seeded};
}

extern "C" void rr64_lod_prepare_shadow(unsigned char *rdram, void *context, int direct_order) {
    if ((!rr64_render_only_max_lod_enabled() && !rr64_highlights_needs_detail(rdram)) || shadow_mapping || !context) {
        return;
    }
    count_activity(lod::ActivityCounter::Prepare);
    if (!observe_scene(rdram)) {
        return;
    }
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    mapping(state, rdram);
    std::uint32_t view = 0, slot = 0;
    if (!viewport(rdram, view, slot) || state.input_count == 0u) {
        count_activity(lod::ActivityCounter::EmptyPrepare);
        return;
    }
    auto visual_context = *static_cast<recomp_context *>(context);
    bind_private_float_registers(visual_context);
    if (!engine::valid_guest_range(static_cast<std::uint32_t>(visual_context.r29) - 0x1000u,
                                   0x1000u)) {
        return;
    }
    state.scratch.resize(engine::kRdramSize);
    std::memcpy(state.scratch.data(), rdram, state.scratch.size());
    auto *scratch = state.scratch.data();
    struct HeldResultClock {
        unsigned char *memory;
        std::uint32_t elapsed = 0;
        std::uint16_t pause = 0;
        bool active = false;
        void restore() {
            if (!active) {
                return;
            }
            engine::write_u16(memory, engine::globals::gameplay_pause_state, pause);
            engine::write_u32(memory, 0x8009cba8u, elapsed);
            active = false;
        }
        ~HeldResultClock() {
            restore();
        }
    } held_clock{scratch};
    std::uint32_t mode = 0, pending = 0;
    std::uint16_t pause_menu = 0;
    if (engine::read_u32(scratch, engine::globals::main_mode, mode) &&
        engine::read_u32(scratch, engine::globals::pending_mode, pending) &&
        lod::supported_scene(scratch) &&
        engine::read_u16(scratch, engine::globals::pause_menu_state, pause_menu) &&
        engine::read_u16(scratch, engine::globals::gameplay_pause_state, held_clock.pause) &&
        held_clock.pause != 0u && engine::read_u32(scratch, 0x8009cba8u, held_clock.elapsed) &&
        std::isfinite(std::bit_cast<float>(held_clock.elapsed))) {
        // Rebuild visual roots in the clone without a simulation time step.
        // Previously certified child poses are frozen below, preventing repeated
        // partial blends while the real pause and menu state remain untouched.
        held_clock.active = true;
        engine::write_u16(scratch, engine::globals::gameplay_pause_state, 0u);
        engine::write_u32(scratch, 0x8009cba8u, 0u);
    }
    std::size_t eligible = 0;
    for (std::size_t i = 0; i < state.input_count; ++i) {
        auto &input = state.inputs[i];
        input.stage_mask = 0;
        input.eligible =
            state.store.can_prepare(rdram, input.bike_node, input.rider_node, view, slot, input.purpose) &&
            lod::capture_root_render_plan(rdram, input.bike_node, view, input.root_plans[0], input.purpose) &&
            lod::capture_root_render_plan(rdram, input.rider_node, view, input.root_plans[1], input.purpose);
        const bool far_pair =
            input.eligible && (input.root_plans[0].normalized || input.root_plans[1].normalized);
        input.eligible = input.eligible &&
                         promote_private_node(scratch, input.bike_node, far_pair) &&
                         promote_private_node(scratch, input.rider_node, far_pair) &&
                         replay_pair_suffix(scratch, input);
        eligible += input.eligible ? 1u : 0u;
        if (!input.eligible) {
            count_activity(lod::ActivityCounter::PreflightRejected);
        }
    }
    if (!eligible) {
        return;
    }
    if (held_clock.active) {
        count_activity(lod::ActivityCounter::HeldResultsPrepared);
    }
    preparation_count.fetch_add(1u, std::memory_order_relaxed);
    pending_animation_node = 0;
    pending_full_weight = {};
    shadow_mapping = scratch;
    // These root passes are essential: D9A4's suffix prepares camera-space
    // vectors, not actor-owned roots. E980/EB50 copy each actor's own original
    // quaternion and matching vector into its selected graph's root.
    {
        // Retain the original range gate, evaluated in the certified far
        // coordinate units. Only these cloned distance caches are temporary;
        // the root writers still copy the exact source-1 position/quaternion.
        struct RestoreRootDistances {
            Runtime &state;
            unsigned char *scratch;
            ~RestoreRootDistances() {
                for (std::size_t i = 0; i < state.input_count; ++i) {
                    if (!state.inputs[i].eligible) {
                        continue;
                    }
                    for (const auto &plan : state.inputs[i].root_plans) {
                        if (plan.normalized) {
                            engine::write_u32(scratch, plan.distance_address,
                                              plan.distance_squared_bits);
                        }
                    }
                }
            }
        } restore{state, scratch};
        for (std::size_t i = 0; i < state.input_count; ++i) {
            if (!state.inputs[i].eligible) {
                continue;
            }
            for (const auto &plan : state.inputs[i].root_plans) {
                if (plan.normalized) {
                    engine::write_u32(scratch, plan.distance_address,
                                      plan.private_distance_squared_bits);
                }
            }
        }
        func_8005E980(scratch, &visual_context);
        func_8005EB50(scratch, &visual_context);
    }
    func_8005AEE0(scratch, &visual_context);
    if (direct_order) {
        func_8005B948(scratch, &visual_context);
        func_8005B63C(scratch, &visual_context);
    } else {
        func_8005B63C(scratch, &visual_context);
        func_8005B948(scratch, &visual_context);
    }
    for (std::size_t i = 0; i < state.input_count; ++i) {
        auto &input = state.inputs[i];
        if (input.eligible && (input.stage_mask & (1u | 2u | 4u)) == (1u | 2u | 4u) &&
            !(input.stage_mask & 128u) &&
            lod::prepare_held_bike_pose(scratch, input.bike_node, &visual_context)) {
            input.stage_mask |= 128u;
            count_activity(lod::ActivityCounter::HeldBikePrepared);
        }
    }
    func_8005BEEC(scratch, &visual_context);
    if (held_clock.active) {
        for (std::size_t i = 0; i < state.input_count; ++i) {
            auto &input = state.inputs[i];
            // Frozen children must not accumulate another weighted animation
            // blend on each pause frame. Roots remain freshly camera-relative.
            if (input.eligible && (input.stage_mask & 3u) == 3u &&
                state.store.seed_previous_rider_children(rdram, scratch, input.rider_node, view,
                                                         slot, true)) {
                input.stage_mask |= 4u | 128u;
            }
        }
    }
    held_clock.restore();
    pending_animation_node = 0;
    pending_full_weight = {};
    shadow_mapping = nullptr;
    for (std::size_t i = 0; i < state.input_count; ++i) {
        const auto &input = state.inputs[i];
        if (input.eligible && input.stage_mask == (1u | 2u | 4u | 128u)) {
            if (state.store.publish(rdram, scratch, input.bike_node, input.rider_node, view,
                                    slot, input.purpose)) {
                // Copy this complete authored tier-zero pose before scratch is
                // reused. The recorder tags it with the current update epoch;
                // no live graph, model preference or simulation word changes.
                rr64_highlights_capture_detail(rdram, scratch, input.bike_node, input.rider_node);
                published_pair_count.fetch_add(1u, std::memory_order_relaxed);
                if (view_diagnostics_enabled())
                    view_published[view].fetch_add(1u, std::memory_order_relaxed);
            } else {
                count_activity(lod::ActivityCounter::PublishRejected);
            }
        } else if (input.eligible) {
            count_activity(lod::ActivityCounter::StageRejected);
            if (!(input.stage_mask & 1u)) {
                count_activity(lod::ActivityCounter::MissingBikeRoot);
            }
            if (!(input.stage_mask & 2u)) {
                count_activity(lod::ActivityCounter::MissingRiderRoot);
            }
            if (!(input.stage_mask & 4u)) {
                count_activity(lod::ActivityCounter::MissingRiderAnimation);
            }
            if (!(input.stage_mask & 128u)) {
                count_activity(lod::ActivityCounter::MissingBikeAnimation);
            }
        }
    }
    // The stock visual passes run next, on the untouched real mapping/context.
    // No private graph allocation, effect state, animation clock or RNG state
    // is copied back. Only validated immutable pose words survive natively.
    state.input_count = 0;
}

extern "C" void rr64_lod_begin_draw(unsigned char *rdram) {
    end_binding();
    if (rr64_render_only_max_lod_enabled()) {
        count_activity(lod::ActivityCounter::Draw);
    }
    draw_active = rr64_render_only_max_lod_enabled() && observe_scene(rdram);
    if (!draw_active && rr64_render_only_max_lod_enabled()) {
        rr64_lod_invalidate(rdram);
    }
}

extern "C" void rr64_lod_end_actor() {
    end_binding();
}

extern "C" int rr64_lod_weapon_root(unsigned char *rdram, unsigned node,
                                     unsigned *words, unsigned *source) {
    if (!words || !source || !draw_active || shadow_mapping ||
        !rr64_render_only_max_lod_enabled()) return 0;
    std::uint32_t view = 0, slot = 0;
    if (!viewport(rdram, view, slot)) return 0;
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    std::array<std::uint32_t, 7> root{};
    std::uint32_t bank = 0;
    if (!state.store.weapon_root(rdram, node, view, slot, root, bank)) return 0;
    std::copy(root.begin(), root.end(), words);
    *source = bank;
    return 1;
}

extern "C" void rr64_lod_end_draw(unsigned char *rdram) {
    end_binding();
    draw_active = false;
    // The next draw must have a new producer. Host interpolation uses RT64's
    // completed matrices and never needs to extend a guest pose transaction.
    if (rr64_render_only_max_lod_enabled() || rr64_highlights_needs_detail(rdram)) {
        rr64_lod_invalidate(rdram);
    }
}

extern "C" unsigned int rr64_lod_select(unsigned char *rdram, unsigned int node,
                                        unsigned int stock_lod) {
    // The earlier hidden-branch check may have installed the certified rider
    // pose already. Consume that exact binding, so visibility cannot outlive a
    // second lookup or fall back to an unprepared hidden stock pose.
    if (visibility_prebound && pose_binding.active() && draw_active && !shadow_mapping &&
        rdram == bound_mapping && node == bound_node && stock_lod == bound_stock_lod) {
        visibility_prebound = false;
        count_activity(lod::ActivityCounter::Select);
        return 0u;
    }
    end_binding();
    if (rr64_render_only_max_lod_enabled() && !shadow_mapping) {
        count_activity(lod::ActivityCounter::Select);
        if (!draw_active) {
            count_activity(lod::ActivityCounter::InactiveSelect);
        }
    }
    if (!draw_active || shadow_mapping || stock_lod >= 3u) {
        return stock_lod;
    }
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    std::uint32_t view = 0, slot = 0;
    if (!viewport(rdram, view, slot)) {
        fallback_actor_count.fetch_add(1u, std::memory_order_relaxed);
        return stock_lod;
    }
    lod::FindFailure failure = lod::FindFailure::None;
    const auto *pair = state.store.find(rdram, node, view, slot, &failure);
    if (!pair) {
        find_failures[static_cast<std::size_t>(failure)].fetch_add(1u, std::memory_order_relaxed);
        fallback_actor_count.fetch_add(1u, std::memory_order_relaxed);
        count_far_fallback(rdram, node, view);
        record_actor_detail(rdram, node, stock_lod, false, failure);
        return stock_lod;
    }
    const auto &actor = pair->actors[pair->actors[0].node == node ? 0 : 1];
    if (actor.stock_lod != stock_lod) {
        count_activity(lod::ActivityCounter::StockLodMismatch);
        fallback_actor_count.fetch_add(1u, std::memory_order_relaxed);
        record_actor_detail(rdram, node, stock_lod, false, lod::FindFailure::StockState);
        return stock_lod;
    }
    return bind_actor(rdram, actor, slot) ? 0u : stock_lod;
}

extern "C" unsigned int rr64_lod_actor_hidden(unsigned char *rdram, unsigned int node,
                                              unsigned int original_hidden) {
    if (original_hidden != 1u || !draw_active || shadow_mapping) {
        return original_hidden;
    }
    end_binding();
    auto &state = runtime();
    std::lock_guard guard(state.mutex);
    std::uint32_t view = 0, slot = 0, type = 0;
    if (!viewport(rdram, view, slot) || !engine::read_u32(rdram, node, type) || type != 2u) {
        return original_hidden;
    }
    const auto reject = [&]() {
        count_activity(lod::ActivityCounter::RiderRangeRejected);
        return original_hidden;
    };
    const RiderRangeProof *proof = nullptr;
    for (const auto &candidate : state.rider_ranges) {
        if (candidate.node == node && candidate.generation == state.store.generation() &&
            candidate.viewport == view) {
            proof = &candidate;
            break;
        }
    }
    if (!proof) {
        return reject();
    }
    const auto *pair = state.store.find(rdram, node, view, slot);
    if (!pair) {
        return reject();
    }
    const auto &bike = pair->actors[0];
    const auto &rider = pair->actors[1];
    std::uint16_t flags = 0, bike_flags = 0, active = 0;
    std::uint32_t model_state = 0, id = 0;
    if (rider.node != node || rider.type != 2u || bike.type != 1u ||
        rider.entity != proof->entity || rider.root_plan != proof->plan ||
        !rider.root_plan.normalized || rider.root_plan.stock_source_index != 1u ||
        !engine::read_u32(rdram, rider.entity + 4u, model_state) ||
        model_state != proof->model_state || !engine::read_u32(rdram, model_state, id) ||
        id >= engine::kMaximumRacers ||
        !engine::read_u16(rdram, 0x800D8570u + id * 0x118u + 0x24u, active) || active == 0u ||
        !engine::read_u16(rdram, rider.stock_model + 0xau, flags) ||
        flags != (proof->original_flags | 1u) ||
        !engine::read_u16(rdram, bike.stock_model + 0xau, bike_flags) || (bike_flags & 1u) != 0u ||
        !lod::racer_in_extended_view(rdram, rider.entity, 2u) || !bind_actor(rdram, rider, slot)) {
        return reject();
    }
    visibility_prebound = true;
    count_activity(lod::ActivityCounter::RiderRangeRestored);
    // Only the renderer's local branch result changes. All live graph flags,
    // attachment, culling and simulation state remain exactly as authored.
    return 0u;
}

extern "C" unsigned int rr64_lod_root_source(unsigned char *rdram, unsigned int node,
                                             unsigned int record, unsigned int original_source) {
    const auto replay_source = rr64_highlights_root_source(rdram, node, record, original_source);
    if (replay_source != original_source)
        return replay_source;
    if (!pose_binding.active() || shadow_mapping || rdram != bound_mapping || node != bound_node ||
        !bound_root_plan.normalized || record != bound_root_plan.record ||
        original_source != bound_root_plan.original_source) {
        return original_source;
    }
    root_source_selected = true;
    // Only the renderer's local index changes. Its existing stack+54 path
    // selects the matching projection, view, and perspective normalization.
    return bound_root_plan.render_source;
}

namespace {
std::mutex rootRangeMutex;
rr64::lod::RootRangeReport rootRangeReport;
void capture_root_range(unsigned char *m, unsigned node, unsigned record, unsigned matrix) {
    static const bool enabled = [] {
        const char *v = std::getenv("RR64_WEAPON_DIAGNOSTICS");
        return v && std::strcmp(v, "1") == 0;
    }();
    if (!enabled || shadow_mapping)
        return;
    rr64::lod::RootRangeSample sample{};
    sample.node = node;
    sample.record = record;
    unsigned views = 0, sourceRecord = 0;
    using namespace rr64::engine;
    if (!read_u32(m, 0x8009DB88u, views) || views < 2 || views > 4 ||
        !read_u32(m, globals::active_viewport, sample.view) || sample.view >= views ||
        !read_u32(m, node, sample.type) || !read_u32(m, 0x800A1830u, sample.epoch) ||
        !read_u32(m, record + 0x14u, sourceRecord))
        return;
    std::uint16_t source = 0;
    if (!read_u16(m, sourceRecord + 0x12u, source))
        return;
    sample.source = source;
    std::array<float, 16> values{};
    for (unsigned i = 0; i < 16; ++i) {
        if (!read_float(m, matrix + i * 4, values[i]) || !std::isfinite(values[i]))
            return;
        sample.maximum = std::max(sample.maximum, std::abs(values[i]));
    }
    sample.x = values[12];
    sample.y = values[13];
    sample.z = values[14];
    std::lock_guard lock(rootRangeMutex);
    ++rootRangeReport.examined;
    if (sample.maximum < 30000)
        return;
    ++rootRangeReport.nearLimit;
    auto &saved = rootRangeReport.samples[((node >> 4) ^ (record >> 3) ^ sample.view) % 64];
    if (saved.node && (saved.node != node || saved.record != record || saved.view != sample.view))
        ++rootRangeReport.replaced;
    saved = sample;
}
}
rr64::lod::RootRangeReport rr64::lod::take_root_range_report() {
    std::lock_guard lock(rootRangeMutex);
    auto result = rootRangeReport;
    rootRangeReport = {};
    return result;
}

extern "C" void rr64_lod_scale_root_matrix(unsigned char *rdram, unsigned int node,
                                           unsigned int record, unsigned int matrix_address) {
    if (rr64_highlights_scale_root_matrix(rdram, node, record, matrix_address))
        return;
    capture_root_range(rdram, node, record, matrix_address);
    if (!root_source_selected || !pose_binding.active() || shadow_mapping ||
        rdram != bound_mapping || node != bound_node || record != bound_root_plan.record ||
        !bound_root_plan.normalized || (matrix_address & 3u) != 0u ||
        !engine::valid_guest_range(matrix_address, 64u)) {
        return;
    }
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (!engine::read_u32(rdram, matrix_address + std::uint32_t(i) * 4u, words[i]) ||
            !std::isfinite(std::bit_cast<float>(words[i]))) {
            return;
        }
    }
    // 15A90 has produced this exact root's float matrix. Convert XYZ output
    // columns (rotation/scale AND translation), retaining its homogeneous
    // column. Every child matrix and the detailed vertex payload stay intact.
    for (std::size_t row = 0; row < 4u; ++row) {
        for (std::size_t column = 0; column < 3u; ++column) {
            const auto index = row * 4u + column;
            const float scaled = std::bit_cast<float>(words[index]) * 0.1f;
            if (!std::isfinite(scaled) || scaled <= -32768.0f || scaled >= 32768.0f) {
                return;
            }
            words[index] = std::bit_cast<std::uint32_t>(scaled);
        }
    }
    for (std::size_t row = 0; row < 4u; ++row) {
        for (std::size_t column = 0; column < 3u; ++column) {
            const auto index = row * 4u + column;
            engine::write_u32(rdram, matrix_address + std::uint32_t(index) * 4u, words[index]);
        }
    }
    root_source_selected = false;
}

extern "C" unsigned int rr64_lod_racer_view(unsigned char *rdram, unsigned int node,
                                            unsigned int visible) {
    // Called only after the stock actor pass admitted an unhidden graph and
    // performed its point cull. Range/inactivity/retirement rejection stays stock.
    if (visible || !rr64_render_only_max_lod_enabled() || shadow_mapping ||
        !lod::supported_scene(rdram))
        return visible;
    unsigned type = 0, entity = 0, model = 0;
    std::uint16_t flags = 0;
    if (!engine::read_u32(rdram, node, type) || (type != 1u && type != 2u) ||
        !engine::read_u32(rdram, node + 4u, entity) ||
        !engine::valid_guest_range(entity,
                                   type == 1u ? engine::bike::stride : engine::rider::stride) ||
        !engine::read_u32(rdram, node + 0x28u, model) ||
        !engine::read_u16(rdram, model + 0xau, flags) || (flags & 1u))
        return visible;
    return lod::racer_in_extended_view(rdram, entity, type) ? 1u : visible;
}
