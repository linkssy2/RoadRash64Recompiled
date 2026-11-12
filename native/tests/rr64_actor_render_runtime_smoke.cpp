#include <array>
#include <bit>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include "highlight_hooks_disabled.hpp"

#include "recomp.h"
#include "rr64_native.hpp"
#include "rr64_actor_render_fixture.hpp"
#ifndef RR64_TEST_LEGACY_RUNTIME
#include "rr64_actor_render_diagnostics.hpp"
#endif

namespace {
using namespace rr64::engine;
using Fixture = rr64::lod::test::Fixture;
unsigned char* live_mapping = nullptr;
unsigned helper_calls = 0;
unsigned missing_stage = 0;
unsigned animation_certificate_mode = 0;
bool corrupt_peer_resource = false;
bool probe_actor_orientation = false;
bool probe_float_copies = false;
bool expected_float_mode = false;
unsigned suffix_float_probes = 0;
unsigned visual_float_probes = 0;
enum class FullWeightProbe {
    Off, Complete, DuplicateComplete, Partial, LowerMask, UpperMask, Offset,
    NonfinitePhase, NegativePhase, NonfinitePrior, ZeroPrior, BrokenChain,
    WrongNode, GraphChanged, TopologyChanged, BeginOnly, CompleteOnly, LiveMapping
};
FullWeightProbe full_weight_probe = FullWeightProbe::Off;
bool probe_result_clock = false;
std::uint16_t expected_private_pause = 0;
std::uint32_t expected_private_elapsed = 0;
unsigned private_clock_probes = 0;
unsigned char* last_private_mapping = nullptr;
std::string visual_order;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "RR64 render runtime failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void probe_float_registers(recomp_context* context, bool suffix) {
    if (!probe_float_copies) { return; }
    require(context->mips3_float_mode == expected_float_mode,
        "private helper must retain the source floating-point register mode");
    // Match an actual generated odd-register write (f19), while checking the
    // destination before dereferencing. The R13 negative control then fails
    // cleanly on its source-context alias instead of corrupting that source.
    const auto destination = reinterpret_cast<std::uintptr_t>(context->f_odd) + 36u * sizeof(std::uint32_t);
    auto* local_f19 = expected_float_mode ? &context->f19.u32l : &context->f18.u32h;
    require(context->f_odd && destination == reinterpret_cast<std::uintptr_t>(local_f19),
        suffix ? "suffix odd FPU register must belong to its copied context"
               : "visual odd FPU register must belong to its copied context");
    const auto untouched = expected_float_mode ? context->f18.u32h : context->f19.u32l;
    constexpr std::uint32_t sentinel = 0x3e800000u;
    context->f_odd[(19 - 1) * 2] = sentinel;
    require(*local_f19 == sentinel,
        "generated-style odd-register write must update the local register bank");
    require((expected_float_mode ? context->f18.u32h : context->f19.u32l) == untouched,
        "odd-register write must preserve the inactive floating-point bank");
    if (suffix) { ++suffix_float_probes; }
    else { ++visual_float_probes; }
}

void private_helper(unsigned char* memory, const char* name) {
    require(memory != live_mapping, "every helper must receive the private mapping");
    if (probe_result_clock) {
        std::uint16_t pause = 0;
        std::uint32_t elapsed = 0;
        read_u16(memory, globals::gameplay_pause_state, pause);
        read_u32(memory, 0x8009cba8u, elapsed);
        require(pause == expected_private_pause && elapsed == expected_private_elapsed,
            "private visual helpers must observe the results-only frozen clock or unchanged original pause");
        last_private_mapping = memory;
        ++private_clock_probes;
    }
    ++helper_calls;
    visual_order += name;
    // Represents animation/allocator/physics-looking data touched by a helper.
    // No such private write is allowed to be copied back to the real mapping.
    write_u32(memory, Fixture::bike_entity + bike::front_wheel_position, 0xDEADBEEFu);
}

void probe_full_weight_call(unsigned char* memory, const recomp_context& caller) {
    auto context = caller;
    context.r19 = static_cast<std::int32_t>(Fixture::rider_node);
    context.r4 = static_cast<std::int32_t>(Fixture::rider_graph);
    context.r6 = 0x100u;
    context.r7 = std::bit_cast<std::uint32_t>(0.375f);
    const auto stack = static_cast<std::uint32_t>(context.r29);
    write_u32(memory, stack + 0x10u, 0u);
    write_u32(memory, stack + 0x14u, 0x3f800000u);
    if (full_weight_probe == FullWeightProbe::Partial) write_float(memory, stack + 0x14u, 0.5f);
    if (full_weight_probe == FullWeightProbe::LowerMask) context.r6 |= 1u;
    if (full_weight_probe == FullWeightProbe::UpperMask) context.r6 |= 2u;
    if (full_weight_probe == FullWeightProbe::Offset) write_u32(memory, stack + 0x10u, 1u);
    if (full_weight_probe == FullWeightProbe::NonfinitePhase) context.r7 = 0x7fc00000u;
    if (full_weight_probe == FullWeightProbe::NegativePhase) context.r7 = std::bit_cast<std::uint32_t>(-1.0f);
    constexpr auto last_pose = Fixture::rider_pose + 3u * 0x20u;
    std::array<std::uint32_t, 4> prior_rotation{};
    for (unsigned i = 0; i < 4; ++i) read_u32(memory, last_pose + 0xcu + i * 4u, prior_rotation[i]);
    if (full_weight_probe == FullWeightProbe::NonfinitePrior) write_u32(memory, last_pose + 0xcu, 0x7fc00000u);
    if (full_weight_probe == FullWeightProbe::ZeroPrior) {
        for (unsigned i = 0; i < 4; ++i) write_u32(memory, last_pose + 0xcu + i * 4u, 0u);
    }
    if (full_weight_probe == FullWeightProbe::BrokenChain) write_u16(memory, Fixture::rider_graph + 2u, 0u);
    if (full_weight_probe != FullWeightProbe::CompleteOnly) {
        rr64_lod_shadow_full_weight(full_weight_probe == FullWeightProbe::LiveMapping ? live_mapping : memory,
            &context, 0);
    }
    // Restore deliberately bad inputs before completion/publication, so the
    // refusal must come from the begin token rather than a later pose check.
    for (unsigned i = 0; i < 4; ++i) write_u32(memory, last_pose + 0xcu + i * 4u, prior_rotation[i]);
    write_u16(memory, Fixture::rider_graph + 2u, 4u);
    if (full_weight_probe == FullWeightProbe::BeginOnly) return;
    if (full_weight_probe == FullWeightProbe::WrongNode) context.r19 = static_cast<std::int32_t>(Fixture::bike_node);
    if (full_weight_probe == FullWeightProbe::GraphChanged)
        write_u32(memory, Fixture::rider_node + actor_scene::current_model, Fixture::rider_graph + 0x100u);
    if (full_weight_probe == FullWeightProbe::TopologyChanged) write_u16(memory, Fixture::rider_graph + 8u, 0u);
    rr64_lod_shadow_full_weight(memory, &context, 1);
    write_u32(memory, Fixture::rider_node + actor_scene::current_model, Fixture::rider_graph);
    write_u16(memory, Fixture::rider_graph + 8u, 4u);
    context.r19 = static_cast<std::int32_t>(Fixture::rider_node);
    if (full_weight_probe == FullWeightProbe::WrongNode || full_weight_probe == FullWeightProbe::DuplicateComplete)
        rr64_lod_shadow_full_weight(memory, &context, 1);
}

