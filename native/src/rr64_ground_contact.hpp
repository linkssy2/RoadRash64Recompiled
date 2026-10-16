#pragma once
#include <cmath>

namespace rr64::ground_contact {
inline float downward_scale(float delta) {
 constexpr float reference_delta=1.0f/30.0f;
 if(!std::isfinite(delta)||delta<=0||delta>=reference_delta)return 1.0f;
 // Preserve the native half-gap decay over elapsed time, not per substep.
 return 2.0f*-std::expm1(-0.6931471805599453f*delta/reference_delta);
}
inline float wheel_gap(float gap,float scale) {
 return std::isfinite(gap)&&gap<0?gap*scale:gap;
}
inline float support_force(float force, float delta) {
    constexpr float reference_delta = 1.0f / 30.0f;
    if (!std::isfinite(force) || force >= 0 || !std::isfinite(delta) ||
        delta <= 0 || delta >= reference_delta) return force;
    // Native negative support includes 1/dt: faster updates must not increase adhesion.
    return force * (delta / reference_delta);
}
}
