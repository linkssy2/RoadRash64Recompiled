#pragma once

#include <array>
#include <cstdint>

#include "rr64_engine_layout.hpp"

namespace rr64::engine {

struct TerrainSceneSnapshot {
    bool valid = false;
    std::uint32_t mode = 0;
    std::uint32_t current_record = 0;
    std::uint32_t cell_grid = 0;
    std::uint32_t map_width = 0;
    std::uint32_t cell_count = 0;
    float camera_x = 0.0f;
    float camera_z = 0.0f;
    std::uint32_t candidate_count = 0;
    std::uint32_t candidate_valid = 0;
    std::uint32_t candidate_unique = 0;
    std::uint32_t candidate_ready = 0;
    std::uint32_t candidate_min_row = 0;
    std::uint32_t candidate_max_row = 0;
    std::uint32_t candidate_min_column = 0;
    std::uint32_t candidate_max_column = 0;
    std::uint32_t display_list_slots_valid = 0;
    std::array<std::uint32_t, 8> state_counts{};
    std::uint32_t other_state_count = 0;
    std::uint32_t ready_cells = 0;
    std::uint32_t ready_resources = 0;
    std::uint32_t ready_in_candidate = 0;
    std::uint32_t ready_outside_candidate = 0;
    std::uint32_t duplicate_slot_owners = 0;
    float maximum_ready_distance = 0.0f;
    float maximum_candidate_distance = 0.0f;
    float minimum_outside_candidate_distance = 0.0f;
    float maximum_outside_candidate_distance = 0.0f;
    std::uint64_t lifecycle_hash = 0;
    std::uint64_t resource_hash = 0;
    std::uint64_t candidate_hash = 0;
    std::array<std::uint8_t, terrain::maximum_map_width * terrain::maximum_map_width> cell_states{};
};

// Aggregates the fixed terrain grid, active 31-cell candidate record, and
// 96 display-list slots without mutating streamer or renderer state.
bool capture_terrain_scene_snapshot(unsigned char *rdram, TerrainSceneSnapshot &snapshot) noexcept;

} // namespace rr64::engine

extern "C" void rr64_trace_terrain_scene(unsigned char *rdram);
