#pragma once
#include <array>
#include <cmath>
namespace rr64::lod {
using ViewPoint = std::array<float, 2>;
inline bool racer_view_contains(ViewPoint p, ViewPoint a, ViewPoint b, ViewPoint c,
                                float side_scale) {
    // 19B28's A is the camera apex; B/C are its far corners. Expand only
    // the lateral span, retaining the original near apex and far distance.
    for (float v : {p[0], p[1], a[0], a[1], b[0], b[1], c[0], c[1], side_scale})
        if (!std::isfinite(v))
            return false;
    if (side_scale < 1.0f || side_scale > 2.0f)
        return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
        const float middle = (b[axis] + c[axis]) * 0.5f;
        b[axis] = middle + (b[axis] - middle) * side_scale;
        c[axis] = middle + (c[axis] - middle) * side_scale;
    }
    const float ax = a[0] - b[0], ay = a[1] - b[1], cx = c[0] - b[0], cy = c[1] - b[1];
    const float determinant = cy * ax - ay * cx;
    if (!std::isfinite(determinant) || determinant == 0.0f)
        return false;
    const float dx = b[0] - p[0], dy = b[1] - p[1];
    const float u = (dy * cx - dx * cy) / determinant, v = (ay * dx - ax * dy) / determinant;
    return std::isfinite(u) && std::isfinite(v) && u >= 0 && v >= 0 && u + v <= 1.0f;
}
}
