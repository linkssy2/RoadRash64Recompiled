#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include "rr64_integrator_state.hpp"

namespace rr64::authority {
// Native integrator values plus detached-body damping inputs from36B78.
// Collision contacts, actor callbacks and resource ownership are not included;
// this is not yet a complete simulation-replay schema.
struct RiderDynamics {
    prediction::IntegratorState bike_physics{},rider_physics{};
    std::array<float,12> values{};
    // 36F70 selects the detached damping branch from this integer at +20.
    std::uint32_t damping_mode=0;
    //37960 installs effect1/2 and its duration;37088 decrements the duration
    // and370B4 clears the effect. These are values, not remote pointers.
    std::uint32_t effect=0;
    float effect_remaining=0;
    // Bike+4CC is the native recovery clock read by3FFA0..3FFC0. Attachment
    // flags alone cannot restore how far a crash has progressed.
    float recovery_age=0;
    bool operator==(const RiderDynamics&) const = default;
};
//37024/37040 also damp physics+A8 = rider+D0. Omitting that independent
// vector leaves a corrected detached body with part of its old motion state.
inline constexpr std::array<unsigned,4> rider_dynamics_offsets{0x98,0x174,0x180,0xd0};
inline bool valid_dynamics(const RiderDynamics &state) {
    for(float value:state.values)if(!std::isfinite(value))return false;
    return prediction::valid_integrator(state.bike_physics) && prediction::valid_integrator(state.rider_physics) &&
        state.effect<=2 && std::isfinite(state.effect_remaining) && state.effect_remaining>=0 && std::isfinite(state.recovery_age);
}
}
