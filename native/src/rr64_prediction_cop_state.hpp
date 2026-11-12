#pragma once
#include <array>
#include <cstdint>

namespace rr64::prediction {
// Native state is not stored in RDRAM. A prediction baseline must retain these
// values alongside its guest state. These are local types, never wire packets.
struct CopRulesState {
    bool race_enabled=false;
    float win_started=-1;
    unsigned initial_cops=0,initial_racers=0;
    bool operator==(const CopRulesState&) const = default;
};
struct CopPostState {
    bool placed=false,pursuit=false,muted=true,shout_held=false,hold_toggled=false,
         trick=false;
    float distance=0,cue=-100,shout_started=0;
    bool operator==(const CopPostState&) const = default;
};
using CopPostsState=std::array<CopPostState,14>;
struct ManualEjectProtection {
    std::uint32_t bike=0;
    float durability=0;
    bool active=false;
    bool operator==(const ManualEjectProtection&) const = default;
};
using ManualEjectState=std::array<ManualEjectProtection,14>;
bool capture_manual_eject(ManualEjectState &out);
bool seed_manual_eject(const ManualEjectState &historical);
// Game-thread only. Capture outside replay; seed only inside a ReplayScope.
// Neither seeding function can replace the live cop state.
bool capture_cop_rules(CopRulesState &out);
bool seed_cop_rules(const CopRulesState &historical);
bool capture_cop_posts(CopPostsState &out);
bool seed_cop_posts(const CopPostsState &historical);
// Export only the current transaction's shadow, never live state. Used to carry
// corrected native state forward across successive replayed input steps.
bool replay_cop_rules(CopRulesState &out);
bool replay_cop_posts(CopPostsState &out);
bool replay_manual_eject(ManualEjectState &out);
// Only the game-thread reconciliation transaction uses these, after validating
// guest ownership and consuming its ticket. Plain assignments cannot fail.
void commit_cop_rules(const CopRulesState&) noexcept;
void commit_cop_posts(const CopPostsState&) noexcept;
void commit_manual_eject(const ManualEjectState&) noexcept;
}
