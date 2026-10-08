#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace RT64 {
    // Draw geometry changes with the camera. Double on growth to avoid repeatedly
    // reallocating mid-race; other uploaders keep their existing 50% headroom.
    inline uint64_t uploadBufferCapacity(uint64_t required, bool drawGeometry = false) {
        constexpr uint64_t maxAligned = std::numeric_limits<uint64_t>::max() & ~uint64_t(255);
        if (required > maxAligned) throw std::length_error("Upload buffer exceeds aligned address space");
        const uint64_t headroom = required > maxAligned / 2 ? maxAligned : required * 2;
        const uint64_t legacy = required > maxAligned - required / 2 ? maxAligned : required + required / 2;
        const uint64_t size = std::max(uint64_t(256), drawGeometry ? headroom : legacy);
        return size >= maxAligned ? maxAligned : (size + 255) & ~uint64_t(255);
    }
}
