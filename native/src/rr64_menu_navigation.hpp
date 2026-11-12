#pragma once

#include <array>
#include <cstdint>

namespace rr64::menu_navigation {

struct State {
    std::int8_t held_x = 0;
    std::int8_t held_y = 0;
    std::uint64_t next_repeat_ms = 0;
    std::uint64_t last_update_ms = 0;
    bool has_last_update = false;
};

// Converts a held stick or D-pad direction into crisp one-item menu pulses.
// The lower release threshold prevents diagonal stick noise from changing the
// selected axis. Race input must bypass this helper entirely.
void filter(State &state, std::uint16_t buttons, float &stick_x, float &stick_y,
            std::uint64_t now_ms);

void reset(State &state);

} // namespace rr64::menu_navigation