void root(unsigned char* memory, std::uint32_t node, std::uint32_t pose,
    std::uint32_t vector, unsigned stage) {
    for (unsigned i = 0; i < 3; ++i) {
        std::uint32_t value = 0;
        read_u32(memory, vector + i * 4u, value);
        write_u32(memory, pose + i * 4u, value);
    }
    if (probe_actor_orientation) {
        const auto source = node == Fixture::bike_node ? Fixture::bike_entity + 0x244u
                                                       : Fixture::rider_entity + 0x164u;
        for (unsigned i = 0; i < 4; ++i) {
            std::uint32_t value = 0;
            read_u32(memory, source + i * 4u, value);
            write_u32(memory, pose + 0xcu + i * 4u, value);
        }
    }
    if (missing_stage != stage) { rr64_lod_shadow_stage(memory, node, stage); }
}

recomp_context input_context() {
    recomp_context context{};
    context.r20 = static_cast<std::int32_t>(Fixture::bike_node);
    context.r19 = static_cast<std::int32_t>(Fixture::rider_node);
    context.r22 = static_cast<std::int32_t>(Fixture::bike_entity);
    context.r18 = static_cast<std::int32_t>(Fixture::owner);
    context.r17 = static_cast<std::int32_t>(0x800D6880u + 4u * 12u);
    context.r16 = static_cast<std::int32_t>(0x800D6940u + 4u * 12u);
    context.r30 = static_cast<std::int32_t>(0x800D69F8u);
    context.r29 = static_cast<std::int32_t>(Fixture::stack);
    return context;
}

void prepare(Fixture& fixture, int direct_order) {
    rr64_lod_begin_preparation(fixture.live.data());
    auto context = input_context();
    std::array<unsigned char, sizeof(context)> original_context{};
    std::memcpy(original_context.data(), &context, sizeof(context));
    rr64_lod_observe_pair(fixture.live.data(), &context);
    visual_order.clear();
    rr64_lod_prepare_shadow(fixture.live.data(), &context, direct_order);
    require(std::memcmp(original_context.data(), &context, sizeof(context)) == 0,
        "private preparation must leave every caller register unchanged");
}

void test_results_scenes(Fixture& fixture, bool enabled) {
    const auto original = fixture.live;
    constexpr unsigned families[][3] = {{0x09u,0x0au,0x0bu}, {0x12u,0x13u,0x14u},
        {0x17u,0x18u,0x19u}, {0x1cu,0x1du,0x1eu}};
    // This test exercises admission, helper routing and rollback. Animation
    // helpers below are substitutes, so this is not an idle-pose proof.
    const auto exercise = [&](unsigned mode, unsigned pending, bool allowed,
                              unsigned slot, int direct_order) {
        fixture.live = original;
        write_u32(live_mapping, globals::main_mode, mode);
        write_u32(live_mapping, globals::pending_mode, pending);
        write_u32(live_mapping, globals::actor_render_buffer_slot, slot);
        write_u16(live_mapping, Fixture::bike_entity + bike::drive_control_lockout, 1u);
        write_u16(live_mapping, Fixture::rider_entity + rider::bike_attached, 0u);
        const auto before = fixture.live;
        const auto prior_helpers = helper_calls;
        require(rr64::lod::supported_scene(live_mapping) == allowed,
            "results admission must follow retained-scene families without changing live/live policy");
        prepare(fixture, direct_order);
        require(fixture.live == before,
            "results preparation preserves every live guest byte");
        const auto expected_lod = enabled && allowed ? 0u : 2u;
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == expected_lod,
            "results bike selection must follow scene admission and process policy");
        rr64_lod_end_actor();
        require(rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == expected_lod,
            "results rider selection must follow scene admission and process policy");
        rr64_lod_end_draw(live_mapping);
        require(fixture.live == before,
            "results drawing must restore all borrowed poses and guest state");
        require((helper_calls > prior_helpers) == (enabled && allowed),
            "disabled or rejected results scenes must not execute private helpers");
    };
    for (const auto& family : families) {
        const auto result = family[2];
        for (unsigned slot = 0; slot < 2; ++slot) {
            for (int order : {0, 1}) {
                exercise(result, result, true, slot, order);
                for (unsigned i = 0; i < 2; ++i) {
                    exercise(family[i], result, true, slot, order);
                    exercise(result, family[i], true, slot, order);
                }
            }
        }
        exercise(result, 0x20u, false, 0u, 0);
        exercise(0x20u, result, false, 1u, 1);
        for (const auto& other : families) {
            if (other[2] == result) { continue; }
            exercise(result, other[2], false, 0u, 0);
            exercise(result, other[0], false, 1u, 1);
            exercise(other[0], result, false, 0u, 1);
        }
        for (unsigned refusal = 0; refusal < 10; ++refusal) {
            fixture.live = original;
            write_u32(live_mapping, globals::main_mode, result);
            write_u32(live_mapping, globals::pending_mode, result);
            prepare(fixture, 0);
            switch (refusal) {
            case 0: write_u32(live_mapping, globals::pending_mode, 0x20u); break;
            case 1: write_u32(live_mapping, Fixture::race_player_count, 2u); break;
            case 2: write_u32(live_mapping, Fixture::race_player_count, 4u); break;
            case 3: write_u32(live_mapping, 0x8009db88u, 2u); break;
            case 4: write_u16(live_mapping, 0x800a65c4u, 0u); break;
            case 5: write_u32(live_mapping, globals::active_viewport, 1u); break;
            case 6: write_u32(live_mapping, Fixture::bike_entity + bike::rider_pointer, 0u); break;
            case 7: write_u32(live_mapping, Fixture::rider_entity + rider::bike_pointer, 0u); break;
            case 8: write_u32(live_mapping, Fixture::rider_node + actor_scene::display_lists, 0u); break;
            case 9: write_u32(live_mapping, Fixture::model_state + actor_scene::model_state_pose_owner, 0u); break;
            }
            const auto rejected = fixture.live;
            const auto check_stock = [&] {
                rr64_lod_begin_draw(live_mapping);
                require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == 2u &&
                    rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == 2u,
                    "results scene exits, alternate views, multiplayer, revoked ownership and broken resources retain stock");
                rr64_lod_end_draw(live_mapping);
                require(fixture.live == rejected, "rejected results draw preserves all live bytes");
            };
            check_stock(); // A once-valid publication cannot survive revocation.
            const auto prior_helpers = helper_calls;
            prepare(fixture, 1);
            require(helper_calls == prior_helpers && fixture.live == rejected,
                "rejected results inputs prevent helper execution before publication");
            check_stock();
        }
    }
    exercise(0x09u, 0x17u, true, 0u, 0);
    exercise(0x20u, 0x20u, false, 1u, 1);
    fixture.live = original;
    std::cout << "RR64 results admission passed: retained families, both slots/orders, rejected scene/ownership/resources, "
        "and exact rollback; animation helpers substituted.\n";
}

