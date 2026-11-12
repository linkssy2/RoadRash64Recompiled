#pragma once
#include "rr64_engine_layout.hpp"
#include <algorithm>
#include <cmath>

namespace rr64::traffic {
inline constexpr float maximum_spawn_extension = 650.0f;
// Keep the stock cadence/allocator; move only its proposed route location.
// Use half the remaining valid route headroom near the finish so an otherwise
// valid spawn never becomes invalid solely because of the extension.
inline float spawn_distance(float original, float limit, int percent) {
    if (percent <= 0 || !std::isfinite(original) || !std::isfinite(limit) || original < 0 ||
        original >= limit)
        return original;
    const float extension = maximum_spawn_extension * std::clamp(percent, 0, 100) / 100.0f;
    return std::min(limit, original + std::min(extension, (limit - original) * 0.5f));
}

// The traffic preparer has already produced this view's root and multiplied
// positions by ten. Its distance routine returns squared length, not length.
// Keep the extended root within signed fixed-point range used by native draws.
inline bool within_distance(unsigned char *m, unsigned node, float squared, float stock_squared,
                            bool stock_visible, int percent) {
    using namespace engine;
    if (stock_visible || percent <= 0)
        return stock_visible;
    unsigned mode = 0, pending = 0, type = 0, entity = 0, views = 0, view = 0;
    std::uint16_t active = 0;
    if (!std::isfinite(squared) || squared < 0 || !std::isfinite(stock_squared) ||
        stock_squared < 0 || !read_u32(m, globals::main_mode, mode) ||
        !read_u32(m, globals::pending_mode, pending) || !is_live_race_transition(mode, pending) ||
        !read_u32(m, 0x8009DB88u, views) || views < 1 || views > 4 ||
        !read_u32(m, globals::active_viewport, view) || view >= views ||
        !valid_guest_range(node, actor_scene::node_minimum_size) ||
        !read_u32(m, node + actor_scene::type, type) || type != traffic_scene::node_type ||
        !read_u32(m, node + actor_scene::entity, entity) ||
        !valid_guest_range(entity, traffic_scene::entity_minimum_size) ||
        !read_u16(m, entity + traffic_scene::entity_active, active) || !active)
        return stock_visible;
    const float stock_radius = std::sqrt(stock_squared);
    const float radius = stock_radius + (std::max(stock_radius, 30000.0f) - stock_radius) *
                                            std::clamp(percent, 0, 100) / 100.0f;
    return squared < radius * radius;
}
}
