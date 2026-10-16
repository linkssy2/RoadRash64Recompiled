#include "rr64_netplay.hpp"
#include "rr64_diagnostic_options.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_mk64_items.hpp"
#endif
#include "rr64_authoritative_eject.hpp"
#include "rr64_prediction_context.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_prediction_cop_state.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rr64_msvc_crt_compat.hpp"

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "recompinput/input_state.h"

#include "rr64_engine_layout.hpp"
#include "rr64_local_eject.hpp"
#include "rr64_traffic_distance.hpp"
#include "rr64_presentation_options.hpp"
#include "rr64_terrain_residency.hpp"
#include "rr64_terrain_snapshot.hpp"
#include "hle/rt64_rr64_authored_cadence.h"

extern "C" void func_8007C524(uint8_t *rdram, recomp_context *ctx);
extern "C" void func_8003F0E8(uint8_t *rdram, recomp_context *ctx);
extern "C" void yield_self_1ms(uint8_t *rdram);
extern "C" void rr64_record_guest_cadence(unsigned int frames, double seconds, double update_hz,
                                          float physics_delta, float update_ticks, float wait_ticks,
                                          float total_ticks, unsigned int mode,
                                          unsigned int pending_mode, unsigned int pause_state,
                                          unsigned int gameplay_active);

namespace {
void log_once(const char *name, std::atomic_bool &flag) {
    if (rr64::diagnostics::claim_once(flag, rr64::diagnostics::routine_enabled())) {
        std::fprintf(stderr, "[RR64-SHIM] %s\n", name);
        std::fflush(stderr);
    }
}

std::atomic_bool logged_dispatch{false};
std::atomic_bool logged_enqueue_yield{false};
std::atomic_bool logged_get_cause{false};
std::atomic_bool logged_probe_tlb{false};
std::atomic_bool logged_set_compare{false};

std::mutex guest_trace_mutex;
std::unordered_set<std::string> guest_trace_stages;

uint32_t race_trace_frame_count = 0;
std::chrono::steady_clock::time_point race_trace_last_sample{};
std::atomic_bool race_mode_active{false};
// Rendering includes the finish camera; gameplay/audio retain their own signal.
std::atomic_bool race_presentation_active{false};
std::atomic_bool gameplay_feedback_active{false};
std::atomic_bool gameplay_shortcuts_active{false};
std::atomic_bool road_rumble_allowed{false};
std::atomic_bool rumble_enabled{true};
std::atomic_uint32_t rider_eject_requests{0};
std::array<std::atomic_bool, 4> local_rider_has_fists_selected{{true, true, true, true}};
std::atomic_uint32_t audio_timeline_epoch{0};
std::atomic_bool maximum_view_distance_enabled{false};
std::atomic_bool player_started_title_session{false};
std::atomic_uint32_t player_session_last_input_mode{~0u};

struct LocalBikeMotionSample {
    float x = 0.0f;
    float z = 0.0f;
    bool valid = false;
};

LocalBikeMotionSample local_bike_motion{};
bool local_bike_has_moved_since_race_start = false;

rr64::prediction::ManualEjectState manual_eject_protections{};
thread_local rr64::prediction::ManualEjectState replay_eject_protections{};
thread_local std::uint64_t replay_eject_epoch=0;

struct LodTraceState {
    uint16_t lod = 0xFFFF;
    uint16_t previous_lod = 0xFFFF;
    uint32_t current_model = 0;
};

std::unordered_map<uint64_t, LodTraceState> lod_trace_states;

uint32_t autotest_last_mode = ~0u;
uint32_t autotest_mode_frame = 0;
bool autotest_scenario_logged = false;

bool environment_flag_enabled(const char *name) {
    char *value = nullptr;
    size_t length = 0;
    const errno_t result = _dupenv_s(&value, &length, name);
    const bool enabled = result == 0 && value != nullptr && value[0] != '\0' && value[0] != '0';
    std::free(value);
    return enabled;
}

bool runtime_trace_enabled() {
    return rr64::diagnostics::runtime_trace_enabled();
}

std::string environment_text(const char *name) {
    char *value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        std::free(value);
        return {};
    }
    std::string result{value};
    std::free(value);
    return result;
}

int maximum_view_distance_diagnostic_override() {
    static const int override_value = [] {
        char *value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "RR64_DIAGNOSTIC_MAXIMUM_VIEW_DISTANCE") != 0 ||
            value == nullptr || value[0] == '\0') {
            std::free(value);
            return -1;
        }
        const int parsed = value[0] == '0' ? 0 : 1;
        std::free(value);
        return parsed;
    }();
    return override_value;
}

bool maximum_view_distance_active() {
    const int diagnostic_override = maximum_view_distance_diagnostic_override();
    return diagnostic_override >= 0 ? diagnostic_override != 0
                                    : maximum_view_distance_enabled.load(std::memory_order_relaxed);
}

constexpr uint32_t kTitleMode = 0x01u;
constexpr uint32_t kStartButton = 0x1000u;

constexpr bool next_player_started_title_session(bool current, uint32_t previous_mode,
                                                 uint32_t mode, uint32_t buttons) noexcept {
    // The unattended attract race enters the same gameplay modes as a real
    // race. Mode 0x01 is the verified PRESS START title state, so only a real
    // Start press there (or in another non-race menu) begins a player session.
    // Pressing Start during an attract race merely exits the demo and must not
    // turn the terrain extension on during its final frames.
    if (mode == kTitleMode && previous_mode != kTitleMode) {
        current = false;
    }
    if (!rr64::engine::is_live_race_mode(mode) && (buttons & kStartButton) != 0u) {
        return true;
    }
    return current;
}

static_assert(!next_player_started_title_session(true, 0x09u, kTitleMode, 0u));
static_assert(next_player_started_title_session(false, 0x39u, kTitleMode, kStartButton));
static_assert(next_player_started_title_session(true, kTitleMode, kTitleMode, 0u));
static_assert(!next_player_started_title_session(false, 0x09u, 0x09u, kStartButton));

bool maximum_view_distance_terrain_active() {
    if (!maximum_view_distance_active()) {
        return false;
    }

    // Explicit diagnostic runs need to exercise the terrain extension without
    // relying on menu navigation. Normal play waits for a title-screen Start
    // press, keeping the original terrain lifecycle in the attract demo.
    if (maximum_view_distance_diagnostic_override() >= 0) {
        return true;
    }
    return player_started_title_session.load(std::memory_order_relaxed);
}

FILE *autotest_trace_file() {
    static FILE *file = [] {
        char *path = nullptr;
        size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_AUTOTEST_LOG") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return static_cast<FILE *>(nullptr);
        }

        FILE *result = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        return result;
    }();
    return file;
}

void append_autotest_trace(const char *line) {
    if (FILE *file = autotest_trace_file()) {
        std::fputs(line, file);
        std::fflush(file);
    }
}

unsigned int input_trace_last_mode = ~0u;
unsigned int input_trace_last_mask = ~0u;
unsigned int input_trace_last_buttons = ~0u;
int input_trace_last_x = 0x100;
int input_trace_last_y = 0x100;
unsigned int input_trace_last_errors = ~0u;

float float_from_bits(uint32_t bits) {
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

constexpr bool is_live_race_mode(uint32_t mode) {
    // Road Rash's main dispatcher contains four live-gameplay state pairs.
    // The pairs share the race simulation/render handlers but cover distinct
    // game-type and multiplayer layouts. Keeping the full table here prevents
    // modes such as Thrash from silently falling back to menu-style rendering.
    return rr64::engine::is_live_race_mode(mode);
}

static_assert(is_live_race_mode(0x09u));
static_assert(is_live_race_mode(0x18u));
static_assert(is_live_race_mode(0x1Du));
static_assert(!is_live_race_mode(0x14u));

// The only Road Rash callers of __osGetCause are the libultra RDB/debug-port
// handshake loops. A retail PC port has no RDB device, so provide a synthetic
// set/clear pulse that lets those waits terminate if they are ever reached.
std::atomic_uint32_t synthetic_rdb_cause_phase{0};
}

extern "C" void rr64_trace_guest_stage(const char *stage) {
    if (stage == nullptr || !runtime_trace_enabled()) {
        return;
    }

    std::lock_guard lock{guest_trace_mutex};
    if (!guest_trace_stages.emplace(stage).second) {
        return;
    }

    std::fprintf(stderr, "[RR64-GUEST] %s\n", stage);
    std::fflush(stderr);
}

extern "C" void rr64_trace_guest_value(const char *stage, unsigned int value) {
    if (stage == nullptr || !runtime_trace_enabled()) {
        return;
    }

    char key[160]{};
    std::snprintf(key, sizeof(key), "%s:%08X", stage, value);

    std::lock_guard lock{guest_trace_mutex};
    if (!guest_trace_stages.emplace(key).second) {
        return;
    }

    std::fprintf(stderr, "[RR64-GUEST] %s=0x%08X\n", stage, value);
    std::fflush(stderr);
}

