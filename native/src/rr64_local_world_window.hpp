#pragma once
#include <array>
#include <algorithm>
#include <cmath>

extern "C" bool rr64_draw_distance_enabled();
extern "C" double rr64_draw_distance_percent();
namespace rr64::world {
// Invert the camera's affine basis: the rebasing origin is not the eye.
inline bool terrain_eye(const std::array<float, 16> &v, double &x, double &y) {
    const double a = v[0], b = v[4], c = v[8], d = v[1], e = v[5], f = v[9], g = v[2], h = v[6],
                 i = v[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12 || v[3] != 0 || v[7] != 0 || v[11] != 0 ||
        v[15] != 1)
        return false;
    const double u = -v[12], w = -v[13], z = -v[14];
    x = (u * (e * i - f * h) - b * (w * i - f * z) + c * (w * h - e * z)) / det;
    y = (a * (w * i - f * z) - u * (d * i - f * g) + c * (d * z - w * g)) / det;
    return std::isfinite(x) && std::isfinite(y);
}
inline bool window_cell(double x, double y, double lowX, double lowY, double highX, double highY,
                        bool retained, double distance = 6000.0) {
    // Same conservative starting distance for 2–4 screens. Not an FPS guarantee.
    if (!std::isfinite(distance) || distance <= 0)
        return false;
    const double radius = retained ? distance * 1.25 : distance;
    const double dx = std::max(std::max(lowX - x, 0.0), x - highX);
    const double dy = std::max(std::max(lowY - y, 0.0), y - highY);
    return dx * dx + dy * dy <= radius * radius;
}
}
