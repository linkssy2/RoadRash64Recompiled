#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

#include "rr64_actor_pose.hpp"
#include "rr64_actor_presentation.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_engine_snapshot.hpp"
#include "rr64_native.hpp"
#include "rr64_terrain_residency.hpp"
#include "rr64_terrain_snapshot.hpp"

namespace {
bool check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[RR64-ENGINE-TEST] FAILED: %s\n", message);
    }
    return condition;
}
} // namespace

int main() {
    using namespace rr64::engine;

    bool passed = true;
    passed &= check(valid_guest_range(kRdramBegin, 1), "RDRAM first byte");
    passed &= check(valid_guest_range(kRdramBegin, kRdramSize), "complete RDRAM range");
    passed &= check(valid_guest_range(kRdramEnd - 4u, 4u), "RDRAM final word");
    passed &= check(!valid_guest_range(kRdramBegin - 1u, 1u), "address below RDRAM");
    passed &= check(!valid_guest_range(kRdramEnd, 1u), "address at RDRAM end");
    passed &= check(!valid_guest_range(kRdramBegin, kRdramSize + 1u), "oversized RDRAM range");

    constexpr std::uint32_t live_modes[] = {
        0x09u, 0x0Au, 0x12u, 0x13u, 0x17u, 0x18u, 0x1Cu, 0x1Du,
    };
    for (std::uint32_t mode : live_modes) {
        passed &= check(is_live_race_mode(mode), "verified live-race mode");
    }
    passed &= check(!is_live_race_mode(0x00u), "mode 0 is not live gameplay");
    passed &= check(!is_live_race_mode(0x0Bu), "race-adjacent mode is not live gameplay");
    passed &= check(!is_live_race_mode(0x23u), "menu mode is not live gameplay");
    constexpr std::uint32_t results_modes[] = {0x0Bu, 0x14u, 0x19u, 0x1Eu};
    for (std::uint32_t mode : results_modes) {
        passed &= check(is_race_results_mode(mode), "verified final-standings mode");
        passed &= check(!is_live_race_mode(mode),
            "final standings remain outside live simulation feedback");
        passed &= check(is_race_shortcut_scene_transition(mode, mode),
            "final standings retain gameplay shortcut access");
    }
    passed &= check(!is_race_results_mode(0x20u) &&
        !is_race_shortcut_scene_transition(0x14u, 0x20u),
        "ordinary menu transitions do not inherit race shortcuts");
    passed &= check(is_live_race_transition(0x09u, 0x0Au),
        "rumble remains enabled across live-race mode transitions");
    passed &= check(!is_live_race_transition(0x09u, 0x20u) &&
        !is_live_race_transition(0x20u, 0x09u),
        "rumble is disabled on both sides of a menu transition");
    passed &= check(is_gameplay_feedback_active(0x17u, 0x17u, 0u),
        "audio and rumble remain active during unpaused gameplay");
    passed &= check(!is_gameplay_feedback_active(0x17u, 0x17u, 1u),
        "audio and rumble stop while the original pause menu is active");
    passed &= check(!is_gameplay_feedback_active(0x17u, 0x20u, 0u),
        "audio and rumble stop while leaving gameplay");
    passed &= check(are_gameplay_shortcuts_active(0x17u, 0x17u, 0u),
        "L3 and R3 remain available during a live race or results wait");
    passed &= check(are_gameplay_shortcuts_active(0x14u, 0x14u, 0u),
        "L3 and R3 remain available on the final standings screen");
    passed &= check(!are_gameplay_shortcuts_active(0x14u, 0x14u, 1u),
        "L3 and R3 stop if the real pause overlay covers final standings");
    passed &= check(!are_gameplay_shortcuts_active(0x17u, 0x17u, 1u) &&
        !are_gameplay_shortcuts_active(0x17u, 0x20u, 0u) &&
        !are_gameplay_shortcuts_active(0x14u, 0x20u, 0u),
        "L3 and R3 stop over the pause menu and while leaving gameplay");
    passed &= check(valid_guest_range(globals::gameplay_pause_state, sizeof(std::uint16_t)),
        "gameplay pause state is inside RDRAM");
    passed &= check(valid_guest_range(globals::pause_menu_state, sizeof(std::uint16_t)),
        "original pause-menu state is inside RDRAM");
    passed &= check(bike_accepts_drive_control(0u),
        "road rumble is allowed while the bike accepts drive input");
    passed &= check(!bike_accepts_drive_control(1u),
        "road rumble is suppressed during crash and recovery lockout");
    passed &= check(rider_can_manual_eject(1u, 1u, 0u),
        "mounted rider may eject independently of drive control");
    passed &= check(rider_can_manual_eject(1u, 0u, 0u),
        "finish wait may clear rider controls while retaining the visual bike link");
    passed &= check(!rider_can_manual_eject(0u, 1u, 0u) &&
        !rider_can_manual_eject(1u, 1u, 1u),
        "detached or already-ejected rider cannot trigger another eject");
    passed &= check(!horizontal_motion_allows_road_rumble(0.0f, 0.0f) &&
        !horizontal_motion_allows_road_rumble(0.01f, 0.01f),
        "stationary result-wait jitter cannot produce road rumble");
    passed &= check(horizontal_motion_allows_road_rumble(0.03f, 0.0f),
        "real horizontal bike movement permits road rumble");
    passed &= check(drive_rumble_allowed(0u, false, false),
        "stationary throttle may rumble on the starting grid");
    passed &= check(drive_rumble_allowed(0u, true, true),
        "moving bike may rumble after the race starts");
    passed &= check(!drive_rumble_allowed(0u, false, true),
        "stationary result wait cannot rumble after the bike has moved");
    passed &= check(!drive_rumble_allowed(1u, true, false),
        "drive lockout suppresses rumble even before first movement");
    passed &= check(rider_has_fists_selected(1u) && !rider_has_fists_selected(2u),
        "weapon selection distinguishes fists from equipped weapons");
    passed &= check(
        bike::durability_current == 0x4F8u && bike::durability_capacity == 0x4FCu,
        "bike HUD durability pair remains canonical");
    passed &= check(
        bike::rider_attached == 0x7F8u && bike::rider_pointer == 0x800u &&
        rider::bike_attached == 0x57Cu && rider::ejected == 0x57Eu &&
        rider::selected_weapon == 0x5B0u,
        "local rider attachment and weapon-selection paths remain canonical");
    passed &= check(is_valid_mode(0x00u), "first mode-table record");
    passed &= check(is_valid_mode(0x39u), "transition mode-table record");
    passed &= check(!is_valid_mode(0x3Au), "first value beyond mode table");
    passed &= check(kModeRecordCount * kModeRecordSize == 0x570u, "decoded mode-table extent");

    TerrainCandidateBounds retention_bounds{};
    retention_bounds.valid = true;
    retention_bounds.minimum_row = 2u;
    retention_bounds.maximum_row = 4u;
    retention_bounds.minimum_column = 2u;
    retention_bounds.maximum_column = 4u;
    passed &= check(should_retain_terrain_presentation_cell(
            true, true, terrain::ready_state, 7u, 1u, 3u,
            retention_bounds, 30u),
        "ready terrain in the presentation ring may be retained");
    passed &= check(!should_retain_terrain_presentation_cell(
            true, true, terrain::ready_state, 7u, 0u, 3u,
            retention_bounds, 30u) &&
        !should_retain_terrain_presentation_cell(
            true, true, terrain::ready_state, 7u, 3u, 3u,
            retention_bounds, 30u),
        "terrain outside the cache ring and inside the active candidate is untouched");
    passed &= check(!should_retain_terrain_presentation_cell(
            false, true, terrain::ready_state, 7u, 1u, 3u,
            retention_bounds, 30u) &&
        !should_retain_terrain_presentation_cell(
            true, true, 4u, 7u, 1u, 3u,
            retention_bounds, 30u) &&
        !should_retain_terrain_presentation_cell(
            true, true, terrain::ready_state, 7u, 1u, 3u,
            retention_bounds,
            terrain::display_list_slot_count - kTerrainPresentationReservedSlots),
        "terrain cache preserves stock fallback, ready-state, and resource headroom");
    passed &= check(
        terrain_cell_in_presentation_ring(7u, 1u, 3u, retention_bounds) &&
        !terrain_cell_in_presentation_ring(7u, 3u, 3u, retention_bounds) &&
        !terrain_cell_in_presentation_ring(7u, 0u, 3u, retention_bounds),
        "terrain renderer and retention share the one-cell safety ring");
    passed &= check(valid_guest_range(globals::bike_pool_pointer, sizeof(std::uint32_t)), "bike pool pointer global");
    passed &= check(valid_guest_range(globals::pending_mode, sizeof(std::uint32_t)), "pending mode global");
    passed &= check(valid_guest_range(globals::terrain_cell_grid, sizeof(std::uint32_t)), "terrain grid global");
    passed &= check(bike::stride * kMaximumRacers < kRdramSize, "bike pool capacity");
    passed &= check(bike::drive_control_lockout + sizeof(std::uint16_t) <= bike::stride,
        "bike drive-control lockout is inside the bike record");
    passed &= check(rider::stride * kMaximumRacers < kRdramSize, "rider pool capacity");

    std::vector<unsigned char> rdram(kRdramSize);
    constexpr std::uint32_t test_matrix = 0x800F0000u;
    for (std::uint32_t component = 0; component < 16u; ++component) {
        write_u16(rdram.data(), test_matrix + component * 2u,
            (component % 5u) == 0u ? 1u : 0u);
        write_u16(rdram.data(), test_matrix + 0x20u + component * 2u, 0u);
    }
    // The translation row deliberately exercises positive and negative
    // fractional values in the N64 split-integer matrix representation.
    write_u16(rdram.data(), test_matrix + 12u * 2u, 3u);
    write_u16(rdram.data(), test_matrix + 0x20u + 12u * 2u, 0x8000u);
    write_u16(rdram.data(), test_matrix + 13u * 2u, 0xFFFFu);
    write_u16(rdram.data(), test_matrix + 0x20u + 13u * 2u, 0x8000u);

    Matrix4x4Snapshot decoded_matrix{};
    passed &= check(decode_n64_matrix(rdram.data(), test_matrix, decoded_matrix) &&
        decoded_matrix.valid && decoded_matrix.at(0u, 0u) == 1.0f &&
        decoded_matrix.at(3u, 0u) == 3.5f && decoded_matrix.at(3u, 1u) == -0.5f,
        "N64 split 16.16 matrix decoding");
    passed &= check(!decode_n64_matrix(rdram.data(), kRdramEnd - 0x20u, decoded_matrix) &&
        !decoded_matrix.valid, "N64 matrix range rejection");

    constexpr std::uint32_t graph_a = 0x800F1000u;
    constexpr std::uint32_t graph_b = 0x800F2000u;
    constexpr std::int16_t graph_a_delta = 4;
    constexpr std::int16_t graph_b_delta = 6;
    for (const auto [graph, graph_delta] : {
            std::pair<std::uint32_t, std::int16_t>{graph_a, graph_a_delta},
            std::pair<std::uint32_t, std::int16_t>{graph_b, graph_b_delta}})
    {
        write_u16(rdram.data(), graph + actor_scene::model_record_type,
            actor_scene::model_record_single_transform);
        write_u16(rdram.data(), graph + actor_scene::model_record_next_delta,
            static_cast<std::uint16_t>(graph_delta));
        write_u16(rdram.data(), graph + graph_delta * actor_scene::model_record_stride +
            actor_scene::model_record_type, actor_scene::model_record_triple_transform);
        write_u16(rdram.data(), graph + graph_delta * actor_scene::model_record_stride +
            actor_scene::model_record_next_delta, 0u);
    }
    ModelGraphTopologySnapshot topology_a{};
    ModelGraphTopologySnapshot topology_b{};
    passed &= check(capture_model_graph_topology(rdram.data(), graph_a, topology_a) &&
        topology_a.record_count == 2u && topology_a.transform_count == 4u &&
        topology_a.records[0].first_transform == 0u &&
        topology_a.records[1].first_transform == 1u &&
        topology_a.records[1].transform_count == 3u,
        "bounded model topology and transform allocation");
    passed &= check(capture_model_graph_topology(rdram.data(), graph_b, topology_b) &&
        !compatible_transform_topology(topology_a, topology_b) &&
        compatible_transform_allocation(topology_a, topology_b) &&
        topology_a.allocation_hash == topology_b.allocation_hash,
        "record stride differs without changing transform allocation");
    write_u16(rdram.data(), graph_b + graph_b_delta * actor_scene::model_record_stride +
        actor_scene::model_record_type, actor_scene::model_record_single_transform);
    passed &= check(capture_model_graph_topology(rdram.data(), graph_b, topology_b) &&
        !compatible_transform_allocation(topology_a, topology_b),
        "equal record count does not imply transform-allocation compatibility");

    RenderTransformSetSnapshot transform_set{};
    passed &= check(capture_render_transform_set(
            rdram.data(), test_matrix, 1u, transform_set) && transform_set.valid &&
        transform_set.transform_count == 1u &&
        transform_set.transforms[0].at(3u, 0u) == 3.5f,
        "bounded decoded render-transform set");
    passed &= check(!capture_render_transform_set(
            rdram.data(), test_matrix, 0u, transform_set) && !transform_set.valid,
        "empty render-transform set rejection");

    constexpr std::uint32_t test_bike_pool = 0x80100000u;
    write_u32(rdram.data(), globals::main_mode, 0x17u);
    write_u32(rdram.data(), globals::multiplayer_stage, 3u);
    write_u32(rdram.data(), globals::active_racer_count, 1u);
    write_u32(rdram.data(), globals::bike_pool_pointer, test_bike_pool);
    write_u16(rdram.data(), globals::controller_buttons, 0x2000u);
    write_s8(rdram.data(), globals::controller_stick_x, 32);
    write_s8(rdram.data(), globals::controller_stick_y, -16);
    write_float(rdram.data(), test_bike_pool + bike::front_wheel_position, 1.0f);
    write_float(rdram.data(), test_bike_pool + bike::body_position + 4u, 2.0f);
    write_float(rdram.data(), test_bike_pool + bike::rear_wheel_position + 8u, 3.0f);

    FrameSnapshot snapshot{};
    passed &= check(capture_frame_snapshot(rdram.data(), snapshot), "typed frame snapshot capture");
    passed &= check(snapshot.mode == 0x17u && snapshot.active_racers == 1u, "snapshot fixed state");
    passed &= check(snapshot.buttons == 0x2000u && snapshot.stick_x == 32 && snapshot.stick_y == -16,
        "snapshot controller state");
    passed &= check(snapshot.valid_bikes == 1u && snapshot.bikes[0].valid, "snapshot bike validity");
    passed &= check(snapshot.bikes[0].front_wheel.x == 1.0f && snapshot.bikes[0].body.y == 2.0f &&
        snapshot.bikes[0].rear_wheel.z == 3.0f, "snapshot bike pose");

    constexpr std::uint32_t terrain_grid = 0x801A0000u;
    constexpr std::uint32_t terrain_record = 0x801A1000u;
    constexpr std::uint32_t terrain_payload_0 = 0x801A2000u;
    constexpr std::uint32_t terrain_payload_1 = 0x801A2100u;
    constexpr std::uint32_t terrain_list_0 = 0x801A3000u;
    constexpr std::uint32_t terrain_list_1 = 0x801A3100u;
    write_u32(rdram.data(), globals::terrain_cell_grid, terrain_grid);
    write_u32(rdram.data(), globals::terrain_current_record, terrain_record);
    write_u32(rdram.data(), globals::terrain_map_width, 2u);
    write_float(rdram.data(), globals::terrain_camera_position, 0.0f);
    write_float(rdram.data(), globals::terrain_camera_position + sizeof(float), 0.0f);
    write_u32(rdram.data(), terrain_record + terrain::candidate_count, 2u);
    write_u32(rdram.data(), terrain_record, terrain_grid);
    write_u32(rdram.data(), terrain_record + sizeof(std::uint32_t),
        terrain_grid + 2u * terrain::cell_stride);

    write_u32(rdram.data(), terrain_grid + terrain::cell_payload, terrain_payload_0);
    write_u32(rdram.data(), terrain_grid + terrain::cell_request_epoch, 7u);
    write_s8(rdram.data(), terrain_grid + terrain::cell_state,
        static_cast<std::int8_t>(terrain::ready_state));
    write_s8(rdram.data(), terrain_grid + terrain::cell_display_list_slot, 2);
    write_float(rdram.data(), terrain_payload_0 + terrain::payload_world_x, 30.0f);
    write_float(rdram.data(), terrain_payload_0 + terrain::payload_world_z, 40.0f);

    const std::uint32_t terrain_cell_1 = terrain_grid + terrain::cell_stride;
    write_u32(rdram.data(), terrain_cell_1 + terrain::cell_payload, terrain_payload_1);
    write_u32(rdram.data(), terrain_cell_1 + terrain::cell_request_epoch, 8u);
    write_s8(rdram.data(), terrain_cell_1 + terrain::cell_state,
        static_cast<std::int8_t>(terrain::ready_state));
    write_s8(rdram.data(), terrain_cell_1 + terrain::cell_display_list_slot, 3);
    write_float(rdram.data(), terrain_payload_1 + terrain::payload_world_x, 60.0f);
    write_float(rdram.data(), terrain_payload_1 + terrain::payload_world_z, 80.0f);

    write_u32(rdram.data(), terrain_grid + 2u * terrain::cell_stride +
        terrain::cell_request_epoch, 9u);
    write_s8(rdram.data(), terrain_grid + 2u * terrain::cell_stride +
        terrain::cell_state, 3);
    write_u32(rdram.data(), globals::terrain_display_list_slots + 2u * sizeof(std::uint32_t),
        terrain_list_0);
    write_u32(rdram.data(), globals::terrain_display_list_slots + 3u * sizeof(std::uint32_t),
        terrain_list_1);

    TerrainSceneSnapshot terrain_snapshot{};
    passed &= check(capture_terrain_scene_snapshot(rdram.data(), terrain_snapshot) &&
        terrain_snapshot.valid && terrain_snapshot.map_width == 2u &&
        terrain_snapshot.cell_count == 4u && terrain_snapshot.candidate_count == 2u &&
        terrain_snapshot.candidate_ready == 1u && terrain_snapshot.ready_cells == 2u &&
        terrain_snapshot.candidate_min_row == 0u &&
        terrain_snapshot.candidate_max_row == 1u &&
        terrain_snapshot.candidate_min_column == 0u &&
        terrain_snapshot.candidate_max_column == 0u &&
        terrain_snapshot.cell_states[0] == terrain::ready_state &&
        terrain_snapshot.cell_states[2] == 3u &&
        terrain_snapshot.ready_resources == 2u &&
        terrain_snapshot.ready_in_candidate == 1u &&
        terrain_snapshot.ready_outside_candidate == 1u &&
        terrain_snapshot.maximum_ready_distance == 100.0f &&
        terrain_snapshot.maximum_candidate_distance == 50.0f &&
        terrain_snapshot.minimum_outside_candidate_distance == 100.0f,
        "terrain mirror preserves candidate, lifecycle, resource, and distance contracts");

    std::array<std::uint8_t, 49> prefetch_states{};
    prefetch_states.fill(1u);
    prefetch_states[1u * 7u + 1u] = 2u;
    TerrainPrefetchPlan prefetch_plan{};
    passed &= check(build_terrain_prefetch_plan(
            7u, 2u, 4u, 2u, 4u, prefetch_states, 70u, 8u, prefetch_plan) &&
        prefetch_plan.valid && prefetch_plan.complete &&
        prefetch_plan.ring_cells_considered == 16u &&
        prefetch_plan.ring_state_counts[1] == 15u &&
        prefetch_plan.ring_state_counts[2] == 1u &&
        prefetch_plan.eligible_cells == 15u &&
        prefetch_plan.available_resource_budget == 18u &&
        prefetch_plan.planned_cells == 15u,
        "terrain prefetch ring respects availability and reserved slot headroom");
    passed &= check(build_terrain_prefetch_plan(
            7u, 2u, 4u, 2u, 4u, prefetch_states, 80u, 8u, prefetch_plan) &&
        prefetch_plan.valid && !prefetch_plan.complete &&
        prefetch_plan.available_resource_budget == 8u &&
        prefetch_plan.planned_cells == 8u,
        "terrain prefetch ring truncates at its prepared-resource budget");
    passed &= check(build_terrain_prefetch_plan(
            7u, 0u, 1u, 0u, 1u, prefetch_states, 0u, 0u, prefetch_plan) &&
        prefetch_plan.valid && prefetch_plan.complete &&
        prefetch_plan.ring_cells_considered == 5u &&
        prefetch_plan.ring_state_counts[1] == 5u &&
        prefetch_plan.planned_cells == 5u,
        "terrain prefetch ring clips cleanly at map boundaries");
    passed &= check(!build_terrain_prefetch_plan(
            7u, 4u, 2u, 2u, 4u, prefetch_states, 0u, 0u, prefetch_plan),
        "terrain prefetch ring rejects inverted candidate bounds");

    constexpr std::uint32_t bike_node = 0x80120000u;
    constexpr std::uint32_t rider_node = 0x80120200u;
    constexpr std::uint32_t model_state = 0x80130000u;
    constexpr std::uint32_t pose_owner = 0x80130100u;
    constexpr std::uint32_t rider_entity = 0x80140000u;
    constexpr std::uint32_t bike_transform_buffer = 0x80170000u;
    constexpr std::uint32_t rider_transform_buffer = 0x80171000u;
    write_u32(rdram.data(), globals::actor_scene_head, bike_node);
    write_u32(rdram.data(), globals::actor_render_buffer_slot, 0u);
    write_u32(rdram.data(), bike_node + actor_scene::type, 1u);
    write_u32(rdram.data(), bike_node + actor_scene::entity, test_bike_pool);
    write_u32(rdram.data(), bike_node + actor_scene::next, 0u);
    write_u16(rdram.data(), bike_node + actor_scene::selected_lod, 2u);
    write_u16(rdram.data(), bike_node + actor_scene::previous_lod, 1u);
    write_float(rdram.data(), bike_node + actor_scene::viewport_depth, 42.0f);
    for (std::uint32_t lod = 0; lod < actor_scene::lod_record_count; ++lod) {
        const std::uint32_t model = 0x80150000u + lod * 0x100u;
        constexpr std::uint16_t second_record_delta = 4u;
        write_u32(rdram.data(), bike_node + actor_scene::lod_models + lod * 4u, model);
        write_u16(rdram.data(), model + actor_scene::model_record_type,
            lod == 2u ? actor_scene::model_record_single_transform :
                actor_scene::model_record_triple_transform);
        write_u16(rdram.data(), model + actor_scene::model_record_next_delta,
            second_record_delta);
        write_u32(rdram.data(), model + 0x0Cu,
            0x80158000u + lod * 0x200u);
        write_u16(rdram.data(), model +
            second_record_delta * actor_scene::model_record_stride +
            actor_scene::model_record_type, actor_scene::model_record_triple_transform);
        write_u16(rdram.data(), model +
            second_record_delta * actor_scene::model_record_stride +
            actor_scene::model_record_next_delta, 0u);
        write_u32(rdram.data(), model +
            second_record_delta * actor_scene::model_record_stride + 0x0Cu,
            0x80158080u + lod * 0x200u);
        write_u32(rdram.data(), bike_node + actor_scene::display_lists + lod * 4u,
            0x80152000u + lod * 0x100u);
        write_u16(rdram.data(), bike_node + actor_scene::segment_counts + lod * 2u, 2u);
        for (std::uint32_t record = 0; record < 2u; ++record) {
            const std::uint32_t pose = 0x80154000u + lod * 0x200u + record * 0x80u;
            write_u32(rdram.data(), bike_node + actor_scene::primary_segment_records +
                lod * 0x20u + record * 4u, pose);
            write_u32(rdram.data(), bike_node + actor_scene::resolved_segment_payloads +
                lod * 0x20u + record * 4u, pose + 0x40u);
            write_u32(rdram.data(), pose + 0x40u, 0x100u + lod * 0x10u + record);
        }
    }
    write_u32(rdram.data(), bike_node + actor_scene::current_model, 0x80150200u);
    write_u32(rdram.data(), bike_node + actor_scene::render_transform_buffers,
        bike_transform_buffer);
    write_u32(rdram.data(), bike_node + actor_scene::render_transform_buffers +
        actor_scene::render_buffer_slot_stride, bike_transform_buffer + 392u);
    write_u32(rdram.data(), bike_transform_buffer, 0x12345678u);

    write_u32(rdram.data(), test_bike_pool + bike::model_state_pointer, model_state);
    write_u32(rdram.data(), model_state + actor_scene::model_state_pose_owner, pose_owner);
    write_u32(rdram.data(), pose_owner + actor_scene::pose_owner_rider_node, rider_node);
    write_u32(rdram.data(), rider_node + actor_scene::type, 2u);
    write_u32(rdram.data(), rider_node + actor_scene::entity, rider_entity);
    write_u16(rdram.data(), rider_node + actor_scene::selected_lod, 1u);
    write_u16(rdram.data(), rider_node + actor_scene::previous_lod, 0u);
    write_float(rdram.data(), rider_node + actor_scene::viewport_depth, 41.0f);
    for (std::uint32_t lod = 0; lod < actor_scene::lod_record_count; ++lod) {
        write_u32(rdram.data(), rider_node + actor_scene::lod_models + lod * 4u,
            0x80160000u + lod * 0x100u);
        write_u16(rdram.data(), 0x80160000u + lod * 0x100u + actor_scene::model_record_type,
            actor_scene::model_record_single_transform);
        write_u16(rdram.data(), 0x80160000u + lod * 0x100u + actor_scene::model_record_next_delta, 0u);
        write_u32(rdram.data(), 0x80160000u + lod * 0x100u + 0x0Cu,
            0x80168000u + lod * 0x80u);
        write_u32(rdram.data(), rider_node + actor_scene::display_lists + lod * 4u,
            0x80162000u + lod * 0x100u);
        write_u16(rdram.data(), rider_node + actor_scene::segment_counts + lod * 2u, 3u);
        for (std::uint32_t record = 0; record < 3u; ++record) {
            const std::uint32_t pose = 0x80166000u + lod * 0x200u + record * 0x80u;
            write_u32(rdram.data(), rider_node + actor_scene::primary_segment_records +
                lod * 0x20u + record * 4u, pose);
            write_u32(rdram.data(), rider_node + actor_scene::resolved_segment_payloads +
                lod * 0x20u + record * 4u, pose + 0x40u);
            write_u32(rdram.data(), pose + 0x40u, 0x200u + lod * 0x10u + record);
        }
    }
    write_u32(rdram.data(), rider_node + actor_scene::current_model, 0x80160100u);
    write_u32(rdram.data(), rider_node + actor_scene::render_transform_buffers,
        rider_transform_buffer);
    write_u32(rdram.data(), rider_transform_buffer, 0x87654321u);

    SegmentTableSnapshot segment_table{};
    passed &= check(capture_segment_table(rdram.data(), bike_node, 0u, 2u, segment_table) &&
        segment_table.valid && segment_table.segment_count == 2u &&
        segment_table.records[0] == 0x80154000u &&
        segment_table.payload_hashes[0] != 0u && segment_table.aggregate_hash != 0u,
        "bounded tier-local segment table hashing");
    passed &= check(!capture_segment_table(rdram.data(), bike_node, 0u,
            actor_scene::maximum_segment_records_per_tier + 1u, segment_table) &&
        !segment_table.valid, "oversized tier-local segment table rejection");

    ActorSceneSnapshot actor_scene_snapshot{};
    passed &= check(capture_actor_scene_snapshot(rdram.data(), 0u, actor_scene_snapshot),
        "typed actor scene snapshot capture");
    passed &= check(actor_scene_snapshot.graph_complete && actor_scene_snapshot.matched_actors == 1u &&
        actor_scene_snapshot.valid_actors == 1u, "actor scene graph validity");
    passed &= check(actor_scene_snapshot.actors[0].bike.selected_lod == 2u &&
        actor_scene_snapshot.actors[0].rider.selected_lod == 1u,
        "independent bike and rider LOD selections");
    passed &= check(actor_scene_snapshot.actors[0].bike.models[2] == 0x80150200u &&
        actor_scene_snapshot.actors[0].rider.primary_segment_tables[1] == 0x80166200u,
        "selected LOD resources remain tier-local");
    passed &= check(actor_scene_snapshot.actors[0].bike.segment_payloads_valid[0] &&
        actor_scene_snapshot.actors[0].bike.segment_payload_hashes[0] != 0u &&
        actor_scene_snapshot.actors[0].rider.segment_payloads_valid[0] &&
        actor_scene_snapshot.actors[0].rider.segment_payload_hashes[0] != 0u,
        "tier-local segment payloads are bounded and hashable");
    passed &= check(actor_scene_snapshot.actors[0].bike.transform_counts[2] == 4u &&
        actor_scene_snapshot.actors[0].bike.selected_transform_buffer_valid &&
        actor_scene_snapshot.actors[0].bike.selected_transform_hash != 0u &&
        actor_scene_snapshot.actors[0].rider.transform_counts[1] == 1u &&
        actor_scene_snapshot.actors[0].rider.selected_transform_buffer_valid,
        "selected model graph and render transform buffer are coherent");
    passed &= check(actor_scene_snapshot.actors[0].bike.current_model ==
            actor_scene_snapshot.actors[0].bike.models[2] &&
        actor_scene_snapshot.actors[0].rider.current_model ==
            actor_scene_snapshot.actors[0].rider.models[1],
        "current model follows the independently selected tier");
    passed &= check(actor_scene_snapshot.actors[0].render_proxy.metadata_valid &&
        !actor_scene_snapshot.actors[0].render_proxy.directly_renderable &&
        actor_scene_snapshot.actors[0].render_proxy.requires_pose_rebuild &&
        actor_scene_snapshot.actors[0].render_proxy.bike.desired_model == 0x80150000u &&
        actor_scene_snapshot.actors[0].render_proxy.rider.desired_model == 0x80160000u,
        "high-detail proxy refuses stale cross-tier poses");
    passed &= check(actor_scene_snapshot.actors[0].physics.front_wheel.x == 1.0f &&
        actor_scene_snapshot.actors[0].physics.body.y == 2.0f &&
        actor_scene_snapshot.actors[0].physics.rear_wheel.z == 3.0f,
        "presentation actor uses authoritative physical pose");

    write_u16(rdram.data(), bike_node + actor_scene::selected_lod, 1u);
    write_u16(rdram.data(), rider_node + actor_scene::selected_lod, 1u);
    write_u32(rdram.data(), bike_node + actor_scene::current_model, 0x80150100u);
    write_u32(rdram.data(), rider_node + actor_scene::current_model, 0x80160100u);
    passed &= check(capture_actor_scene_snapshot(rdram.data(), 0u, actor_scene_snapshot) &&
        actor_scene_snapshot.actors[0].render_proxy.metadata_valid &&
        !actor_scene_snapshot.actors[0].render_proxy.directly_renderable &&
        actor_scene_snapshot.actors[0].render_proxy.renderable_with_selected_transforms &&
        !actor_scene_snapshot.actors[0].render_proxy.requires_pose_rebuild &&
        actor_scene_snapshot.actors[0].render_proxy.bike.selected_transform_remap_safe &&
        actor_scene_snapshot.actors[0].render_proxy.rider.selected_transform_remap_safe,
        "allocation-compatible tiers retain stock pose with tier-zero presentation metadata");

    ActorPresentationDecision presentation_decision{};
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), bike_node, 1u, true, presentation_decision) &&
        presentation_decision.presentation_lod == 0u &&
        presentation_decision.reason ==
            ActorPresentationDecisionReason::TierZeroGraphSubstitution,
        "renderer rebuilds the complete tier-zero graph for compatible bike tiers");
    const auto memory_before_complete_graph = rdram;
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), bike_node, 1u, true, presentation_decision) &&
        presentation_decision.presentation_lod == 0u &&
        presentation_decision.reason ==
            ActorPresentationDecisionReason::TierZeroGraphSubstitution &&
        choose_actor_transform_model(
            rdram.data(), bike_node, 1u, 0x80150100u, true,
            presentation_decision) == 0x80150000u &&
        rdram == memory_before_complete_graph,
        "complete tier-zero bike graph is selected locally without guest writes");
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), rider_node, 1u, true, presentation_decision) &&
        presentation_decision.presentation_lod == 0u &&
        presentation_decision.reason ==
            ActorPresentationDecisionReason::TierZeroGraphSubstitution &&
        choose_actor_transform_model(
            rdram.data(), rider_node, 1u, 0x80160100u, true,
            presentation_decision) == 0x80160000u &&
        rdram == memory_before_complete_graph,
        "complete tier-zero rider graph is selected locally without guest writes");
    passed &= check(choose_actor_transform_model(
            rdram.data(), rider_node, 1u, 0x8016FFFFu, true,
            presentation_decision) == 0x8016FFFFu,
        "local graph selector rejects a stale caller model");
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), bike_node, 1u, false, presentation_decision) &&
        presentation_decision.presentation_lod == 1u &&
        presentation_decision.reason == ActorPresentationDecisionReason::Disabled,
        "renderer switch leaves stock tier untouched");

    std::uint16_t transaction_bike_lod = 0;
    std::uint16_t transaction_bike_previous_lod = 0;
    std::uint16_t transaction_rider_lod = 0;
    std::uint16_t transaction_rider_previous_lod = 0;
    std::uint32_t transaction_bike_model = 0;
    std::uint32_t transaction_rider_model = 0;
    constexpr std::uint32_t original_pose_source = 0x80158100u;
    write_float(rdram.data(), 0x80158200u + 0x00u, 12.0f);
    write_float(rdram.data(), 0x80158200u + 0x04u, 34.0f);
    write_float(rdram.data(), 0x80158200u + 0x08u, 56.0f);
    write_float(rdram.data(), 0x80158200u + 0x0Cu, 0.0f);
    write_float(rdram.data(), 0x80158200u + 0x10u, 0.0f);
    write_float(rdram.data(), 0x80158200u + 0x14u, 0.0f);
    write_float(rdram.data(), 0x80158200u + 0x18u, 1.0f);
    // Seed the detailed graph's actor-owned pose blocks. The renderer-local
    // selector must require these prepared poses rather than treating an
    // allocated-but-cleared graph as renderable.
    write_float(rdram.data(), 0x80158000u + 0x00u, 10.0f);
    write_float(rdram.data(), 0x80158000u + 0x04u, 20.0f);
    write_float(rdram.data(), 0x80158000u + 0x08u, 30.0f);
    write_float(rdram.data(), 0x80158000u + 0x0Cu, 0.0f);
    write_float(rdram.data(), 0x80158000u + 0x10u, 0.0f);
    write_float(rdram.data(), 0x80158000u + 0x14u, 0.0f);
    write_float(rdram.data(), 0x80158000u + 0x18u, 1.0f);
    write_float(rdram.data(), 0x80158080u + 0x00u, 0.0f);
    write_float(rdram.data(), 0x80158080u + 0x04u, 0.5f);
    write_float(rdram.data(), 0x80158080u + 0x08u, 0.0f);
    write_float(rdram.data(), 0x80158080u + 0x0Cu, 0.0f);
    write_float(rdram.data(), 0x80158080u + 0x10u, 0.0f);
    write_float(rdram.data(), 0x80158080u + 0x14u, 0.0f);
    write_float(rdram.data(), 0x80158080u + 0x18u, 1.0f);
    write_float(rdram.data(), 0x80168000u + 0x00u, 11.0f);
    write_float(rdram.data(), 0x80168000u + 0x04u, 21.0f);
    write_float(rdram.data(), 0x80168000u + 0x08u, 31.0f);
    write_float(rdram.data(), 0x80168000u + 0x0Cu, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x10u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x14u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x18u, 1.0f);
    rr64_set_high_detail_actors_enabled(1);
    passed &= check(
        rr64_actor_select_render_lod(rdram.data(), bike_node, 1u) == 1u &&
        rr64_actor_select_render_lod(rdram.data(), rider_node, 1u) == 1u,
        "renderer-local LOD selector rejects nodes before authoritative pair validation");

    // The stock actor-preparation function is shared by the update thread and
    // render worker. A model transaction must be impossible outside an
    // explicit presentation scope.
    const auto memory_before_out_of_scope_transaction = rdram;
    rr64_actor_begin_presentation_pair(rdram.data(), bike_node, rider_node);
    passed &= check(rdram == memory_before_out_of_scope_transaction,
        "update-thread actor preparation cannot begin a render transaction");

    rr64_actor_begin_presentation_scope(rdram.data());
    const auto memory_before_other_thread_transaction = rdram;
    std::thread simulated_update_thread([&] {
        rr64_actor_begin_presentation_pair(rdram.data(), bike_node, rider_node);
    });
    simulated_update_thread.join();
    passed &= check(rdram == memory_before_other_thread_transaction,
        "render scope is local to the draw thread and invisible to update thread");
    rr64_actor_end_presentation_scope(rdram.data());

    write_float(rdram.data(),
        rider_entity + rider::front_contact_gap, -0.12f);
    write_float(rdram.data(),
        rider_entity + rider::rear_contact_gap, -0.08f);
    rr64_actor_begin_presentation_scope(rdram.data());
    rr64_actor_begin_presentation_pair(rdram.data(), bike_node, rider_node);
    const auto memory_before_render_lod_selection = rdram;
    passed &= check(
        rr64_actor_presentation_transaction_active(rdram.data(), bike_node) == 1 &&
        rr64_actor_presentation_transaction_active(rdram.data(), rider_node) == 1 &&
        rr64_actor_presentation_transaction_active(rdram.data(), 0x80170000u) == 0,
        "visual update bypass is limited to nodes owned by the active presentation transaction");
    passed &= check(
        rr64_actor_select_render_lod(rdram.data(), bike_node, 0u) == 0u &&
        rr64_actor_select_render_lod(rdram.data(), rider_node, 0u) == 0u &&
        rdram == memory_before_render_lod_selection,
        "active renderer consumes one coherent prepared tier-zero pair without guest writes");
    const std::int32_t selected_root_pose_source =
        rr64_actor_select_render_pose_source(
            rdram.data(), bike_node, 0u, 0x80150000u,
            original_pose_source);
    passed &= check(
        selected_root_pose_source ==
            static_cast<std::int32_t>(original_pose_source) &&
            selected_root_pose_source < 0,
        "tier-zero renderer preserves the actor-owned root pose source");
    const std::int32_t selected_child_pose_source =
        rr64_actor_select_render_pose_source(
            rdram.data(), bike_node, 0u, 0x80150020u,
            0x80158180u);
    passed &= check(
        selected_child_pose_source == static_cast<std::int32_t>(0x80158180u) &&
            selected_child_pose_source < 0,
        "tier-zero child preserves the sign-extended detailed pose source");
    passed &= check(
        read_u16(rdram.data(), bike_node + actor_scene::selected_lod,
            transaction_bike_lod) &&
        read_u16(rdram.data(), bike_node + actor_scene::previous_lod,
            transaction_bike_previous_lod) &&
        read_u32(rdram.data(), bike_node + actor_scene::current_model,
            transaction_bike_model) &&
        read_u16(rdram.data(), rider_node + actor_scene::selected_lod,
            transaction_rider_lod) &&
        read_u16(rdram.data(), rider_node + actor_scene::previous_lod,
            transaction_rider_previous_lod) &&
        read_u32(rdram.data(), rider_node + actor_scene::current_model,
            transaction_rider_model) &&
        transaction_bike_lod == 0u && transaction_bike_previous_lod == 1u &&
        transaction_bike_model == 0x80150000u &&
        transaction_rider_lod == 0u && transaction_rider_previous_lod == 0u &&
        transaction_rider_model == 0x80160000u,
        "presentation transaction exposes tier zero while preserving stock transition history");
    rr64_actor_end_presentation_scope(rdram.data());
    passed &= check(
        rr64_actor_presentation_transaction_active(rdram.data(), bike_node) == 0 &&
        rr64_actor_presentation_transaction_active(rdram.data(), rider_node) == 0,
        "visual update bypass closes with the presentation scope");
    const auto memory_before_deferred_render_lod_selection = rdram;
    passed &= check(
        rr64_actor_select_render_lod(rdram.data(), bike_node, 1u) == 0u &&
        rr64_actor_select_render_lod(rdram.data(), rider_node, 1u) == 0u &&
        rdram == memory_before_deferred_render_lod_selection,
        "deferred renderer reuses the prepared detailed pair after stock state restoration");
    write_float(rdram.data(), 0x80168000u + 0x00u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x04u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x08u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x0Cu, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x10u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x14u, 0.0f);
    write_float(rdram.data(), 0x80168000u + 0x18u, 1.0f);
    passed &= check(
        rr64_actor_select_render_lod(rdram.data(), bike_node, 1u) == 1u &&
        rr64_actor_select_render_lod(rdram.data(), rider_node, 1u) == 1u,
        "cleared rider pose makes both halves fall back to their stock distance tier");
    write_float(rdram.data(), 0x80168000u + 0x00u, 11.0f);
    write_float(rdram.data(), 0x80168000u + 0x04u, 21.0f);
    write_float(rdram.data(), 0x80168000u + 0x08u, 31.0f);
    float front_contact_gap_after = 0.0f;
    float rear_contact_gap_after = 0.0f;
    passed &= check(
        read_u16(rdram.data(), bike_node + actor_scene::selected_lod,
            transaction_bike_lod) &&
        read_u16(rdram.data(), bike_node + actor_scene::previous_lod,
            transaction_bike_previous_lod) &&
        read_u32(rdram.data(), bike_node + actor_scene::current_model,
            transaction_bike_model) &&
        read_u16(rdram.data(), rider_node + actor_scene::selected_lod,
            transaction_rider_lod) &&
        read_u16(rdram.data(), rider_node + actor_scene::previous_lod,
            transaction_rider_previous_lod) &&
        read_u32(rdram.data(), rider_node + actor_scene::current_model,
            transaction_rider_model) &&
        transaction_bike_lod == 1u && transaction_bike_previous_lod == 1u &&
        transaction_bike_model == 0x80150100u &&
        transaction_rider_lod == 1u && transaction_rider_previous_lod == 0u &&
        transaction_rider_model == 0x80160100u &&
        read_float(rdram.data(),
            rider_entity + rider::front_contact_gap,
            front_contact_gap_after) &&
        read_float(rdram.data(),
            rider_entity + rider::rear_contact_gap,
            rear_contact_gap_after) &&
        front_contact_gap_after == -0.12f && rear_contact_gap_after == -0.08f,
        "draw-scope exit restores exact stock graph history without touching contact state");
    rr64_actor_end_presentation_scope(rdram.data());

    write_u16(rdram.data(), bike_node + actor_scene::selected_lod, 1u);
    write_u16(rdram.data(), bike_node + actor_scene::previous_lod, 1u);
    write_u16(rdram.data(), rider_node + actor_scene::selected_lod, 1u);
    write_u16(rdram.data(), rider_node + actor_scene::previous_lod, 0u);
    write_u32(rdram.data(), bike_node + actor_scene::current_model, 0x80150100u);
    write_u32(rdram.data(), rider_node + actor_scene::current_model, 0x80160100u);
    write_u16(rdram.data(), test_bike_pool + bike::rider_attached, 0u);
    write_float(rdram.data(), bike_node + actor_scene::viewport_depth, 42.0f);

    // In single-player, active_racer_count is the local-controller count and
    // does not contain AI competitors. The stock preparation loop's verified
    // bike -> model-state -> pose-owner -> rider relationship is therefore the
    // authority for a render-only AI presentation pair.
    constexpr std::uint32_t ai_bike_entity =
        test_bike_pool + 4u * bike::stride;
    write_u32(rdram.data(), ai_bike_entity + bike::model_state_pointer, model_state);
    write_u32(rdram.data(), bike_node + actor_scene::entity, ai_bike_entity);
    passed &= check(valid_actor_presentation_pair(
            rdram.data(), bike_node, rider_node),
        "stock actor-preparation ownership chain validates an AI rider/bike pair");
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), bike_node, 1u, true, presentation_decision) &&
        presentation_decision.reason ==
            ActorPresentationDecisionReason::InactiveActorNode,
        "local-racer heuristic alone does not misclassify an AI bike as local");
    rr64_actor_begin_presentation_scope(rdram.data());
    rr64_actor_begin_presentation_pair(rdram.data(), bike_node, rider_node);
    passed &= check(
        read_u16(rdram.data(), bike_node + actor_scene::selected_lod,
            transaction_bike_lod) &&
        read_u16(rdram.data(), rider_node + actor_scene::selected_lod,
            transaction_rider_lod) &&
        transaction_bike_lod == 0u && transaction_rider_lod == 0u,
        "validated AI presentation pair receives coherent tier-zero state");
    rr64_actor_end_presentation_scope(rdram.data());
    passed &= check(
        read_u16(rdram.data(), bike_node + actor_scene::selected_lod,
            transaction_bike_lod) &&
        read_u16(rdram.data(), rider_node + actor_scene::selected_lod,
            transaction_rider_lod) &&
        transaction_bike_lod == 1u && transaction_rider_lod == 1u,
        "validated AI presentation pair restores exact stock tiers");
    write_u32(rdram.data(), bike_node + actor_scene::entity, test_bike_pool);

    rr64_set_high_detail_actors_enabled(0);
    const auto memory_before_disabled_transaction = rdram;
    rr64_actor_begin_presentation_scope(rdram.data());
    rr64_actor_begin_presentation_pair(rdram.data(), bike_node, rider_node);
    rr64_actor_end_presentation_scope(rdram.data());
    passed &= check(rdram == memory_before_disabled_transaction,
        "disabled presentation transaction performs no guest writes");
    rr64_set_high_detail_actors_enabled(1);

    write_u16(rdram.data(), bike_node + actor_scene::selected_lod, 2u);
    write_u32(rdram.data(), bike_node + actor_scene::current_model, 0x80150200u);
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), bike_node, 2u, true, presentation_decision) &&
        presentation_decision.presentation_lod == 0u &&
        presentation_decision.reason ==
            ActorPresentationDecisionReason::TierZeroGraphSubstitution,
        "renderer accepts a capacity-checked local far-bike graph rebuild");
    passed &= check(choose_actor_transform_model(
            rdram.data(), bike_node, 2u, 0x80150200u, true,
            presentation_decision) == 0x80150000u,
        "local transform loop receives tier-zero graph without changing guest LOD");

    write_u32(rdram.data(), bike_node + actor_scene::render_transform_buffers +
        actor_scene::render_buffer_slot_stride, bike_transform_buffer + 320u);
    passed &= check(choose_actor_presentation_lod(
            rdram.data(), bike_node, 2u, true, presentation_decision) &&
        presentation_decision.presentation_lod == 2u &&
        presentation_decision.reason ==
            ActorPresentationDecisionReason::IncompatibleTransformAllocation,
        "far-bike graph falls back when sibling-buffer capacity is insufficient");
    const auto memory_before_rejected_transaction = rdram;
    rr64_actor_begin_presentation_scope(rdram.data());
    rr64_actor_begin_presentation_pair(rdram.data(), bike_node, rider_node);
    rr64_actor_end_presentation_scope(rdram.data());
    passed &= check(rdram == memory_before_rejected_transaction,
        "capacity-rejected presentation transaction performs no guest writes");
    write_u32(rdram.data(), bike_node + actor_scene::render_transform_buffers +
        actor_scene::render_buffer_slot_stride, bike_transform_buffer + 392u);
    rr64_set_high_detail_actors_enabled(0);

    write_u16(rdram.data(), bike_node + actor_scene::selected_lod, 0u);
    write_u16(rdram.data(), rider_node + actor_scene::selected_lod, 0u);
    write_u32(rdram.data(), bike_node + actor_scene::current_model, 0x80150000u);
    write_u32(rdram.data(), rider_node + actor_scene::current_model, 0x80160000u);
    passed &= check(capture_actor_scene_snapshot(rdram.data(), 0u, actor_scene_snapshot) &&
        actor_scene_snapshot.actors[0].render_proxy.metadata_valid &&
        actor_scene_snapshot.actors[0].render_proxy.directly_renderable &&
        !actor_scene_snapshot.actors[0].render_proxy.requires_pose_rebuild,
        "tier-zero proxy is direct only when stock selected both tier-zero poses");

    write_u32(rdram.data(), bike_node + actor_scene::current_model, 0x80150100u);
    passed &= check(capture_actor_scene_snapshot(rdram.data(), 0u, actor_scene_snapshot) &&
        !actor_scene_snapshot.actors[0].render_proxy.metadata_valid &&
        !actor_scene_snapshot.actors[0].render_proxy.directly_renderable &&
        !actor_scene_snapshot.actors[0].render_proxy.requires_pose_rebuild,
        "proxy rejects a current-model/selected-tier mismatch");

    write_u32(rdram.data(), bike_node + actor_scene::next, bike_node);
    passed &= check(capture_actor_scene_snapshot(rdram.data(), 0u, actor_scene_snapshot) &&
        !actor_scene_snapshot.graph_complete, "actor scene cycle is bounded");

    if (passed) {
        std::puts("[RR64-ENGINE-TEST] contract boundaries and mode set passed.");
        return 0;
    }
    return 1;
}