extern "C" void rr64_trace_race_frame(unsigned char *rdram, void *context, unsigned int mode,
                                      unsigned int pending_mode, unsigned int pause_state,
                                      unsigned int physics_delta_bits,
                                      unsigned int update_ticks_bits, unsigned int wait_ticks_bits,
                                      unsigned int total_ticks_bits) {
    const bool detailed_trace = runtime_trace_enabled();
    const uint32_t sample_frames = detailed_trace ? 30u : 300u;
    const bool live_race_mode = rr64::engine::is_live_race_transition(mode, pending_mode);
    const bool live_gameplay_feedback =
        rr64::engine::is_gameplay_feedback_active(mode, pending_mode, pause_state);
    const bool race_shortcut_scene =
        rr64::engine::is_race_shortcut_scene_transition(mode, pending_mode);
    std::uint16_t pause_menu_state = 1u;
    const bool live_gameplay_shortcuts =
        rdram != nullptr &&
        rr64::engine::read_u16(rdram, rr64::engine::globals::pause_menu_state, pause_menu_state) &&
        rr64::engine::are_gameplay_shortcuts_active(mode, pending_mode, pause_menu_state);
    gameplay_shortcuts_active.store(live_gameplay_shortcuts, std::memory_order_release);

    // Publish presentation on every mode transition, including authoritative
    // sessions that skip the legacy controller work below. Rendering and music
    // must distinguish the race/results scene from menus containing 3D models.
    race_mode_active.store(live_race_mode, std::memory_order_relaxed);
    race_presentation_active.store(race_shortcut_scene, std::memory_order_relaxed);

    // Authority admits eject edges into the same sequenced stream as movement.
    // The legacy physical-controller loop must not consume them first.
    if(rr64::netplay::get_status().authoritative)return;
    const unsigned eject_requests = rider_eject_requests.exchange(0, std::memory_order_acq_rel);
    for (unsigned slot = 0; slot < 4; ++slot) {
        auto &manual_eject_durability = manual_eject_protections[slot];
        if ((eject_requests & (1u << slot)) == 0 && !manual_eject_durability.active)
            continue;
        std::uint32_t local_bike = 0;
        std::uint32_t local_rider = 0;
        std::uint32_t active_racers = 0;
        std::uint16_t bike_rider_attached = 0u;
        std::uint16_t rider_bike_attached = 0u;
        std::uint16_t rider_ejected = 1u;
        const bool local_rider_can_eject =
            live_gameplay_shortcuts && rr64::local_eject::resolve_bike(rdram, slot, local_bike) &&
            rr64::engine::read_u32(rdram, rr64::engine::globals::active_racer_count,
                                   active_racers) &&
            active_racers > 0u && active_racers <= rr64::engine::kMaximumRacers &&
            rr64::engine::valid_guest_range(local_bike, rr64::engine::bike::stride) &&
            rr64::engine::read_u16(rdram, local_bike + rr64::engine::bike::rider_attached,
                                   bike_rider_attached) &&
            rr64::engine::read_u32(rdram, local_bike + rr64::engine::bike::rider_pointer,
                                   local_rider) &&
            rr64::engine::valid_guest_range(local_rider, rr64::engine::rider::stride) &&
            rr64::engine::read_u16(rdram, local_rider + rr64::engine::rider::bike_attached,
                                   rider_bike_attached) &&
            rr64::engine::read_u16(rdram, local_rider + rr64::engine::rider::ejected,
                                   rider_ejected) &&
            rr64::engine::rider_can_manual_eject(bike_rider_attached, rider_bike_attached,
                                                 rider_ejected);
        const bool eject_requested = (eject_requests & (1u << slot)) != 0;
        if (eject_requested && local_rider_can_eject && context != nullptr) {
            float durability = 0.0f;
            float durability_capacity = 0.0f;
            const bool durability_valid =
                rr64::engine::read_float(rdram, local_bike + rr64::engine::bike::durability_current,
                                         durability) &&
                rr64::engine::read_float(rdram,
                                         local_bike + rr64::engine::bike::durability_capacity,
                                         durability_capacity) &&
                std::isfinite(durability) && std::isfinite(durability_capacity) &&
                durability >= 0.0f && durability_capacity > 0.0f &&
                durability <= durability_capacity;

            // func_8003F0E8 is the game's mounted-to-ejected transition. A
            // synthetic eject can still make the unattended bike take damage
            // during its subsequent fall, so preserve only the HUD's actual
            // durability value until the native recovery lockout ends.
            auto *guest_context = static_cast<recomp_context *>(context);
            const recomp_context saved_context = *guest_context;
            guest_context->r4 = rr64::engine::guest_address(local_bike);
            func_8003F0E8(rdram, guest_context);
            *guest_context = saved_context;
            if (durability_valid) {
                manual_eject_durability = {local_bike, durability, true};
            }
            if (rumble_enabled.load(std::memory_order_acquire)) {
                recompinput::trigger_rumble_pulse(slot, 0.55f);
            }
            if (rr64::diagnostics::routine_enabled()) {
                std::fprintf(stderr, "[RR64-RIDER] Left-stick eject triggered for player %u.\n",
                             slot + 1);
            }
        } else if (eject_requested && live_gameplay_shortcuts &&
                   rr64::diagnostics::routine_enabled()) {
            // This only emits on an actual L3 edge. If an unrecognized race-end
            // state ever rejects the request, the next user test captures all
            // relevant guest flags without adding per-frame log noise.
            std::fprintf(
                stderr,
                "[RR64-RIDER] Eject rejected bike=%08X rider=%08X racers=%u bike-link=%u rider-link=%u ejected=%u context=%p.\n",
                local_bike, local_rider, active_racers,
                static_cast<unsigned int>(bike_rider_attached),
                static_cast<unsigned int>(rider_bike_attached),
                static_cast<unsigned int>(rider_ejected), context);
        }

        if (manual_eject_durability.active) {
            if (!race_shortcut_scene || rdram == nullptr) {
                manual_eject_durability = {};
            } else {
                std::uint32_t bike_pool = 0;
                std::uint16_t drive_control_lockout = 0u;
                float current_durability = 0.0f;
                const bool state_valid =
                    rr64::local_eject::resolve_bike(rdram, slot, bike_pool) &&
                    bike_pool == manual_eject_durability.bike &&
                    rr64::engine::read_u16(rdram,
                                           bike_pool + rr64::engine::bike::drive_control_lockout,
                                           drive_control_lockout) &&
                    rr64::engine::read_float(rdram,
                                             bike_pool + rr64::engine::bike::durability_current,
                                             current_durability) &&
                    std::isfinite(current_durability);
                if (!state_valid) {
                    manual_eject_durability = {};
                } else {
                    if (current_durability < manual_eject_durability.durability) {
                        rr64::engine::write_float(
                            rdram, bike_pool + rr64::engine::bike::durability_current,
                            manual_eject_durability.durability);
                    }
                    if (rr64::engine::bike_accepts_drive_control(drive_control_lockout)) {
                        manual_eject_durability = {};
                    }
                }
            }
        }

    } // Per-player eject and durability protection.

    for (unsigned slot = 0; slot < 4; ++slot) {
    bool fists_selected = true;
    if (race_shortcut_scene && rdram != nullptr) {
        std::uint32_t bike_pool = 0;
        std::uint32_t rider = 0;
        std::uint32_t selected_weapon = rr64::engine::rider::fists_weapon;
        if (rr64::local_eject::resolve_bike(rdram, slot, bike_pool) &&
            rr64::engine::valid_guest_range(bike_pool, rr64::engine::bike::stride) &&
            rr64::engine::read_u32(rdram, bike_pool + rr64::engine::bike::rider_pointer, rider) &&
            rr64::engine::valid_guest_range(rider, rr64::engine::rider::stride) &&
            rr64::engine::read_u32(rdram, rider + rr64::engine::rider::selected_weapon,
                                   selected_weapon)) {
            fists_selected = rr64::engine::rider_has_fists_selected(selected_weapon);
        }
    }
    local_rider_has_fists_selected[slot].store(fists_selected, std::memory_order_release);
    }

    bool local_bike_accepts_drive = false;
    if (live_gameplay_feedback && rdram != nullptr) {
        std::uint32_t bike_pool = 0;
        std::uint16_t drive_control_lockout = 1u;
        if (rr64::engine::read_u32(rdram, rr64::engine::globals::bike_pool_pointer, bike_pool) &&
            rr64::engine::valid_guest_range(bike_pool, rr64::engine::bike::stride) &&
            rr64::engine::read_u16(rdram, bike_pool + rr64::engine::bike::drive_control_lockout,
                                   drive_control_lockout)) {
            float body_x = 0.0f;
            float body_z = 0.0f;
            const bool position_valid =
                rr64::engine::read_float(rdram, bike_pool + rr64::engine::bike::body_position,
                                         body_x) &&
                rr64::engine::read_float(rdram, bike_pool + rr64::engine::bike::body_position + 8u,
                                         body_z) &&
                std::isfinite(body_x) && std::isfinite(body_z);
            const bool moving = position_valid && local_bike_motion.valid &&
                                rr64::engine::horizontal_motion_allows_road_rumble(
                                    body_x - local_bike_motion.x, body_z - local_bike_motion.z);
            if (moving) {
                local_bike_has_moved_since_race_start = true;
            }
            local_bike_accepts_drive = position_valid && rr64::engine::drive_rumble_allowed(
                                                             drive_control_lockout, moving,
                                                             local_bike_has_moved_since_race_start);
            if (position_valid) {
                local_bike_motion = {body_x, body_z, true};
            } else {
                local_bike_motion.valid = false;
            }
        }
    } else {
        local_bike_motion.valid = false;
    }
    if (!live_race_mode) {
        local_bike_has_moved_since_race_start = false;
    }
    road_rumble_allowed.store(local_bike_accepts_drive &&
                                  rumble_enabled.load(std::memory_order_acquire),
                              std::memory_order_release);

    const bool previous_gameplay_feedback =
        gameplay_feedback_active.exchange(live_gameplay_feedback, std::memory_order_release);
    if (previous_gameplay_feedback != live_gameplay_feedback) {
        // The native audio queue is not part of the emulated timeline. Mark
        // each exact gameplay/pause boundary so it can discard samples from
        // the old timeline even when the paused game produces no callbacks.
        audio_timeline_epoch.fetch_add(1u, std::memory_order_release);
        if (!live_gameplay_feedback) {
            // A paused guest may stop producing VI callbacks, so silence the
            // physical motors at the boundary. Preserve its authored Rumble
            // Pak state only while the live race remains resumable.
            if (live_race_mode) {
                recompinput::suspend_all_rumble();
            } else {
                recompinput::stop_all_rumble();
            }
        }
    }

    if (!live_race_mode) {
        race_trace_frame_count = 0;
        race_trace_last_sample = {};
        return;
    }

    ++race_trace_frame_count;
    const auto now = std::chrono::steady_clock::now();

    if (race_trace_frame_count == 1) {
        std::lock_guard lock{guest_trace_mutex};
        race_trace_last_sample = now;
        if (detailed_trace) {
            rr64_record_guest_cadence(
                race_trace_frame_count, 0.0, 0.0, float_from_bits(physics_delta_bits),
                float_from_bits(update_ticks_bits), float_from_bits(wait_ticks_bits),
                float_from_bits(total_ticks_bits), mode, pending_mode, pause_state,
                live_gameplay_feedback ? 1u : 0u);
            append_autotest_trace("[RR64-RACE] live-race-start\n");
        }
        return;
    }

    if (((race_trace_frame_count - 1) % sample_frames) != 0) {
        return;
    }

    std::lock_guard lock{guest_trace_mutex};
    const double seconds = std::chrono::duration<double>(now - race_trace_last_sample).count();
    const double updates_per_second =
        seconds > 0.0 ? static_cast<double>(sample_frames) / seconds : 0.0;
    rr64_record_guest_cadence(race_trace_frame_count, seconds, updates_per_second,
                              float_from_bits(physics_delta_bits),
                              float_from_bits(update_ticks_bits), float_from_bits(wait_ticks_bits),
                              float_from_bits(total_ticks_bits), mode, pending_mode, pause_state,
                              live_gameplay_feedback ? 1u : 0u);
    race_trace_last_sample = now;
}

