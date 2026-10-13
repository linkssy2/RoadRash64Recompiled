#pragma once

#ifdef __cplusplus
namespace rr64::offline_modifiers {
enum class Flag : unsigned {
    RiderHealth = 1,
    BikeDurability = 2,
    AllWeapons = 4,
    AllBikes = 8,
    FreezeOpponents = 16,
    AllTracks = 32,
};

// UI publishes preferences only. Guest mutations remain on the game thread.
void set_requested(Flag flag, bool value);
bool enabled(Flag flag);
bool enabled_any();
void reset(unsigned char *live_memory);
void register_config_tab();
void apply_config();
void update_ui();
}

extern "C" {
#endif
void rr64_offline_modifiers_frame(unsigned char *memory);
void rr64_offline_modifiers_race_begin(unsigned char *memory);
void rr64_offline_modifiers_equipment(unsigned char *memory, unsigned actor);
// True only for an attached offline AI racer whose control and paired physics
// pass may be held. Never changes actor/race flags or retained control locks.
int rr64_offline_modifiers_freeze_actor(unsigned char *memory, unsigned actor);
int rr64_offline_rider_protected(unsigned char *memory, unsigned rider);
int rr64_offline_bike_protected(unsigned char *memory, unsigned bike);
int rr64_player_session_started();
#ifdef __cplusplus
}
#endif
