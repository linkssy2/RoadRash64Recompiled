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
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

extern "C" void func_8007C524(uint8_t *rdram, recomp_context *ctx);
extern "C" void func_8003F0E8(uint8_t *rdram, recomp_context *ctx);
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

// The boot intro (func_80021B0C) picks its page from the mode frame counter:
// the N64 logo until frame 200, then pages starting at 300, 400, 500 and 600.
// The last page counts its own frames and moves to the attract demo after 300.
// Past the logo, a fresh A or Start press moves the counter to the next page;
// on the last page it opens the main menu, the same request func_80071F94
// makes for Start on that page.
extern "C" void rr64_intro_skip(unsigned char *rdram) {
    constexpr std::array<std::uint32_t, 4> page_starts = {300u, 400u, 500u, 600u};
    constexpr std::uint16_t a_or_start = 0x9000u;
    static bool held_before = true; // a button held through boot is not a press
    std::uint32_t frames = 0;
    std::uint16_t buttons = 0;
    if (!rr64::engine::read_u32(rdram, rr64::engine::globals::mode_frame_count, frames) ||
        !rr64::engine::read_u16(rdram, rr64::engine::globals::controller_buttons, buttons)) {
        return;
    }
    const bool held = (buttons & a_or_start) != 0u;
    const bool pressed = held && !held_before;
    held_before = held;
    if (!pressed || frames < 200u) {
        return;
    }
    for (const std::uint32_t start : page_starts) {
        if (frames < start) {
            rr64::engine::write_u32(rdram, rr64::engine::globals::mode_frame_count, start);
            return;
        }
    }
    // Entering the menu this way is a player starting the game, as a Start
    // press on this page is (next_player_started_title_session); A otherwise
    // leaves the cheats and other session features off.
    player_started_title_session.store(true, std::memory_order_relaxed);
    // func_80048544(0x20), then the main menu cursor func_80071F94 selects.
    rr64::engine::write_u32(rdram, rr64::engine::globals::pending_mode, 0x20u);
    rr64::engine::write_u32(rdram, rr64::engine::globals::mode_request_latch, 0u);
    rr64::engine::write_u32(rdram, rr64::engine::local_race::menu_cursor, 2u);
}

#include "rr64_autotest_control.hpp"
extern "C" void rr64_prediction_request_verification_restart();
extern "C" void rr64_autotest_input(unsigned char *rdram, unsigned int mode) {
    static const bool enabled = environment_flag_enabled("RR64_AUTOTEST");
    static const bool drive_enabled = environment_flag_enabled("RR64_AUTOTEST_DRIVE");
    static const std::string requested_scenario = environment_text("RR64_AUTOTEST_SCENARIO");

    if (!enabled || rdram == nullptr) {
        return;
    }

    static rr64::autotest::ControlFile control_file;
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