// Game-time clock shared by the handling probe and the autotest handling
// scenarios, so runs at different step sizes compare at equal game time.
// race_time sums the deltas of every update since the race mode began;
// moving_since is the race_time at which the local bike first passed speed 1.
struct HandlingClock {
    bool active = false;
    double race_time = 0.0;
    double moving_since = -1.0;
};
HandlingClock handling_clock;

// RR64_HANDLING_PROBE=<path> writes the local bike's rigid-body state once per
// update during a race: the game time, the step about to run, speed, velocity
// and the three orientation axes of the body at bike+0x108.
void handling_probe_frame(unsigned char *rdram, float step) {
    static FILE *const probe = [] {
        const std::string path = environment_text("RR64_HANDLING_PROBE");
        return path.empty() ? nullptr : std::fopen(path.c_str(), "w");
    }();
    if (!race_mode_active.load(std::memory_order_relaxed)) {
        handling_clock = {};
        return;
    }
    std::uint32_t bike = 0;
    float speed = 0.0f;
    if (!rr64::engine::read_u32(rdram, rr64::engine::globals::bike_pool_pointer, bike) ||
        !rr64::engine::valid_guest_range(bike, rr64::engine::bike::stride) ||
        !rr64::engine::read_float(rdram, bike + 0x184u, speed)) {
        return;
    }
    if (!handling_clock.active) {
        handling_clock = {true, 0.0, -1.0};
    }
    if (handling_clock.moving_since < 0.0 && speed > 1.0f) {
        handling_clock.moving_since = handling_clock.race_time;
    }
    if (probe) {
        float v[12]{};
        const std::uint32_t offsets[12] = {0x178u, 0x17Cu, 0x180u, 0x220u, 0x224u, 0x228u,
                                           0x22Cu, 0x230u, 0x234u, 0x238u, 0x23Cu, 0x240u};
        for (int i = 0; i < 12; ++i) {
            rr64::engine::read_float(rdram, bike + offsets[i], v[i]);
        }
        std::uint16_t buttons = 0;
        std::int8_t stick_x = 0;
        rr64::engine::read_u16(rdram, rr64::engine::globals::controller_buttons, buttons);
        rr64::engine::read_s8(rdram, rr64::engine::globals::controller_stick_x, stick_x);
        std::fprintf(probe,
                     "[RR64-HANDLING] t=%.5f moving=%.5f dt=%.6f speed=%.4f "
                     "vel=%.4f,%.4f,%.4f a0=%.4f,%.4f,%.4f a1=%.4f,%.4f,%.4f "
                     "a2=%.4f,%.4f,%.4f buttons=0x%04X stick=%d\n",
                     handling_clock.race_time, handling_clock.moving_since, step, speed, v[0],
                     v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
                     static_cast<unsigned>(buttons), static_cast<int>(stick_x));
        std::fflush(probe);
    }
    // RR64_HANDLING_DUMP=<path> appends the race time, then the raw words of
    // the local bike (0x868 bytes) and the first rider (0x118 bytes), per update.
    static FILE *const dump = [] {
        const std::string path = environment_text("RR64_HANDLING_DUMP");
        return path.empty() ? nullptr : std::fopen(path.c_str(), "wb");
    }();
    if (dump != nullptr) {
        const double time = handling_clock.race_time;
        std::fwrite(&time, sizeof(time), 1, dump);
        for (std::uint32_t offset = 0; offset < 0x868u; offset += 4u) {
            std::uint32_t word = 0;
            rr64::engine::read_u32(rdram, bike + offset, word);
            std::fwrite(&word, sizeof(word), 1, dump);
        }
        for (std::uint32_t offset = 0; offset < 0x118u; offset += 4u) {
            std::uint32_t word = 0;
            rr64::engine::read_u32(rdram, 0x800D8570u + offset, word);
            std::fwrite(&word, sizeof(word), 1, dump);
        }
        std::fflush(dump);
    }
    handling_clock.race_time += step;
}

// RR64_HANDLING_CONVERT=<Hz> converts the bike's per-update rules, written for
// steps of 1/<Hz> s, to the actual step: handling_ratio = step * Hz. Tire
// friction impulses scale by the ratio (rr64_handling_friction) and per-update
// decay factors k become k^ratio, so both act the same per second as at <Hz>.
float handling_ratio = 1.0f;

// RR64_HANDLING_FROM=<seconds> leaves the handling stock until that race time,
// so a corrected run can share its state with an uncorrected one up to there.
bool handling_corrections_live() {
    static const double from = [] {
        const std::string text = environment_text("RR64_HANDLING_FROM");
        return text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr);
    }();
    return from <= 0.0 || (handling_clock.active && handling_clock.race_time >= from - 1e-4);
}

void write_guest_float(unsigned char *rdram, std::uint32_t address, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    rr64::engine::write_u32(rdram, address, bits);
}

void handling_convert_frame(unsigned char *rdram, float step) {
    static const float reference = [] {
        const std::string text = environment_text("RR64_HANDLING_CONVERT");
        const float parsed = text.empty() ? 0.0f : std::strtof(text.c_str(), nullptr);
        return std::isfinite(parsed) && parsed >= 16.0f && parsed <= 240.0f ? parsed : 0.0f;
    }();
    if (reference == 0.0f) {
        return;
    }
    // Read-only constants, one reader each: func_8003F4D4's velocity and
    // angular damping (0.98, 0.999) and the 0.9 spin limit decay in
    // func_80038640 and func_800348D8.
    static constexpr std::array<std::uint32_t, 8> decay_constants = {
        0x80004880u, 0x80004884u, 0x8000488Cu, 0x80004890u,
        0x80004894u, 0x80004898u, 0x800044ACu, 0x80004290u};
    static std::array<float, decay_constants.size()> originals{};
    static bool captured = false;
    if (!captured) {
        for (std::size_t i = 0; i < decay_constants.size(); ++i) {
            rr64::engine::read_float(rdram, decay_constants[i], originals[i]);
        }
        captured = true;
    }
    const float ratio = handling_corrections_live() ? step * reference : 1.0f;
    if (ratio == handling_ratio) {
        return;
    }
    handling_ratio = ratio;
    for (std::size_t i = 0; i < decay_constants.size(); ++i) {
        write_guest_float(rdram, decay_constants[i], std::pow(originals[i], ratio));
    }
    // func_8004090C smooths throttle as t = 0.75 t + 0.25 input each update.
    write_guest_float(rdram, 0x8000494Cu, std::pow(0.75f, ratio));
    write_guest_float(rdram, 0x80004948u, 1.0f - std::pow(0.75f, ratio));
}

// func_8004090C moves the steering state 15% of the way to its target each
// update. Called with the step about to be added; returns it rescaled so the
// approach takes the same game time as at the reference rate.
extern "C" float rr64_handling_steering_step(float step) {
    if (handling_ratio == 1.0f) {
        return step;
    }
    return step * (1.0f - std::pow(0.85f, handling_ratio)) / 0.15f;
}

// Physics Rate option: offline races run on a fixed 1/rate s step paced to
// real time (10-60, default 17, near the original console's race rate). Menus keep the
// measured step, and online sessions ignore the option so every peer shares
// one clock.
std::atomic<unsigned> physics_rate_option{17u};

extern "C" void rr64_set_physics_rate(unsigned int hz) {
    physics_rate_option.store(std::clamp(hz, 10u, 60u), std::memory_order_relaxed);
}

