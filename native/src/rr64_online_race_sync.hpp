#pragma once

#include <cstdint>

namespace rr64::online_race_sync {

struct ViewportRenderPlan {
    bool peer_fullscreen = false;
    std::uint32_t layout = 0;
    std::uint32_t first_viewport = 0;
    std::uint32_t geometry_viewport = 0;
};

// Online sessions still simulate every remote controller/rider, but each
// machine presents only its own rider. Keeping this decision independent from
// the guest's controller count prevents online rendering from inheriting the
// stock local-multiplayer split screen.
constexpr ViewportRenderPlan make_viewport_render_plan(std::uint32_t stock_layout, bool active,
                                                       bool connected, bool race_phase,
                                                       bool replicated_riders,
                                                       std::uint32_t local_slot) {
    const bool local_guest_valid = replicated_riders ? local_slot < 14u : local_slot < 4u;
    const bool peer_fullscreen = active && connected && race_phase && local_guest_valid;
    return ViewportRenderPlan{
        .peer_fullscreen = peer_fullscreen,
        .layout = peer_fullscreen ? 0u : stock_layout,
        .first_viewport = peer_fullscreen ? (replicated_riders ? 0u : local_slot) : 0u,
        .geometry_viewport = 0u,
    };
}

// Returns the stock racer count unless a replicated-rider online session is
// being prepared. Road Rash's native pools contain fourteen records.
std::uint32_t requested_racer_count(std::uint32_t original_count);

void before_guest_update(unsigned char *rdram, std::uint32_t mode);
void after_guest_update(unsigned char *rdram, std::uint32_t mode);

} // namespace rr64::online_race_sync
