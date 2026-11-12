#include "rr64_online_race_sync.hpp"

int main() {
    using rr64::online_race_sync::make_viewport_render_plan;

    const auto offline = make_viewport_render_plan(2u, false, false, false, false, 0u);
    if (offline.peer_fullscreen || offline.layout != 2u || offline.first_viewport != 0u) {
        return 1;
    }

    const auto local_multiplayer =
        make_viewport_render_plan(2u, false, false, true, false, 3u);
    if (local_multiplayer.peer_fullscreen || local_multiplayer.layout != 2u) {
        return 2;
    }

    for (unsigned int slot = 0; slot < 4u; ++slot) {
        const auto online =
            make_viewport_render_plan(2u, true, true, true, false, slot);
        if (!online.peer_fullscreen || online.layout != 0u ||
            online.first_viewport != slot || online.geometry_viewport != 0u) {
            return 3;
        }
    }

    const auto lobby = make_viewport_render_plan(2u, true, true, false, false, 2u);
    if (lobby.peer_fullscreen || lobby.layout != 2u) {
        return 4;
    }

    const auto replicated = make_viewport_render_plan(2u, true, true, true, true, 2u);
    if (!replicated.peer_fullscreen || replicated.layout != 0u ||
        replicated.first_viewport != 0u) {
        return 5;
    }

    const auto remote_replicated =
        make_viewport_render_plan(2u, true, true, true, true, 13u);
    if (!remote_replicated.peer_fullscreen || remote_replicated.layout != 0u ||
        remote_replicated.first_viewport != 0u) {
        return 6;
    }

    const auto invalid_slot = make_viewport_render_plan(2u, true, true, true, false, 4u);
    if (invalid_slot.peer_fullscreen || invalid_slot.layout != 2u) {
        return 7;
    }

    const auto invalid_replicated =
        make_viewport_render_plan(2u, true, true, true, true, 14u);
    if (invalid_replicated.peer_fullscreen || invalid_replicated.layout != 2u) {
        return 8;
    }

    return 0;
}
