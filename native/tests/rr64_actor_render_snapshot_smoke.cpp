#include "rr64_actor_render_fixture.hpp"
#include "rr64_world_render.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {
bool check(bool condition, const char* text) {
    if (!condition) { std::fprintf(stderr, "[RR64-LOD-TEST] FAILED: %s\n", text); }
    return condition;
}

bool check_visual_attachment() {
    using namespace rr64::engine;
    using namespace rr64::lod;
    using rr64::lod::test::Fixture;
    auto fixture = std::make_unique<Fixture>();
    auto store = std::make_unique<SnapshotStore>();
    auto& f = *fixture;
    // Exercise the weapon's supported camera-source bank rather than the
    // generic fixture's source-zero transform path.
    for(unsigned tier=0;tier<3;++tier)
        write_u16(f.live.data(),f.rider_source+tier*0x100u+0x12u,2u);
    const auto original = f.live;
    f.register_allocations(*store);
    bool passed = true;
    for (unsigned selected = 0; selected < 3; ++selected) {
        for (unsigned previous = 0; previous < 3; ++previous) {
            f.live = original;
            for (const auto node : {f.bike_node, f.rider_node}) {
                const auto graph = node == f.bike_node ? f.bike_graph : f.rider_graph;
                write_u16(f.live.data(), node + actor_scene::selected_lod, selected);
                write_u16(f.live.data(), node + actor_scene::previous_lod, previous);
                write_u32(f.live.data(), node + actor_scene::current_model, graph + selected * 0x100u);
            }
            // The finish-wait contract permits control/contact attachment to
            // clear while the bike-side visual link and pose ownership remain.
            write_u16(f.live.data(), f.bike_entity + bike::drive_control_lockout, 1u);
            write_u16(f.live.data(), f.rider_entity + rider::bike_attached, 0u);
            const auto visually_linked = f.live;
            f.prepare();
            const auto prepared = f.shadow;
            store->invalidate();
            passed &= check(mounted_pair(f.live.data(), f.bike_node, f.rider_node) &&
                store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, 0u) &&
                store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, 0u),
                "visually attached finish-wait pair remains eligible across current and previous LOD states");
            for (unsigned slot = 0; slot < 2; ++slot) {
                const auto* pair = store->find(f.live.data(), f.bike_node, 0u, slot);
                passed &= check(pair && store->find(f.live.data(), f.rider_node, 0u, slot) == pair,
                    "both finish-wait actors retain the same detailed snapshot in either renderer slot");
                std::array<std::uint32_t,7> root{};
                std::uint32_t source=0;
                passed &= check(store->weapon_root(f.live.data(),f.rider_node,0u,slot,root,source),
                    "weapon can borrow the certified rider root after model restoration");
                if(pair) for(unsigned i=0;i<root.size();++i)
                    passed &= check(root[i]==pair->actors[1].poses[0].words[i],"weapon receives authored root words");
                const auto savedRoot=root;
                const auto savedSource=source;
                passed &= check(!store->weapon_root(f.live.data(),f.rider_node,1u,slot,root,source) &&
                    !store->weapon_root(f.live.data(),f.bike_node,0u,slot,root,source) &&
                    root==savedRoot && source==savedSource,"wrong view or bike cannot supply a weapon root");
            }
            passed &= check(f.live == visually_linked && f.shadow == prepared,
                "finish-wait eligibility preserves both guest mappings");
            for (unsigned transition = 0; transition < 4; ++transition) {
                f.live = visually_linked;
                if (transition == 0u) {
                    write_u16(f.live.data(), f.bike_entity + bike::rider_attached, 0u);
                }
                else if (transition == 1u) {
                    write_u16(f.live.data(), f.rider_entity + rider::ejected, 1u);
                }
                else if (transition == 2u) {
                    write_u32(f.live.data(), f.rider_entity + rider::bike_pointer, 0u);
                }
                else {
                    write_u32(f.live.data(), f.model_state + actor_scene::model_state_pose_owner, 0u);
                }
                const auto changed = f.live;
                passed &= check(!mounted_pair(f.live.data(), f.bike_node, f.rider_node) &&
                    store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, 0u) == (transition < 2u) &&
                    !store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, 0u) &&
                    !store->find(f.live.data(), f.bike_node, 0u, 0u) &&
                    !store->find(f.live.data(), f.rider_node, 0u, 1u) && f.live == changed,
                    "detachment/ejection needs a fresh sampled pose; broken ownership blocks new preparation too");
            }
        }
    }
    return passed;
}