void test_crash_pairs(Fixture& fixture, bool enabled) {
    const auto original = fixture.live;
    const std::array<float, 3> camera{3.0f, -7.0f, 11.0f};
    const std::array<float, 4> bike_rotation{0.0f, 0.0f, 0.6f, 0.8f};
    const std::array<float, 4> rider_rotation{0.8f, 0.0f, 0.0f, 0.6f};
    unsigned positive_cases = 0;
    probe_actor_orientation = true;
    // Routing/certification test only: these root helpers intentionally stand
    // in for E980/EB50. The separate math fixture runs the actual originals.
    for (unsigned attached : {0u, 1u}) {
        for (unsigned ejected : {0u, 1u}) {
            for (unsigned slot : {0u, 1u}) {
                for (int order : {0, 1}) {
                    fixture.live = original;
                    write_u16(live_mapping, Fixture::bike_entity + bike::rider_attached, attached);
                    write_u16(live_mapping, Fixture::rider_entity + rider::ejected, ejected);
                    write_u16(live_mapping, Fixture::rider_entity + rider::bike_attached, 0u);
                    write_u16(live_mapping, Fixture::bike_entity + bike::drive_control_lockout, 1u);
                    write_u32(live_mapping, globals::actor_render_buffer_slot, slot);
                    const std::array<float, 3> bike_anchor{17.0f + float(slot), -23.0f, 31.0f};
                    const std::array<float, 3> rider_anchor{-43.0f, 59.0f + float(order), 71.0f};
                    for (unsigned i = 0; i < 3; ++i) {
                        write_float(live_mapping, Fixture::bike_entity + 0x53cu + i * 4u, bike_anchor[i]);
                        write_float(live_mapping, Fixture::rider_entity + 0x5dcu + i * 4u, rider_anchor[i]);
                        write_float(live_mapping, 0x800d69f8u + i * 4u, camera[i]);
                    }
                    for (unsigned i = 0; i < 4; ++i) {
                        write_float(live_mapping, Fixture::bike_entity + 0x244u + i * 4u, bike_rotation[i]);
                        write_float(live_mapping, Fixture::rider_entity + 0x164u + i * 4u, rider_rotation[i]);
                    }
                    const auto before = fixture.live;
                    const auto prior_helpers = helper_calls;
                    prepare(fixture, order);
                    require(fixture.live == before, "crash preparation must preserve all real state and poses");
                    require((helper_calls > prior_helpers) == enabled,
                        "visual ownership must admit crash poses only when Max LOD is enabled");
                    rr64_lod_begin_draw(live_mapping);
                    for (unsigned actor = 0; actor < 2; ++actor) {
                        const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
                        const auto pose = actor ? Fixture::rider_pose : Fixture::bike_pose;
                        require(rr64_lod_select(live_mapping, node, 2u) == (enabled ? 0u : 2u),
                            "owned detached/ejected actors must publish their detailed pair");
                        if (enabled) {
                            const auto& anchor = actor ? rider_anchor : bike_anchor;
                            const auto& rotation = actor ? rider_rotation : bike_rotation;
                            for (unsigned i = 0; i < 3; ++i) {
                                float value = 0;
                                read_float(live_mapping, pose + i * 4u, value);
                                require(value == (anchor[i] - camera[i]) * 100.0f,
                                    "each detached actor must use its own freshly observed world anchor");
                            }
                            for (unsigned i = 0; i < 4; ++i) {
                                float value = 0;
                                read_float(live_mapping, pose + 0xcu + i * 4u, value);
                                require(value == rotation[i],
                                    "bike and detached rider must retain their distinct root orientations");
                            }
                        }
                        rr64_lod_end_actor();
                        require(fixture.live == before, "crash actor binding must exactly roll back");
                    }
                    rr64_lod_end_draw(live_mapping);
                    ++positive_cases;
                    for (unsigned mutation = 0; mutation < 4; ++mutation) {
                        fixture.live = before;
                        prepare(fixture, order);
                        if (mutation == 0u) write_u16(live_mapping, Fixture::bike_entity + bike::rider_attached, attached ^ 1u);
                        if (mutation == 1u) write_u16(live_mapping, Fixture::rider_entity + rider::ejected, ejected ^ 1u);
                        if (mutation == 2u) write_u32(live_mapping, Fixture::bike_entity + bike::rider_pointer, 0u);
                        if (mutation == 3u) write_u32(live_mapping, Fixture::model_state + actor_scene::model_state_pose_owner, 0u);
                        const auto changed = fixture.live;
                        rr64_lod_begin_draw(live_mapping);
                        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == 2u &&
                            rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == 2u,
                            "a crash-state or ownership change must invalidate both published actors");
                        rr64_lod_end_draw(live_mapping);
                        require(fixture.live == changed, "stale crash publication must leave changed live state intact");
                    }
                }
            }
        }
    }
    probe_actor_orientation = false;
    fixture.live = original;
    std::cout << "RR64 crash pair runtime passed: " << positive_cases
        << " attachment/ejection/slot/order cases, distinct fresh roots and poses, state revocation and exact rollback; root helpers substituted.\n";
}

void test_float_context_copies(Fixture& fixture) {
    for (bool float_mode : {false, true}) {
        for (int direct_order : {0, 1}) {
            auto observed = input_context();
            auto caller = input_context();
            observed.mips3_float_mode = caller.mips3_float_mode = float_mode;
            observed.f18.u32h = 0x3f000001u;
            observed.f19.u32l = 0x3f000002u;
            caller.f18.u32h = 0x3f000003u;
            caller.f19.u32l = 0x3f000004u;
            // These pointers must be initialized after their owning objects
            // reach their final local addresses, just as in the guest runtime.
            observed.f_odd = float_mode ? &observed.f1.u32l : &observed.f0.u32h;
            caller.f_odd = float_mode ? &caller.f1.u32l : &caller.f0.u32h;
            std::array<unsigned char, sizeof(recomp_context)> observed_before{}, caller_before{};
            std::memcpy(observed_before.data(), &observed, sizeof(observed));
            std::memcpy(caller_before.data(), &caller, sizeof(caller));
            const auto memory_before = fixture.live;
            rr64_lod_begin_preparation(live_mapping);
            rr64_lod_observe_pair(live_mapping, &observed);
            expected_float_mode = float_mode;
            suffix_float_probes = visual_float_probes = 0;
            probe_float_copies = true;
            rr64_lod_prepare_shadow(live_mapping, &caller, direct_order);
            probe_float_copies = false;
            require(suffix_float_probes != 0 && visual_float_probes != 0,
                "both suffix and visual execution copies must exercise odd FPU writes");
            require(std::memcmp(observed_before.data(), &observed, sizeof(observed)) == 0,
                "private odd-register writes must preserve every captured source-context byte");
            require(std::memcmp(caller_before.data(), &caller, sizeof(caller)) == 0,
                "private odd-register writes must preserve every prepare-caller context byte");
            require(fixture.live == memory_before,
                "FPU copy isolation must preserve every real guest-memory byte");
            rr64_lod_begin_draw(live_mapping);
            require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == 0u,
                "FPU-isolated preparation must still publish the bike pose");
            rr64_lod_end_actor();
            require(rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == 0u,
                "FPU-isolated preparation must still publish the rider pose");
            rr64_lod_end_draw(live_mapping);
            require(fixture.live == memory_before,
                "FPU-isolated draw must restore all borrowed poses");
        }
    }
    std::cout << "RR64 copied FPU context passed: FR0/FR1, cached/direct order, local odd-register writes and unchanged source/caller bytes.\n";
}

