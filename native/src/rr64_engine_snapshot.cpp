#include "rr64_engine_snapshot.hpp"

#include "rr64_actor_pose.hpp"

namespace rr64::engine {
namespace {

constexpr std::uint64_t kPoseFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kPoseFnvPrime = 1099511628211ull;

void hash_pose_word(std::uint64_t &hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= kPoseFnvPrime;
    }
}

bool capture_segment_payload_hash(unsigned char *rdram, std::uint32_t node, std::uint32_t lod,
                                  std::uint16_t draw_count, std::uint64_t &payload_hash) {
    constexpr std::uint32_t pointer_size = sizeof(std::uint32_t);
    constexpr std::uint32_t payload_offset = actor_scene::segment_record_payload;
    constexpr std::uint32_t payload_size = actor_scene::render_transform_size;
    constexpr std::uint16_t maximum_segment_count = actor_scene::maximum_segment_records_per_tier;
    if (lod >= actor_scene::lod_record_count || draw_count == 0u ||
        draw_count > maximum_segment_count) {
        return false;
    }

    std::uint64_t hash = kPoseFnvOffset;
    hash_pose_word(hash, draw_count);
    const std::uint32_t table =
        node + actor_scene::primary_segment_records + lod * actor_scene::segment_table_stride;
    for (std::uint32_t record = 0; record < draw_count; ++record) {
        std::uint32_t pose_record = 0;
        if (!read_u32(rdram, table + record * pointer_size, pose_record) ||
            !valid_guest_range(pose_record, payload_offset + payload_size)) {
            return false;
        }
        for (std::uint32_t offset = 0; offset < payload_size; offset += pointer_size) {
            std::uint32_t word = 0;
            if (!read_u32(rdram, pose_record + payload_offset + offset, word)) {
                return false;
            }
            hash_pose_word(hash, word);
        }
    }

    payload_hash = hash;
    return true;
}

bool capture_transform_buffer_hash(unsigned char *rdram, std::uint32_t transform_buffer,
                                   std::uint16_t transform_count, std::uint64_t &transform_hash) {
    if (transform_count == 0u || transform_count > actor_scene::maximum_render_transforms ||
        !valid_guest_range(transform_buffer, static_cast<std::uint32_t>(transform_count) *
                                                 actor_scene::render_transform_size)) {
        return false;
    }

    std::uint64_t hash = kPoseFnvOffset;
    hash_pose_word(hash, transform_count);
    const std::uint32_t byte_count =
        static_cast<std::uint32_t>(transform_count) * actor_scene::render_transform_size;
    for (std::uint32_t offset = 0; offset < byte_count; offset += sizeof(std::uint32_t)) {
        std::uint32_t word = 0;
        if (!read_u32(rdram, transform_buffer + offset, word)) {
            return false;
        }
        hash_pose_word(hash, word);
    }
    transform_hash = hash;
    return true;
}

bool read_vec3(unsigned char *rdram, std::uint32_t address, Vec3Snapshot &value) {
    return read_float(rdram, address + 0u, value.x) && read_float(rdram, address + 4u, value.y) &&
           read_float(rdram, address + 8u, value.z);
}

bool capture_bike_pose(unsigned char *rdram, std::uint32_t address, BikePoseSnapshot &pose) {
    pose.valid = read_vec3(rdram, address + bike::front_wheel_position, pose.front_wheel) &&
                 read_vec3(rdram, address + bike::body_position, pose.body) &&
                 read_vec3(rdram, address + bike::rear_wheel_position, pose.rear_wheel);
    return pose.valid;
}

bool capture_model_lod(unsigned char *rdram, std::uint32_t node, std::uint32_t viewport,
                       std::uint32_t render_buffer_slot, ModelLodSnapshot &snapshot) {
    snapshot = {};
    if (viewport >= actor_scene::maximum_viewports ||
        !valid_guest_range(node, actor_scene::node_minimum_size)) {
        return false;
    }

    snapshot.node = node;
    bool valid = read_u32(rdram, node + actor_scene::type, snapshot.type) &&
                 read_u32(rdram, node + actor_scene::entity, snapshot.entity) &&
                 read_u32(rdram, node + actor_scene::current_model, snapshot.current_model) &&
                 read_u16(rdram, node + actor_scene::selected_lod, snapshot.selected_lod) &&
                 read_u16(rdram, node + actor_scene::previous_lod, snapshot.previous_lod) &&
                 read_float(rdram, node + actor_scene::viewport_depth + viewport * sizeof(float),
                            snapshot.viewport_depth);

    for (std::uint32_t lod = 0; lod < actor_scene::lod_record_count; ++lod) {
        valid =
            valid &&
            read_u32(rdram, node + actor_scene::lod_models + lod * actor_scene::lod_pointer_stride,
                     snapshot.models[lod]) &&
            read_u32(rdram,
                     node + actor_scene::display_lists + lod * actor_scene::lod_pointer_stride,
                     snapshot.display_lists[lod]) &&
            read_u16(rdram, node + actor_scene::segment_counts + lod * sizeof(std::uint16_t),
                     snapshot.segment_counts[lod]) &&
            read_u32(rdram,
                     node + actor_scene::primary_segment_records +
                         lod * actor_scene::segment_table_stride,
                     snapshot.primary_segment_tables[lod]) &&
            read_u32(rdram,
                     node + actor_scene::resolved_segment_payloads +
                         lod * actor_scene::segment_table_stride,
                     snapshot.resolved_segment_tables[lod]);
        snapshot.segment_payloads_valid[lod] = capture_segment_payload_hash(
            rdram, node, lod, snapshot.segment_counts[lod], snapshot.segment_payload_hashes[lod]);
        ModelGraphTopologySnapshot topology{};
        snapshot.model_graphs_valid[lod] =
            capture_model_graph_topology(rdram, snapshot.models[lod], topology);
        snapshot.transform_counts[lod] = topology.transform_count;
        snapshot.model_graph_hashes[lod] = topology.topology_hash;
        snapshot.model_allocation_hashes[lod] = topology.allocation_hash;
    }

    if (snapshot.selected_lod < actor_scene::lod_record_count &&
        render_buffer_slot < actor_scene::render_buffer_slot_count) {
        const std::uint32_t transform_pointer_address =
            node + actor_scene::render_transform_buffers +
            viewport * actor_scene::render_buffer_viewport_stride +
            render_buffer_slot * actor_scene::render_buffer_slot_stride;
        if (read_u32(rdram, transform_pointer_address, snapshot.selected_transform_buffer)) {
            snapshot.selected_transform_buffer_valid = capture_transform_buffer_hash(
                rdram, snapshot.selected_transform_buffer,
                snapshot.transform_counts[snapshot.selected_lod], snapshot.selected_transform_hash);
        }
    }

    snapshot.valid = valid && snapshot.selected_lod < actor_scene::lod_record_count &&
                     snapshot.previous_lod < actor_scene::lod_record_count &&
                     valid_guest_range(snapshot.entity, sizeof(std::uint32_t));
    return snapshot.valid;
}

ModelRenderProxySnapshot build_render_proxy(const ModelLodSnapshot &source) {
    ModelRenderProxySnapshot proxy{};
    proxy.stock_lod = source.selected_lod;
    proxy.desired_lod = 0u;
    proxy.current_model = source.current_model;

    if (!source.valid || source.selected_lod >= actor_scene::lod_record_count) {
        return proxy;
    }

    proxy.desired_model = source.models[proxy.desired_lod];
    proxy.desired_display_list = source.display_lists[proxy.desired_lod];
    proxy.desired_segment_count = source.segment_counts[proxy.desired_lod];
    proxy.desired_primary_segment_table = source.primary_segment_tables[proxy.desired_lod];
    proxy.desired_resolved_segment_table = source.resolved_segment_tables[proxy.desired_lod];
    proxy.desired_segments_valid = source.segment_payloads_valid[proxy.desired_lod];
    proxy.desired_segment_payload_hash = source.segment_payload_hashes[proxy.desired_lod];
    proxy.stock_segment_count = source.segment_counts[source.selected_lod];
    proxy.stock_segments_valid = source.segment_payloads_valid[source.selected_lod];
    proxy.stock_transform_count = source.transform_counts[source.selected_lod];
    proxy.desired_transform_count = source.transform_counts[proxy.desired_lod];
    proxy.stock_model_graph_hash = source.model_graph_hashes[source.selected_lod];
    proxy.desired_model_graph_hash = source.model_graph_hashes[proxy.desired_lod];
    proxy.selected_transform_buffer_valid = source.selected_transform_buffer_valid;
    proxy.selected_transform_hash = source.selected_transform_hash;
    proxy.stock_contract_coherent =
        source.current_model != 0u && source.current_model == source.models[source.selected_lod];

    // Each tier owns a 0x20-byte table of 32-bit segment-record pointers. A count beyond
    // eight would make the stock submission loop cross into the next tier.
    constexpr std::uint16_t maximum_tier_draw_count = actor_scene::maximum_segment_records_per_tier;
    proxy.metadata_valid = proxy.stock_contract_coherent && proxy.desired_model != 0u &&
                           proxy.desired_display_list != 0u && proxy.desired_segment_count > 0u &&
                           proxy.desired_segment_count <= maximum_tier_draw_count &&
                           proxy.desired_primary_segment_table != 0u &&
                           proxy.desired_segments_valid &&
                           source.model_graphs_valid[source.selected_lod] &&
                           source.model_graphs_valid[proxy.desired_lod];
    // func_80011CC0 rebuilds the secondary table from the primary pointers as
    // it emits each display list, so a zero secondary entry before rendering
    // is normal and cannot be used as an eligibility failure.
    proxy.direct_submission_safe = proxy.metadata_valid && proxy.stock_lod == proxy.desired_lod &&
                                   proxy.selected_transform_buffer_valid;
    const bool transform_allocation_compatible =
        proxy.stock_transform_count == proxy.desired_transform_count &&
        source.model_allocation_hashes[source.selected_lod] != 0u &&
        source.model_allocation_hashes[source.selected_lod] ==
            source.model_allocation_hashes[proxy.desired_lod];
    proxy.selected_transform_remap_safe = proxy.metadata_valid && !proxy.direct_submission_safe &&
                                          proxy.selected_transform_buffer_valid &&
                                          transform_allocation_compatible;
    proxy.requires_pose_rebuild = proxy.metadata_valid && !proxy.direct_submission_safe &&
                                  !proxy.selected_transform_remap_safe;
    return proxy;
}

ActorRenderProxySnapshot build_actor_render_proxy(const ModelLodSnapshot &bike,
                                                  const ModelLodSnapshot &rider) {
    ActorRenderProxySnapshot proxy{};
    proxy.bike = build_render_proxy(bike);
    proxy.rider = build_render_proxy(rider);
    proxy.metadata_valid = proxy.bike.metadata_valid && proxy.rider.metadata_valid;
    proxy.directly_renderable = proxy.metadata_valid && proxy.bike.direct_submission_safe &&
                                proxy.rider.direct_submission_safe;
    const bool bike_renderable =
        proxy.bike.direct_submission_safe || proxy.bike.selected_transform_remap_safe;
    const bool rider_renderable =
        proxy.rider.direct_submission_safe || proxy.rider.selected_transform_remap_safe;
    proxy.renderable_with_selected_transforms =
        proxy.metadata_valid && !proxy.directly_renderable && bike_renderable && rider_renderable;
    proxy.requires_pose_rebuild = proxy.metadata_valid && !proxy.directly_renderable &&
                                  !proxy.renderable_with_selected_transforms;
    return proxy;
}

} // namespace