bool check_crash_attachment() {
    using namespace rr64::engine;
    using namespace rr64::lod;
    using rr64::lod::test::Fixture;
    auto fixture = std::make_unique<Fixture>();
    auto store = std::make_unique<SnapshotStore>();
    auto& f = *fixture;
    const auto original = f.live;
    f.register_allocations(*store);
    bool passed = true;
    // Exercise the existing public store API so the same test can run against
    // archived R16 and fail on its mounted-only admission rule.
    for (unsigned state = 1; state <= 3; ++state) {
        for (unsigned selected = 0; selected < 3; ++selected) {
            for (unsigned prepared_slot = 0; prepared_slot < 2; ++prepared_slot) {
                f.live = original;
                write_u16(f.live.data(), f.bike_entity + bike::drive_control_lockout, 1u);
                write_u16(f.live.data(), f.bike_entity + bike::rider_attached, (state & 1u) ? 0u : 1u);
                write_u16(f.live.data(), f.rider_entity + rider::bike_attached, (state & 1u) ? 0u : 1u);
                write_u16(f.live.data(), f.rider_entity + rider::ejected, (state & 2u) ? 1u : 0u);
                for (const auto node : {f.bike_node, f.rider_node}) {
                    const auto graph = node == f.bike_node ? f.bike_graph : f.rider_graph;
                    write_u16(f.live.data(), node + actor_scene::selected_lod, selected);
                    write_u16(f.live.data(), node + actor_scene::previous_lod, (selected + 1u) % 3u);
                    write_u32(f.live.data(), node + actor_scene::current_model, graph + selected * 0x100u);
                }
                const auto crashed = f.live;
                f.prepare();
                const auto prepared = f.shadow;
                store->invalidate();
                passed &= check(!mounted_pair(f.live.data(), f.bike_node, f.rider_node),
                    "render-only crash admission does not change the gameplay mounted-pair contract");
                passed &= check(store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                    store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, prepared_slot),
                    "detached, ejected and combined states publish freshly prepared poses at every stock tier");
                for (unsigned slot = 0; slot < 2; ++slot) {
                    const auto* pair = store->find(f.live.data(), f.bike_node, 0u, slot);
                    passed &= check(pair && store->find(f.live.data(), f.rider_node, 0u, slot) == pair,
                        "both crashed actors consume one immutable pose generation in either slot");
                    if (pair) {
                        for (const auto& actor : pair->actors) {
                            PoseBinding binding;
                            passed &= check(binding.begin(f.live.data(), actor), "crashed actor pose binding succeeds");
                            binding.end();
                            passed &= check(f.live == crashed, "crashed actor binding restores every live byte");
                        }
                    }
                }
                passed &= check(f.live == crashed && f.shadow == prepared,
                    "crash preparation eligibility and publication change neither mapping");

                // A state transition cannot relabel a pose sampled before it.
                for (unsigned transition = 0; transition < 2; ++transition) {
                    f.live = crashed;
                    if (transition == 0u) {
                        write_u16(f.live.data(), f.bike_entity + bike::rider_attached, (state & 1u) ? 1u : 0u);
                    }
                    else { write_u16(f.live.data(), f.rider_entity + rider::ejected, (state & 2u) ? 0u : 1u); }
                    const auto transitioned = f.live;
                    passed &= check(store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                        !store->find(f.live.data(), f.bike_node, 0u, 0u) &&
                        !store->find(f.live.data(), f.rider_node, 0u, 1u) &&
                        !store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                        f.live == transitioned && f.shadow == prepared,
                        "attachment/ejection transitions permit fresh work but reject old snapshots and old shadows");
                }
                for (unsigned ownership = 0; ownership < 6; ++ownership) {
                    f.live = crashed;
                    switch (ownership) {
                    case 0: write_u32(f.live.data(), f.bike_entity + bike::rider_pointer, 0u); break;
                    case 1: write_u32(f.live.data(), f.rider_entity + rider::bike_pointer, f.bike_entity + 0x100u); break;
                    case 2: write_u32(f.live.data(), f.model_state + actor_scene::model_state_pose_owner, 0u); break;
                    case 3: write_u32(f.live.data(), f.owner + actor_scene::pose_owner_rider_node, f.bike_node); break;
                    case 4: write_u32(f.live.data(), f.rider_node + 4u, f.rider_entity + 0x100u); break;
                    case 5: write_u32(f.live.data(), f.bike_entity + bike::model_state_pointer, 0u); break;
                    }
                    const auto broken = f.live;
                    passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                        !store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                        !store->find(f.live.data(), f.bike_node, 0u, 0u) &&
                        !store->find(f.live.data(), f.rider_node, 0u, 1u) && f.live == broken,
                        "crash admission preserves reciprocal pointer, model-owner and node identity checks");
                }
                f.live = crashed;
                store->invalidate();
                passed &= check(!store->find(f.live.data(), f.bike_node, 0u, 0u) &&
                    !store->find(f.live.data(), f.rider_node, 0u, 1u),
                    "the next simulation generation expires both crashed actors");
                write_u16(f.live.data(), f.bike_entity + bike::rider_attached, 1u);
                write_u16(f.live.data(), f.rider_entity + rider::bike_attached, 1u);
                write_u16(f.live.data(), f.rider_entity + rider::ejected, 0u);
                f.prepare();
                passed &= check(mounted_pair(f.live.data(), f.bike_node, f.rider_node) &&
                    store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                    store->find(f.live.data(), f.bike_node, 0u, 0u) &&
                    store->find(f.live.data(), f.rider_node, 0u, 1u),
                    "remount publishes only a new current-state generation");
            }
        }
    }
    return passed;
}