// Runs after func_8004E830 derives the frame's step and before the update
// dispatch. RR64_FIXED_STEP replaces the measured, smoothed delta with exactly
// 1/RR64_FIXED_STEP_HZ s (default 60), written as func_8000B1DC would derive
// it. RR64_FIXED_STEP_STATS reports the measured cadence with or without the
// override.
extern "C" void rr64_fixed_step_frame(unsigned char *rdram) {
    static const bool fixed = environment_flag_enabled("RR64_FIXED_STEP");
    static const bool stats = environment_flag_enabled("RR64_FIXED_STEP_STATS");
    if (rdram == nullptr) {
        return;
    }
    std::uint32_t mode = 0;
    rr64::engine::read_u32(rdram, rr64::engine::globals::main_mode, mode);
    const bool option_step = !fixed && rr64::engine::is_live_race_mode(mode) &&
                             !rr64::netplay::get_status().active;
    const unsigned option_rate = physics_rate_option.load(std::memory_order_relaxed);
    if (!fixed && !option_step) {
        RT64::RR64FramePacing::pacedSourceRate.store(0u, std::memory_order_relaxed);
    }
    if (!fixed && !stats && !option_step) {
        std::uint32_t delta_bits = 0;
        rr64::engine::read_u32(rdram, rr64::engine::globals::physics_delta, delta_bits);
        handling_probe_frame(rdram, float_from_bits(delta_bits));
        return;
    }

    if (stats) {
        constexpr std::uint32_t report_frames = 600;
        static std::uint32_t frames = 0;
        static double delta_sum = 0.0;
        static double delta_min = 0.0;
        static double delta_max = 0.0;
        static auto window_start = std::chrono::steady_clock::now();
        std::uint32_t measured_bits = 0;
        if (rr64::engine::read_u32(rdram, rr64::engine::globals::physics_delta, measured_bits)) {
            const double measured = float_from_bits(measured_bits);
            delta_min = frames == 0 ? measured : std::min(delta_min, measured);
            delta_max = frames == 0 ? measured : std::max(delta_max, measured);
            delta_sum += measured;
            ++frames;
        }
        if (frames >= report_frames) {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            std::fprintf(stderr,
                         "[RR64-FIXED-STEP] fixed=%u updates_per_second=%.3f "
                         "measured_delta_ms=min %.3f mean %.3f max %.3f\n",
                         fixed ? 1u : 0u, seconds > 0.0 ? frames / seconds : 0.0,
                         delta_min * 1000.0, delta_sum / frames * 1000.0, delta_max * 1000.0);
            std::fflush(stderr);
            frames = 0;
            delta_sum = 0.0;
            window_start = now;
        }
    }

    if (!fixed && !option_step) {
        std::uint32_t delta_bits = 0;
        rr64::engine::read_u32(rdram, rr64::engine::globals::physics_delta, delta_bits);
        handling_probe_frame(rdram, float_from_bits(delta_bits));
        return;
    }
    static const float env_rate = [] {
        const std::string text = environment_text("RR64_FIXED_STEP_HZ");
        const float parsed = text.empty() ? 60.0f : std::strtof(text.c_str(), nullptr);
        return std::isfinite(parsed) && parsed >= 16.0f && parsed <= 240.0f ? parsed : 60.0f;
    }();
    const auto write_float = [rdram](std::uint32_t address, float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        rr64::engine::write_u32(rdram, address, bits);
    };
    // RR64_STEP_SWITCH=<seconds>:<Hz> changes to <Hz> once the race time
    // reaches <seconds>, so two runs can share a state up to that moment.
    static const std::pair<double, float> step_switch = [] {
        const std::string text = environment_text("RR64_STEP_SWITCH");
        const auto colon = text.find(':');
        if (colon == std::string::npos) {
            return std::pair<double, float>{-1.0, 0.0f};
        }
        const double at = std::strtod(text.c_str(), nullptr);
        const float hz = std::strtof(text.c_str() + colon + 1, nullptr);
        return std::isfinite(hz) && hz >= 16.0f && hz <= 240.0f
                   ? std::pair<double, float>{at, hz}
                   : std::pair<double, float>{-1.0, 0.0f};
    }();
    const bool switched = step_switch.first >= 0.0 && handling_clock.active &&
                          handling_clock.race_time >= step_switch.first - 1e-4;
    const float rate = fixed ? env_rate : static_cast<float>(option_rate);
    const float step = 1.0f / (switched ? step_switch.second : rate);
    // RR64_FIXED_STEP_PACE holds the update to real time: update n starts no
    // earlier than t0 + n * step, so a 24 Hz step authors 24 frames a second
    // and the renderer interpolates presentation between them. The schedule is
    // a count of whole steps, not accumulated float time, so it cannot drift.
    static const bool env_pace = environment_flag_enabled("RR64_FIXED_STEP_PACE");
    const bool pace = env_pace || option_step;
    // The renderer interpolates a paced step below the console rate; telling
    // it the exact rate spares it from inferring one from arrival times.
    const unsigned paced_rate = static_cast<unsigned>(std::lround(1.0f / step));
    RT64::RR64FramePacing::pacedSourceRate.store(pace && paced_rate < 60u ? paced_rate : 0u,
                                                 std::memory_order_relaxed);
    if (pace) {
        using clock = std::chrono::steady_clock;
        static clock::time_point origin = clock::now();
        static std::uint64_t ticks = 0;
        static float paced_step = step;
        if (step != paced_step) {
            origin = clock::now();
            ticks = 0;
            paced_step = step;
        }
        const auto due = origin + std::chrono::duration_cast<clock::duration>(
                                      std::chrono::duration<double>(ticks * double(step)));
        const auto now = clock::now();
        if (due > now) {
            // Guest threads run one at a time, so a host sleep here would hold
            // off the audio and scheduler threads. Wait through the guest
            // scheduler, delivering retrace and audio messages as they arrive.
            while (clock::now() < due) {
                yield_self_1ms(rdram);
            }
        } else if (now - due > std::chrono::milliseconds(250)) {
            // A stall (loading, a breakpoint) restarts the schedule instead of
            // running a burst of catch-up updates.
            origin = now;
            ticks = 0;
        }
        ++ticks;
    }
    write_float(rr64::engine::globals::physics_delta, step);
    write_float(0x8009CBACu, step * step);
    write_float(0x8009CBB0u, step * 60.0f);
    write_float(0x8009CBB4u, 1.0f / step);
    write_float(0x8009CD78u, step); // func_8000B1DC moving-average state
    // Frame time in the game's 1e-4 s units; func_8005D9A4 adapts above 350.
    write_float(rr64::engine::globals::total_ticks, 10000.0f * step);
    handling_convert_frame(rdram, step);
    handling_probe_frame(rdram, step);
}

// RR64_HANDLING_HOLD=<Hz> recomputes the torque and force that func_8004090C's
// lean and heading springs add to the bike body only once per 1/<Hz> s and
// holds them for the updates in between, as the original applied them over a
// whole step. Their damping then acts on an angular velocity that is up to a
// step old, as it did at <Hz>.
namespace {
struct SpringHold {
    std::uint32_t bike = 0;
    double remaining = 0.0;
    float before[6]{};
    float held[6]{};
};
std::array<SpringHold, 8> spring_holds{};
// Body torque accumulator (bike+0x26C) and force accumulator (bike+0x1FC).
constexpr std::array<std::uint32_t, 6> spring_hold_offsets = {0x26Cu, 0x270u, 0x274u,
                                                             0x1FCu, 0x200u, 0x204u};

float spring_hold_rate() {
    static const float rate = [] {
        const std::string text = environment_text("RR64_HANDLING_HOLD");
        const float parsed = text.empty() ? 0.0f : std::strtof(text.c_str(), nullptr);
        return std::isfinite(parsed) && parsed >= 16.0f && parsed <= 240.0f ? parsed : 0.0f;
    }();
    return rate;
}

SpringHold *spring_hold_for(std::uint32_t bike) {
    for (auto &hold : spring_holds) {
        if (hold.bike == bike) {
            return &hold;
        }
    }
    for (auto &hold : spring_holds) {
        if (hold.bike == 0) {
            hold = {};
            hold.bike = bike;
            return &hold;
        }
    }
    return nullptr;
}
} // namespace

// RR64_HANDLING_LAG=<Hz> recreates the heading lag of a 1/<Hz> s step. Each
// update the body turns by yaw rate * step before contact pulls the velocity
// into line, so travel trails heading by one step of rotation. Before the
// update, turn the velocity back about the body's up axis by the rotation of
// the step difference, so it trails by 1/<Hz> s of rotation instead.
void heading_lag_frame(unsigned char *rdram, std::uint32_t bike) {
    static const float rate = [] {
        const std::string text = environment_text("RR64_HANDLING_LAG");
        const float parsed = text.empty() ? 0.0f : std::strtof(text.c_str(), nullptr);
        return std::isfinite(parsed) && parsed >= 16.0f && parsed <= 240.0f ? parsed : 0.0f;
    }();
    std::uint32_t step_bits = 0;
    if (rate == 0.0f ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::physics_delta, step_bits)) {
        return;
    }
    const float extra = 1.0f / rate - float_from_bits(step_bits);
    if (extra <= 0.0f) {
        return;
    }
    float v[3]{}, up[3]{}, w[3]{};
    for (int i = 0; i < 3; ++i) {
        rr64::engine::read_float(rdram, bike + 0x178u + 4u * i, v[i]);
        rr64::engine::read_float(rdram, bike + 0x238u + 4u * i, up[i]);
        rr64::engine::read_float(rdram, bike + 0x254u + 4u * i, w[i]);
    }
    const float yaw_rate = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
    const float angle = -yaw_rate * extra;
    const float c = std::cos(angle), s = std::sin(angle);
    const float along = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
    const float cross[3] = {up[1] * v[2] - up[2] * v[1], up[2] * v[0] - up[0] * v[2],
                            up[0] * v[1] - up[1] * v[0]};
    for (int i = 0; i < 3; ++i) {
        write_guest_float(rdram, bike + 0x178u + 4u * i,
                          v[i] * c + cross[i] * s + up[i] * along * (1.0f - c));
    }
}

extern "C" void rr64_handling_hold_begin(unsigned char *rdram, unsigned int bike) {
    if (rdram == nullptr || !handling_corrections_live() ||
        !rr64::engine::valid_guest_range(bike, rr64::engine::bike::stride)) {
        return;
    }
    heading_lag_frame(rdram, bike);
    if (spring_hold_rate() == 0.0f) {
        return;
    }
    if (SpringHold *hold = spring_hold_for(bike)) {
        for (std::size_t i = 0; i < spring_hold_offsets.size(); ++i) {
            rr64::engine::read_float(rdram, bike + spring_hold_offsets[i], hold->before[i]);
        }
    }
}

extern "C" void rr64_handling_hold_end(unsigned char *rdram, unsigned int bike) {
    const float rate = spring_hold_rate();
    if (rdram == nullptr || rate == 0.0f || !handling_corrections_live() ||
        !rr64::engine::valid_guest_range(bike, rr64::engine::bike::stride)) {
        return;
    }
    SpringHold *hold = spring_hold_for(bike);
    std::uint32_t step_bits = 0;
    if (hold == nullptr ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::physics_delta, step_bits)) {
        return;
    }
    if (hold->remaining <= 1e-5) {
        for (std::size_t i = 0; i < spring_hold_offsets.size(); ++i) {
            float after = 0.0f;
            rr64::engine::read_float(rdram, bike + spring_hold_offsets[i], after);
            hold->held[i] = after - hold->before[i];
        }
        hold->remaining += 1.0 / rate;
    } else {
        for (std::size_t i = 0; i < spring_hold_offsets.size(); ++i) {
            write_guest_float(rdram, bike + spring_hold_offsets[i],
                              hold->before[i] + hold->held[i]);
        }
    }
    hold->remaining -= float_from_bits(step_bits);
}

extern "C" void rr64_handling_friction(unsigned char *rdram, unsigned int stack_pointer) {
    if (rdram == nullptr || handling_ratio == 1.0f) {
        return;
    }
    for (std::uint32_t offset = 0x40u; offset <= 0x48u; offset += 4u) {
        float value = 0.0f;
        if (rr64::engine::read_float(rdram, stack_pointer + offset, value)) {
            write_guest_float(rdram, stack_pointer + offset, value * handling_ratio);
        }
    }
}

extern "C" int rr64_is_live_race_mode(unsigned int mode) {
    return is_live_race_mode(mode) ? 1 : 0;
}

