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
}