bool check_results_scenes() {
    using namespace rr64::engine;
    using namespace rr64::lod;
    using rr64::lod::test::Fixture;
    auto fixture = std::make_unique<Fixture>();
    auto store = std::make_unique<SnapshotStore>();
    auto& f = *fixture;
    write_u16(f.live.data(), f.bike_entity + bike::drive_control_lockout, 1u);
    write_u16(f.live.data(), f.rider_entity + rider::bike_attached, 0u);
    const auto original = f.live;
    f.register_allocations(*store);
    bool passed = true;
    // These cases validate scene admission and immutable publication. The
    // fixture supplies completed poses; it does not prove idle animation.
    const auto exercise = [&](unsigned mode, unsigned pending, bool allowed) {
        f.live = original;
        write_u32(f.live.data(), globals::main_mode, mode);
        write_u32(f.live.data(), globals::pending_mode, pending);
        const auto before = f.live;
        f.prepare();
        const auto prepared = f.shadow;
        store->invalidate();
        bool ok = supported_scene(f.live.data()) == allowed &&
            store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, 0u) == allowed &&
            store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, 0u) == allowed;
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto* pair = store->find(f.live.data(), f.bike_node, 0u, slot);
            const auto* rider_pair = store->find(f.live.data(), f.rider_node, 0u, slot);
            ok &= (pair != nullptr) == allowed && (rider_pair != nullptr) == allowed && pair == rider_pair;
            if (pair) {
                for (const auto& actor : pair->actors) {
                    PoseBinding binding;
                    ok &= binding.begin(f.live.data(), actor);
                    binding.end();
                    ok &= f.live == before;
                }
            }
        }
        ok &= f.live == before && f.shadow == prepared;
        if (!ok) {
            std::fprintf(stderr, "[RR64-RESULTS-SNAPSHOT] FAILED mode=%02X pending=%02X allowed=%u\n",
                mode, pending, allowed ? 1u : 0u);
        }
        passed &= ok;
    };
    constexpr unsigned families[][3] = {{0x09u,0x0au,0x0bu}, {0x12u,0x13u,0x14u},
        {0x17u,0x18u,0x19u}, {0x1cu,0x1du,0x1eu}};
    for (const auto& family : families) {
        const auto result = family[2];
        exercise(result, result, true);
        for (unsigned i = 0; i < 2; ++i) {
            exercise(family[i], result, true);
            exercise(result, family[i], true);
        }
        exercise(result, 0x20u, false);
        exercise(0x20u, result, false);
        for (const auto& other : families) {
            if (other[2] == result) { continue; }
            exercise(result, other[2], false);
            exercise(result, other[0], false);
            exercise(other[0], result, false);
        }
        for (unsigned refusal = 0; refusal < 8; ++refusal) {
            f.live = original;
            write_u32(f.live.data(), globals::main_mode, result);
            write_u32(f.live.data(), globals::pending_mode, result);
            f.prepare();
            store->invalidate();
            passed &= check(store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, 0u),
                "results rejection fixture begins with a certified mounted pair");
            switch (refusal) {
            case 0: write_u32(f.live.data(), globals::pending_mode, 0x20u); break;
            case 1: write_u32(f.live.data(), Fixture::race_player_count, 2u); break;
            case 2: write_u32(f.live.data(), Fixture::race_player_count, 4u); break;
            case 3: write_u32(f.live.data(), 0x8009db88u, 2u); break;
            case 4: write_u16(f.live.data(), 0x800a65c4u, 0u); break;
            case 5: write_u16(f.live.data(), f.bike_entity + bike::rider_attached, 0u); break;
            case 6: write_u16(f.live.data(), f.rider_entity + rider::ejected, 1u); break;
            case 7: write_u32(f.live.data(), f.rider_node + actor_scene::display_lists, 0u); break;
            }
            const auto rejected = f.live;
            passed &= check(store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, 0u) == (refusal == 5u || refusal == 6u) &&
                !store->find(f.live.data(), f.bike_node, 0u, 0u) &&
                !store->find(f.live.data(), f.rider_node, 0u, 1u) &&
                !store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, 0u) &&
                f.live == rejected, "results exit, multiplayer, renderer, attachment and resource changes revoke both actors");
        }
    }
    exercise(0x09u, 0x17u, true); // Existing live/live acceptance is unchanged.
    exercise(0x20u, 0x20u, false);
    return passed;
}
}

