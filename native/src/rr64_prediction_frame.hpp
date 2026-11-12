#pragma once
#include "rr64_prediction_resource_worker.hpp"
#include "rr64_prediction_cop_state.hpp"
#include "rr64_authoritative_input.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_prediction_timing.hpp"
#include "rr64_prediction_streaming.hpp"
#include "rr64_prediction_visibility.hpp"
#include "rr64_prediction_terrain_availability.hpp"
#include "rr64_prediction_item_state.hpp"
namespace rr64::prediction {
struct FrameInput {
    authority::NativeTiming timing;
    UpdateCounters counters{};
    SessionRules session;
    CpuContext entry;
    CopRulesState rules;
    CopPostsState posts;
    ManualEjectState eject;
    authority::Command command;
    unsigned actor=0,passes=0;
    std::uint16_t previous_buttons=0;
    VisibilityInputs visibility;
    TerrainAvailability terrain;
    mk64_items::ReplayState items;
    // Private replay can begin partway through a command. Zero selects the
    // original duration; nonzero is the exact unconsumed interval in history.
    unsigned replay_duration_us=0;
};
struct FrameOutput {
    CopRulesState rules;
    CopPostsState posts;
    ManualEjectState eject;
    mk64_items::ReplayState items;
};
// Disposable replay only. Success means the isolated native update completed,
// not that its memory can be copied into live RDRAM. Correction ownership and
// native cadence are separate integration gates.
bool replay_native_frame(Resources&,const FrameInput&,FrameOutput&);
// Diagnostic integration gate: derive order within a disposable image only.
bool replay_native_order(Resources&,const FrameInput&);
bool replay_native_streaming(Resources&,const StreamingInput&,const ResourceInventory&);
const char *last_native_replay_error();
}
