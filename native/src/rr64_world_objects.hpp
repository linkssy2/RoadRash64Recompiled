#pragma once
#include "rr64_world_object_assets.hpp"
#include <algorithm>
#include <cmath>

namespace rr64::world {
using ObjectMatrix = std::array<float, 16>;
// Pure equivalents of the source-2 5F15C/5F21C root and 15A90 matrix writers.
// Keep the original subtraction order: camera rebasing makes it observable.
inline bool object_matrix(const ObjectPlacementAsset &placement, const std::array<float, 3> &camera,
                          const std::array<float, 3> &sector,
                          const std::array<float, 2> &billboard_eye,
                          ObjectMatrix &result) noexcept {
    std::array<float, 3> position{};
    auto q = placement.quaternion;
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(placement.position[i]) || !std::isfinite(camera[i]) ||
            !std::isfinite(sector[i]))
            return false;
        position[i] = placement.billboard ? (placement.position[i] - sector[i] - camera[i]) * 10.0f
                                          : (placement.position[i] - camera[i] - sector[i]) * 10.0f;
        if (!std::isfinite(position[i]) || std::abs(position[i]) > 10000000.0f)
            return false;
    }
    if (placement.billboard) {
        const float dx = billboard_eye[0] - (placement.position[0] - sector[0]);
        const float dy = billboard_eye[1] - (placement.position[1] - sector[1]);
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(length))
            return false;
        if (length == 0.0f)
            q = {0, 0, 0, 1};
        else {
            const float half_x = 0.5f * (dx / length);
            q = {0, 0, std::sqrt(0.5f - half_x), std::sqrt(0.5f + half_x)};
            if (dy < 0.0f)
                q[2] = -q[2];
        }
    }
    float norm = 0;
    for (float f : q) {
        if (!std::isfinite(f))
            return false;
        norm += f * f;
    }
    if (std::abs(norm - 1.0f) > 0.002f)
        return false;
    const auto [x, y, z, w] = q;
    const float xx = x * (x + x), xy = x * (y + y), xz = x * (z + z), yy = y * (y + y),
                yz = y * (z + z), zz = z * (z + z);
    const float wx = w * (x + x), wy = w * (y + y), wz = w * (z + z);
    result = {1 - (yy + zz), xy + wz, xz - wy,       0, xy - wz,     1 - (xx + zz), yz + wx,     0,
              xz + wy,       yz - wx, 1 - (xx + yy), 0, position[0], position[1],   position[2], 1};
    return true;
}
inline bool object_in_frustum(const ObjectModelAsset &model, const ObjectMatrix &root,
                              const ObjectMatrix &view, const ObjectMatrix &projection) noexcept {
    const auto transform = [](const std::array<float, 4> &p, const ObjectMatrix &m) {
        std::array<float, 4> out{};
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 4; ++r)
                out[c] += p[r] * m[r * 4 + c];
        return out;
    };
    unsigned outside_all = 63u;
    for (unsigned corner = 0; corner < 8; ++corner) {
        std::array<float, 4> p{(corner & 1) ? model.maximum[0] : model.minimum[0],
                               (corner & 2) ? model.maximum[1] : model.minimum[1],
                               (corner & 4) ? model.maximum[2] : model.minimum[2], 1};
        p = transform(transform(transform(p, root), view), projection);
        if (!std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); }))
            return false;
        unsigned mask = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const float limit = p[3] * (i == 0u ? 4.0f / 3.0f : 1.0f);
            if (p[i] < -limit)
                mask |= 1u << (i * 2u);
            if (p[i] > limit)
                mask |= 2u << (i * 2u);
        }
        outside_all &= mask;
    }
    return outside_all == 0u;
}
struct ObjectStatistics {
    unsigned cached_models = 0, cached_placements = 0, cached_bytes = 0;
    unsigned visible_placements = 0, stock_placements = 0, drawn_triangles = 0;
    unsigned long long frames = 0, refusals = 0, unmatched_stock = 0, texture_syncs = 0;
};
ObjectStatistics objects_statistics() noexcept;
void objects_reset_session() noexcept;
}
extern "C" {
void rr64_world_objects_begin(unsigned char *rdram);
void rr64_world_objects_observe(unsigned char *rdram, unsigned placement);
void rr64_world_objects_sample(unsigned char *rdram, unsigned placement, unsigned graph);
void rr64_world_objects_draw(unsigned char *rdram);
}
