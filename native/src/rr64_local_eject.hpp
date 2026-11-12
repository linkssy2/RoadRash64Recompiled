#pragma once
#include "rr64_engine_layout.hpp"

namespace rr64::local_eject {
// Human controller records bind slots to bikes; roster order alone is not an
// ownership certificate. Keep slot zero's existing single-player/online path.
inline bool resolve_bike(unsigned char *rdram, unsigned slot, unsigned &bike) {
    using namespace engine;
    unsigned pool = 0, racers = 0;
    if (slot >= 4 || !read_u32(rdram, globals::bike_pool_pointer, pool) ||
        !read_u32(rdram, globals::active_racer_count, racers) || racers == 0 ||
        racers > kMaximumRacers || !valid_guest_range(pool, racers * engine::bike::stride))
        return false;
    unsigned selected = pool;
    if (slot != 0) {
        unsigned humans = 0;
        if (!read_u32(rdram, local_race::humans, humans) || humans > 4 || slot >= humans ||
            !read_u32(rdram, 0x800D8570u + slot * 0x118u + 0xE0u, selected))
            return false;
    }
    if (selected < pool || (selected - pool) % engine::bike::stride != 0 ||
        (selected - pool) / engine::bike::stride >= racers)
        return false;
    bike = selected;
    return true;
}
}
