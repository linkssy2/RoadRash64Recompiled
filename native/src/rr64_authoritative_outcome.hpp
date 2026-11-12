#pragma once
#include <cmath>
#include <array>
#include <cstdint>

namespace rr64::authority {
// Pointer-free race results and cop presentation from the same host tick as
// movement. Receiving these values never awards local hits or busts.
struct Outcome {
    std::uint32_t valid=0;
    std::uint32_t role=0,busts=0;
    std::uint16_t eligible=0,busted=0;
    //66AC8 reads stats+50 before recovery;66B84 consumes+40 as an integer.
    // Preserve the native values instead of reconstructing them from health.
    std::uint32_t recovery_count=0;
    std::uint16_t recovery_flag=0;
    // 6EF78 sets stats+52 on crossing the finish; 6EA8C/6EF50 read
    // the full +50 word. Both halves must survive authoritative correction.
    std::uint16_t finished=0;
    // 68D28..68D50 computes race-order distance from stats+8,+C,+20.
    // Carry the three scalar inputs together, not a rounded HUD distance.
    std::array<float,3> progress{};
    // 68D48 tests the full +4C word; retain its second half as well.
    std::uint16_t progress_gate=0;
    std::uint32_t siren=0;
    float cue_age=-1;
    bool operator==(const Outcome&)const=default;
};
inline bool valid_outcome(const Outcome &state){
    for(float value:state.progress)if(!std::isfinite(value))return false;
    return state.valid<=1 && state.siren<=1 && std::isfinite(state.cue_age) &&
        (state.cue_age==-1 || (state.cue_age>=0 && state.cue_age<4));
}
}