extern "C" int rr64_player_session_started() {
    return player_started_title_session.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int rr64_is_race_mode_active() {
    return race_mode_active.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int rr64_is_race_presentation_active() {
    return race_presentation_active.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int rr64_is_gameplay_feedback_active() {
    return gameplay_feedback_active.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int rr64_are_gameplay_shortcuts_active() {
    return gameplay_shortcuts_active.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int rr64_is_road_rumble_allowed() {
    return road_rumble_allowed.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void rr64_set_rumble_enabled(int enabled) {
    const bool rumble_on = enabled != 0;
    rumble_enabled.store(rumble_on, std::memory_order_release);
    if (!rumble_on) {
        road_rumble_allowed.store(false, std::memory_order_release);
        recompinput::stop_all_rumble();
    }
}

extern "C" int rr64_is_rumble_enabled() {
    return rumble_enabled.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void rr64_request_rider_eject(unsigned int slot) {
    if (slot < 4)
        rider_eject_requests.fetch_or(1u << slot, std::memory_order_release);
}

extern "C" int rr64_local_rider_has_fists_selected(unsigned slot) {
    return slot >= 4 || local_rider_has_fists_selected[slot].load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" unsigned int rr64_audio_timeline_epoch() {
    return audio_timeline_epoch.load(std::memory_order_acquire);
}

extern "C" void rr64_combat_impact_rumble(unsigned char *rdram, unsigned int first_bike,
                                          unsigned int second_bike, unsigned int strength_percent) {
    if (rr64::prediction::active() || rdram == nullptr || !rumble_enabled.load(std::memory_order_acquire) ||
        !gameplay_feedback_active.load(std::memory_order_acquire) ||
        recompinput::game_input_disabled()) {
        return;
    }

    const float strength = std::clamp(static_cast<float>(strength_percent) / 100.0f, 0.0f, 1.0f);
    const auto pulse_bike_controller = [rdram, strength](std::uint32_t bike) {
        // The collision records passed to func_80061224 carry their controller/
        // racer slot at +0x08. Only the four original controller ports can own
        // a local device; AI-only slots are intentionally ignored here.
        if (!rr64::engine::valid_guest_range(bike, 0x0Cu)) {
            return;
        }

        std::uint32_t controller = 0;
        if (rr64::engine::read_u32(rdram, bike + 0x08u, controller) && controller < 4u) {
            recompinput::trigger_rumble_pulse(static_cast<int>(controller), strength);
        }
    };

    pulse_bike_controller(first_bike);
    if (second_bike != first_bike) {
        pulse_bike_controller(second_bike);
    }
}

extern "C" void rr64_set_maximum_view_distance_enabled(int enabled) {
    maximum_view_distance_enabled.store(enabled != 0, std::memory_order_relaxed);
}

extern "C" int rr64_is_maximum_view_distance_enabled() {
    return maximum_view_distance_active() ? 1 : 0;
}

// The original spawn guard uses route end minus its authored margin. Change
// only the proposed distance; road validation and the 20-car allocator follow.
extern "C" unsigned int rr64_traffic_spawn_distance(unsigned char *rdram, unsigned int bits) {
    using namespace rr64::engine;
    if (!maximum_view_distance_terrain_active())
        return bits;
    unsigned mode = 0, pending = 0;
    float end = 0, margin = 0;
    if (!read_u32(rdram, globals::main_mode, mode) ||
        !read_u32(rdram, globals::pending_mode, pending) ||
        !is_live_race_transition(mode, pending) || !read_float(rdram, 0x800D762Cu, end) ||
        !read_float(rdram, 0x80006A98u, margin) || !std::isfinite(end) || !std::isfinite(margin))
        return bits;
    const float extended = rr64::traffic::spawn_distance(
        float_from_bits(bits), end - margin,
        rr64::presentation_options::draw_distance.load(std::memory_order_relaxed));
    unsigned result = 0;
    std::memcpy(&result, &extended, sizeof(result));
    return result;
}

extern "C" int rr64_traffic_within_draw_distance(unsigned char *rdram, unsigned int node,
                                                 unsigned int squared_bits, unsigned int stock_bits,
                                                 int stock_visible) {
    if (!maximum_view_distance_terrain_active())
        return stock_visible;
    const int visible = rr64::traffic::within_distance(
        rdram, node, float_from_bits(squared_bits), float_from_bits(stock_bits), stock_visible != 0,
        rr64::presentation_options::draw_distance.load(std::memory_order_relaxed));
    return visible;
}

extern "C" unsigned int rr64_traffic_render_visibility(unsigned char *rdram, unsigned int node,
                                                       unsigned int stock_hidden) {
    using namespace rr64::engine;

    // Preserve every stock-visible node and keep the extension scoped to the
    // user-selected far-view race path. The attract demo retains its authored
    // visibility and streaming lifecycle.
    if (stock_hidden == 0u || !maximum_view_distance_terrain_active() || rdram == nullptr ||
        !valid_guest_range(node, actor_scene::node_minimum_size)) {
        return stock_hidden;
    }

    std::uint32_t mode = 0;
    std::uint32_t pending_mode = 0;
    if (!read_u32(rdram, globals::main_mode, mode) ||
        !read_u32(rdram, globals::pending_mode, pending_mode) ||
        !is_live_race_transition(mode, pending_mode)) {
        return stock_hidden;
    }

    std::uint32_t type = 0;
    std::uint32_t entity = 0;
    if (!read_u32(rdram, node + actor_scene::type, type) || type != traffic_scene::node_type ||
        !read_u32(rdram, node + actor_scene::entity, entity) ||
        !valid_guest_range(entity, traffic_scene::entity_minimum_size)) {
        return stock_hidden;
    }

    std::uint32_t entity_type = 0;
    std::uint16_t active = 0;
    if (!read_u32(rdram, entity + traffic_scene::entity_type, entity_type) ||
        entity_type < traffic_scene::first_entity_type ||
        entity_type > traffic_scene::last_entity_type ||
        !read_u16(rdram, entity + traffic_scene::entity_active, active)) {
        return stock_hidden;
    }

    // Confirm membership in the traffic list rather than trusting the node
    // type alone. Bound traversal to the game's twenty-car allocation and
    // reject malformed/cyclic links without changing guest memory.
    std::uint32_t cursor = 0;
    bool traffic_list_member = false;
    if (read_u32(rdram, globals::traffic_scene_head, cursor)) {
        for (std::uint32_t i = 0; i < traffic_scene::maximum_entities && cursor != 0u; ++i) {
            if (cursor == node) {
                traffic_list_member = true;
                break;
            }
            if (!valid_guest_range(cursor, actor_scene::next + sizeof(std::uint32_t))) {
                break;
            }
            std::uint32_t next = 0;
            if (!read_u32(rdram, cursor + actor_scene::next, next) || next == cursor) {
                break;
            }
            cursor = next;
        }
    }

    float entity_x = 0.0f;
    float entity_z = 0.0f;
    float origin_x = 0.0f;
    float origin_z = 0.0f;
    bool within_presentation_radius = false;
    if (read_float(rdram, entity + traffic_scene::entity_position, entity_x) &&
        read_float(rdram, entity + traffic_scene::entity_position + sizeof(float), entity_z) &&
        read_float(rdram, globals::scene_cull_origin, origin_x) &&
        read_float(rdram, globals::scene_cull_origin + sizeof(float), origin_z) &&
        std::isfinite(entity_x) && std::isfinite(entity_z) && std::isfinite(origin_x) &&
        std::isfinite(origin_z)) {
        const float delta_x = entity_x - origin_x;
        const float delta_z = entity_z - origin_z;
        constexpr float radius = traffic_scene::maximum_presentation_radius;
        within_presentation_radius = (delta_x * delta_x) + (delta_z * delta_z) <= radius * radius;
    }

    return should_keep_active_traffic_visible(true, true, traffic_list_member, active != 0u,
                                              within_presentation_radius)
               ? 0u
               : stock_hidden;
}

extern "C" unsigned int rr64_terrain_unload_decision(unsigned char *rdram, unsigned int cell,
                                                     unsigned int stock_unload) {
    if (!maximum_view_distance_terrain_active() || stock_unload == 0u || rdram == nullptr ||
        !rr64::engine::valid_guest_range(cell, rr64::engine::terrain::cell_stride)) {
        return stock_unload;
    }

    struct CachedTerrainFrame {
        bool initialized = false;
        uint32_t request_epoch = 0;
        uint32_t current_record = 0;
        rr64::engine::TerrainSceneSnapshot snapshot{};
    };
    static thread_local CachedTerrainFrame cache{};

    uint32_t request_epoch = 0;
    uint32_t current_record = 0;
    if (!rr64::engine::read_u32(rdram, rr64::engine::globals::terrain_request_epoch,
                                request_epoch) ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::terrain_current_record,
                                current_record)) {
        return stock_unload;
    }
    if (!cache.initialized || cache.request_epoch != request_epoch ||
        cache.current_record != current_record) {
        cache = {};
        cache.request_epoch = request_epoch;
        cache.current_record = current_record;
        cache.initialized = rr64::engine::capture_terrain_scene_snapshot(rdram, cache.snapshot);
    }
    const auto &snapshot = cache.snapshot;
    if (!cache.initialized || !snapshot.valid || snapshot.duplicate_slot_owners != 0u ||
        cell < snapshot.cell_grid) {
        return stock_unload;
    }

    const uint32_t cell_offset = cell - snapshot.cell_grid;
    if ((cell_offset % rr64::engine::terrain::cell_stride) != 0u) {
        return stock_unload;
    }
    const uint32_t cell_index = cell_offset / rr64::engine::terrain::cell_stride;
    if (cell_index >= snapshot.cell_count) {
        return stock_unload;
    }

    rr64::engine::TerrainCandidateBounds bounds{};
    bounds.valid = snapshot.candidate_count > 0u &&
                   snapshot.candidate_valid == snapshot.candidate_count &&
                   snapshot.candidate_unique == snapshot.candidate_count;
    bounds.minimum_row = snapshot.candidate_min_row;
    bounds.maximum_row = snapshot.candidate_max_row;
    bounds.minimum_column = snapshot.candidate_min_column;
    bounds.maximum_column = snapshot.candidate_max_column;

    const uint32_t occupied_or_inflight_resources =
        snapshot.ready_resources + snapshot.state_counts[3] + snapshot.state_counts[4];
    const bool retain = rr64::engine::should_retain_terrain_presentation_cell(
        true, true, snapshot.cell_states[cell_index], snapshot.map_width,
        cell_index / snapshot.map_width, cell_index % snapshot.map_width, bounds,
        occupied_or_inflight_resources);
    return retain ? 0u : stock_unload;
}

extern "C" void rr64_render_resident_terrain(unsigned char *rdram, void *context) {
    auto *ctx = static_cast<recomp_context *>(context);
    rr64_trace_terrain_scene(rdram);
    if (!maximum_view_distance_terrain_active() || rdram == nullptr || ctx == nullptr) {
        return;
    }

    // The stock terrain renderer consumes one fixed 31-entry record. Do not
    // enlarge, reorder, or replace that record: it is shared with the terrain
    // streamer's load/unload state machine. Instead, make additional render-only
    // batches from cells that the stock engine has already marked fully ready
    // and that still own a valid one-of-96 display-list slot. func_8007C524 then
    // applies the original bounds/frustum test and rebuilds each selected cell's
    // view-relative display list for the current camera.
    constexpr uint32_t kCellStride = 0x10u;
    constexpr uint32_t kRecordPointerCapacity = 31u;
    constexpr uint32_t kRecordIndexOffset = 0x7Cu;
    constexpr uint32_t kRecordCountOffset = 0xF8u;
    constexpr uint32_t kRecordSize = 0xFCu;
    constexpr uint32_t kDisplayListSlotCount = 0x60u;
    constexpr uint32_t kReadyState = 5u;

    const auto guest = [](uint32_t address) {
        return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(address)));
    };
    const auto valid_rdram_range = [](uint32_t address, uint64_t size) {
        constexpr uint64_t kBegin = 0x80000000ull;
        constexpr uint64_t kEnd = 0x80800000ull;
        const uint64_t begin = address;
        return begin >= kBegin && size <= (kEnd - kBegin) && begin + size <= kEnd;
    };

    const uint32_t current_record =
        static_cast<uint32_t>(MEM_W(0, guest(rr64::engine::globals::terrain_current_record)));
    const uint32_t cell_grid =
        static_cast<uint32_t>(MEM_W(0, guest(rr64::engine::globals::terrain_cell_grid)));
    const int32_t map_width =
        static_cast<int32_t>(MEM_W(0, guest(rr64::engine::globals::terrain_map_width)));
    if (!valid_rdram_range(current_record, kRecordSize) || map_width <= 0 || map_width > 128) {
        return;
    }

    rr64::engine::TerrainSceneSnapshot snapshot{};
    if (!rr64::engine::capture_terrain_scene_snapshot(rdram, snapshot) || !snapshot.valid ||
        snapshot.current_record != current_record || snapshot.cell_grid != cell_grid ||
        snapshot.map_width != static_cast<uint32_t>(map_width) || snapshot.candidate_count == 0u) {
        // A ready cell with incomplete or duplicate resource ownership can be
        // visible for a short transition while the streamer rotates records.
        // The stock pass owns that frame; never expose the transitional cell
        // through the presentation extension.
        return;
    }

    rr64::engine::TerrainCandidateBounds candidate_bounds{};
    candidate_bounds.valid = true;
    candidate_bounds.minimum_row = snapshot.candidate_min_row;
    candidate_bounds.maximum_row = snapshot.candidate_max_row;
    candidate_bounds.minimum_column = snapshot.candidate_min_column;
    candidate_bounds.maximum_column = snapshot.candidate_max_column;

    const uint64_t cell_count = static_cast<uint64_t>(map_width) * static_cast<uint64_t>(map_width);
    if (!valid_rdram_range(cell_grid, cell_count * kCellStride)) {
        return;
    }

    std::array<uint32_t, kRecordPointerCapacity> current_cells{};
    const int32_t current_count =
        std::clamp(static_cast<int32_t>(MEM_W(kRecordCountOffset, guest(current_record))), 0,
                   static_cast<int32_t>(kRecordPointerCapacity));
    for (int32_t i = 0; i < current_count; ++i) {
        current_cells[static_cast<size_t>(i)] = static_cast<uint32_t>(
            MEM_W(static_cast<uint32_t>(i) * sizeof(uint32_t), guest(current_record)));
    }

    struct ExtraTerrainCell {
        uint32_t cell = 0u;
        float distance_squared = 0.0f;
    };
    std::array<ExtraTerrainCell, kDisplayListSlotCount> extra_cells{};
    size_t extra_count = 0;
    for (uint64_t i = 0; i < cell_count && extra_count < extra_cells.size(); ++i) {
        const uint32_t cell = cell_grid + static_cast<uint32_t>(i * kCellStride);
        const gpr guest_cell = guest(cell);
        if (static_cast<uint32_t>(MEM_BU(0xC, guest_cell)) != kReadyState) {
            continue;
        }

        const uint32_t cell_index = static_cast<uint32_t>(i);
        const uint32_t cell_row = cell_index / static_cast<uint32_t>(map_width);
        const uint32_t cell_column = cell_index % static_cast<uint32_t>(map_width);
        if (!rr64::engine::terrain_cell_in_presentation_ring(
                static_cast<uint32_t>(map_width), cell_row, cell_column, candidate_bounds)) {
            continue;
        }

        const uint32_t payload = static_cast<uint32_t>(MEM_W(0, guest_cell));
        const int32_t slot = static_cast<int8_t>(MEM_B(0xD, guest_cell));
        if (!valid_rdram_range(payload, 0x40u) || slot < 0 ||
            slot >= static_cast<int32_t>(kDisplayListSlotCount)) {
            continue;
        }

        const uint32_t slot_list =
            static_cast<uint32_t>(MEM_W(0, guest(rr64::engine::globals::terrain_display_list_slots +
                                                 static_cast<uint32_t>(slot) * sizeof(uint32_t))));
        if (!valid_rdram_range(slot_list, 8u)) {
            continue;
        }

        bool already_in_stock_pass = false;
        for (int32_t current_index = 0; current_index < current_count; ++current_index) {
            if (current_cells[static_cast<size_t>(current_index)] == cell) {
                already_in_stock_pass = true;
                break;
            }
        }
        if (!already_in_stock_pass) {
            const float world_x = float_from_bits(static_cast<uint32_t>(
                MEM_W(rr64::engine::terrain::payload_world_x, guest(payload))));
            const float world_z = float_from_bits(static_cast<uint32_t>(
                MEM_W(rr64::engine::terrain::payload_world_z, guest(payload))));
            if (!std::isfinite(world_x) || !std::isfinite(world_z)) {
                continue;
            }
            const float delta_x = world_x - snapshot.camera_x;
            const float delta_z = world_z - snapshot.camera_z;
            extra_cells[extra_count++] = {
                cell,
                delta_x * delta_x + delta_z * delta_z,
            };
        }
    }

    if (extra_count == 0) {
        return;
    }

    // One stock-sized supplementary batch is the hard presentation budget.
    // Prefer the nearest ring cells when the ring is larger; they cover the
    // transition boundary while avoiding a second terrain setup and its extra
    // command-buffer/resource pressure.
    std::sort(extra_cells.begin(), extra_cells.begin() + extra_count,
              [](const ExtraTerrainCell &left, const ExtraTerrainCell &right) {
                  return left.distance_squared < right.distance_squared;
              });
    extra_count = std::min<size_t>(extra_count, kRecordPointerCapacity);

    static uint8_t *synthetic_record_host = nullptr;
    static uint8_t *synthetic_record_owner = nullptr;
    if (synthetic_record_host == nullptr || synthetic_record_owner != rdram) {
        synthetic_record_host = static_cast<uint8_t *>(recomp::alloc(rdram, kRecordSize));
        synthetic_record_owner = synthetic_record_host != nullptr ? rdram : nullptr;
    }
    if (synthetic_record_host == nullptr) {
        return;
    }

    const uint64_t synthetic_offset = static_cast<uint64_t>(synthetic_record_host - rdram);
    if (synthetic_offset > 0x1FFFFFFFull) {
        return;
    }
    const uint32_t synthetic_record = 0x80000000u + static_cast<uint32_t>(synthetic_offset);
    const gpr guest_synthetic_record = guest(synthetic_record);

    for (size_t batch_begin = 0; batch_begin < extra_count; batch_begin += kRecordPointerCapacity) {
        const size_t batch_count =
            std::min<size_t>(kRecordPointerCapacity, extra_count - batch_begin);
        for (uint32_t i = 0; i < kRecordPointerCapacity; ++i) {
            const uint32_t cell = i < batch_count ? extra_cells[batch_begin + i].cell : 0u;
            MEM_W(i * sizeof(uint32_t), guest_synthetic_record) = cell;
            MEM_W(kRecordIndexOffset + i * sizeof(uint32_t), guest_synthetic_record) = 0;
        }
        MEM_W(kRecordCountOffset, guest_synthetic_record) = static_cast<uint32_t>(batch_count);

        MEM_W(0, guest(rr64::engine::globals::terrain_current_record)) = synthetic_record;
        const recomp_context saved_context = *ctx;
        ctx->r4 = 0;
        func_8007C524(rdram, ctx);
        *ctx = saved_context;
    }

    MEM_W(0, guest(rr64::engine::globals::terrain_current_record)) = current_record;
}

extern "C" void rr64_trace_lod_node(unsigned char *rdram, unsigned int kind, unsigned int node) {
    constexpr uint32_t kTraceSize = 0x120;
    static const bool trace_enabled =
        environment_flag_enabled("RR64_LOD_TRACE") || runtime_trace_enabled();
    if (!trace_enabled || rdram == nullptr || node < 0x80000000u ||
        node > 0x80800000u - kTraceSize) {
        return;
    }

    // Recomp memory macros expect the sign-extended form of a KSEG0 address.
    // The hook ABI passes a 32-bit guest pointer, so restore that form before
    // using MEM_*; otherwise the unsigned address resolves 4 GiB past RDRAM.
    const gpr guest_node = static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(node)));
    const uint16_t lod = MEM_HU(0x44, guest_node);
    const uint16_t previous_lod = MEM_HU(0x46, guest_node);
    const uint32_t current_model = static_cast<uint32_t>(MEM_W(0x28, guest_node));
    const uint64_t key = (static_cast<uint64_t>(kind) << 32) | node;

    std::lock_guard lock{guest_trace_mutex};
    auto &state = lod_trace_states[key];
    if (state.lod == lod && state.previous_lod == previous_lod &&
        state.current_model == current_model) {
        return;
    }

    state = {lod, previous_lod, current_model};
    char line[1024];
    std::snprintf(
        line, sizeof(line),
        "[RR64-LOD] kind=%s node=0x%08X type=%d entity=0x%08X lod=%u previous=%u "
        "current=0x%08X "
        "models=[0x%08X,0x%08X,0x%08X] draws=[0x%08X,0x%08X,0x%08X] "
        "counts=[%u,%u,%u] buffers7c=[0x%08X,0x%08X,0x%08X] buffersdc=[0x%08X,0x%08X,0x%08X]\n",
        kind == 0 ? "bike" : "rider", node, MEM_W(0x00, guest_node),
        static_cast<uint32_t>(MEM_W(0x04, guest_node)), static_cast<unsigned>(lod),
        static_cast<unsigned>(previous_lod), current_model,
        static_cast<uint32_t>(MEM_W(0x2C, guest_node)),
        static_cast<uint32_t>(MEM_W(0x30, guest_node)),
        static_cast<uint32_t>(MEM_W(0x34, guest_node)),
        static_cast<uint32_t>(MEM_W(0x50, guest_node)),
        static_cast<uint32_t>(MEM_W(0x54, guest_node)),
        static_cast<uint32_t>(MEM_W(0x58, guest_node)),
        static_cast<unsigned>(MEM_HU(0x4A, guest_node)),
        static_cast<unsigned>(MEM_HU(0x4C, guest_node)),
        static_cast<unsigned>(MEM_HU(0x4E, guest_node)),
        static_cast<uint32_t>(MEM_W(0x7C, guest_node)),
        static_cast<uint32_t>(MEM_W(0x9C, guest_node)),
        static_cast<uint32_t>(MEM_W(0xBC, guest_node)),
        static_cast<uint32_t>(MEM_W(0xDC, guest_node)),
        static_cast<uint32_t>(MEM_W(0xFC, guest_node)),
        static_cast<uint32_t>(MEM_W(0x11C, guest_node)));
    std::fputs(line, stderr);
    std::fflush(stderr);
    append_autotest_trace(line);
}

extern "C" void rr64_trace_guest_input(unsigned int mode, unsigned int active_mask,
                                       unsigned int buttons, unsigned int stick_x_byte,
                                       unsigned int stick_y_byte, unsigned int error_0,
                                       unsigned int error_1, unsigned int error_2,
                                       unsigned int error_3) {
    const int stick_x = static_cast<int8_t>(stick_x_byte & 0xFFu);
    const int stick_y = static_cast<int8_t>(stick_y_byte & 0xFFu);
    const unsigned int errors = (error_0 & 0xFFu) | ((error_1 & 0xFFu) << 8) |
                                ((error_2 & 0xFFu) << 16) | ((error_3 & 0xFFu) << 24);

    const uint32_t previous_input_mode =
        player_session_last_input_mode.exchange(mode, std::memory_order_relaxed);
    const bool session_started = next_player_started_title_session(
        player_started_title_session.load(std::memory_order_relaxed), previous_input_mode, mode,
        buttons);
    player_started_title_session.store(session_started, std::memory_order_relaxed);

    // Input tracing is diagnostic-only. Analog motion changes almost every
    // poll, and synchronously flushing each sample can stall a busy race.
    static const bool input_trace_enabled = environment_flag_enabled("RR64_INPUT_TRACE");
    if (!input_trace_enabled) {
        return;
    }

    std::lock_guard lock{guest_trace_mutex};
    if (mode == input_trace_last_mode && active_mask == input_trace_last_mask &&
        buttons == input_trace_last_buttons && stick_x == input_trace_last_x &&
        stick_y == input_trace_last_y && errors == input_trace_last_errors) {
        return;
    }

    input_trace_last_mode = mode;
    input_trace_last_mask = active_mask;
    input_trace_last_buttons = buttons;
    input_trace_last_x = stick_x;
    input_trace_last_y = stick_y;
    input_trace_last_errors = errors;

    std::fprintf(
        stderr,
        "[RR64-GUEST-INPUT] mode=0x%02X active=0x%X buttons=0x%04X stick=(%d,%d) errors=(%u,%u,%u,%u)\n",
        mode, active_mask, buttons & 0xFFFFu, stick_x, stick_y, error_0 & 0xFFu, error_1 & 0xFFu,
        error_2 & 0xFFu, error_3 & 0xFFu);
    std::fflush(stderr);
}

// Race input recording and replay for handling comparisons. Samples are keyed
// by race time (the summed update deltas since the race mode began), so one
// recording replays at the same game times under any step rate.
// RR64_INPUT_RECORD=<path> writes "time buttons stick_x stick_y" whenever the
// race input changes. RR64_INPUT_REPLAY=<path> replaces race input with the
// latest recorded sample at or before the current race time.
struct RaceInputSample {
    double time = 0.0;
    std::uint16_t buttons = 0;
    std::int8_t x = 0, y = 0;
};

// Returns true when the replay owns this frame's input.
bool race_input_record_replay(unsigned char *rdram) {
    static FILE *const record = [] {
        const std::string path = environment_text("RR64_INPUT_RECORD");
        return path.empty() ? nullptr : std::fopen(path.c_str(), "w");
    }();
    static const std::vector<RaceInputSample> replay = [] {
        std::vector<RaceInputSample> samples;
        const std::string path = environment_text("RR64_INPUT_REPLAY");
        if (path.empty()) {
            return samples;
        }
        std::ifstream file(path);
        RaceInputSample sample;
        unsigned buttons = 0;
        int x = 0, y = 0;
        while (file >> sample.time >> std::hex >> buttons >> std::dec >> x >> y) {
            sample.buttons = static_cast<std::uint16_t>(buttons);
            sample.x = static_cast<std::int8_t>(x);
            sample.y = static_cast<std::int8_t>(y);
            samples.push_back(sample);
        }
        return samples;
    }();
    static RaceInputSample last_recorded{-1.0, 0xFFFF, 127, 127};
    static std::uint16_t replay_previous = 0;
    if (!race_mode_active.load(std::memory_order_relaxed) || !handling_clock.active) {
        last_recorded = {-1.0, 0xFFFF, 127, 127};
        replay_previous = 0;
        return false;
    }
    const double now = handling_clock.race_time;
    if (!replay.empty()) {
        // Recorded times are rounded to microseconds, so allow for a sample
        // printed slightly after the race time it was captured at.
        const auto next = std::upper_bound(
            replay.begin(), replay.end(), now + 1e-4,
            [](double time, const RaceInputSample &sample) { return time < sample.time; });
        const RaceInputSample sample = next == replay.begin() ? RaceInputSample{} : *(next - 1);
        const auto changed = static_cast<std::uint16_t>(replay_previous ^ sample.buttons);
        rr64::engine::write_u16(rdram, rr64::engine::globals::controller_buttons, sample.buttons);
        rr64::engine::write_u16(rdram, rr64::engine::globals::controller_changed_buttons, changed);
        rr64::engine::write_u16(rdram, rr64::engine::globals::controller_pressed_buttons,
                                changed & sample.buttons);
        rr64::engine::write_s8(rdram, rr64::engine::globals::controller_stick_x, sample.x);
        rr64::engine::write_s8(rdram, rr64::engine::globals::controller_stick_y, sample.y);
        replay_previous = sample.buttons;
        return true;
    }
    if (record != nullptr) {
        RaceInputSample sample{now};
        rr64::engine::read_u16(rdram, rr64::engine::globals::controller_buttons, sample.buttons);
        rr64::engine::read_s8(rdram, rr64::engine::globals::controller_stick_x, sample.x);
        rr64::engine::read_s8(rdram, rr64::engine::globals::controller_stick_y, sample.y);
        if (sample.buttons != last_recorded.buttons || sample.x != last_recorded.x ||
            sample.y != last_recorded.y) {
            std::fprintf(record, "%.6f %04X %d %d\n", now, static_cast<unsigned>(sample.buttons),
                         static_cast<int>(sample.x), static_cast<int>(sample.y));
            std::fflush(record);
            last_recorded = sample;
        }
    }
    return false;
}

// RR64_AUTOTEST_MENU=solo drives the menus into a one-player multiplayer race:
// Start at the title, Up and A on the main menu, A through game type, track
// and bike, then Start on the rumble prompt, with traffic switched off.
// Returns true while it owns input.
bool autotest_solo_menu(unsigned char *rdram, unsigned int mode) {
    static const bool solo = environment_text("RR64_AUTOTEST_MENU") == "solo";
    static std::uint32_t current = ~0u, frames = 0;
    if (!solo) {
        return false;
    }
    if (mode != current) {
        current = mode;
        frames = 0;
    }
    ++frames;
    // Multiplayer traffic off, so replays have no traffic RNG. Pedestrians
    // (0x8009EAE0) already default to off.
    rr64::engine::write_u32(rdram, 0x8009EAD8u, 0u);
    std::uint16_t press = 0;
    switch (mode) {
    case 0x01: // title
    case 0x1B: // rumble prompt
        press = frames % 90u == 45u ? 0x1000u : 0u;
        break;
    case 0x20: // main menu: Multiplayer sits one above the default Big Game
        press = frames == 45u ? 0x0800u : frames == 75u ? 0x8000u : 0u;
        break;
    case 0x23: // game type, track, bike
        press = frames % 90u == 45u ? 0x8000u : 0u;
        break;
    default:
        break;
    }
    if (press != 0) {
        for (const std::uint32_t address :
             {rr64::engine::globals::controller_buttons,
              rr64::engine::globals::controller_changed_buttons,
              rr64::engine::globals::controller_pressed_buttons}) {
            std::uint16_t value = 0;
            rr64::engine::read_u16(rdram, address, value);
            rr64::engine::write_u16(rdram, address, static_cast<std::uint16_t>(value | press));
        }
    }
    return true;
}

// RR64_RAM_SNAPSHOT=<dir> writes the guest RDRAM to <dir>/snapNN.bin on each
// L press, so menu settings can be located by diffing snapshots.
void ram_snapshot_on_l(unsigned char *rdram) {
    static const std::string dir = environment_text("RR64_RAM_SNAPSHOT");
    static unsigned count = 0;
    static bool held_before = false;
    if (dir.empty()) {
        return;
    }
    // The pressed-edge global is not refreshed on every screen, so detect the
    // edge from the held buttons instead.
    std::uint16_t buttons = 0;
    rr64::engine::read_u16(rdram, rr64::engine::globals::controller_buttons, buttons);
    const bool held = (buttons & 0x0020u) != 0;
    const bool pressed = held && !held_before;
    held_before = held;
    if (!pressed) {
        return;
    }
    char name[32]{};
    std::snprintf(name, sizeof(name), "/snap%02u.bin", count++);
    if (FILE *file = std::fopen((dir + name).c_str(), "wb")) {
        std::fwrite(rdram, 1, 0x800000, file);
        std::fclose(file);
    }
}

#include "rr64_autotest_control.hpp"
extern "C" void rr64_prediction_request_verification_restart();
extern "C" void rr64_autotest_input(unsigned char *rdram, unsigned int mode) {
    static const bool enabled = environment_flag_enabled("RR64_AUTOTEST");
    static const bool drive_enabled = environment_flag_enabled("RR64_AUTOTEST_DRIVE");
    static const std::string requested_scenario = environment_text("RR64_AUTOTEST_SCENARIO");

    if (rdram == nullptr) {
        return;
    }
    ram_snapshot_on_l(rdram);
    if (race_input_record_replay(rdram)) {
        return;
    }
    if (!race_mode_active.load(std::memory_order_relaxed) && autotest_solo_menu(rdram, mode)) {
        return;
    }
    if (!enabled) {
        return;
    }

    static rr64::autotest::ControlFile control_file;
    static std::uint32_t traced_mode = ~0u;
    if (mode != traced_mode) {
        traced_mode = mode;
        char line[64]{};
        std::snprintf(line, sizeof(line), "[RR64-AUTOTEST] mode=0x%02X\n", mode);
        append_autotest_trace(line);
    }
    if(control_file.enabled()){
        static std::uint16_t previous=0;
        static std::uint64_t last_action_serial=0;
        const auto control=control_file.sample();
        if(control.serial && control.serial!=last_action_serial){
            last_action_serial=control.serial;
            if(control.actions&2)rr64_prediction_request_verification_restart();
            if((control.actions&1) && race_mode_active.load(std::memory_order_relaxed))
                rr64_request_rider_eject(0);
        }
        const auto changed=static_cast<std::uint16_t>(previous^control.buttons);
        rr64::engine::write_u16(rdram,rr64::engine::globals::controller_buttons,control.buttons);
        rr64::engine::write_u16(rdram,rr64::engine::globals::controller_changed_buttons,changed);
        rr64::engine::write_u16(rdram,rr64::engine::globals::controller_pressed_buttons,changed&control.buttons);
        rr64::engine::write_s8(rdram,rr64::engine::globals::controller_stick_x,control.x);
        rr64::engine::write_s8(rdram,rr64::engine::globals::controller_stick_y,control.y);
        previous=control.buttons;
        return;
    }

    const auto inject_at = [rdram](uint32_t address, uint16_t injected_buttons) {
        const gpr guest_address =
            static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(address)));
        MEM_H(0, guest_address) = static_cast<int16_t>(
            static_cast<uint16_t>(MEM_HU(0, guest_address)) | injected_buttons);
    };

    const auto inject_edge = [&inject_at](uint16_t injected_buttons) {
        inject_at(rr64::engine::globals::controller_buttons, injected_buttons);
        inject_at(rr64::engine::globals::controller_changed_buttons, injected_buttons);
        inject_at(rr64::engine::globals::controller_pressed_buttons, injected_buttons);
    };

    if (race_mode_active.load(std::memory_order_relaxed)) {
        const std::string &scenario = requested_scenario;
        const bool scripted_drive = drive_enabled || !scenario.empty();
        if (scripted_drive && !autotest_scenario_logged) {
            const char *scenario_name = scenario.empty() ? "straight" : scenario.c_str();
            char line[160]{};
            std::snprintf(line, sizeof(line), "[RR64-AUTOTEST] race-scenario=%s\n", scenario_name);
            std::fputs(line, stderr);
            std::fflush(stderr);
            append_autotest_trace(line);
            autotest_scenario_logged = true;
        }
        if (scenario == "brake" || scenario == "turn") {
            // Timed in game seconds from the moment the bike first moves, so
            // runs at different step sizes see the same inputs at the same
            // game time. brake: full throttle 6 s, then heavy brake only.
            // turn: full throttle, weaving right then left for one second
            // each from 2 s to 6 s.
            const double moving = handling_clock.moving_since < 0.0
                                      ? 0.0
                                      : handling_clock.race_time - handling_clock.moving_since;
            const bool braking = scenario == "brake" && moving >= 6.0;
            inject_at(rr64::engine::globals::controller_buttons, braking ? 0x4000u : 0x2000u);
            if (scenario == "turn") {
                const bool weaving = moving >= 2.0 && moving < 6.0;
                const bool right = (static_cast<int>(moving) & 1) == 0;
                rr64::engine::write_s8(rdram, rr64::engine::globals::controller_stick_x,
                                       weaving ? (right ? 40 : -40) : 0);
            }
            return;
        }
        if (scripted_drive) {
            const std::uint32_t scenario_frame = race_trace_frame_count;
            const bool coast_phase = scenario == "stop-go" && (scenario_frame % 300u) >= 210u;
            if (!coast_phase) {
                // Held acceleration must not populate edge arrays.
                inject_at(rr64::engine::globals::controller_buttons, 0x2000u);
            }

            if (scenario == "slalom" || scenario == "combat") {
                const std::uint32_t steering_phase = (scenario_frame / 90u) % 4u;
                const std::int8_t stick_x = steering_phase == 0u   ? 38
                                            : steering_phase == 1u ? 0
                                            : steering_phase == 2u ? -38
                                                                   : 0;
                rr64::engine::write_s8(rdram, rr64::engine::globals::controller_stick_x, stick_x);
            }

            if (scenario == "combat" && scenario_frame > 0u && (scenario_frame % 90u) == 0u) {
                const std::uint16_t attack =
                    ((scenario_frame / 90u) & 1u) != 0u ? 0x0001u : 0x0002u;
                inject_edge(attack);
            }
        } else {
            autotest_scenario_logged = false;
        }
        if (scripted_drive) {
            // The scenario owns race input until the mode changes.
            return;
        }
        return;
    }

    if (mode != autotest_last_mode) {
        autotest_last_mode = mode;
        autotest_mode_frame = 0;
        autotest_scenario_logged = false;
        std::fprintf(stderr, "[RR64-AUTOTEST] mode=0x%02X\n", mode);
        std::fflush(stderr);
    }

    ++autotest_mode_frame;
    const uint32_t cycle = autotest_mode_frame % 120u;
    uint16_t injected_buttons = 0;
    if (cycle >= 20u && cycle < 24u) {
        injected_buttons = 0x1000u; // N64 Start: title screens and skippable intros.
    } else if (cycle >= 80u && cycle < 84u) {
        injected_buttons = 0x8000u; // N64 A: accept the currently selected menu item.
    }

    if (injected_buttons != 0) {
        // func_8000BF8C has already converted the controller packet into its
        // held/changed/just-pressed arrays at this hook point. Populate all
        // three so menu code sees a real edge, not merely a held button.
        inject_edge(injected_buttons);

        if (cycle == 20u || cycle == 80u) {
            std::fprintf(stderr, "[RR64-AUTOTEST] inject buttons=0x%04X mode=0x%02X frame=%u\n",
                         static_cast<unsigned>(injected_buttons), mode, autotest_mode_frame);
            std::fflush(stderr);
        }
    }
}

