#include "render/rt64_buffer_capacity.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const bool old = argc == 2 && std::strcmp(argv[1], "--old-capacity") == 0;
    auto capacity = [old](uint64_t size) {
        return RT64::uploadBufferCapacity(size, !old);
    };
    // A later camera view grows geometry beyond the old 50% headroom.
    const auto initial = capacity(1024 * 1024);
    if (initial < 1792 * 1024) return 1;
    for (uint64_t n = 0; n < 100000; ++n) {
        const auto legacy = std::max(uint64_t(256), ((n * 3 / 2) + 255) & ~uint64_t(255));
        if (RT64::uploadBufferCapacity(n) != legacy) return 2;
        for (bool draw : {false, true}) {
            auto size = RT64::uploadBufferCapacity(n, draw);
            if (size < n || size % 256 || size < legacy ||
                size > std::max(uint64_t(256), (n * 2 + 255) & ~uint64_t(255))) return 3;
        }
    }
    const uint64_t largest = UINT64_MAX & ~uint64_t(255);
    if (RT64::uploadBufferCapacity(largest, true) != largest) return 4;
    try { RT64::uploadBufferCapacity(UINT64_MAX); return 5; }
    catch (const std::length_error&) {}
    std::puts("Buffer capacity: draw headroom, bounded memory, legacy callers and overflow pass");
}
