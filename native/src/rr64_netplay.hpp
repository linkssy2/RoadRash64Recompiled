#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "rr64_online_flow.hpp"
#include "rr64_race_pack_identity.hpp"
#include "rr64_course_hazard_state.hpp"
#include "rr64_mk64_item_state.hpp"
#include "rr64_world_sync.hpp"
#include "rr64_attack_visual.hpp"
#include "rr64_authoritative_round.hpp"
#include "rr64_authoritative_timing.hpp"
#include "rr64_authoritative_outcome.hpp"
#include "rr64_authoritative_dynamics.hpp"
#include "rr64_highlight_network.hpp"

namespace rr64::netplay {

constexpr std::uint16_t kDefaultPort = 6464;
// Road Rash allocates fourteen native bike and rider records. Network slots
// use that full pool, while N64 controller emulation remains limited to the
// console's four physical ports.
constexpr std::uint8_t kMaximumNetworkPlayers = 14;
constexpr std::uint8_t kMaximumLocalControllers = 4;
constexpr std::uint8_t kMaximumPlayers = kMaximumNetworkPlayers;
constexpr std::uint8_t kInvalidSlot = 0xFF;
constexpr std::size_t kVoicePayloadCapacity = 256;

enum class Mode : std::uint8_t {
    Offline = 0,
    Host,
    Join,
};

enum class Phase : std::uint8_t {
    Offline = 0,
    Connecting,
    Lobby,
    GameSetup,
    CharacterSelect,
    TrackSelect,
    Race,
};

// The game-side bridge owns the meaning and guest addresses of these words.
// Netplay only transports the host's finalized stock multiplayer settings.
constexpr std::size_t kGameSetupWordCount = 17;

struct GameSetupState {
    bool valid = false;
    std::uint32_t revision = 0;
    std::uint16_t transition_buttons = 0;
    // Recomp-added AI count, pedestrian density, bike tier and Custom Cop mode.
    std::uint32_t race_options = 0;
    std::uint32_t random_seed = 0;
    std::uint32_t start_requested = 0;
    std::array<std::uint32_t, kGameSetupWordCount> words{};
    race_pack::Identity course{};

    bool operator==(const GameSetupState &) const = default;
};

struct Config {
    std::uint8_t maximum_players = kMaximumPlayers;
    Mode mode = Mode::Offline;
    std::string player_name = "Rider";
    std::string host_address = "127.0.0.1";
    std::uint16_t port = kDefaultPort;

