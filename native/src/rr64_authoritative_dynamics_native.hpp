#pragma once
#include "rr64_authoritative_dynamics.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_integrator.hpp"

namespace rr64::authority {
// Restore only the audited, pointer-free movement fields. All validation precedes
// writes; unrelated animation, resource and collision pointers remain local.
inline bool restore_dynamics(unsigned char *memory,unsigned rider,const RiderDynamics &state) {
    if(!memory || (rider&3) || !engine::valid_guest_range(rider,engine::rider::stride) || !valid_dynamics(state))return false;
    unsigned bike=0,owner=0;
    if(!engine::read_u32(memory,rider+engine::rider::bike_pointer,bike) ||
       (bike&3) || !engine::valid_guest_range(bike,engine::bike::stride) ||
       !engine::read_u32(memory,bike+engine::bike::rider_pointer,owner) || owner!=rider)return false;
    // Pair and every value have been validated before either object changes.
    prediction::restore_integrator(memory,bike+0x108,state.bike_physics);
    prediction::restore_integrator(memory,rider+0x28,state.rider_physics);
    for(unsigned vector=0;vector<rider_dynamics_offsets.size();++vector)
        for(unsigned axis=0;axis<3;++axis)
            engine::write_float(memory,rider+rider_dynamics_offsets[vector]+axis*4,state.values[vector*3+axis]);
    engine::write_u32(memory,rider+0x20,state.damping_mode);
    engine::write_u32(memory,rider+0x5d4,state.effect);
    engine::write_float(memory,rider+0x5d8,state.effect_remaining);
    engine::write_float(memory,bike+0x4cc,state.recovery_age);
    return true;
}
}