bool check_split_screen() {
    using namespace rr64::engine;
    using namespace rr64::lod;
    using Fixture = rr64::lod::test::Fixture;
    auto f = std::make_unique<Fixture>();
    auto store = std::make_unique<SnapshotStore>();
    f->register_allocations(*store);
    bool world_gate_passed = check(rr64::world::supported_scene(f->live.data()), "single-camera world extension remains supported");
    write_u32(f->live.data(), Fixture::race_player_count, 4u);
    write_u32(f->live.data(), 0x8009DB88u, 4u);
    write_u16(f->live.data(), globals::gameplay_pause_state, 1u);
    bool passed = world_gate_passed && check(!rr64::world::supported_scene(f->live.data()) &&
        supported_scene(f->live.data()), "actor split-screen admission does not enable single-camera world buffers");
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned view = 0; view < 4; ++view) {
            write_u32(f->live.data(), globals::active_viewport, view);
            store->invalidate();
            store->begin_pose_epoch(f->live.data());
            f->prepare();
            if (round) {
                passed &= check(store->seed_previous_rider_children(f->live.data(), f->shadow.data(),
                    Fixture::rider_node, view, 0u, true), "each camera retains its own previous child pose");
                float child = 0;
                read_float(f->shadow.data(), Fixture::rider_pose + 0x20u, child);
                passed &= check(child == 10.0f + view, "other cameras cannot overwrite animation history");
            }
            write_float(f->shadow.data(), Fixture::rider_pose + 0x20u, 10.0f + view);
            const auto original = f->live;
            passed &= check(store->publish(f->live.data(), f->shadow.data(), Fixture::bike_node,
                Fixture::rider_node, view, 0u), "split-screen publishes a certified pair for each camera");
            for (unsigned slot = 0; slot < 2; ++slot) {
                passed &= check(store->find(f->live.data(), Fixture::bike_node, view, slot) &&
                    store->find(f->live.data(), Fixture::rider_node, view, slot), "both actors use certified per-camera buffer slots");
            }
            passed &= check(!store->find(f->live.data(), Fixture::bike_node, (view + 1u) % 4u, 0u),
                "another camera cannot consume this camera's snapshot");
            passed &= check(f->live == original, "split-screen preparation preserves live guest bytes");
        }
    }
    return passed;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--crash-only") == 0) {
        const bool passed = check_crash_attachment();
        std::puts(passed ? "[RR64-CRASH-SNAPSHOT] PASS" : "[RR64-CRASH-SNAPSHOT] FAIL");
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--results-only") == 0) {
        const bool passed = check_results_scenes();
        std::puts(passed ? "[RR64-RESULTS-SNAPSHOT] PASS" : "[RR64-RESULTS-SNAPSHOT] FAIL");
        return passed ? 0 : 1;
    }
    using namespace rr64::engine;
    using namespace rr64::lod;
    using rr64::lod::test::Fixture;
    auto fixture = std::make_unique<Fixture>();
    auto store = std::make_unique<SnapshotStore>();
    auto& f = *fixture;
    bool passed = true;
    const auto original = f.live;
    passed &= check(supported_scene(f.live.data()) && f.live == original,
        "a live one-player race accepts stale multiplayer setup count 2 without changing guest memory");
    write_u32(f.live.data(), Fixture::race_player_count, 0u);
    passed &= check(supported_scene(f.live.data()), "attract actors with zero humans and one camera are supported");
    write_u32(f.live.data(), 0x8009DB88u, 2u);
    passed &= check(!supported_scene(f.live.data()), "zero humans does not admit a stale split-screen layout");
    f.live = original;
    passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, 0u),
        "graph addresses and pointer spacing alone never prove allocation capacity");
    f.register_allocations(*store);
    passed &= check(f.live == original &&
        store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, 0u),
        "exact original allocator observations are read-only and accept a mounted AI pair");
    f.prepare();
    const auto prepared = f.shadow;
    passed &= check(!store->publish(f.live.data(), f.live.data(), f.bike_node, f.rider_node, 0, 0),
        "real guest preparation cannot masquerade as an isolated producer");
    passed &= check(store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0, 0) &&
        f.live == original && f.shadow == prepared,
        "completed private pair publication writes neither guest mapping");
    const auto* pair = store->find(f.live.data(), f.bike_node, 0, 0);
    passed &= check(pair && store->find(f.live.data(), f.rider_node, 0, 0) == pair,
        "both actors consume the same completed native pair");
    if (pair) {
        PoseBinding binding;
        const auto owned_pose = pair->actors[0].poses[0].words;
        write_float(f.shadow.data(), f.bike_pose, 999.0f);
        passed &= check(pair->actors[0].poses[0].words == owned_pose,
            "native snapshots own their data after private RDRAM changes");
        passed &= check(binding.begin(f.live.data(), pair->actors[0]), "bounded matrix binding starts");
        float root = 0;
        read_float(f.live.data(), f.bike_pose, root);
        passed &= check(root == 20.0f, "matrix loop receives only the captured detailed actor pose");
        binding.end();
        binding.end();
        passed &= check(f.live == original, "all pose, physical-anchor and actor-node bytes restore exactly");
        passed &= check(binding.begin(f.live.data(), pair->actors[0]) &&
            binding.begin(f.live.data(), pair->actors[1]), "next actor restores the preceding actor before binding");
        binding.end();
        passed &= check(f.live == original, "actor re-entry cannot leak a previous pose");
        ActorSnapshot invalid = pair->actors[0];
        invalid.poses[1].address = kRdramEnd - 4u;
        passed &= check(!binding.begin(f.live.data(), invalid) && f.live == original,
            "failed binding validates every destination before its first write");
    }
    passed &= check(pair && store->find(f.live.data(), f.bike_node, 0u, 1u) == pair &&
        store->find(f.live.data(), f.rider_node, 0u, 1u) == pair && f.live == original,
        "slot 0 preparation remains consumable after the stock renderer selects certified slot 1");
    f.prepare();
    passed &= check(store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, 1u),
        "the next immutable pair can be prepared while slot 1 is current");
    pair = store->find(f.live.data(), f.bike_node, 0u, 0u);
    passed &= check(pair && store->find(f.live.data(), f.rider_node, 0u, 0u) == pair &&
        store->find(f.live.data(), f.bike_node, 0u, 1u) == pair && f.live == original,
        "slot 1 preparation remains consumable after the stock renderer selects certified slot 0");
    passed &= check(!store->find(f.live.data(), f.bike_node, 1u, 0u) &&
        !store->find(f.live.data(), f.bike_node, 0u, 2u), "viewport identity and slot bounds remain exact");
    for (const std::uint32_t prepared_slot : {0u, 1u}) {
        const auto other_slot = prepared_slot ^ 1u;
        const auto other_pointer = f.rider_node + actor_scene::render_transform_buffers + other_slot * 4u;
        std::uint32_t original_buffer = 0;
        read_u32(f.live.data(), other_pointer, original_buffer);
        for (const std::uint32_t changed_buffer : {0u, original_buffer + 8u, kRdramEnd - 0x100u}) {
            write_u32(f.live.data(), other_pointer, changed_buffer);
            const auto changed = f.live;
            passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                !store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0u, prepared_slot) &&
                !store->find(f.live.data(), f.bike_node, 0u, other_slot) &&
                !store->find(f.live.data(), f.rider_node, 0u, other_slot) &&
                !store->find(f.live.data(), f.bike_node, 0u, prepared_slot) && f.live == changed,
                "missing, replaced, or undersized alternate destination rejects the entire pair without writes");
            f.live = original;
        }
    }
    write_u32(f.live.data(), f.rider_node + actor_scene::display_lists, f.rider_source + 0x410u);
    passed &= check(!store->find(f.live.data(), f.bike_node, 0, 0) &&
        !store->find(f.live.data(), f.rider_node, 0, 0), "one peer's changed resource rejects both halves");
    f.live = original;
    constexpr std::uint32_t replacement_state = 0x80113000u;
    write_u32(f.live.data(), replacement_state, 4u);
    write_u32(f.live.data(), replacement_state + actor_scene::model_state_pose_owner, f.owner);
    write_u32(f.live.data(), f.bike_entity + bike::model_state_pointer, replacement_state);
    passed &= check(mounted_pair(f.live.data(), f.bike_node, f.rider_node) &&
        !store->find(f.live.data(), f.bike_node, 0, 0) &&
        !store->find(f.live.data(), f.rider_node, 0, 0),
        "otherwise identical replacement racer/model state invalidates both actors");
    f.live = original;
    write_u16(f.live.data(), f.bike_entity + bike::rider_attached, 0u);
    passed &= check(!store->find(f.live.data(), f.bike_node, 0, 0) &&
        !store->find(f.live.data(), f.rider_node, 0, 0), "detachment revokes both halves");
    f.live = original;
    for (const std::uint32_t players : {2u, 4u}) {
        write_u32(f.live.data(), globals::active_racer_count, 1u);
        write_u32(f.live.data(), Fixture::race_player_count, players);
        const auto multiplayer = f.live;
        passed &= check(!supported_scene(f.live.data()) &&
            !store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 0) &&
            !store->find(f.live.data(), f.bike_node, 0, 0) &&
            !store->find(f.live.data(), f.rider_node, 0, 0) && f.live == multiplayer,
            "current multiplayer count rejects both actors even with setup count 1 and a stale single viewport");
    }
    f.live = original;
    write_u32(f.live.data(), 0x8009DB88u, 2u);
    passed &= check(!store->find(f.live.data(), f.bike_node, 0, 0), "local multiplayer remains stock");
    f.live = original;
    write_u16(f.live.data(), 0x800A65C4u, 0u);
    passed &= check(!store->find(f.live.data(), f.bike_node, 0, 0), "alternate graph renderer remains stock");
    f.live = original;
    write_u32(f.live.data(), globals::pending_mode, 0x14u);
    passed &= check(!store->find(f.live.data(), f.bike_node, 0, 0), "race exit rejects stale presentation");
    f.live = original;
    store->invalidate();
    passed &= check(!store->find(f.live.data(), f.bike_node, 0, 0) &&
        !store->find(f.live.data(), f.rider_node, 0, 0) &&
        !store->find(f.live.data(), f.bike_node, 0, 1) &&
        !store->find(f.live.data(), f.rider_node, 0, 1),
        "new simulation invalidates the same completed pair for both certified slots");
    f.prepare();
    write_u32(f.shadow.data(), f.rider_pose + 0x40u + 4u, 0x7fc00000u);
    passed &= check(!store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0, 0),
        "one nonfinite child rejects the pair even when other children are prepared");
    f.prepare();
    for (unsigned i = 0; i < 7; ++i) { write_u32(f.shadow.data(), f.rider_pose + 0x20u + i * 4u, 0u); }
    passed &= check(!store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0, 0),
        "zero quaternion is invalid even in otherwise populated graphs");
    f.prepare();
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned w = 0; w < 6; ++w) { write_u32(f.shadow.data(), f.rider_pose + i * 32u + w * 4u, 0u); }
    }
    passed &= check(!store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0, 0),
        "dormant cleared rider falls back as a pair");
    f.prepare();
    write_u32(f.shadow.data(), f.bike_graph + 0x2cu, 0x80380000u);
    passed &= check(!store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0, 0),
        "shadow-allocated or redirected pose pointers never enter the real renderer");
    f.prepare();
    write_u32(f.live.data(), f.bike_graph + 0x2cu, f.rider_pose);
    write_u32(f.shadow.data(), f.bike_graph + 0x2cu, f.rider_pose);
    passed &= check(!store->publish(f.live.data(), f.shadow.data(), f.bike_node, f.rider_node, 0, 0),
        "shared rider/bike pose ownership is rejected before any binding");
    f.live = original;
    f.prepare();
    write_u32(f.live.data(), f.bike_graph + 0x0cu, f.bike_source);
    write_u32(f.shadow.data(), f.bike_graph + 0x0cu, f.bike_source);
    passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 0),
        "pose cannot overlap its renderer source record");
    f.live = original;
    write_u32(f.live.data(), f.bike_graph + 0x2cu, f.model_state + 0x20u);
    passed &= check(mounted_pair(f.live.data(), f.bike_node, f.rider_node) &&
        !store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 0),
        "otherwise valid pair cannot borrow poses inside separate racer/model state");
    f.live = original;
    write_u32(f.live.data(), f.bike_node + actor_scene::render_transform_buffers, f.bike_buffer + 8u);
    passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 0),
        "a changed pointer cannot inherit another buffer's allocation certificate");
    f.live = original;
    store->observe_allocation(f.live.data(), f.bike_node, 0u, 0x100u);
    passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 0) &&
        !store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 1),
        "exact four-matrix allocations reject six-matrix expansion in either destination slot");
    f.register_allocations(*store);
    f.prepare();
    // Naturally tier-zero halves are still fully validated; no old shortcut.
    write_u16(f.live.data(), f.bike_node + actor_scene::selected_lod, 0u);
    write_u32(f.live.data(), f.bike_node + actor_scene::current_model, f.bike_graph);
    write_u32(f.live.data(), f.bike_node + actor_scene::display_lists, 0u);
    passed &= check(!store->can_prepare(f.live.data(), f.bike_node, f.rider_node, 0, 0),
        "naturally detailed actor still requires coherent resources");
    // Reproduce the long-session registry leak with entirely different node
    // addresses. Twenty-eight simultaneous certificates are legitimate; old
    // scenes must release them so the next full field can enter the registry.
    {
        Fixture lifetime;
        auto registry = std::make_unique<SnapshotStore>();
        auto* m = lifetime.live.data();
        registry->reset(m);
        for (unsigned race = 0; race < 128u; ++race) {
            std::array<unsigned, maximum_actors> nodes{};
            for (unsigned i = 0; i < nodes.size(); ++i) {
                const unsigned node = 0x80140000u + (race % 4u) * 0x8000u + i * 0x200u;
                nodes[i] = node;
                std::memcpy(m + node - kRdramBegin, m + Fixture::bike_node - kRdramBegin, 0x148u);
                passed &= check(registry->observe_allocation(m, node, 0u, 6u * 64u),
                    "every racer certificate admitted across 128 retired scene generations");
            }
            passed &= check(!registry->observe_allocation(m, Fixture::bike_node, 0u, 6u * 64u),
                "live full registry still refuses overflow instead of evicting an active actor");
            const auto before = lifetime.live;
            for (auto node : nodes) {
                passed &= check(registry->forget_allocation(m, node), "destructor retires exact node certificate");
                passed &= check(!registry->forget_allocation(m, node), "duplicate retirement is harmless");
            }
            passed &= check(before == lifetime.live, "retirement changes no guest memory");
        }
        lifetime.register_allocations(*registry);
        passed &= check(registry->can_prepare(m, Fixture::bike_node, Fixture::rider_node, 0u, 0u),
            "real detailed racer remains eligible after repeated full scene retirement");
        registry->forget_allocation(m, Fixture::rider_node);
        passed &= check(!registry->can_prepare(m, Fixture::bike_node, Fixture::rider_node, 0u, 0u),
            "freed actor cannot inherit its old allocation certificate even with unchanged bytes");
    }
    passed &= check_visual_attachment();
    passed &= check_crash_attachment();
    passed &= check_results_scenes();
    passed &= check_split_screen();
    std::puts(passed ? "[RR64-LOD-TEST] PASS" : "[RR64-LOD-TEST] FAIL");
    return passed ? 0 : 1;
}