bool capture_frame_snapshot(unsigned char *rdram, FrameSnapshot &snapshot) noexcept {
    snapshot = {};
    const bool fixed_state_valid =
        read_u32(rdram, globals::main_mode, snapshot.mode) &&
        read_u32(rdram, globals::multiplayer_stage, snapshot.multiplayer_stage) &&
        read_u32(rdram, globals::active_racer_count, snapshot.active_racers) &&
        read_u32(rdram, globals::bike_pool_pointer, snapshot.bike_pool) &&
        read_u16(rdram, globals::controller_buttons, snapshot.buttons) &&
        read_s8(rdram, globals::controller_stick_x, snapshot.stick_x) &&
        read_s8(rdram, globals::controller_stick_y, snapshot.stick_y);
    if (!fixed_state_valid) {
        return false;
    }

    if (!is_live_race_mode(snapshot.mode) || snapshot.active_racers == 0u ||
        snapshot.active_racers > kMaximumRacers ||
        !valid_guest_range(snapshot.bike_pool, snapshot.active_racers * bike::stride)) {
        return true;
    }

    for (std::uint32_t racer = 0; racer < snapshot.active_racers; ++racer) {
        BikePoseSnapshot &pose = snapshot.bikes[racer];
        const std::uint32_t bike_address = snapshot.bike_pool + racer * bike::stride;
        capture_bike_pose(rdram, bike_address, pose);
        if (pose.valid) {
            ++snapshot.valid_bikes;
        }
    }

    return true;
}

