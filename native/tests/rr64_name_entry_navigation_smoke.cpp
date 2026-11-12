#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "rr64_engine_layout.hpp"
#include "rr64_native.hpp"

namespace {

bool near(float first, float second) {
    return std::fabs(first - second) < 0.01f;
}

bool cursor_is(unsigned char* rdram, float expected_x, float expected_y) {
    float x = 0.0f;
    float y = 0.0f;
    return rr64::engine::read_float(rdram, rr64::engine::globals::name_entry_cursor_x, x) &&
        rr64::engine::read_float(rdram, rr64::engine::globals::name_entry_cursor_y, y) &&
        near(x, expected_x) && near(y, expected_y);
}

void set_input(unsigned char* rdram, std::uint16_t buttons, std::int8_t x, std::int8_t y) {
    rr64::engine::write_u16(rdram, rr64::engine::globals::controller_pressed_buttons, buttons);
    rr64::engine::write_s8(rdram, rr64::engine::globals::controller_stick_x, x);
    rr64::engine::write_s8(rdram, rr64::engine::globals::controller_stick_y, y);
}

} // namespace

int main() {
    std::vector<unsigned char> memory(rr64::engine::kRdramSize, 0);
    unsigned char* rdram = memory.data();
    rr64::engine::write_float(rdram, rr64::engine::globals::name_entry_cell_width, 32.0f);
    rr64::engine::write_float(rdram, rr64::engine::globals::name_entry_cell_height, 24.0f);
    rr64::engine::write_float(rdram, rr64::engine::globals::name_entry_cursor_x, 16.0f);
    rr64::engine::write_float(rdram, rr64::engine::globals::name_entry_cursor_y, 12.0f);

    set_input(rdram, 0x0100u, 127, 0); // D-pad right, mirrored stick present.
    rr64_name_entry_navigation(rdram, 8, 4);
    if (!cursor_is(rdram, 48.0f, 12.0f)) {
        std::fprintf(stderr, "D-pad press did not advance exactly one column.\n");
        return 1;
    }

    std::int8_t x = 1;
    std::int8_t y = 1;
    rr64::engine::read_s8(rdram, rr64::engine::globals::controller_stick_x, x);
    rr64::engine::read_s8(rdram, rr64::engine::globals::controller_stick_y, y);
    if (x != 0 || y != 0) {
        std::fprintf(stderr, "Original analog drift was not suppressed.\n");
        return 1;
    }

    set_input(rdram, 0x0100u, 127, 0);
    rr64_name_entry_navigation(rdram, 8, 4);
    if (!cursor_is(rdram, 48.0f, 12.0f)) {
        std::fprintf(stderr, "Held direction repeated before its delay.\n");
        return 1;
    }

    set_input(rdram, 0, 0, 0);
    rr64_name_entry_navigation(rdram, 8, 4);
    set_input(rdram, 0x0400u, 0, -127); // D-pad down.
    rr64_name_entry_navigation(rdram, 8, 4);
    if (!cursor_is(rdram, 48.0f, 36.0f)) {
        std::fprintf(stderr, "D-pad down did not advance exactly one row.\n");
        return 1;
    }

    set_input(rdram, 0, 0, 0);
    rr64_name_entry_navigation(rdram, 8, 4);
    set_input(rdram, 0, -50, 8); // Intentional left tilt with minor vertical noise.
    rr64_name_entry_navigation(rdram, 8, 4);
    if (!cursor_is(rdram, 16.0f, 36.0f)) {
        std::fprintf(stderr, "Analog dominant-axis navigation was unstable.\n");
        return 1;
    }

    std::puts("RR64 name-entry navigation smoke test passed.");
    return 0;
}
