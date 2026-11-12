#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "rr64_engine_layout.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint16_t kDpadUp = 0x0800u;
constexpr std::uint16_t kDpadDown = 0x0400u;
constexpr std::uint16_t kDpadLeft = 0x0200u;
constexpr std::uint16_t kDpadRight = 0x0100u;
constexpr int kStickEngageThreshold = 36;
constexpr int kStickReleaseThreshold = 22;
constexpr auto kInitialRepeatDelay = std::chrono::milliseconds(285);
constexpr auto kRepeatInterval = std::chrono::milliseconds(95);
constexpr auto kScreenReentryReset = std::chrono::milliseconds(250);

struct Direction {
    int x = 0;
    int y = 0;

    bool operator==(const Direction &) const = default;
};

struct NavigationState {
    Direction held_direction{};
    Clock::time_point next_repeat{};
    Clock::time_point last_call{};
    bool has_last_call = false;
};

NavigationState g_navigation{};

int sign_of(std::int8_t value) {
    return value < 0 ? -1 : 1;
}

Direction analog_direction(std::int8_t stick_x, std::int8_t stick_y) {
    const int abs_x = std::abs(static_cast<int>(stick_x));
    const int abs_y = std::abs(static_cast<int>(stick_y));

    // Retain the selected axis down to a lower release threshold. This keeps a
    // slightly diagonal stick from rapidly switching rows and columns.
    if (g_navigation.held_direction.x != 0 && abs_x >= kStickReleaseThreshold &&
        sign_of(stick_x) == g_navigation.held_direction.x) {
        return Direction{g_navigation.held_direction.x, 0};
    }
    if (g_navigation.held_direction.y != 0 && abs_y >= kStickReleaseThreshold &&
        sign_of(stick_y) == g_navigation.held_direction.y) {
        return Direction{0, g_navigation.held_direction.y};
    }

    if (abs_x < kStickEngageThreshold && abs_y < kStickEngageThreshold) {
        return {};
    }

    // Letter selection is a grid, so choose the dominant stick axis instead of
    // allowing a near-diagonal tilt to skip two choices at once.
    if (abs_x >= abs_y) {
        return abs_x >= kStickEngageThreshold ? Direction{sign_of(stick_x), 0} : Direction{};
    }
    return abs_y >= kStickEngageThreshold ? Direction{0, sign_of(stick_y)} : Direction{};
}

Direction requested_direction(std::uint16_t buttons, std::int8_t stick_x, std::int8_t stick_y) {
    const bool left = (buttons & kDpadLeft) != 0;
    const bool right = (buttons & kDpadRight) != 0;
    const bool up = (buttons & kDpadUp) != 0;
    const bool down = (buttons & kDpadDown) != 0;

    Direction dpad{};
    if (left != right) {
        dpad.x = left ? -1 : 1;
    }
    if (up != down) {
        // N64 stick Y is positive upward; keep that convention so D-pad and
        // analog navigation share the same row math below.
        dpad.y = up ? 1 : -1;
    }
    if (dpad.x != 0 || dpad.y != 0) {
        return dpad;
    }

    return analog_direction(stick_x, stick_y);
}

bool valid_grid_value(float value) {
    return std::isfinite(value) && value >= 1.0f && value <= 256.0f;
}

void step_cursor(unsigned char *rdram, Direction direction, std::uint32_t column_count,
                 std::uint32_t maximum_row) {
    if (column_count == 0 || column_count > 16 || maximum_row > 8) {
        return;
    }

    float cell_width = 0.0f;
    float cell_height = 0.0f;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    using namespace rr64::engine;
    if (!read_float(rdram, globals::name_entry_cell_width, cell_width) ||
        !read_float(rdram, globals::name_entry_cell_height, cell_height) ||
        !read_float(rdram, globals::name_entry_cursor_x, cursor_x) ||
        !read_float(rdram, globals::name_entry_cursor_y, cursor_y) ||
        !valid_grid_value(cell_width) || !valid_grid_value(cell_height) ||
        !std::isfinite(cursor_x) || !std::isfinite(cursor_y)) {
        return;
    }

    const int last_column = static_cast<int>(column_count) - 1;
    const int last_row = static_cast<int>(maximum_row);
    const int current_column = std::clamp(
        static_cast<int>(std::floor(std::max(0.0f, cursor_x) / cell_width)), 0, last_column);
    const int current_row = std::clamp(
        static_cast<int>(std::floor(std::max(0.0f, cursor_y) / cell_height)), 0, last_row);
    const int target_column = std::clamp(current_column + direction.x, 0, last_column);
    const int target_row = std::clamp(current_row - direction.y, 0, last_row);

    // Store the cursor at the center of the chosen cell. The original routine
    // selects a letter by truncating cursor/cell-size, so centering provides a
    // stable target and eliminates the mushy boundary behavior.
    const float target_x = (static_cast<float>(target_column) + 0.5f) * cell_width;
    const float target_y = (static_cast<float>(target_row) + 0.5f) * cell_height;
    write_float(rdram, globals::name_entry_cursor_x, target_x);
    write_float(rdram, globals::name_entry_cursor_y, target_y);
}

} // namespace

extern "C" void rr64_name_entry_navigation(unsigned char *rdram, unsigned int column_count,
                                           unsigned int maximum_row) {
    using namespace rr64::engine;
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0;
    std::int8_t stick_y = 0;
    if (!read_u16(rdram, globals::controller_pressed_buttons, buttons) ||
        !read_s8(rdram, globals::controller_stick_x, stick_x) ||
        !read_s8(rdram, globals::controller_stick_y, stick_y)) {
        return;
    }

    const auto now = Clock::now();
    if (g_navigation.has_last_call && now - g_navigation.last_call > kScreenReentryReset) {
        g_navigation.held_direction = {};
    }
    g_navigation.last_call = now;
    g_navigation.has_last_call = true;

    const Direction direction = requested_direction(buttons, stick_x, stick_y);

    // This shim owns navigation only while the name-entry routine calls it.
    // Neutralizing the original free cursor prevents a snapped D-pad/stick
    // move from also receiving the game's slower analog drift in the same frame.
    write_s8(rdram, globals::controller_stick_x, 0);
    write_s8(rdram, globals::controller_stick_y, 0);

    if (direction == Direction{}) {
        g_navigation.held_direction = {};
        return;
    }

    bool should_step = direction != g_navigation.held_direction;
    if (should_step) {
        g_navigation.held_direction = direction;
        g_navigation.next_repeat = now + kInitialRepeatDelay;
    } else if (now >= g_navigation.next_repeat) {
        should_step = true;
        g_navigation.next_repeat = now + kRepeatInterval;
    }

    if (should_step) {
        step_cursor(rdram, direction, column_count, maximum_row);
    }
}
