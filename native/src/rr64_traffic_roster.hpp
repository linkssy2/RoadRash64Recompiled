#pragma once
#include "rr64_engine_layout.hpp"

namespace rr64::traffic {
inline constexpr unsigned capacity = 20;
inline unsigned table(unsigned char *) { return 0x800D76E0u; }
inline unsigned storage(unsigned char *) { return capacity; }
inline bool read_entity(unsigned char *memory, unsigned index, unsigned &value) {
    return index < capacity && engine::read_u32(memory, table(memory) + index * 4, value);
}
inline unsigned entity(unsigned char *memory, unsigned index) {
    unsigned value = 0;
    read_entity(memory, index, value);
    return value;
}
inline bool owns_ground_query(unsigned char *memory, unsigned query) {
    // 75EA0 queries both axles; authenticate the owner against the live roster.
    for (unsigned offset : {0x238u, 0x2a4u}) {
        if (query < offset) continue;
        const unsigned car = query - offset;
        unsigned index = capacity;
        if (engine::valid_guest_range(car, 0x360) &&
            engine::read_u32(memory, car + 0x28, index) && index < capacity &&
            entity(memory, index) == car) return true;
    }
    return false;
}
}
