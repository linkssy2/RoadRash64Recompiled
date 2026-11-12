#pragma once
#include <array>
#include <cmath>
#include "rr64_view_width.hpp"

namespace rr64::world {
// Immutable asset bounds; safe to reuse across cameras. Terrain's certified
// transform is diagonal (1,1,0.5) plus XY translation, with no rotation.
struct TerrainBounds {
    std::array<double, 3> center{}, half{};
    bool valid = true;
    TerrainBounds(const std::array<float, 3> &low, const std::array<float, 3> &high) {
        for (unsigned i = 0; i < 3; ++i) {
            center[i] = (double(low[i]) + high[i]) * 0.5;
            half[i] = (double(high[i]) - low[i]) * 0.5;
            valid &= std::isfinite(center[i]) && std::isfinite(half[i]) && half[i] >= 0;
        }
    }
};
// Build six world-space clip planes once per camera pass. Test each model's
// transformed box against them instead of transforming eight corners three times.
// A small relative margin keeps boundary rounding conservative.
struct WorldFrustum {
    std::array<std::array<double, 4>, 6> planes{};
    bool valid = true;
    WorldFrustum(const std::array<float, 16> &view, const std::array<float, 16> &projection) {
        std::array<double, 16> clip{};
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                for (unsigned k = 0; k < 4; ++k)
                    clip[r * 4 + c] += double(view[r * 4 + k]) * projection[k * 4 + c];
        for (unsigned axis = 0; axis < 3; ++axis)
            for (unsigned sign = 0; sign < 2; ++sign)
                for (unsigned r = 0; r < 4; ++r) {
                    const double value =
                        clip[r * 4 + 3] *
                            (axis == 0 ? rr64::view_width.load(std::memory_order_relaxed) : 1.0) +
                        (sign ? -1.0 : 1.0) * clip[r * 4 + axis];
                    planes[axis * 2 + sign][r] = value;
                    valid &= std::isfinite(value);
                }
    }
    bool intersects(const std::array<float, 3> &low, const std::array<float, 3> &high,
                    const std::array<float, 16> &model) const {
        if (!valid)
            return false;
        std::array<double, 3> center{}, half{};
        for (unsigned i = 0; i < 3; ++i) {
            center[i] = (double(low[i]) + high[i]) * 0.5;
            half[i] = (double(high[i]) - low[i]) * 0.5;
            if (!std::isfinite(center[i]) || !std::isfinite(half[i]) || half[i] < 0)
                return false;
        }
        for (const auto &plane : planes) {
            double distance = 0, radius = 0;
            for (unsigned row = 0; row < 4; ++row) {
                double coefficient = 0;
                for (unsigned c = 0; c < 4; ++c)
                    coefficient += double(model[row * 4 + c]) * plane[c];
                if (!std::isfinite(coefficient))
                    return false;
                if (row < 3) {
                    distance += center[row] * coefficient;
                    radius += half[row] * std::abs(coefficient);
                } else
                    distance += coefficient;
            }
            if (distance + radius < -0.0001 * (std::abs(distance) + radius + 1.0))
                return false;
        }
        return true;
    }
    bool intersectsTerrain(const TerrainBounds &bounds, float x, float y) const {
        if (!valid || !bounds.valid || !std::isfinite(x) || !std::isfinite(y))
            return false;
        for (const auto &p : planes) {
            const double z = p[2] * 0.5;
            const double distance = bounds.center[0] * p[0] + bounds.center[1] * p[1] +
                                    bounds.center[2] * z +
                                    (double(x) * p[0] + double(y) * p[1] + p[3]);
            const double radius = bounds.half[0] * std::abs(p[0]) +
                                  bounds.half[1] * std::abs(p[1]) + bounds.half[2] * std::abs(z);
            if (distance + radius < -0.0001 * (std::abs(distance) + radius + 1.0))
                return false;
        }
        return true;
    }
};
}