void test_full_weight_tokens(Fixture& fixture) {
    const auto original = fixture.live;
    for (unsigned i = 0; i < 4; ++i)
        write_u16(live_mapping, Fixture::rider_graph + i * 0x20u + 2u, i == 3 ? 0u : 4u);
    const auto chain_memory = fixture.live;
    for (int order : {0, 1}) {
        for (const auto probe : {FullWeightProbe::Complete, FullWeightProbe::Partial,
                FullWeightProbe::LowerMask, FullWeightProbe::UpperMask, FullWeightProbe::Offset,
                FullWeightProbe::NonfinitePhase, FullWeightProbe::NegativePhase,
                FullWeightProbe::NonfinitePrior, FullWeightProbe::ZeroPrior, FullWeightProbe::BrokenChain,
                FullWeightProbe::WrongNode, FullWeightProbe::GraphChanged, FullWeightProbe::TopologyChanged,
                FullWeightProbe::BeginOnly, FullWeightProbe::CompleteOnly, FullWeightProbe::LiveMapping,
                FullWeightProbe::DuplicateComplete}) {
            fixture.live = chain_memory;
            full_weight_probe = probe;
#ifndef RR64_TEST_LEGACY_RUNTIME
            const auto before_activity = rr64::lod::read_activity();
#endif
            prepare(fixture, order);
            const bool accepted = probe == FullWeightProbe::Complete || probe == FullWeightProbe::DuplicateComplete;
            rr64_lod_begin_draw(live_mapping);
            const auto bike_lod = rr64_lod_select(live_mapping, Fixture::bike_node, 2u);
            rr64_lod_end_actor();
            const auto rider_lod = rr64_lod_select(live_mapping, Fixture::rider_node, 2u);
            rr64_lod_end_draw(live_mapping);
            if (bike_lod != (accepted ? 0u : 2u) || rider_lod != (accepted ? 0u : 2u))
                std::cerr << "Full-weight token case=" << static_cast<unsigned>(probe) << " order=" << order << '\n';
            require(bike_lod == (accepted ? 0u : 2u) && rider_lod == (accepted ? 0u : 2u),
                "only completed exact full-weight calls with a valid fresh same-node token certify rider animation");
            require(fixture.live == chain_memory, "full-weight token checks and borrowed poses preserve real memory");
#ifndef RR64_TEST_LEGACY_RUNTIME
            const auto activity = rr64::lod::read_activity();
            constexpr auto counter = static_cast<std::size_t>(rr64::lod::ActivityCounter::FullWeightRiderPrepared);
            require(activity.counts[counter] - before_activity.counts[counter] == (accepted ? 1u : 0u),
                "a full-weight completion token is consumed exactly once");
#endif
        }
    }
    full_weight_probe = FullWeightProbe::Off;
    fixture.live = original;
    std::cout << "RR64 full-weight tokens passed: exact completion, single consumption, masked/partial/stale/invalid refusal; callee substituted.\n";
}

void test_held_results_clock(Fixture& fixture) {
    const auto original = fixture.live;
    const auto exercise = [&](unsigned mode, unsigned pending, std::uint16_t pause,
                              std::uint16_t menu, int order, bool held, bool eligible) {
        fixture.live = original;
        write_u32(live_mapping, globals::main_mode, mode);
        write_u32(live_mapping, globals::pending_mode, pending);
        write_u16(live_mapping, globals::gameplay_pause_state, pause);
        write_u16(live_mapping, globals::pause_menu_state, menu);
        constexpr auto elapsed = std::bit_cast<std::uint32_t>(0.025f);
        write_u32(live_mapping, 0x8009cba8u, elapsed);
        const auto before = fixture.live;
        expected_private_pause = held ? 0u : pause;
        expected_private_elapsed = held ? 0u : elapsed;
        private_clock_probes = 0;
        last_private_mapping = nullptr;
        probe_result_clock = true;
        prepare(fixture, order);
        probe_result_clock = false;
        require((private_clock_probes != 0u) == eligible,
            "only eligible scene preparation may inspect the private visual clock");
        if (last_private_mapping) {
            std::uint16_t restored_pause = 0;
            std::uint32_t restored_elapsed = 0;
            read_u16(last_private_mapping, globals::gameplay_pause_state, restored_pause);
            read_u32(last_private_mapping, 0x8009cba8u, restored_elapsed);
            require(restored_pause == pause && restored_elapsed == elapsed,
                "private results clock is restored before preparation returns");
        }
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == (eligible ? 0u : 2u),
            "scoped results clock retains the expected publication behavior");
        rr64_lod_end_draw(live_mapping);
        require(fixture.live == before, "results clock preparation and drawing preserve all real globals and context");
    };
    for (unsigned result : {0x0bu, 0x14u, 0x19u, 0x1eu}) {
        for (int order : {0, 1}) {
            exercise(result, result, 1u, 0u, order, true, true);
            exercise(result, result, 1u, 1u, order, true, true);
            exercise(result, result, 0u, 0u, order, false, true);
        }
    }
    exercise(0x17u, 0x17u, 1u, 0u, 0, true, true);
    exercise(0x17u, 0x17u, 1u, 1u, 1, true, true);
    exercise(0x17u, 0x19u, 1u, 0u, 0, true, true);
    exercise(0x19u, 0x17u, 1u, 0u, 1, true, true);
    exercise(0x19u, 0x20u, 1u, 0u, 0, false, false);
    exercise(0x20u, 0x20u, 1u, 0u, 1, false, false);
    fixture.live = original;
    std::cout << "RR64 held results clock passed: all result families, scoped clone-only zero time, restored globals and unchanged live/user pause.\n";
}

#ifdef RR64_TEST_REAL_HELD_MATH
template<std::size_t N>
void seed_words(unsigned char* memory, std::uint32_t address, const std::array<std::uint32_t, N>& values) {
    for (unsigned i = 0; i < N; ++i) write_u32(memory, address + i * 4u, values[i]);
}