extern "C" void __osDispatchThread_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)ctx;
    log_once("__osDispatchThread -> ultramodern scheduler", logged_dispatch);

    if (ultramodern::this_thread() == NULLPTR) {
        return;
    }

    ultramodern::run_next_thread_and_wait(rdram);
}

extern "C" void __osEnqueueAndYield_recomp(uint8_t *rdram, recomp_context *ctx) {
    log_once("__osEnqueueAndYield -> ultramodern scheduler", logged_enqueue_yield);

    const PTR(OSThread) self = ultramodern::this_thread();
    if (self == NULLPTR) {
        return;
    }

    const PTR(PTR(OSThread)) queue = static_cast<PTR(PTR(OSThread))>(static_cast<int32_t>(ctx->r4));
    if (queue != NULLPTR) {
        // __osEnqueueAndYield's callers set the thread state before entering;
        // the libultra routine itself enqueues the running thread and dispatches.
        ultramodern::thread_queue_insert(rdram, queue, self);
    }

    ultramodern::run_next_thread_and_wait(rdram);
}

extern "C" void __osGetCause_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    log_once("__osGetCause -> synthetic retail RDB cause pulse", logged_get_cause);

    const uint32_t phase = synthetic_rdb_cause_phase.fetch_add(1, std::memory_order_relaxed);
    ctx->r2 = ((phase & 1u) == 0u) ? 0x00002000u : 0u;
}

