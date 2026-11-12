#include <cmath>
#include <cstdio>

#include "rr64_menu_navigation.hpp"

namespace {

bool near(float first, float second) {
    return std::abs(first - second) < 0.001f;
}

bool expect(float x, float y, float expected_x, float expected_y, const char* message) {
    if (near(x, expected_x) && near(y, expected_y)) {
        return true;
    }
    std::fprintf(stderr, "%s got=(%.2f,%.2f) expected=(%.2f,%.2f)\n",
        message, x, y, expected_x, expected_y);
    return false;
}

} // namespace

int main() {
    rr64::menu_navigation::State state{};

    float x = 0.62f;
    float y = 0.12f;
    rr64::menu_navigation::filter(state, 0, x, y, 1000);
    if (!expect(x, y, 1.0f, 0.0f, "Initial dominant-axis pulse failed.")) return 1;

    x = 0.62f;
    y = 0.12f;
    rr64::menu_navigation::filter(state, 0, x, y, 1100);
    if (!expect(x, y, 0.0f, 0.0f, "Held direction repeated too early.")) return 1;

    x = 0.62f;
    y = 0.12f;
    rr64::menu_navigation::filter(state, 0, x, y, 1285);
    if (!expect(x, y, 1.0f, 0.0f, "Initial repeat did not fire.")) return 1;

    x = 0.0f;
    y = 0.0f;
    rr64::menu_navigation::filter(state, 0, x, y, 1300);
    x = 0.0f;
    y = 0.0f;
    rr64::menu_navigation::filter(state, 0x0400u, x, y, 1310);
    if (!expect(x, y, 0.0f, -1.0f, "D-pad menu pulse failed.")) return 1;

    x = 0.05f;
    y = 0.07f;
    rr64::menu_navigation::filter(state, 0, x, y, 1320);
    if (!expect(x, y, 0.0f, 0.0f, "Stick noise escaped the dead zone.")) return 1;

    std::puts("RR64 general menu navigation smoke test passed.");
    return 0;
}
