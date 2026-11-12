#pragma once

#include <cstdint>

namespace rr64::lod {
// Evaluate the two detailed wheel poses and their model's static children for the
// original low-speed or airborne hold, including an owned ejected rider/bike.
// The caller supplies private RDRAM after fresh roots and rider animation.
// Only these certified pose spans and temporary guest stack may change. Wheel
// phases, simulation state, and the supplied register context never advance.
bool prepare_held_bike_pose(unsigned char *scratch, std::uint32_t bike_node,
                            const void *context) noexcept;
}