void test_held_pair(Fixture& fixture) {
    const auto original = fixture.live;
    // Both observed renderer layouts: static/front/rear and
    // front/static/rear/static. The +2 chain reaches only the two wheels.
    constexpr std::uint32_t static_source = 0x80580000u;
    unsigned cases = 0;
    for (unsigned children : {3u, 4u}) {
    const unsigned front = children == 3u ? 1u : 0u;
    constexpr unsigned rear = 2u;
    for (float speed : {0.0f, 2.0f, -2.0f,23.72f,45.08f}) {
    for (unsigned crash_state : {0u,2u,8u}) {
        for (unsigned slot : {0u, 1u}) {
            for (int order : {0, 1}) {
                fixture.live = original;
                write_u32(live_mapping, globals::actor_render_buffer_slot, slot);
                write_u16(live_mapping, 0x800a65bcu, 1u);
                write_u16(live_mapping, 0x800d8570u + 4u * 0x118u + 0x24u, 1u);
                write_float(live_mapping, Fixture::bike_entity + 0x184u, speed);
                if(speed>=10.0f){
                    write_u32(live_mapping,globals::main_mode,28u);write_u32(live_mapping,globals::pending_mode,28u);
                    write_u32(live_mapping,Fixture::bike_entity+0x100u,8u);
                    write_float(live_mapping,Fixture::bike_entity+0x4d4u,1.633f);
                }
                if(crash_state){
                    write_u32(live_mapping,globals::main_mode,28u);write_u32(live_mapping,globals::pending_mode,28u);
                    write_u32(live_mapping,Fixture::bike_entity+0x100u,crash_state);
                    write_float(live_mapping,Fixture::bike_entity+0x4d4u,1.633f);
                    write_u16(live_mapping,Fixture::bike_entity+bike::rider_attached,0u);
                    write_u16(live_mapping,Fixture::rider_entity+rider::ejected,1u);
                }
                write_float(live_mapping, Fixture::bike_entity + 0x3f0u, 0.7f);
                write_float(live_mapping, Fixture::bike_entity + 0x328u, -1.1f);
                write_float(live_mapping, Fixture::bike_entity + 0x560u, 0.02f);
                write_float(live_mapping, Fixture::bike_entity + 0x55cu, 0.01f);
                write_u16(live_mapping, Fixture::bike_graph + 2u, (front + 1u) * 4u);
                for (unsigned part = 0; part < children; ++part) {
                    const auto source = static_source + part * 0x100u;
                    const auto record = Fixture::bike_graph + (part + 1u) * 0x20u;
                    write_u16(live_mapping, record + 2u, part == front ? (rear - front) * 4u : 0u);
                    write_u16(live_mapping, record + 4u, 0x12u);
                    write_u16(live_mapping, record + 8u, part + 1u == children ? 0u : 4u);
                    write_u32(live_mapping, record + 0xcu, Fixture::bike_pose + (part + 1u) * 0x20u);
                    write_float(live_mapping, Fixture::bike_pose + (part + 1u) * 0x20u + 0x18u, 1.0f);
                    write_u32(live_mapping, record + 0x14u, source);
                    write_u32(live_mapping, source, 0x12u);
                    write_u16(live_mapping, source + 0x16u, part == front || part == rear ? 1u : 0u);
                    write_float(live_mapping, source + 0x28u, 0.0f);
                    write_float(live_mapping, source + 0x2cu, 0.0f);
                    write_float(live_mapping, source + 0x30u, 0.6f);
                    write_float(live_mapping, source + 0x34u, 0.8f);
                    write_float(live_mapping, source + 0x38u, 20.0f + float(part));
                    write_float(live_mapping, source + 0x3cu, -3.0f);
                    write_float(live_mapping, source + 0x40u, 40.0f);
                }
                // Seven matrices need more space than the basic six-matrix
                // fixture. Certify all actual slots after changing pointers.
                for (unsigned actor = 0; actor < 2; ++actor) {
                    const auto node = actor ? Fixture::rider_node : Fixture::bike_node;
                    const auto base = actor ? 0x80602000u : 0x80600000u;
                    for (unsigned view = 0; view < 4; ++view) {
                        for (unsigned bank = 0; bank < 2; ++bank)
                            write_u32(live_mapping, node + actor_scene::render_transform_buffers + view * 8u + bank * 4u,
                                base + (view * 2u + bank) * 0x200u);
                        rr64_lod_observe_allocation(live_mapping, node, view, 0x1c0u);
                    }
                }
                seed_words(live_mapping, 0x80005da0u, std::array<std::uint32_t, 11>{
                    0x42c80000u, 0x3f400000u, 0xc1200000u, 0x40800000u,
                    0x41200000u, 0x3f3851ecu, 0xc0a00000u, 0x3f3851ecu,
                    0x40000000u, 0x40000000u, 0x3eb4b4afu});
                write_float(live_mapping, 0x80000db0u, 1.0f);
                write_float(live_mapping, 0x80000dd0u, 0.5f);
                seed_words(live_mapping, 0x80007f88u, std::array<std::uint32_t, 24>{
                    0xbfc55554u,0xbc83656du,0x3f8110edu,0x3804c2a0u,
                    0xbf29f6ffu,0xeea56814u,0x3ec5dbdfu,0x0e314bfeu,
                    0x3fd45f30u,0x6dc9c883u,0x400921fbu,0x50000000u,
                    0x3e6110b4u,0x611a6263u,0u,0u,0x3fe00000u,0u,
                    0x3fe00000u,0u,0u,0u,0x7f810000u,0u});
                seed_words(live_mapping, 0x80008058u, std::array<std::uint32_t, 20>{
                    0xbfc55554u,0xbc83656du,0x3f8110edu,0x3804c2a0u,
                    0xbf29f6ffu,0xeea56814u,0x3ec5dbdfu,0x0e314bfeu,
                    0x3fd45f30u,0x6dc9c883u,0x400921fbu,0x50000000u,
                    0x3e6110b4u,0x611a6263u,0u,0u,0x3fe00000u,0u,0x3fe00000u,0u});
                const auto before = fixture.live;
                missing_stage = 128u; // Represents the separately tested stock speed gate.
#ifndef RR64_TEST_LEGACY_RUNTIME
                const auto before_count = rr64::lod::read_activity().counts[
                    static_cast<std::size_t>(rr64::lod::ActivityCounter::HeldBikePrepared)];
#endif
                prepare(fixture, order);
                missing_stage = 0u;
                require(fixture.live == before, "real held evaluator must run only in private guest memory");
                rr64_lod_begin_draw(live_mapping);
                require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == 0u,
                    "real held evaluator must supply runtime bike completion and publish the pair");
                for (unsigned part = 0; part < children; ++part) {
                    if (part == front || part == rear) continue;
                    const auto static_pose = Fixture::bike_pose + (part + 1u) * 0x20u;
                    const auto source = static_source + part * 0x100u;
                    for (unsigned i = 0; i < 7; ++i) {
                        float value = 0, expected = 0;
                        read_float(live_mapping, static_pose + i * 4u, value);
                        read_float(live_mapping, source + (i < 3u ? 0x38u + i * 4u : 0x28u + (i - 3u) * 4u), expected);
                        require(value == expected, "held pair must restore every immutable static child from its source pose");
                    }
                }
                for (unsigned wheel = 0; wheel < 2; ++wheel) {
                    const auto pose = Fixture::bike_pose + ((wheel ? rear : front) + 1u) * 0x20u;
                    float z = 0, qy = 0;
                    read_float(live_mapping, pose + 8u, z);
                    read_float(live_mapping, pose + 0x10u, qy);
                    require(z == (wheel ? 39.25f : 41.5f) && std::isfinite(qy) && qy != 0.0f,
                        "held pair must contain fresh suspension and actual generated wheel rotation");
                }
                rr64_lod_end_actor();
                require(rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == 0u,
                    "fresh stopped bike completion must release its owned rider for detailed drawing");
                rr64_lod_end_draw(live_mapping);
                require(fixture.live == before, "held-pair drawing must restore every live byte");
#ifndef RR64_TEST_LEGACY_RUNTIME
                const auto after_count = rr64::lod::read_activity().counts[
                    static_cast<std::size_t>(rr64::lod::ActivityCounter::HeldBikePrepared)];
                require(after_count - before_count == 1u,
                    "only the actual held helper must certify this pair's missing bike completion");
#endif
                ++cases;
            }
        }
    }
    }
    }
    fixture.live = original;
    std::cout << "RR64 held pair integration passed: " << cases
        << " layout/low-speed/slot/order cases, actual held evaluator and generated quaternion math, three/four-child publication and exact rollback; other visual helpers substituted.\n";
}
#endif
} // namespace

