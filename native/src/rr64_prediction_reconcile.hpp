#pragma once
#include "rr64_prediction_history.hpp"
#include "rr64_prediction_correction.hpp"
namespace rr64::prediction {
// Execute only at a native update boundary, before admitting the next input.
// True also covers a temporarily unavailable resource baseline: no ACK or
// correction is committed in that case; later snapshots may be retried.
// False leaves the existing live state/history intact. A caller must not retire
// a rejected packet or silently change back to non-authoritative simulation.
bool reconcile_movement(unsigned char*,std::span<const unsigned char> rom);
bool capture_prediction_movement(unsigned char*,const authority::Stamp&,unsigned local,
                                unsigned humans,bool mapped,netplay::AuthorityFrame&,
                                const char **failure=nullptr,bool diagnostic_ai_roster=false);
}
