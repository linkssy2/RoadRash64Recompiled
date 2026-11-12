#include "rr64_terrain_residency.hpp"

#include <algorithm>

namespace rr64::engine {
namespace {

bool valid_candidate_bounds(std::uint32_t map_width, std::uint32_t candidate_min_row,
                            std::uint32_t candidate_max_row, std::uint32_t candidate_min_column,
                            std::uint32_t candidate_max_column,
                            std::span<const std::uint8_t> cell_states) noexcept {
    const std::uint64_t cell_count = static_cast<std::uint64_t>(map_width) * map_width;
    return map_width > 0u && map_width <= terrain::maximum_map_width &&
           cell_count <= cell_states.size() && candidate_min_row <= candidate_max_row &&
           candidate_min_column <= candidate_max_column && candidate_max_row < map_width &&
           candidate_max_column < map_width;
}

} // namespace

bool terrain_cell_in_presentation_ring(std::uint32_t map_width, std::uint32_t cell_row,
                                       std::uint32_t cell_column,
                                       const TerrainCandidateBounds &candidate) noexcept {
    if (!candidate.valid || map_width == 0u || map_width > terrain::maximum_map_width ||
        cell_row >= map_width || cell_column >= map_width ||
        candidate.minimum_row > candidate.maximum_row ||
        candidate.minimum_column > candidate.maximum_column || candidate.maximum_row >= map_width ||
        candidate.maximum_column >= map_width) {
        return false;
    }

    const std::uint32_t extended_minimum_row =
        candidate.minimum_row > kTerrainPresentationCacheMargin
            ? candidate.minimum_row - kTerrainPresentationCacheMargin
            : 0u;
    const std::uint32_t extended_maximum_row =
        std::min(candidate.maximum_row + kTerrainPresentationCacheMargin, map_width - 1u);
    const std::uint32_t extended_minimum_column =
        candidate.minimum_column > kTerrainPresentationCacheMargin
            ? candidate.minimum_column - kTerrainPresentationCacheMargin
            : 0u;
    const std::uint32_t extended_maximum_column =
        std::min(candidate.maximum_column + kTerrainPresentationCacheMargin, map_width - 1u);

    const bool inside_candidate =
        cell_row >= candidate.minimum_row && cell_row <= candidate.maximum_row &&
        cell_column >= candidate.minimum_column && cell_column <= candidate.maximum_column;
    const bool inside_extended_bounds =
        cell_row >= extended_minimum_row && cell_row <= extended_maximum_row &&
        cell_column >= extended_minimum_column && cell_column <= extended_maximum_column;
    return !inside_candidate && inside_extended_bounds;
}

bool should_retain_terrain_presentation_cell(
    bool maximum_view_distance, bool stock_unload, std::uint8_t cell_state, std::uint32_t map_width,
    std::uint32_t cell_row, std::uint32_t cell_column, const TerrainCandidateBounds &candidate,
    std::uint32_t occupied_or_inflight_resources) noexcept {
    if (!maximum_view_distance || !stock_unload || cell_state != terrain::ready_state ||
        !candidate.valid || map_width == 0u || map_width > terrain::maximum_map_width ||
        cell_row >= map_width || cell_column >= map_width ||
        candidate.minimum_row > candidate.maximum_row ||
        candidate.minimum_column > candidate.maximum_column || candidate.maximum_row >= map_width ||
        candidate.maximum_column >= map_width) {
        return false;
    }

    constexpr std::uint32_t kRetentionCeiling =
        terrain::display_list_slot_count - kTerrainPresentationReservedSlots;
    if (occupied_or_inflight_resources >= kRetentionCeiling) {
        return false;
    }

    return terrain_cell_in_presentation_ring(map_width, cell_row, cell_column, candidate);
}

bool build_terrain_prefetch_plan(
    std::uint32_t map_width, std::uint32_t candidate_min_row, std::uint32_t candidate_max_row,
    std::uint32_t candidate_min_column, std::uint32_t candidate_max_column,
    std::span<const std::uint8_t> cell_states, std::uint32_t occupied_or_inflight_resources,
    std::uint32_t reserved_resource_slots, TerrainPrefetchPlan &plan) noexcept {
    plan = {};
    if (!valid_candidate_bounds(map_width, candidate_min_row, candidate_max_row,
                                candidate_min_column, candidate_max_column, cell_states) ||
        occupied_or_inflight_resources > terrain::display_list_slot_count ||
        reserved_resource_slots > terrain::display_list_slot_count) {
        return false;
    }

    const std::uint32_t unoccupied_resources =
        terrain::display_list_slot_count - occupied_or_inflight_resources;
    plan.available_resource_budget = unoccupied_resources > reserved_resource_slots
                                         ? unoccupied_resources - reserved_resource_slots
                                         : 0u;

    const std::uint32_t ring_min_row =
        candidate_min_row > 0u ? candidate_min_row - 1u : candidate_min_row;
    const std::uint32_t ring_max_row = std::min(candidate_max_row + 1u, map_width - 1u);
    const std::uint32_t ring_min_column =
        candidate_min_column > 0u ? candidate_min_column - 1u : candidate_min_column;
    const std::uint32_t ring_max_column = std::min(candidate_max_column + 1u, map_width - 1u);

    for (std::uint32_t row = ring_min_row; row <= ring_max_row; ++row) {
        for (std::uint32_t column = ring_min_column; column <= ring_max_column; ++column) {
            const bool inside_candidate = row >= candidate_min_row && row <= candidate_max_row &&
                                          column >= candidate_min_column &&
                                          column <= candidate_max_column;
            if (inside_candidate) {
                continue;
            }
            ++plan.ring_cells_considered;
            const std::uint32_t cell_index = row * map_width + column;
            const std::uint8_t state = cell_states[cell_index];
            if (state < plan.ring_state_counts.size()) {
                ++plan.ring_state_counts[state];
            } else {
                ++plan.ring_other_state_count;
            }
            if (state != 1u) {
                continue;
            }
            ++plan.eligible_cells;
            if (plan.planned_cells >= plan.available_resource_budget ||
                plan.planned_cells >= plan.cell_indices.size()) {
                continue;
            }
            plan.cell_indices[plan.planned_cells++] = cell_index;
        }
    }

    plan.complete = plan.planned_cells == plan.eligible_cells;
    plan.valid = true;
    return true;
}

} // namespace rr64::engine
