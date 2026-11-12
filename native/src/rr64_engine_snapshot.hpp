#pragma once

#include <array>
#include <cstdint>

#include "rr64_engine_layout.hpp"

namespace rr64::engine {

struct Vec3Snapshot {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BikePoseSnapshot {
    bool valid = false;
    Vec3Snapshot front_wheel{};
    Vec3Snapshot body{};
    Vec3Snapshot rear_wheel{};
};

struct FrameSnapshot {
    std::uint32_t mode = 0;
    std::uint32_t multiplayer_stage = 0;
    std::uint32_t active_racers = 0;
    std::uint32_t bike_pool = 0;
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0;
    std::int8_t stick_y = 0;
    std::uint32_t valid_bikes = 0;
    std::array<BikePoseSnapshot, kMaximumRacers> bikes{};
};

struct ModelLodSnapshot {
    bool valid = false;
    std::uint32_t node = 0;
    std::uint32_t type = 0;
    std::uint32_t entity = 0;
    std::uint32_t current_model = 0;
    std::uint16_t selected_lod = 0;
    std::uint16_t previous_lod = 0;
    float viewport_depth = 0.0f;
    std::array<std::uint32_t, actor_scene::lod_record_count> models{};
    std::array<std::uint32_t, actor_scene::lod_record_count> display_lists{};
    std::array<std::uint16_t, actor_scene::lod_record_count> segment_counts{};
    std::array<std::uint32_t, actor_scene::lod_record_count> primary_segment_tables{};
    std::array<std::uint32_t, actor_scene::lod_record_count> resolved_segment_tables{};
    std::array<bool, actor_scene::lod_record_count> segment_payloads_valid{};
    std::array<std::uint64_t, actor_scene::lod_record_count> segment_payload_hashes{};
    std::array<bool, actor_scene::lod_record_count> model_graphs_valid{};
    std::array<std::uint16_t, actor_scene::lod_record_count> transform_counts{};
    std::array<std::uint64_t, actor_scene::lod_record_count> model_graph_hashes{};
    std::array<std::uint64_t, actor_scene::lod_record_count> model_allocation_hashes{};
    std::uint32_t selected_transform_buffer = 0;
    bool selected_transform_buffer_valid = false;
    std::uint64_t selected_transform_hash = 0;
};

// A read-only plan for a future native actor renderer. Tier zero is the
// requested high-detail presentation, while stock simulation, stock LOD
// selection, and the selected tier's dynamic transform generation remain
// authoritative. A different tier-zero display list/segment table may reuse
// that selected transform buffer only when the ordered transform-allocation
// contract matches. Incompatible cases require a native pose rebuild.
struct ModelRenderProxySnapshot {
    bool metadata_valid = false;
    bool stock_contract_coherent = false;
    bool direct_submission_safe = false;
    bool selected_transform_remap_safe = false;
    bool requires_pose_rebuild = false;
    std::uint16_t stock_lod = 0;
    std::uint16_t desired_lod = 0;
    std::uint32_t current_model = 0;
    std::uint32_t desired_model = 0;
    std::uint32_t desired_display_list = 0;
    std::uint16_t desired_segment_count = 0;
    std::uint32_t desired_primary_segment_table = 0;
    std::uint32_t desired_resolved_segment_table = 0;
    bool desired_segments_valid = false;
    std::uint64_t desired_segment_payload_hash = 0;
    std::uint16_t stock_segment_count = 0;
    bool stock_segments_valid = false;
    std::uint16_t stock_transform_count = 0;
    std::uint16_t desired_transform_count = 0;
    std::uint64_t stock_model_graph_hash = 0;
    std::uint64_t desired_model_graph_hash = 0;
    bool selected_transform_buffer_valid = false;
    std::uint64_t selected_transform_hash = 0;
};

struct ActorRenderProxySnapshot {
    bool metadata_valid = false;
    bool directly_renderable = false;
    bool renderable_with_selected_transforms = false;
    bool requires_pose_rebuild = false;
    ModelRenderProxySnapshot bike{};
    ModelRenderProxySnapshot rider{};
};

struct ActorPresentationSnapshot {
    bool valid = false;
    std::uint32_t racer_index = 0;
    BikePoseSnapshot physics{};
    ModelLodSnapshot bike{};
    ModelLodSnapshot rider{};
    ActorRenderProxySnapshot render_proxy{};
};

struct ActorSceneSnapshot {
    std::uint32_t mode = 0;
    std::uint32_t viewport = 0;
    std::uint32_t render_buffer_slot = 0;
    std::uint32_t active_racers = 0;
    std::uint32_t bike_pool = 0;
    std::uint32_t scene_head = 0;
    std::uint32_t matched_actors = 0;
    std::uint32_t valid_actors = 0;
    bool graph_complete = false;
    std::array<ActorPresentationSnapshot, kMaximumRacers> actors{};
};

// Captures one coherent, read-only view after the original update callback.
// A false return means the fixed frame globals could not be read. Individual
// bike entries remain invalid when their owning pool is unavailable.
bool capture_frame_snapshot(unsigned char *rdram, FrameSnapshot &snapshot) noexcept;

// Captures the render-side actor graph after the original scene preparation
// pass. The scene list is bounded and cycle-checked, and every bike node is
// matched back to its authoritative physics record before it is exposed.
bool capture_actor_scene_snapshot(unsigned char *rdram, std::uint32_t viewport,
                                  ActorSceneSnapshot &snapshot) noexcept;

} // namespace rr64::engine