bool capture_actor_scene_snapshot(unsigned char *rdram, std::uint32_t viewport,
                                  ActorSceneSnapshot &snapshot) noexcept {
    snapshot = {};
    snapshot.viewport = viewport;
    const bool fixed_state_valid =
        read_u32(rdram, globals::main_mode, snapshot.mode) &&
        read_u32(rdram, globals::active_racer_count, snapshot.active_racers) &&
        read_u32(rdram, globals::bike_pool_pointer, snapshot.bike_pool) &&
        read_u32(rdram, globals::actor_scene_head, snapshot.scene_head) &&
        read_u32(rdram, globals::actor_render_buffer_slot, snapshot.render_buffer_slot);
    if (!fixed_state_valid || viewport >= actor_scene::maximum_viewports) {
        return false;
    }

    if (!is_live_race_mode(snapshot.mode) || snapshot.active_racers == 0u) {
        snapshot.graph_complete = true;
        return true;
    }
    if (snapshot.active_racers > kMaximumRacers ||
        !valid_guest_range(snapshot.bike_pool, snapshot.active_racers * bike::stride)) {
        return true;
    }

    std::array<std::uint32_t, kMaximumRacers> visited{};
    std::uint32_t visited_count = 0;
    std::uint32_t node = snapshot.scene_head;
    while (node != 0u && visited_count < visited.size()) {
        if (!valid_guest_range(node, actor_scene::node_minimum_size)) {
            return true;
        }
        for (std::uint32_t i = 0; i < visited_count; ++i) {
            if (visited[i] == node) {
                return true;
            }
        }
        visited[visited_count++] = node;

        std::uint32_t next = 0;
        std::uint32_t entity = 0;
        if (!read_u32(rdram, node + actor_scene::next, next) ||
            !read_u32(rdram, node + actor_scene::entity, entity)) {
            return true;
        }

        if (entity >= snapshot.bike_pool) {
            const std::uint32_t pool_offset = entity - snapshot.bike_pool;
            const std::uint32_t racer_index = pool_offset / bike::stride;
            if ((pool_offset % bike::stride) == 0u && racer_index < snapshot.active_racers) {
                ActorPresentationSnapshot &actor = snapshot.actors[racer_index];
                actor.racer_index = racer_index;
                capture_bike_pose(rdram, entity, actor.physics);
                capture_model_lod(rdram, node, viewport, snapshot.render_buffer_slot, actor.bike);

                std::uint32_t model_state = 0;
                std::uint32_t pose_owner = 0;
                std::uint32_t rider_node = 0;
                if (read_u32(rdram, entity + bike::model_state_pointer, model_state) &&
                    valid_guest_range(model_state + actor_scene::model_state_pose_owner,
                                      sizeof(std::uint32_t)) &&
                    read_u32(rdram, model_state + actor_scene::model_state_pose_owner,
                             pose_owner) &&
                    valid_guest_range(pose_owner + actor_scene::pose_owner_rider_node,
                                      sizeof(std::uint32_t)) &&
                    read_u32(rdram, pose_owner + actor_scene::pose_owner_rider_node, rider_node)) {
                    capture_model_lod(rdram, rider_node, viewport, snapshot.render_buffer_slot,
                                      actor.rider);
                }

                actor.valid = actor.physics.valid && actor.bike.valid && actor.rider.valid &&
                              actor.bike.type == 1u && actor.rider.type == 2u &&
                              actor.bike.entity == entity;
                actor.render_proxy = build_actor_render_proxy(actor.bike, actor.rider);
                ++snapshot.matched_actors;
                if (actor.valid) {
                    ++snapshot.valid_actors;
                }
            }
        }

        node = next;
    }

    snapshot.graph_complete = node == 0u;
    return true;
}

} // namespace rr64::engine