// These are deliberate helper substitutes, not original game execution. They
// verify routing, suffix inputs, ordering and rollback, not animation parity.
#ifndef RR64_TEST_REAL_HELD_MATH
extern "C" void func_80015834(unsigned char*, recomp_context*) {
    require(false, "the basic runtime fixture has no certified held-wheel inputs; held-pose preflight must refuse it");
}
#endif

extern "C" void func_80012CE0(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, true);
    private_helper(memory, "V");
    std::array<float, 3> left{}, right{};
    for (unsigned i = 0; i < 3; ++i) {
        read_float(memory, static_cast<std::uint32_t>(context->r4) + i * 4u, left[i]);
        read_float(memory, static_cast<std::uint32_t>(context->r5) + i * 4u, right[i]);
    }
    for (unsigned i = 0; i < 3; ++i) {
        write_float(memory, static_cast<std::uint32_t>(context->r6) + i * 4u, left[i] - right[i]);
    }
}

extern "C" void func_80012DBC(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, true);
    private_helper(memory, "S");
    const float scale = std::bit_cast<float>(static_cast<std::uint32_t>(context->r6));
    for (unsigned i = 0; i < 3; ++i) {
        float value = 0;
        read_float(memory, static_cast<std::uint32_t>(context->r4) + i * 4u, value);
        write_float(memory, static_cast<std::uint32_t>(context->r5) + i * 4u, value * scale);
    }
}

extern "C" void func_8005E980(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, false);
    private_helper(memory, "B");
    root(memory, Fixture::bike_node, Fixture::bike_pose, 0x800D6880u + 4u * 12u, 1u);
}

extern "C" void func_8005EB50(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, false);
    private_helper(memory, "R");
    root(memory, Fixture::rider_node, Fixture::rider_pose, 0x800D6940u + 4u * 12u, 2u);
}

extern "C" void func_8005AEE0(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, false);
    private_helper(memory, "A");
    require(rr64_lod_shadow_rider(memory, Fixture::rider_node) == 1 &&
        rr64_lod_shadow_rider(live_mapping, Fixture::rider_node) == 0,
        "distance bypass must apply only to the exact private rider");
    for (auto pose : {Fixture::bike_pose, Fixture::rider_pose}) {
        for (unsigned i = 1; i < 4; ++i) {
            write_float(memory, pose + i * 0x20u, 0.25f * i);
        }
    }
    if (full_weight_probe != FullWeightProbe::Off) {
        probe_full_weight_call(memory, *context);
        return;
    }
    if (missing_stage != 4u) {
        if (animation_certificate_mode == 0u) {
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 4u);
        }
        else if (animation_certificate_mode == 1u) {
            // The stock skip branch reaches the shared return without calling
            // the final animation helper. Existing nonidentity poses are not proof.
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 16u);
        }
        else if (animation_certificate_mode == 2u) {
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 8u);
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 16u);
        }
        else if (animation_certificate_mode == 3u) {
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 8u);
            rr64_lod_shadow_stage(memory, Fixture::rider_node + 0x100u, 16u);
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 16u);
        }
        else {
            // An unconsumed token must not survive into the next preparation.
            rr64_lod_shadow_stage(memory, Fixture::rider_node, 8u);
        }
    }
}

extern "C" void func_8005B63C(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, false);
    private_helper(memory, "C");
}
extern "C" void func_8005B948(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, false);
    private_helper(memory, "W");
    write_float(memory, Fixture::bike_pose + 0x20u, 0.5f);
    if (missing_stage != 128u) {
        rr64_lod_shadow_stage(memory, Fixture::bike_node, 128u);
    }
}
extern "C" void func_8005BEEC(unsigned char* memory, recomp_context* context) {
    probe_float_registers(context, false);
    private_helper(memory, "E");
    if (corrupt_peer_resource) {
        write_u32(memory, Fixture::rider_source + 0x400u, 0xDEADBEEFu);
    }
}

