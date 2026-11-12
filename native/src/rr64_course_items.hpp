#pragma once

#ifdef __cplusplus
#include "rr64_netplay.hpp"
#include <array>
#include <span>

namespace rr64::course_items {
// Positions are final, atlas-translated rider-world coordinates. Conversion
// owns the source box's vertical offset; gameplay and rendering use one center.
struct ItemBoxDefinition {
    std::uint32_t id = 0;
    std::array<float, 3> position{};
    float radius = 0.275f;
    std::uint8_t kind = 2;
    std::uint32_t parent_hazard = ~0u;
    std::array<float, 3> parent_offset{};
};
// Render and collision resolve moving boxes from the same complete authority
// state. An unavailable parent hides its box rather than leaving a stale one.
inline bool posed_definition(const ItemBoxDefinition &source,
                             const netplay::CourseHazardState &hazards,
                             ItemBoxDefinition &out) noexcept {
    out = source;
    if (source.parent_hazard == ~0u)
        return true;
    if (source.parent_hazard >= hazards.count || hazards.count > hazards.poses.size())
        return false;
    const auto &parent = hazards.poses[source.parent_hazard];
    if (!parent.active || !netplay::valid_course_hazard_pose(parent))
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        out.position[axis] = parent.position[axis] + source.parent_offset[axis];
    return true;
}
std::span<const ItemBoxDefinition> definitions() noexcept;
void reset_runtime() noexcept;
netplay::CourseItemState capture_state() noexcept;
bool apply_state(const netplay::CourseItemState &, std::uint32_t round,
                 std::uint64_t tick) noexcept;
} // namespace rr64::course_items
extern "C" {
#endif
void rr64_course_items_step(unsigned char *memory, void *context);
void rr64_course_items_draw(unsigned char *memory);
// HUD presentation overrides never change equipped gameplay state to animate
// the weapon box; each native viewport keeps its layout.
void rr64_course_items_hud_begin(void);
void rr64_course_items_hud_end(void);
// A completed native weapon cycle changes only which inventory the HUD shows.
void rr64_course_items_weapon_switched(unsigned char *, unsigned rider);
void rr64_course_items_weapon_wrapped(unsigned char *, unsigned rider);
void rr64_course_items_cycle_publish(unsigned char *);
// Native three/four-player HUD omits weapon icons. Add only the short item
// roulette/reward indication, using the native sprite producer per quadrant.
void rr64_course_items_hud_quadrants(unsigned char *, void *context);
unsigned rr64_course_items_hud_weapon(unsigned char *, unsigned rider, unsigned original);
unsigned rr64_course_items_hud_quantity(unsigned original);
// Only the three audited weapon-icon call sites use this. Inventory indexing
// keeps the separate safe weapon value. A hidden frame becomes a sprite-only
// marker caught before atlas lookup; persistent multiplier icons are untouched.
unsigned rr64_course_items_hud_sprite(unsigned char *, unsigned original);
// Paired after the same native call: fit only its committed replacement record
// to the original weapon icon's rectangle, including texture-mod dimensions.
void rr64_course_items_hud_sprite_end(unsigned char *);
int rr64_course_items_hud_hide(unsigned sprite);
#ifdef __cplusplus
}
#endif