extern "C" void __osProbeTLB_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    log_once("__osProbeTLB -> unmapped", logged_probe_tlb);

    // Normal KSEG0/KSEG1 virtual-to-physical conversions are handled before
    // libultra reaches __osProbeTLB. Road Rash has no known TLB-mapped game
    // code/data, so report no mapping for the fallback path for now.
    ctx->r2 = 0;
}

extern "C" void __osSetCompare_recomp(uint8_t *rdram, recomp_context *ctx) {
    (void)rdram;
    (void)ctx;
    log_once("__osSetCompare -> host timer runtime owns compare timing", logged_set_compare);

    // The R4300 CP0 Compare register does not exist in the native process.
    // N64ModernRuntime owns host-side VI/timer scheduling; this hardware write
    // is intentionally ignored at the native boundary.
}

// Restore voluntary-eject damage before the stock recovery/death decision,
// not only afterward in the frame shortcut pass. Do not change arrest flags.
extern "C" void rr64_restore_manual_eject_health(unsigned char *m, unsigned actor) {
    using namespace rr64::engine;
    std::uint32_t bike = 0;
    if (!read_u32(m, actor + 0xE0, bike))
        return;
    for (const auto &manual_eject_durability : rr64::prediction::isolated_state(manual_eject_protections,replay_eject_protections,replay_eject_epoch)) {
        if (!manual_eject_durability.active || bike != manual_eject_durability.bike)
            continue;
        float health = 0;
        if (read_float(m, bike + rr64::engine::bike::durability_current, health) &&
            std::isfinite(health) && health < manual_eject_durability.durability)
            write_float(m, bike + rr64::engine::bike::durability_current,
                        manual_eject_durability.durability);
    }
}