int main(int argc, char** argv) {
    require(argc == 2 || argc == 3, "supply enabled or disabled policy expectation");
    const bool fpu_only = argc == 3 && std::string(argv[2]) == "--fpu-only";
    const bool results_only = argc == 3 && std::string(argv[2]) == "--results-only";
    const bool full_weight_only = argc == 3 && std::string(argv[2]) == "--full-weight-only";
    const bool held_clock_only = argc == 3 && std::string(argv[2]) == "--held-clock-only";
    const bool crash_only = argc == 3 && std::string(argv[2]) == "--crash-only";
    const bool held_pair_only = argc == 3 && std::string(argv[2]) == "--held-pair-only";
    require(argc != 3 || fpu_only || results_only || full_weight_only || held_clock_only || crash_only || held_pair_only, "unknown runtime smoke option");
    const bool enabled = std::string(argv[1]) == "enabled";
    require(rr64_render_only_max_lod_enabled() == static_cast<int>(enabled),
        "the independent Max LOD process switch must match the requested mode");
    Fixture fixture;
    live_mapping = fixture.live.data();
    write_float(live_mapping, Fixture::owner + 0x5dcu, 2.0f);
    write_float(live_mapping, Fixture::owner + 0x5dcu + 8u, 3.0f);
    for (auto node : {Fixture::bike_node, Fixture::rider_node}) {
        for (unsigned view = 0; view < 4; ++view) {
            rr64_lod_observe_allocation(live_mapping, node, view, 0x180u);
        }
    }
    const auto original_memory = fixture.live;
    require(rr64::lod::supported_scene(live_mapping) && fixture.live == original_memory,
        "live race count 1 must accept the unchanged stale multiplayer setup count 2");
#ifndef RR64_TEST_LEGACY_RUNTIME
    {
        write_float(live_mapping,0x80000eb8u,1.0f);write_float(live_mapping,0x80000d50u,1.0f);
        for(auto address:{0x800a4fdcu,0x800a4fe0u,0x800baca0u,0x800baca4u})write_float(live_mapping,address,0.0f);
        write_float(live_mapping,0x800bacb0u,-10.0f);write_float(live_mapping,0x800bacb4u,10.0f);
        write_float(live_mapping,0x800bacc0u,10.0f);write_float(live_mapping,0x800bacc4u,10.0f);
        for(auto node:{Fixture::bike_node,Fixture::rider_node}) {
            unsigned entity=0,type=0,model=0;
            read_u32(live_mapping,node,type);read_u32(live_mapping,node+4u,entity);read_u32(live_mapping,node+0x28u,model);
            write_u16(live_mapping,model+0xau,0);
            auto position=entity+(type==1u?0x16cu:0x8cu);
            for(float x:{-6.5f,6.5f}) {
                write_float(live_mapping,position,x);write_float(live_mapping,position+4u,5.0f);
                const auto before_cull=fixture.live;
                require(rr64_lod_racer_view(live_mapping,node,0)==unsigned(enabled),"active racer side margin follows Max LOD");
                require(fixture.live==before_cull,"racer side margin does not mutate guest memory");
                write_u16(live_mapping,model+0xau,1);
                require(rr64_lod_racer_view(live_mapping,node,0)==0,"pre-hidden racers remain hidden");
                write_u16(live_mapping,model+0xau,0);
            }
            write_float(live_mapping,position,0);write_float(live_mapping,position+4u,11);
            require(rr64_lod_racer_view(live_mapping,node,0)==0,"far-plane rejection retained");
        }
        fixture.live=original_memory;live_mapping=fixture.live.data();
    }
#endif
    if (results_only) {
        test_results_scenes(fixture, enabled);
        return 0;
    }
    if (crash_only) {
        test_crash_pairs(fixture, enabled);
        return 0;
    }
    if (held_pair_only) {
        require(enabled, "held-pair integration requires the enabled process");
#ifdef RR64_TEST_REAL_HELD_MATH
        test_held_pair(fixture);
#else
        require(false, "held-pair integration requires the generated-math target");
#endif
        return 0;
    }
    if (full_weight_only || held_clock_only) {
        require(enabled, "completion-token and held-clock probes require the enabled process");
        if (full_weight_only) test_full_weight_tokens(fixture);
        else test_held_results_clock(fixture);
        return 0;
    }
    if (fpu_only) {
        require(enabled, "FPU copy probe requires the Max LOD enabled process");
        test_float_context_copies(fixture);
        return 0;
    }
    // Run actual hook transactions for every local camera, with both slots.
    for (unsigned players = 2; players <= 4; ++players) {
        for (unsigned view = 0; view < players; ++view) {
            for (unsigned slot = 0; slot < 2; ++slot) {
                fixture.live = original_memory;
                write_u32(live_mapping, Fixture::race_player_count, players);
                write_u32(live_mapping, 0x8009DB88u, players);
                write_u32(live_mapping, globals::active_viewport, view);
                write_u32(live_mapping, globals::actor_render_buffer_slot, slot);
                const auto before = fixture.live;
                prepare(fixture, 1);
                rr64_lod_begin_draw(live_mapping);
                require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == (enabled ? 0u : 2u),
                    "split-screen bike selection follows MAX LOD in every viewport and slot");
                rr64_lod_end_actor();
                require(rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == (enabled ? 0u : 2u),
                    "split-screen rider selection follows MAX LOD in every viewport and slot");
                rr64_lod_end_draw(live_mapping);
                require(fixture.live == before, "split-screen restores every borrowed guest byte");
            }
        }
    }
    fixture.live = original_memory;
    prepare(fixture, 0);
    require(fixture.live == original_memory,
        "preparation must preserve every byte of real RDRAM, including physics and animation");
    if (!enabled) {
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2,
            "disabled rendering must retain stock LOD");
        rr64_lod_end_draw(live_mapping);
        require(helper_calls == 0 && fixture.live == original_memory,
            "disabled hooks must perform no helper calls or guest writes");
        test_results_scenes(fixture, false);
        test_crash_pairs(fixture, false);
        std::cout << "RR64 render runtime disabled isolation passed.\n";
        return 0;
    }
    require(visual_order == "VVSSBRACWE",
        "update-tail preparation must replay roots and visual passes in stock order");
    require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2,
        "prepared poses must not install outside a renderer scope");
    rr64_lod_begin_draw(live_mapping);
    unsigned other_thread_lod = 0;
    std::thread other_thread([&] { other_thread_lod = rr64_lod_select(live_mapping, Fixture::bike_node, 2); });
    other_thread.join();
    require(other_thread_lod == 2, "renderer scope must be local to its owning thread");
    require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 0,
        "the prepared compiled renderer must admit the certified bike");
    float root_z = 0;
    read_float(live_mapping, Fixture::bike_pose + 8u, root_z);
    require(root_z == 1375.0f, "the bike root must consume the exact suffix vector and scale");
    std::uint16_t selected_lod = 0;
    read_u16(live_mapping, Fixture::bike_node + actor_scene::selected_lod, selected_lod);
    require(selected_lod == 2, "renderer selection must never change the guest LOD field");
    rr64_lod_end_actor();
    require(fixture.live == original_memory, "actor-loop exit must restore all borrowed pose bytes");
    require(rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 0,
        "the matching rider must consume the same completed pair");
    rr64_lod_end_draw(live_mapping);
    require(fixture.live == original_memory, "draw exit must restore an unfinished actor binding");
    rr64_lod_begin_draw(live_mapping);
    require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2,
        "a second draw must not reuse the expired completed pair");
    rr64_lod_end_draw(live_mapping);

    // Cached preparation runs before func_800167BC chooses this draw's buffer
    // from 0x800A1830 & 1. DBD4 still names the previous draw's slot at that point.
    // Reproduce only those stock writes, without calling original game code.
    constexpr std::uint32_t frame_counter_address = 0x800A1830u;
    for (const std::uint32_t frame_counter : {41u, 42u}) {
        const auto next_slot = frame_counter & 1u;
        const auto previous_slot = next_slot ^ 1u;
        write_u32(live_mapping, frame_counter_address, frame_counter);
        write_u32(live_mapping, globals::actor_render_buffer_slot, previous_slot);
        const auto memory_before_cached_prepare = fixture.live;
        prepare(fixture, 0);
        require(visual_order == "VVSSBRACWE" && fixture.live == memory_before_cached_prepare,
            "cached preparation before the stock buffer flip must preserve pass order and all guest memory");

        std::uint32_t observed_frame_counter = 0;
        require(read_u32(live_mapping, frame_counter_address, observed_frame_counter),
            "the stock frame counter must remain readable after cached preparation");
        write_u32(live_mapping, globals::actor_render_buffer_slot, observed_frame_counter & 1u);
        const auto memory_after_stock_flip = fixture.live;
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 0,
            "cached bike preparation must survive the stock buffer flip in either direction");
        rr64_lod_end_actor();
        require(fixture.live == memory_after_stock_flip,
            "bike pose rollback must preserve the intentional stock buffer flip exactly");
        require(rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 0,
            "the paired rider must consume the same preparation after the stock buffer flip");
        rr64_lod_end_draw(live_mapping);
        require(fixture.live == memory_after_stock_flip,
            "cached draw exit must restore all borrowed bytes without undoing stock buffer selection");
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2 &&
            rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2,
            "buffer-independent preparation must still expire after its completed draw");
        rr64_lod_end_draw(live_mapping);
        require(fixture.live == memory_after_stock_flip,
            "expired cached preparation must fall back without touching guest memory");

        // The alternate slot is admissible only with its exact certified
        // allocation. Repointing one actor to the other valid bank must reject
        // the entire pair, even though that address is also a known allocation.
        write_u32(live_mapping, globals::actor_render_buffer_slot, previous_slot);
        prepare(fixture, 0);
        write_u32(live_mapping, globals::actor_render_buffer_slot, next_slot);
        write_u32(live_mapping,
            Fixture::rider_node + actor_scene::render_transform_buffers + next_slot * 4u,
            Fixture::rider_buffer + previous_slot * 0x180u);
        const auto memory_after_buffer_replacement = fixture.live;
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2 &&
            rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2,
            "a changed active rider buffer after publication must refuse both paired actors");
        rr64_lod_end_draw(live_mapping);
        require(fixture.live == memory_after_buffer_replacement,
            "changed-buffer refusal must preserve every byte relative to the intentional stock writes");
        fixture.live = original_memory;
    }

    prepare(fixture, 1);
    require(visual_order == "VVSSBRAWCE", "direct-draw preparation must retain its distinct pass order");
    rr64_lod_begin_draw(live_mapping);
    require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 0,
        "direct draw must accept fresh preparation");
    rr64_lod_invalidate(live_mapping);
    require(fixture.live == original_memory && rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2,
        "simulation entry must restore active poses and expire both actors");
    rr64_lod_end_draw(live_mapping);

    for (unsigned stage : {1u, 2u, 4u, 128u}) {
        missing_stage = stage;
        prepare(fixture, 0);
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2 &&
            rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2,
            "missing actual root, rider base animation or bike child completion must reject both actors");
        rr64_lod_end_draw(live_mapping);
    }
    missing_stage = 0;
    for (unsigned mode : {1u, 2u, 3u, 4u, 1u}) {
        animation_certificate_mode = mode;
        prepare(fixture, 0);
        rr64_lod_begin_draw(live_mapping);
        const unsigned expected_lod = mode == 2u ? 0u : 2u;
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == expected_lod &&
            rr64_lod_select(live_mapping, Fixture::rider_node, 2) == expected_lod,
            "shared animation return requires an unconsumed same-node call token");
        rr64_lod_end_draw(live_mapping);
        require(fixture.live == original_memory,
            "animation call-token acceptance and fallback must both restore real memory");
    }
    animation_certificate_mode = 0;
    for (const std::uint32_t players : {2u, 4u}) {
        write_u32(live_mapping, globals::active_racer_count, 1u);
        write_u32(live_mapping, Fixture::race_player_count, players);
        const auto multiplayer_memory = fixture.live;
        const auto prior_helper_calls = helper_calls;
        prepare(fixture, 0);
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2 &&
            rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2,
            "current multiplayer count must retain stock actor detail even with setup count 1");
        rr64_lod_end_draw(live_mapping);
        require(helper_calls == prior_helper_calls && fixture.live == multiplayer_memory,
            "current multiplayer count must prevent shadow helpers and preserve every guest byte");
        fixture.live = original_memory;
    }
    // A context may still point at a prior entity even if the node pair itself
    // remains valid. Seed a plausible stale entity so range checks alone pass.
    constexpr std::uint32_t stale_entity = 0x80120000u;
    write_u32(live_mapping, stale_entity + bike::model_state_pointer, Fixture::model_state);
    write_float(live_mapping, stale_entity + bike::rear_wheel_position + 8u, 99.0f);
    const auto memory_with_stale_entity = fixture.live;
    rr64_lod_begin_preparation(live_mapping);
    auto stale_context = input_context();
    stale_context.r22 = static_cast<std::int32_t>(stale_entity);
    rr64_lod_observe_pair(live_mapping, &stale_context);
    rr64_lod_prepare_shadow(live_mapping, &stale_context, 0);
    rr64_lod_begin_draw(live_mapping);
    require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2 &&
        rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2 &&
        fixture.live == memory_with_stale_entity,
        "captured entity identity must match the live pair before suffix replay");
    rr64_lod_end_draw(live_mapping);
    fixture.live = original_memory;
    corrupt_peer_resource = true;
    prepare(fixture, 0);
    rr64_lod_begin_draw(live_mapping);
    require(rr64_lod_select(live_mapping, Fixture::bike_node, 2) == 2 &&
        rr64_lod_select(live_mapping, Fixture::rider_node, 2) == 2,
        "a private peer display-list mutation must reject both halves");
    rr64_lod_end_draw(live_mapping);
    require(fixture.live == original_memory, "every rejected transaction must leave real memory unchanged");
    corrupt_peer_resource = false;
    test_float_context_copies(fixture);
    test_results_scenes(fixture, true);
    test_full_weight_tokens(fixture);
    test_held_results_clock(fixture);
    test_crash_pairs(fixture, true);
