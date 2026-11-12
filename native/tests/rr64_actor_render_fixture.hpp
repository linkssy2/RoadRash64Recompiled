#pragma once

#include <cstdint>
#include <vector>

#include "rr64_actor_render_snapshot.hpp"

namespace rr64::lod::test {
struct Fixture {
    static constexpr std::uint32_t race_player_count = 0x800A6578u;
    static constexpr std::uint32_t bike_node = 0x80100000u;
    static constexpr std::uint32_t rider_node = 0x80100200u;
    static constexpr std::uint32_t bike_entity = 0x80110000u;
    static constexpr std::uint32_t rider_entity = 0x80111000u;
    static constexpr std::uint32_t model_state = 0x80112000u;
    static constexpr std::uint32_t owner = rider_entity;
    static constexpr std::uint32_t bike_graph = 0x80200000u;
    static constexpr std::uint32_t rider_graph = 0x80201000u;
    static constexpr std::uint32_t bike_pose = 0x80300000u;
    static constexpr std::uint32_t rider_pose = 0x80301000u;
    static constexpr std::uint32_t bike_source = 0x80310000u;
    static constexpr std::uint32_t rider_source = 0x80311000u;
    static constexpr std::uint32_t bike_buffer = 0x80400000u;
    static constexpr std::uint32_t rider_buffer = 0x80401000u;
    static constexpr std::uint32_t stack = 0x807f0000u;
    std::vector<unsigned char> live = std::vector<unsigned char>(engine::kRdramSize);
    std::vector<unsigned char> shadow;

    Fixture() {
        using namespace engine;
        auto* m = live.data();
        write_u32(m, globals::main_mode, 0x17u);
        write_u32(m, globals::pending_mode, 0x17u);
        // Keep the stale multiplayer-menu default while rendering a current
        // one-player race with an AI field, as the real setup functions do.
        write_u32(m, globals::active_racer_count, 2u);
        write_u32(m, race_player_count, 1u);
        write_u32(m, 0x800A656Cu, engine::kMaximumRacers);
        write_u32(m, 0x8009DB88u, 1u);
        write_u16(m, 0x800A65C4u, 1u);
        write_u32(m, globals::active_viewport, 0u);
        write_u32(m, globals::actor_render_buffer_slot, 0u);
        write_u32(m, globals::actor_scene_head, bike_node);
        write_u32(m, bike_node, 1u);
        write_u32(m, rider_node, 2u);
        write_u32(m, bike_node + 4u, bike_entity);
        write_u32(m, rider_node + 4u, rider_entity);
        write_u16(m, bike_entity + bike::rider_attached, 1u);
        write_u16(m, rider_entity + rider::bike_attached, 1u);
        write_u32(m, bike_entity + bike::rider_pointer, rider_entity);
        write_u32(m, rider_entity + rider::bike_pointer, bike_entity);
        write_u32(m, bike_entity + bike::model_state_pointer, model_state);
        write_u32(m, model_state, 4u); // AI outside the local-controller range.
        write_u32(m, model_state + actor_scene::model_state_pose_owner, owner);
        write_u32(m, owner + actor_scene::pose_owner_rider_node, rider_node);
        write_float(m, bike_entity + bike::front_wheel_position + 8u, 11.25f);
        write_float(m, bike_entity + bike::body_position + 8u, 12.5f);
        write_float(m, bike_entity + bike::rear_wheel_position + 8u, 13.75f);
        write_float(m, rider_entity + rider::front_contact_gap, -0.12f);
        write_float(m, rider_entity + rider::rear_contact_gap, -0.08f);
        for (unsigned a = 0; a < 2; ++a) {
            const auto node = a ? rider_node : bike_node;
            const auto graph = a ? rider_graph : bike_graph;
            const auto pose = a ? rider_pose : bike_pose;
            const auto source = a ? rider_source : bike_source;
            const auto buffer = a ? rider_buffer : bike_buffer;
            write_u16(m, node + actor_scene::selected_lod, 2u);
            write_u16(m, node + actor_scene::previous_lod, 0u);
            write_u32(m, node + actor_scene::current_model, graph + 0x200u);
            for (unsigned lod = 0; lod < 3; ++lod) {
                const auto model = graph + lod * 0x100u;
                const auto count = lod == 2u ? 2u : 4u; // 4 vs 6 matrices.
                write_u32(m, node + actor_scene::lod_models + lod * 4u, model);
                write_u32(m, node + actor_scene::display_lists + lod * 4u,
                    source + 0x400u + lod * 0x80u);
                write_u32(m, node + 0x13cu + lod * 4u,
                    source + 0x408u + lod * 0x80u);
                write_u16(m, node + actor_scene::segment_counts + lod * 2u, 1u);
                write_u32(m, node + actor_scene::primary_segment_records + lod * 0x20u,
                    source + 0x800u + lod * 0x80u);
                for (unsigned i = 0; i < count; ++i) {
                    const auto record = model + i * 0x20u;
                    write_u16(m, record + 4u, i == 0u ? 0x13u : 0x12u);
                    write_u16(m, record + 8u, i + 1 == count ? 0u : 4u);
                    write_u32(m, record + 0x0cu, pose + lod * 0x100u + i * 0x20u);
                    write_u32(m, record + 0x14u, source + lod * 0x100u + i * 0x20u);
                    write_u16(m, source + lod * 0x100u + i * 0x20u + 0x12u, 0u);
                    write_float(m, pose + lod * 0x100u + i * 0x20u + 0x18u, 1.0f);
                }
            }
            for (unsigned view = 0; view < 4; ++view) {
                for (unsigned slot = 0; slot < 2; ++slot) {
                    write_u32(m, node + actor_scene::render_transform_buffers + view * 8u + slot * 4u,
                        buffer + (view * 2u + slot) * 0x180u);
                }
            }
        }
        write_float(m, 0x8009DBACu, 100.0f);
        shadow = live;
    }

    void register_allocations(SnapshotStore& store) {
        for (auto node : {bike_node, rider_node}) {
            for (unsigned view = 0; view < 4; ++view) {
                store.observe_allocation(live.data(), node, view, 0x180u);
            }
        }
    }

    void prepare() {
        using namespace engine;
        shadow = live;
        for (unsigned a = 0; a < 2; ++a) {
            const auto node = a ? rider_node : bike_node;
            const auto graph = a ? rider_graph : bike_graph;
            const auto pose = a ? rider_pose : bike_pose;
            write_u32(shadow.data(), node + actor_scene::current_model, graph);
            write_u16(shadow.data(), node + actor_scene::selected_lod, 0u);
            for (unsigned i = 0; i < 4; ++i) {
                write_float(shadow.data(), pose + i * 0x20u, i ? 0.5f : (a ? 21.0f : 20.0f));
                write_float(shadow.data(), pose + i * 0x20u + 8u, i ? 0.0f : 30.0f);
            }
        }
    }
};
} // namespace rr64::lod::test
