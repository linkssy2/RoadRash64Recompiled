#pragma once

#include <array>
#include <cstdint>

#include "recomp.h"

namespace rr64::course_impact {
using Vec3 = std::array<float, 3>;
enum class Body { Bike, Rider };
struct Contact {
    Vec3 point{};
    Vec3 normal{}; // Out of the obstacle, in native rider-world coordinates.
    Vec3 surface_velocity{};
    float penetration = 0;
    // Native solid-traffic/static-plane response at 8004DAC8, not a crash threshold.
    float response = 1.02f;
};
struct Result {
    bool applied = false;
    float impulse = 0;
    Vec3 velocity_delta{};
};

// Call immediately after native34594 and before native348D8. This integrates
// only the new contact impulse; the caller owns positional contact correction.
// Native rider update36B78 decides detachment from the resulting velocity
// difference, and37554 supplies the original trajectory and durability loss.
// No manual-eject, damage, bust, attribution, or crash flag is written here.
// Accepted impacts also use native bike/body hit sounds and native cooldowns.
// Online clients/replay return unchanged; their geometric projection is separate.
Result apply(unsigned char *memory, const recomp_context &context,
             std::uint32_t actor_index, Body body, const Contact &contact);
}