#ifndef RR64_TEST_LEGACY_RUNTIME
    {
        Fixture lifecycle;
        lifecycle.live = original_memory;
        live_mapping = lifecycle.live.data();
        const auto unchanged = lifecycle.live;
        rr64_lod_reset_actor_pool(live_mapping);
        rr64_lod_observe_allocation(live_mapping, Fixture::bike_node, 0u, 6u * 64u);
        rr64_lod_observe_allocation(live_mapping, Fixture::rider_node, 0u, 6u * 64u);
        prepare(lifecycle, 0);
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == 0u,
            "new pool allocations produce a detailed racer");
        rr64_lod_release_node(live_mapping, Fixture::rider_node);
        require(lifecycle.live == unchanged, "destruction ends a bound pose without changing guest state");
        require(rr64_lod_select(live_mapping, Fixture::rider_node, 2u) == 2u,
            "destroyed node cannot consume the prior prepared pose");
        rr64_lod_end_draw(live_mapping);
        rr64_lod_reset_actor_pool(live_mapping);
        prepare(lifecycle, 0);
        rr64_lod_begin_draw(live_mapping);
        require(rr64_lod_select(live_mapping, Fixture::bike_node, 2u) == 2u,
            "whole-pool reset revokes allocation certificates");
        rr64_lod_end_draw(live_mapping);
        const auto diagnostics = rr64::lod::read_activity();
        for (const auto& actor : diagnostics.actor_details)
            require(actor.fallback == actor.fallback_by_lod[0] + actor.fallback_by_lod[1] + actor.fallback_by_lod[2],
                "per-tier counters retain every transient fallback");
    }
#endif
    std::cout << "RR64 render runtime passed: private helper routing, pass order, exact rollback, "
        "cached buffer flips, allocation refusal, thread scope, stage freshness, and paired resource fallback.\n";
    return 0;
}


