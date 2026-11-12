#include "rr64_menu_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace rr64::menu_navigation {
namespace {

constexpr std::uint16_t kDpadUp = 0x0800u;
constexpr std::uint16_t kDpadDown = 0x0400u;
constexpr std::uint16_t kDpadLeft = 0x0200u;
constexpr std::uint16_t kDpadRight = 0x0100u;
constexpr float kEngageThreshold = 0.28f;
constexpr float kReleaseThreshold = 0.17f;
constexpr std::uint64_t kInitialRepeatDelayMs = 285;
constexpr std::uint64_t kRepeatIntervalMs = 95;
constexpr std::uint64_t kScreenReentryResetMs = 250;

struct Direction {
    std::int8_t x = 0;
    std::int8_t y = 0;

    bool operator==(const Direction &) const = default;
};

std::int8_t sign_of(float value) {
    return value < 0.0f ? -1 : 1;
}

Direction requested_direction(const State &state, std::uint16_t buttons, float stick_x,
                              float stick_y) {
    const bool left = (buttons & kDpadLeft) != 0;
    const bool right = (buttons & kDpadRight) != 0;
    const bool up = (buttons & kDpadUp) != 0;
    const bool down = (buttons & kDpadDown) != 0;

    Direction dpad{};
    if (left != right) {
        dpad.x = left ? -1 : 1;
    }
    if (up != down) {
        dpad.y = up ? 1 : -1;
    }
    if (dpad.x != 0 || dpad.y != 0) {
        // A diagonal D-pad press is reduced to one axis so a menu never skips
        // both a row and an option during one poll.
        if (dpad.x != 0) {
            dpad.y = 0;
        }
        return dpad;
    }

    const float abs_x = std::abs(stick_x);
    const float abs_y = std::abs(stick_y);
    if (state.held_x != 0 && abs_x >= kReleaseThreshold && sign_of(stick_x) == state.held_x) {
        return Direction{state.held_x, 0};
    }
    if (state.held_y != 0 && abs_y >= kReleaseThreshold && sign_of(stick_y) == state.held_y) {
        return Direction{0, state.held_y};
    }
    if (abs_x < kEngageThreshold && abs_y < kEngageThreshold) {
        return {};
    }
    if (abs_x >= abs_y) {
        return abs_x >= kEngageThreshold ? Direction{sign_of(stick_x), 0} : Direction{};
    }
    return abs_y >= kEngageThreshold ? Direction{0, sign_of(stick_y)} : Direction{};
}

} // namespace

void reset(State &state) {
    state = {};
}

void filter(State &state, std::uint16_t buttons, float &stick_x, float &stick_y,
            std::uint64_t now_ms) {
    if (state.has_last_update && now_ms - state.last_update_ms > kScreenReentryResetMs) {
        state.held_x = 0;
        state.held_y = 0;
    }
    state.last_update_ms = now_ms;
    state.has_last_update = true;

    const Direction direction = requested_direction(state, buttons, stick_x, stick_y);

    // The stock front end sees only deliberate one-step pulses. This removes
    // the slow free-drift feeling and makes controller and keyboard/D-pad menu
    // navigation use the same repeat cadence as the repaired name grid.
    stick_x = 0.0f;
    stick_y = 0.0f;
    if (direction == Direction{}) {
        state.held_x = 0;
        state.held_y = 0;
        return;
    }

    const Direction held{state.held_x, state.held_y};
    bool emit = direction != held;
    if (emit) {
        state.held_x = direction.x;
        state.held_y = direction.y;
        state.next_repeat_ms = now_ms + kInitialRepeatDelayMs;
    } else if (now_ms >= state.next_repeat_ms) {
        emit = true;
        state.next_repeat_ms = now_ms + kRepeatIntervalMs;
    }

    if (emit) {
        stick_x = static_cast<float>(direction.x);
        stick_y = static_cast<float>(direction.y);
    }
}

} // namespace rr64::menu_navigation