    bool operator==(const Config &) const = default;
};

struct PlayerInfo {
    online_flow::Selection selection{};
    race_pack::Compatibility course_compatibility = race_pack::Compatibility::Pending;
    bool connected = false;
    bool ready = false;
    std::uint8_t slot = kInvalidSlot;
    std::uint8_t character = 0;
    std::uint8_t track = 0;
    std::uint16_t ping_ms = 0;
    std::string name{};
};

// Explicit pointer-free visual-root state, captured with the positions at one tick.
struct RiderRootState {
    AttackVisual attack{};
    std::uint16_t valid = 0;
    // Native bike +818: 4CBFC starts a vault; 63264 advances 1 -> 2 -> 3 -> 0.
    // Shares the former 32-bit validity slot to preserve the UDP packet budget.
    std::uint16_t vault_latch = 0;
    std::array<float, 4> bike_rotation{}, rider_rotation{};
    // Alternate bike graph builder 5E880 uses these instead of the quaternion.
    std::array<float, 3> bike_display_angles{}; // +21C, +4AC, +4B8
    // Primary movement sources, not camera-space or LOD-derived positions.
    std::array<float, 3> bike_origin{}, bike_velocity{}, bike_motion{}, rider_velocity{}, rider_anchor{};
    float bike_height = 0, rider_height = 0;
    // Native durability is float, not the legacy uint16 health placeholder.
    float durability = 0, durability_capacity = 0;
    // Rider +310 is depleted by native impact handling independently of bike durability.
    float rider_impact_reserve = 0;
    std::uint16_t equipment_valid = 0;
    std::array<std::uint16_t,15> inventory{};
    std::uint16_t bike_attached = 0, rider_attached = 0, ejected = 0, drive_lockout = 0;
};

struct RiderState {
    bool host_ai = false; // Only host can publish a non-player roster actor.
    bool active = false;
    std::uint32_t tick = 0;
    std::uint64_t sample_time_us = 0; // Owner monotonic time; not wall clock.
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float front_wheel_x = 0.0f;
    float front_wheel_y = 0.0f;
    float front_wheel_z = 0.0f;
    float rear_wheel_x = 0.0f;
    float rear_wheel_y = 0.0f;
    float rear_wheel_z = 0.0f;
    std::uint32_t rider_position_valid = 0;
    float rider_x = 0.0f, rider_y = 0.0f, rider_z = 0.0f;
    RiderRootState root{};
    float bike_lean = 0.0f;
    float front_suspension = 0.0f;
    float rear_suspension = 0.0f;
    std::uint16_t health = 0;
    std::uint16_t flags = 0;
    std::uint8_t animation = 0;
    std::uint8_t weapon = 0;
    std::uint8_t character = 0;
    std::uint8_t bike = 0;
};

// A native contact proposal, not a damage amount invented by networking.
struct HitEvent {
    std::uint32_t round=0, id=0;
    std::uint8_t attacker=0, victim=0, kind=0; // 0:61224, 1:616BC
    float strength=0;
    AttackVisual attack{};
};
bool submit_hit(HitEvent event);
bool take_hit(HitEvent &event);

struct ReceivedVoiceFrame {
    std::uint8_t speaker_slot = kInvalidSlot;
    std::uint32_t sequence = 0;
    std::uint16_t payload_size = 0;
    std::array<std::uint8_t, kVoicePayloadCapacity> payload{};
};

// Internal simulation integration API; not a launcher setting.
bool acknowledge_course(std::uint32_t revision, const race_pack::Identity& course);
// A pre-race content failure revokes this round's ACK without leaving the lobby.
bool report_course_compatibility(std::uint32_t revision, const race_pack::Identity& course,
                                 race_pack::Compatibility compatibility);
const char* course_compatibility_message(race_pack::Compatibility compatibility) noexcept;
// Legacy caller convenience; preserves the connection and reports Different.
void reject_course_setup(const char *reason);
bool authority_start();
// Native race initialization calls this while its update is still held. Host
// releases only after every race participant has acknowledged this round.
bool authority_race_gate(bool native_loaded);
// Stock results/main-menu transitions are selected by the host. Zero means
// no transition; the game dispatcher still owns teardown and initialization.
bool authority_set_finish_mode(unsigned mode);
unsigned authority_finish_mode();
// Post-race presentation only. The canonical final simulation/results stay frozen.
bool publish_highlight_playlist(highlight_network::Blob, std::uint64_t duration_us);
std::optional<highlight_network::Blob> take_received_highlight_playlist();
void acknowledge_highlight_decoded(bool accepted);
highlight_network::Status highlight_status();
bool host_begin_highlights();
bool host_skip_highlights();
// End a failed authority session without falling back into local simulation.
void authority_fail(const char *reason);
bool authority_queue_input(std::uint16_t buttons,std::int8_t x,std::int8_t y);
bool authority_queue_input_recorded(std::uint16_t buttons,std::int8_t x,std::int8_t y,authority::Command &accepted,std::uint8_t actions=0,unsigned duration_us=16667);
bool authority_begin_step(authority::Step &step,unsigned duration_us=16667);
bool authority_finish_step(std::uint64_t tick,authority::Stamp &stamp);
// Imported item placements are immutable course data. Only their host-owned
// clock, cooldowns and break generations travel with a completed race tick.
// Rewards use the existing rider inventory/selection snapshot, never events
// that a duplicated packet could award twice.
constexpr unsigned kMaximumCourseItems = 64;
constexpr unsigned kCourseItemCooldown = 90; // Deliberate 3s adaptation at30Hz.
constexpr unsigned kCourseRouletteTicks = 48;
constexpr unsigned kCourseRewardBlinkTicks = 30;
// Item rewards have their own IDs. 15/16 are power-ups, never native weapon
// inventory indices; only the weapon subset may reach the weapon dispatcher.
constexpr unsigned kCourseRewardAttackX2 = 15, kCourseRewardAttackX4 = 16;
constexpr bool is_course_weapon_reward(unsigned reward) noexcept {
    return reward >= 2 && reward <= 14;
}
constexpr bool valid_course_reward(unsigned reward) noexcept {
    return is_course_weapon_reward(reward) || reward == kCourseRewardAttackX2 ||
           reward == kCourseRewardAttackX4 || mk64_items::is_reward(reward);
}
constexpr unsigned course_reward_effect(unsigned reward) noexcept {
    return reward == kCourseRewardAttackX2 ? 1u : reward == kCourseRewardAttackX4 ? 2u : 0u;
}
// The host chooses once on contact and grants once when the scroll completes.
// This travels in the same complete tick as inventories, never as a replayable
// reward command. Phase:0 inactive,1 scrolling,2 granted/blinking.
struct CourseWeaponRoll {
    std::uint32_t generation = 0, start_clock = 0;
    std::uint16_t weapon = 0, phase = 0; // Reward ID; retained wire field name/layout.
    bool operator==(const CourseWeaponRoll &) const = default;
};
struct CourseItemState {
    std::uint32_t clock = 0;
    std::uint16_t count = 0;
    std::array<std::uint16_t,kMaximumCourseItems> cooldown{};
    std::array<std::uint32_t,kMaximumCourseItems> generation{};
    std::array<CourseWeaponRoll,kMaximumPlayers> roulette{};
    bool operator==(const CourseItemState &) const = default;
};
inline bool valid_course_item_state(const CourseItemState &state) noexcept {
    if(state.count>kMaximumCourseItems || (!state.count && state.clock))return false;
    for(unsigned i=0;i<kMaximumCourseItems;++i)
        if(state.cooldown[i]>kCourseItemCooldown ||
           (i>=state.count && (state.cooldown[i] || state.generation[i])))return false;
    for (const auto &roll : state.roulette) {
        if (roll.phase > 2 || (!state.count && roll.generation) ||
            (roll.phase && (!roll.generation || !valid_course_reward(roll.weapon) ||
                            roll.start_clock > state.clock)) ||
            (!roll.phase && (roll.weapon || roll.start_clock)) ||
            (roll.phase == 2 && state.clock - roll.start_clock < kCourseRouletteTicks))
            return false;
    }
    return true;
}
struct AuthorityFrame {
    authority::Stamp stamp{};
    authority::NativeTiming timing{};
    std::array<RiderState,kMaximumPlayers> riders{};
    std::array<authority::Outcome,kMaximumPlayers> outcomes{};
    std::array<authority::RiderDynamics,kMaximumPlayers> dynamics{};
    std::array<world_sync::Traffic,world_sync::capacity> traffic{};
    std::uint32_t cop_mode=0;
    float cop_win_age=-1;
    CourseItemState course_items{};
    CourseHazardState course_hazards{};
    mk64_items::Snapshot mk64_items{};
};
// Publication is allowed only for the last completed host step. Reading a
// snapshot does not discard predicted inputs; reconciliation must do that.
bool authority_publish_frame(const AuthorityFrame &frame);
bool authority_get_frame(AuthorityFrame &frame);
// Game-thread update boundary: keep rider, traffic and correction reads on one
// complete host snapshot until the next update, despite concurrent reception.
void authority_pin_frame();
bool authority_get_outcome(unsigned slot,authority::Outcome &out,bool &cop_mode);
struct AuthorityReplayPlan {
    std::uint64_t ticket=0;
    AuthorityFrame frame{};
    std::array<authority::Command,authority::history_capacity> commands{};
    std::size_t count=0;
};
// Prepare under the network lock, replay outside it into isolated state, then
// commit the ticket before exposing that result on the game thread. A failed
// ticket leaves all input history intact; never run native code under g_mutex.
bool authority_prepare_replay(AuthorityReplayPlan &plan);
bool authority_commit_replay(std::uint64_t ticket);

struct Status {
    online_flow::PostRaceState postrace{};
    bool authoritative=false;
    std::uint32_t authority_humans=0;
    bool host_disconnected = false;
    std::uint32_t host_disconnect_age_ms = 0;
    std::uint8_t maximum_players = kMaximumPlayers;
    bool active = false;
    bool connected = false;
    bool is_host = false;
    bool replicated_riders = false;
    std::uint8_t local_slot = kInvalidSlot;
    std::uint8_t connected_players = 0;
    Phase phase = Phase::Offline;
    GameSetupState game_setup{};
    std::string message{};
    std::array<PlayerInfo, kMaximumPlayers> players{};
};

void configure(const Config &config);
void shutdown();
void update();

Status get_status();
// Allocation-free ownership view for per-body native physics. UI strings and
// roster presentation remain in get_status(); both read the same session lock.
struct PhysicsRules {
    bool active = false, connected = false, authoritative = false, is_host = false;
    bool replicated_riders = false;
    std::uint8_t local_slot = kInvalidSlot;
    std::uint32_t authority_humans = 0;
    Phase phase = Phase::Offline;
};
PhysicsRules get_physics_rules();
bool set_ready(bool ready);
bool set_character(std::uint8_t character);
bool set_track(std::uint8_t track);
bool host_set_phase(Phase phase);
bool host_advance_postrace(unsigned stage);
bool acknowledge_postrace(online_flow::PostRaceState state);
bool host_resume_postrace_setup();
bool host_commit_game_setup(const GameSetupState &setup);
bool set_selection(const online_flow::Selection &selection);
bool host_release_selection();
bool host_request_race_start();
bool all_connected_players_ready();

void set_local_input(std::uint16_t buttons, float stick_x, float stick_y);
bool get_player_input(std::uint8_t slot, std::uint16_t &buttons, float &stick_x, float &stick_y);

// The local machine publishes its controlled rider under its network slot.
// The host turns proposals into the canonical snapshot distributed to every
// peer. Consumers may request a blend between the two newest snapshots.
void set_local_rider_state(const RiderState &state);
bool get_rider_state(std::uint8_t slot, RiderState &state);
bool set_host_ai_rider_state(std::uint8_t slot, const RiderState &state);
bool set_host_world_state(const world_sync::Snapshot &state);
bool get_world_state(world_sync::Snapshot &state);
bool get_interpolated_rider_state(std::uint8_t slot, float alpha, RiderState &state);

// Encoded voice frames follow the same authenticated direct-connect topology:
// clients send to the host, which relays a canonical speaker slot to the other
// peers. Frames are accepted only during an active race.
bool submit_local_voice(std::span<const std::uint8_t> encoded_frame);
std::vector<ReceivedVoiceFrame> take_received_voice_frames();

} // namespace rr64::netplay
