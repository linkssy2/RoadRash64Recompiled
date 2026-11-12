#pragma once
#include <array>
#include <cstdint>

namespace rr64::weapon {
struct Sample {
    std::uint32_t node{}, graph{}, record{}, view{}, model{};
    std::uint16_t source{}, parentSource{}, lod{};
    float distanceSquared{};
    std::array<float, 3> parent{}, translation{}, packed{};
};
struct Report {
    std::array<Sample, 64> samples{};
    unsigned size{}, omitted{}, examined{};
};
// At most one latest sample per owner/view/root per logging interval.
Report take_report();
}
