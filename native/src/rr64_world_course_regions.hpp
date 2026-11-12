#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <mutex>

namespace rr64::world {
// Connected authored terrain islands, not a distance or detail cutoff.
// A stock-visible cell anchors its entire island. Unknown/empty cells remain
// permitted; this filter must never invent terrain ownership for them.
struct CourseRegions {
    static constexpr unsigned width = 70, count = width * width;
    using Mask = std::array<bool, count>;
    std::array<unsigned, count> region{};
    void build(const Mask &occupied) {
        region = {};
        unsigned label = 0;
        std::array<unsigned, count> queue{};
        for (unsigned seed = 0; seed < count; ++seed) {
            if (!occupied[seed] || region[seed])
                continue;
            ++label;
            unsigned head = 0, tail = 0;
            queue[tail++] = seed;
            region[seed] = label;
            while (head < tail) {
                const auto i = queue[head++];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0)
                            continue;
                        const int x = int(i % width) + dx, y = int(i / width) + dy;
                        if (x < 0 || y < 0 || x >= int(width) || y >= int(width))
                            continue;
                        const unsigned next = unsigned(y) * width + unsigned(x);
                        if (occupied[next] && !region[next]) {
                            region[next] = label;
                            queue[tail++] = next;
                        }
                    }
            }
        }
    }
    Mask select(const Mask &stock) const {
        Mask selected{}, result{};
        bool anchored = false;
        for (unsigned i = 0; i < count; ++i)
            if (stock[i] && region[i]) {
                selected[region[i] - 1] = true;
                anchored = true;
            }
        for (unsigned i = 0; i < count; ++i)
            result[i] = !anchored || !region[i] || selected[region[i] - 1];
        return result;
    }
};
struct CourseFrame {
    std::mutex mutex;
    unsigned char *mapping = nullptr;
    unsigned epoch = 0;
    CourseRegions::Mask allowed{};
    void publish(unsigned char *m, unsigned e, const CourseRegions::Mask &mask) {
        std::lock_guard lock(mutex);
        mapping = m;
        epoch = e;
        allowed = mask;
    }
    CourseRegions::Mask read(unsigned char *m, unsigned e) {
        std::lock_guard lock(mutex);
        if (mapping == m && epoch == e)
            return allowed;
        CourseRegions::Mask all{};
        all.fill(true);
        return all;
    }
};
inline CourseFrame &course_frame() {
    static CourseFrame frame;
    return frame;
}
}