namespace rr64::prediction {
bool capture_manual_eject(ManualEjectState &out){
    if(active())return false;out=manual_eject_protections;return true;
}
bool seed_manual_eject(const ManualEjectState &historical){
    if(!active())return false;
    isolated_state(manual_eject_protections,replay_eject_protections,replay_eject_epoch)=historical;return true;
}

void commit_manual_eject(const ManualEjectState &state) noexcept {if(!active())manual_eject_protections=state;}
bool replay_manual_eject(ManualEjectState &out){
    if(!active() || replay_eject_epoch!=replay_epoch)return false;
    out=replay_eject_protections;return true;
}
}

extern "C" unsigned rr64_authority_take_actions(unsigned controller) {
    if(controller>=4)return 0;
    const auto mask=1u<<controller;
    unsigned actions=(rider_eject_requests.fetch_and(~mask,std::memory_order_acq_rel)&mask)
        ? rr64::authority::action_eject : 0;
#ifdef RR64_EXPERIMENTAL_COURSE
    if(rr64::mk64_items::take_action(controller))actions|=rr64::authority::action_mk64_item;
#endif
    return actions;
}
extern "C" int rr64_authority_eject_step(unsigned char *m,void *context,unsigned slot,unsigned actions) {
    if(!context || (actions&~rr64::authority::allowed_actions))return 0;
    auto &protections=rr64::prediction::isolated_state(manual_eject_protections,replay_eject_protections,replay_eject_epoch);
    return rr64::authority::eject_step(m,slot,(actions&rr64::authority::action_eject)!=0,protections,[&](unsigned bike){
        rr64::prediction::CpuContext saved;
        recomp_context call{};
        if(!saved.capture(*static_cast<recomp_context*>(context)) || !saved.restore(call))return false;
        call.r4=rr64::engine::guest_address(bike);
        func_8003F0E8(m,&call);
        return true;
    });
}

extern "C" void rr64_authority_reset_eject(){
    if(!rr64::prediction::active())manual_eject_protections={};
}
