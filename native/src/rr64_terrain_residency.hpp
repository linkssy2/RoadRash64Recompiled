#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "rr64_engine_layout.hpp"

namespace rr64::engine {

struct TerrainPrefetchPlan {
    bool valid = false;
    bool complete = false;
    std::uint32_t ring_cells_considered = 0;
    std::uint32_t eligible_cells = 0;
    std::uint32_t available_resource_budget = 0;
    std::uint32_t planned_cells = 0;
    std::array<std::uint32_t, 8> ring_state_counts{};
    std::uint32_t ring_other_state_count = 0;
    std::array<std::uint32_t, 64> cell_indices{};
};

struct TerrainCandidateBounds {
    bool valid = false;
    std::uint32_t minimum_row = 0;
    std::uint32_t maximum_row = 0;
    std::uint32_t minimum_column = 0;
    std::uint32_t maximum_column = 0;
};

inline constexpr std::uint32_t kTerrainPresentationCacheMargin = 1u;
// Keep enough of the 96-slot resource pool free for one complete stock
// candidate record. This prevents presentation-only retention from competing
// with the streamer's next authored load window.
inline constexpr std::uint32_t kTerrainPresentationReservedSlots =
    terrain::candidate_pointer_capacity;

// Returns true only for a cell in the bounded presentation ring immediately
// outside the stock candidate rectangle. Residency and rendering share this
// predicate so the renderer cannot submit a transitional cell that the
// retention policy did not explicitly validate.
bool terrain_cell_in_presentation_ring(std::uint32_t map_width, std::uint32_t cell_row,
                                       std::uint32_t cell_column,
                                       const TerrainCandidateBounds &candidate) noexcept;

// Determines whether a fully prepared cell that the stock streamer wants to
// retire may remain in a bounded presentation cache around the active
// candidate. The cache never promotes an unloaded cell or changes collision.
bool should_retain_terrain_presentation_cell(bool maximum_view_distance, bool stock_unload,
                                             std::uint8_t cell_state, std::uint32_t map_width,
                                             std::uint32_t cell_row, std::uint32_t cell_column,
                                             const TerrainCandidateBounds &candidate,
                                             std::uint32_t occupied_or_inflight_resources) noexcept;

// Builds a read-only one-cell ring around the stock rectangular candidate.
// The planner never mutates guest state or starts transfers. Its resource
// budget deliberately leaves caller-selected headroom in the 96-slot pool.
bool build_terrain_prefetch_plan(
    std::uint32_t map_width, std::uint32_t candidate_min_row, std::uint32_t candidate_max_row,
    std::uint32_t candidate_min_column, std::uint32_t candidate_max_column,
    std::span<const std::uint8_t> cell_states, std::uint32_t occupied_or_inflight_resources,
    std::uint32_t reserved_resource_slots, TerrainPrefetchPlan &plan) noexcept;

} // namespace rr64::engine
