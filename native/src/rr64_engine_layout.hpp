#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "recomp.h"

namespace rr64::engine {

// Canonical USA v1.0 guest-memory contract. Feature code must use these
// definitions instead of introducing private copies of raw RDRAM addresses.
inline constexpr std::uint32_t kRdramBegin = 0x80000000u;
inline constexpr std::uint32_t kRdramSize = 0x00800000u;
inline constexpr std::uint32_t kRdramEnd = kRdramBegin + kRdramSize;
inline constexpr std::uint32_t kMaximumRacers = 14u;
inline constexpr std::uint32_t kModeRecordCount = 0x3Au;
inline constexpr std::uint32_t kModeRecordSize = 0x18u;

// Combat assist and scenery-break credit address a racer's statistics block.
// A recent-hit timer or a loose bike does not prove its previous-attacker
// pointer is initialized. Skip missing credit; retain the collision effects.
constexpr bool valid_combat_statistics(std::uint32_t address) {
    constexpr std::uint32_t first = 0x800D8570u + 0x2Cu;
    return address >= first && (address - first) % 0x118u == 0 &&
           (address - first) / 0x118u < kMaximumRacers;
}

// Stock multiplayer options and race construction, USA v1.0. The menu count
// and the human/racer counts have different consumers and must stay separate.
namespace local_race {
inline constexpr std::uint32_t menu_humans = 0x8009EF5Cu;
inline constexpr std::uint32_t menu_table = 0x8009ED94u;
inline constexpr std::uint32_t menu_cursor = 0x8009E128u;
inline constexpr std::uint32_t menu_buttons = 0x8009E238u;
inline constexpr std::uint32_t menu_dirty = 0x8009ECA4u;
inline constexpr std::uint32_t visibility = 0x8009EF60u;
inline constexpr std::uint32_t ai_choice = 0x8009EAD4u;
inline constexpr std::uint32_t pedestrian_choice = 0x8009EAE0u;
inline constexpr std::uint32_t humans = 0x800A6578u;
inline constexpr std::uint32_t racers = 0x800A6574u;
}

// Native 597A0 initializes thirteen cached effects per racer. 5980C expires
// only the first five sustained race sounds: engine, brakes, body sliding,
// surface scraping and siren. The later impact/voice slots are independent.
namespace racer_audio {
inline constexpr std::uint32_t cache = 0x800D5C70u;
inline constexpr std::uint32_t row_stride = 0xD0u;
inline constexpr std::uint32_t entry_stride = 0x10u;
inline constexpr std::uint32_t loop_slot_count = 5u;
inline constexpr std::uint32_t unused_handle = 0xFFFFFFFFu;
}

constexpr gpr guest_address(std::uint32_t address) {
    return static_cast<gpr>(static_cast<std::int64_t>(static_cast<std::int32_t>(address)));
}

constexpr bool valid_guest_range(std::uint32_t address, std::uint32_t size) {
    return address >= kRdramBegin && size <= kRdramSize &&
           (address - kRdramBegin) <= (kRdramSize - size);
}

inline bool read_u32(unsigned char *rdram, std::uint32_t address, std::uint32_t &value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    value = static_cast<std::uint32_t>(MEM_W(0, guest_address(address)));
    return true;
}

inline bool read_u16(unsigned char *rdram, std::uint32_t address, std::uint16_t &value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    value = static_cast<std::uint16_t>(MEM_HU(0, guest_address(address)));
    return true;
}

inline bool read_s8(unsigned char *rdram, std::uint32_t address, std::int8_t &value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    value = static_cast<std::int8_t>(MEM_BU(0, guest_address(address)));
    return true;
}

inline bool read_u8(unsigned char *rdram, std::uint32_t address, std::uint8_t &value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    value = static_cast<std::uint8_t>(MEM_BU(0, guest_address(address)));
    return true;
}

inline bool write_u32(unsigned char *rdram, std::uint32_t address, std::uint32_t value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    MEM_W(0, guest_address(address)) = value;
    return true;
}

inline bool write_u16(unsigned char *rdram, std::uint32_t address, std::uint16_t value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    MEM_H(0, guest_address(address)) = static_cast<std::int16_t>(value);
    return true;
}

inline bool write_s8(unsigned char *rdram, std::uint32_t address, std::int8_t value) {
    if (rdram == nullptr || !valid_guest_range(address, sizeof(value))) {
        return false;
    }
    MEM_B(0, guest_address(address)) = value;
    return true;
}

inline bool read_float(unsigned char *rdram, std::uint32_t address, float &value) {
    std::uint32_t bits = 0;
    if (!read_u32(rdram, address, bits)) {
        return false;
    }
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

inline bool write_float(unsigned char *rdram, std::uint32_t address, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    return write_u32(rdram, address, bits);
}

namespace globals {
inline constexpr std::uint32_t main_mode = 0x800A1810u;
inline constexpr std::uint32_t pending_mode = 0x800A1814u;
// func_80048544 clears this word with every pending_mode request.
inline constexpr std::uint32_t mode_request_latch = 0x8009CC50u;
// Frames since the current mode began; the boot intro times its pages by it.
// The terrain code reads the same word as terrain_request_epoch.
inline constexpr std::uint32_t mode_frame_count = 0x800A1830u;
inline constexpr std::uint32_t mode_records = 0x800A1BDCu;
inline constexpr std::uint32_t gameplay_pause_state = 0x800A2192u;
inline constexpr std::uint32_t active_input_mask = 0x800A2194u;
inline constexpr std::uint32_t update_ticks = 0x800A2198u;
inline constexpr std::uint32_t wait_ticks = 0x800A219Cu;
inline constexpr std::uint32_t total_ticks = 0x800A21A4u;
inline constexpr std::uint32_t physics_delta = 0x8009CBA8u;
// func_8001A500 advances this state with the exact LCG recurrence
// state = state * 0x0019660D + 0x3C6EF35F (mod 2^32). Every gameplay random
// decision funnels through that producer, so deterministic AI replay must
// capture the post-update value without modifying it.
inline constexpr std::uint32_t random_state = 0x8009DC30u;
// func_80074200 owns the original pause overlay and mirrors this value into
// gameplay_pause_state. The latter is also reused by the post-finish wait, so
// host-only gameplay shortcuts must use the actual overlay state instead.
inline constexpr std::uint32_t pause_menu_state = 0x800A65A8u;

inline constexpr std::uint32_t controller_buttons = 0x8009CD90u;
inline constexpr std::uint32_t controller_changed_buttons = 0x8009CD98u;
inline constexpr std::uint32_t controller_pressed_buttons = 0x8009CDA8u;
inline constexpr std::uint32_t controller_stick_x = 0x8009CDA0u;
inline constexpr std::uint32_t controller_stick_y = 0x8009CDA4u;

// The Big Game name-entry keyboard stores its free-moving cursor in cell-space
// coordinates. The native navigation shim snaps that cursor to one letter at a
// time while this specific screen is active.
// 8009EF9C is the keyboard's letter grid, not a player-name buffer.
// 7280C initializes eleven editable name characters and a terminator in the
// campaign record. 2B7A8 edits that record using a separate character cursor.
inline constexpr std::uint32_t campaign_name = 0x800D6A48u;
inline constexpr std::uint32_t campaign_name_capacity = 12u;
inline constexpr std::uint32_t name_entry_character = 0x800A668Cu;
inline constexpr std::uint32_t name_entry_cell_width = 0x8009F03Cu;
inline constexpr std::uint32_t name_entry_cell_height = 0x8009F040u;
inline constexpr std::uint32_t name_entry_cursor_x = 0x8009F04Cu;
inline constexpr std::uint32_t name_entry_cursor_y = 0x8009F050u;

inline constexpr std::uint32_t multiplayer_stage = 0x8009EF28u;
inline constexpr std::uint32_t active_racer_count = 0x8009EF5Cu;
// Four fixed twelve-byte strings used by the stock multiplayer HUD and race
// presentation. Online sessions replace the original "Player 1"-style text
// with the display names transported by the direct-connect lobby.
inline constexpr std::uint32_t multiplayer_display_names = 0x800A74F0u;
inline constexpr std::uint32_t multiplayer_display_name_count = 4u;
inline constexpr std::uint32_t multiplayer_display_name_stride = 12u;
inline constexpr std::uint32_t bike_pool_pointer = 0x800D1300u;
inline constexpr std::uint32_t actor_scene_head = 0x800A1450u;
inline constexpr std::uint32_t traffic_scene_head = 0x800A145Cu;
inline constexpr std::uint32_t scene_cull_origin = 0x800A4FDCu;
inline constexpr std::uint32_t active_viewport = 0x8009DB84u;
inline constexpr std::uint32_t actor_render_buffer_slot = 0x8009DBD4u;

inline constexpr std::uint32_t terrain_display_list_slots = 0x800DDD00u;
inline constexpr std::uint32_t terrain_cell_grid = 0x800DDEA4u;
inline constexpr std::uint32_t terrain_map_width = 0x800DEA8Cu;
inline constexpr std::uint32_t terrain_current_record = 0x800DF084u;
inline constexpr std::uint32_t terrain_camera_position = 0x800DDE80u;
inline constexpr std::uint32_t terrain_request_epoch = 0x800A1830u;

inline constexpr std::array<std::uint32_t, 17> multiplayer_game_setup_words = {
    0x8009EAC0u, 0x8009EAC4u, 0x8009EAC8u, 0x8009EACCu, 0x8009EAD0u, 0x8009EAD4u,
    0x8009EAD8u, 0x8009EADCu, 0x8009EAE0u, 0x8009EAE4u, 0x8009EAE8u, 0x8009EF5Cu,
    0x800A6574u, 0x800A6578u, 0x800A65F8u, 0x800A6680u, 0x800A6690u,
};
} // namespace globals

namespace bike {
inline constexpr std::uint32_t stride = 0x868u;
inline constexpr std::uint32_t model_state_pointer = 0x004u;
inline constexpr std::uint32_t front_wheel_position = 0x33Cu;
inline constexpr std::uint32_t body_position = 0x404u;
inline constexpr std::uint32_t rear_wheel_position = 0x53Cu;
// The stock HUD divides durability_current by durability_capacity for the
// lower bike-health bar. Keeping these separate from the nearby suspension
// vectors is important: the latter are rewritten continuously by physics.
inline constexpr std::uint32_t durability_current = 0x4F8u;
inline constexpr std::uint32_t durability_capacity = 0x4FCu;
// func_80040664 returns before applying throttle or steering whenever this
// halfword is nonzero. It is therefore the authoritative bike-side gate for
// road vibration during crashes, knockdowns, and recovery animations.
inline constexpr std::uint32_t drive_control_lockout = 0x7F6u;
// The attachment flag and reciprocal rider state remain live after the local
// racer crosses the finish line, even though normal drive control is locked.
// They therefore form the safe contract for a manual results-wait eject.
inline constexpr std::uint32_t rider_attached = 0x7F8u;
inline constexpr std::uint32_t rider_pointer = 0x800u;
} // namespace bike

namespace rider {
inline constexpr std::uint32_t stride = 0x5F0u;
// func_80036B78 passes the rider record to func_800358DC. The two-end terrain
// cache and signed gaps therefore belong to rider ground-contact state, while
// reciprocal +0x584 points to the physical bike. func_80036948 bypasses the
// adjacent response solve while +0x57C is nonzero (attached rider).
inline constexpr std::uint32_t front_contact_gap = 0x228u;
inline constexpr std::uint32_t rear_contact_gap = 0x22Cu;
inline constexpr std::uint32_t front_contact_query = 0x230u;
inline constexpr std::uint32_t rear_contact_query = 0x29Cu;
inline constexpr std::uint32_t bike_attached = 0x57Cu;
inline constexpr std::uint32_t ejected = 0x57Eu;
inline constexpr std::uint32_t bike_pointer = 0x584u;
// func_80040EAC cycles this value through the rider's available inventory.
// Value one is the native unarmed/fists entry.
inline constexpr std::uint32_t selected_weapon = 0x5B0u;
inline constexpr std::uint32_t fists_weapon = 1u;
} // namespace rider

namespace terrain {
inline constexpr std::uint32_t maximum_map_width = 128u;
inline constexpr std::uint32_t cell_stride = 0x10u;
inline constexpr std::uint32_t cell_payload = 0x00u;
// func_8007B8D4 stamps this with the current terrain epoch while a cell is in
// the candidate record. It is a last-request marker, not immutable identity.
inline constexpr std::uint32_t cell_request_epoch = 0x08u;
inline constexpr std::uint32_t cell_state = 0x0Cu;
inline constexpr std::uint32_t cell_display_list_slot = 0x0Du;
inline constexpr std::uint8_t ready_state = 5u;
inline constexpr std::uint32_t payload_minimum_size = 0x58u;
inline constexpr std::uint32_t payload_world_x = 0x1Cu;
inline constexpr std::uint32_t payload_world_z = 0x20u;

inline constexpr std::uint32_t candidate_pointer_capacity = 31u;
inline constexpr std::uint32_t candidate_indices = 0x7Cu;
inline constexpr std::uint32_t candidate_count = 0xF8u;
inline constexpr std::uint32_t candidate_record_size = 0xFCu;
inline constexpr std::uint32_t display_list_slot_count = 96u;
} // namespace terrain

// The live actor scene is a linked list of bike presentation nodes. Each bike
// owns the authoritative physical transforms above, while its model state
// points to a separate rider presentation node. Dynamic render transforms live
// in the +0x5C buffers. The +0x7C/+0xDC tier tables instead describe static RSP
// segment resources and must stay paired with the display list for that tier.
// Cross-tier presentation is allowed only after its ordered transform
// allocation has been proven compatible.
namespace actor_scene {
inline constexpr std::uint32_t maximum_viewports = 4u;
inline constexpr std::uint32_t node_minimum_size = 0x120u;
inline constexpr std::uint32_t type = 0x000u;
inline constexpr std::uint32_t entity = 0x004u;
inline constexpr std::uint32_t viewport_depth = 0x008u;
// Rewritten by func_8001BBA8 to lod_models[selected_lod]. This is the model
// consumed by both actor display-list submission paths, not a tier-zero base.
inline constexpr std::uint32_t current_model = 0x028u;
inline constexpr std::uint32_t lod_models = 0x02Cu;
inline constexpr std::uint32_t next = 0x03Cu;
inline constexpr std::uint32_t selected_lod = 0x044u;
inline constexpr std::uint32_t previous_lod = 0x046u;
inline constexpr std::uint32_t segment_counts = 0x04Au;
inline constexpr std::uint32_t display_lists = 0x050u;
inline constexpr std::uint32_t render_transform_buffers = 0x05Cu;
inline constexpr std::uint32_t primary_segment_records = 0x07Cu;
inline constexpr std::uint32_t resolved_segment_payloads = 0x0DCu;
inline constexpr std::uint32_t lod_record_count = 3u;
inline constexpr std::uint32_t lod_pointer_stride = 4u;
inline constexpr std::uint32_t segment_table_stride = 0x20u;
inline constexpr std::uint32_t maximum_segment_records_per_tier =
    segment_table_stride / sizeof(std::uint32_t);
inline constexpr std::uint32_t segment_record_payload = 0x40u;
inline constexpr std::uint32_t render_buffer_viewport_stride = 0x08u;
inline constexpr std::uint32_t render_buffer_slot_stride = 0x04u;
inline constexpr std::uint32_t render_buffer_slot_count = 2u;

inline constexpr std::uint32_t model_record_minimum_size = 0x18u;
inline constexpr std::uint32_t model_record_type = 0x04u;
inline constexpr std::uint32_t model_record_next_delta = 0x08u;
inline constexpr std::uint32_t model_record_pose_source = 0x0Cu;
inline constexpr std::uint16_t model_record_single_transform = 0x12u;
inline constexpr std::uint16_t model_record_triple_transform = 0x13u;
inline constexpr std::uint32_t model_record_stride = 0x08u;
inline constexpr std::uint32_t maximum_model_records = 128u;
inline constexpr std::uint32_t maximum_render_transforms = 128u;
inline constexpr std::uint32_t render_transform_size = 0x40u;

inline constexpr std::uint32_t model_state_pose_owner = 0x0E4u;
inline constexpr std::uint32_t pose_owner_rider_node = 0x008u;
} // namespace actor_scene

// func_80047668 creates traffic presentation nodes in the fourth scene list.
// The renderer's stock visibility test uses a single point at entity +0xA8,
// which can cull a large car while part of it is still visible at a wide-screen
// edge. Keep these offsets separate from the bike/rider contract: traffic uses
// a smaller 0x35C-byte entity and has its own active lifecycle flag.
namespace traffic_scene {
inline constexpr std::uint32_t maximum_entities = 20u;
inline constexpr std::uint32_t node_type = 4u;
inline constexpr std::uint32_t entity_minimum_size = 0x338u;
inline constexpr std::uint32_t entity_type = 0x000u;
inline constexpr std::uint32_t entity_position = 0x0A8u;
inline constexpr std::uint32_t entity_active = 0x334u;
inline constexpr std::uint32_t first_entity_type = 1u;
inline constexpr std::uint32_t last_entity_type = 5u;
inline constexpr float maximum_presentation_radius = 3400.0f;
} // namespace traffic_scene

constexpr bool should_keep_active_traffic_visible(bool maximum_view_distance, bool live_race,
                                                  bool traffic_list_member, bool active,
                                                  bool within_presentation_radius) {
    return maximum_view_distance && live_race && traffic_list_member && active &&
           within_presentation_radius;
}

constexpr bool is_valid_mode(std::uint32_t mode) {
    return mode < kModeRecordCount;
}

constexpr bool is_live_race_mode(std::uint32_t mode) {
    switch (mode) {
    case 0x09u:
    case 0x0Au:
    case 0x12u:
    case 0x13u:
    case 0x17u:
    case 0x18u:
    case 0x1Cu:
    case 0x1Du:
        return true;
    default:
        return false;
    }
}

// Mode changes are requested through pending_mode and committed later in the
// frame by func_80048564. Treat a transition away from gameplay as paused/
// inactive immediately so host feedback cannot leak into the first menu frame.
constexpr bool is_live_race_transition(std::uint32_t mode, std::uint32_t pending_mode) {
    return is_live_race_mode(mode) && is_live_race_mode(pending_mode);
}

// These four modes retain and render the completed race scene while the
// standings and "A CONTINUE" overlay are active. They deliberately remain
// outside is_live_race_mode: the original result handler, rather than the live
// simulation handler, owns their update phase.
constexpr bool is_race_results_mode(std::uint32_t mode) {
    switch (mode) {
    case 0x0Bu:
    case 0x14u:
    case 0x19u:
    case 0x1Eu:
        return true;
    default:
        return false;
    }
}

constexpr bool is_race_shortcut_scene_transition(std::uint32_t mode, std::uint32_t pending_mode) {
    if (is_live_race_transition(mode, pending_mode)) {
        return true;
    }

    // During the handoff to or from standings, accept only a result/result or
    // result/live pair. This prevents the shortcuts from leaking into the menu
    // selected by A Continue.
    return (is_race_results_mode(mode) &&
            (is_race_results_mode(pending_mode) || is_live_race_mode(pending_mode))) ||
           (is_race_results_mode(pending_mode) &&
            (is_race_results_mode(mode) || is_live_race_mode(mode)));
}

// The original pause menu does not leave the live-race mode pair. Its own
// halfword becomes nonzero while simulation and audio are suspended, so host
// feedback must include it rather than treating every rendered race frame as
// active gameplay.
constexpr bool is_gameplay_feedback_active(std::uint32_t mode, std::uint32_t pending_mode,
                                           std::uint32_t pause_state) {
    return is_live_race_transition(mode, pending_mode) && pause_state == 0u;
}

constexpr bool are_gameplay_shortcuts_active(std::uint32_t mode, std::uint32_t pending_mode,
                                             std::uint16_t pause_menu_state) {
    // L3/R3 remain available both during the finish wait and on the final
    // standings screen, but never over the real pause menu or a normal menu.
    return is_race_shortcut_scene_transition(mode, pending_mode) && pause_menu_state == 0u;
}

constexpr bool bike_accepts_drive_control(std::uint16_t drive_control_lockout) {
    return drive_control_lockout == 0u;
}

constexpr bool rider_can_manual_eject(std::uint16_t bike_rider_attached,
                                      std::uint16_t /* rider_bike_attached */,
                                      std::uint16_t rider_ejected) {
    // Deliberately do not inspect drive_control_lockout here. The finish-line
    // wait disables driving while the rider is still mounted, and L3 should
    // remain available during that wait. The finish transition also clears
    // the rider-side control attachment before the bike-side visual link, so
    // that reciprocal flag cannot be required here. func_8003F0E8 itself uses
    // the bike-side link to reach the rider, while rider_ejected prevents a
    // duplicate transition during crash/recovery.
    return bike_rider_attached != 0u && rider_ejected == 0u;
}

constexpr bool horizontal_motion_allows_road_rumble(float delta_x, float delta_z) {
    // Ignore stationary simulation jitter. At 30 updates per second this
    // threshold is far below the bike's first visible forward movement.
    constexpr float minimum_displacement_squared = 0.0004f;
    return (delta_x * delta_x) + (delta_z * delta_z) >= minimum_displacement_squared;
}

constexpr bool drive_rumble_allowed(std::uint16_t drive_control_lockout, bool moving,
                                    bool has_moved_since_race_start) {
    // Before the starting signal the stationary bike may be revved. Once the
    // bike has moved, require real motion so throttle cannot vibrate through
    // result waits, crashes, knockdowns, or recovery.
    return bike_accepts_drive_control(drive_control_lockout) &&
           (moving || !has_moved_since_race_start);
}

constexpr bool rider_has_fists_selected(std::uint32_t selected_weapon) {
    return selected_weapon == rider::fists_weapon;
}

static_assert(valid_guest_range(globals::main_mode, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::pending_mode, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::mode_records, kModeRecordCount *kModeRecordSize));
static_assert(valid_guest_range(
    globals::multiplayer_display_names,
    globals::multiplayer_display_name_count *globals::multiplayer_display_name_stride));
static_assert(valid_guest_range(globals::gameplay_pause_state, sizeof(std::uint16_t)));
static_assert(valid_guest_range(globals::pause_menu_state, sizeof(std::uint16_t)));
static_assert(valid_guest_range(globals::terrain_current_record, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::bike_pool_pointer, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::actor_scene_head, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::traffic_scene_head, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::scene_cull_origin, sizeof(float) * 2u));
static_assert(valid_guest_range(globals::active_viewport, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::terrain_request_epoch, sizeof(std::uint32_t)));
static_assert(valid_guest_range(globals::actor_render_buffer_slot, sizeof(std::uint32_t)));
static_assert(bike::durability_current + sizeof(float) <= bike::stride);
static_assert(bike::durability_capacity + sizeof(float) <= bike::stride);
static_assert(bike::drive_control_lockout + sizeof(std::uint16_t) <= bike::stride);
static_assert(bike::rider_attached + sizeof(std::uint16_t) <= bike::stride);
static_assert(bike::rider_pointer + sizeof(std::uint32_t) <= bike::stride);
static_assert(rider::bike_attached + sizeof(std::uint16_t) <= rider::stride);
static_assert(rider::ejected + sizeof(std::uint16_t) <= rider::stride);
static_assert(rider::selected_weapon + sizeof(std::uint32_t) <= rider::stride);
static_assert(globals::multiplayer_game_setup_words.size() == 17);
static_assert(is_live_race_mode(0x09u));
static_assert(is_live_race_mode(0x1Du));
static_assert(!is_live_race_mode(0x14u));
static_assert(is_race_results_mode(0x0Bu));
static_assert(is_race_results_mode(0x14u));
static_assert(is_race_results_mode(0x19u));
static_assert(is_race_results_mode(0x1Eu));
static_assert(!is_race_results_mode(0x20u));
static_assert(is_live_race_transition(0x09u, 0x0Au));
static_assert(!is_live_race_transition(0x09u, 0x20u));
static_assert(is_race_shortcut_scene_transition(0x14u, 0x14u));
static_assert(is_race_shortcut_scene_transition(0x13u, 0x14u));
static_assert(!is_race_shortcut_scene_transition(0x14u, 0x20u));
static_assert(is_gameplay_feedback_active(0x09u, 0x0Au, 0u));
static_assert(!is_gameplay_feedback_active(0x09u, 0x0Au, 1u));
static_assert(are_gameplay_shortcuts_active(0x09u, 0x0Au, 0u));
static_assert(are_gameplay_shortcuts_active(0x14u, 0x14u, 0u));
static_assert(!are_gameplay_shortcuts_active(0x09u, 0x0Au, 1u));
static_assert(!are_gameplay_shortcuts_active(0x14u, 0x20u, 0u));
static_assert(bike_accepts_drive_control(0u));
static_assert(!bike_accepts_drive_control(1u));
static_assert(rider_can_manual_eject(1u, 1u, 0u));
static_assert(rider_can_manual_eject(1u, 0u, 0u));
static_assert(!rider_can_manual_eject(0u, 1u, 0u));
static_assert(!rider_can_manual_eject(1u, 1u, 1u));
static_assert(!horizontal_motion_allows_road_rumble(0.0f, 0.0f));
static_assert(horizontal_motion_allows_road_rumble(0.03f, 0.0f));
static_assert(drive_rumble_allowed(0u, false, false));
static_assert(drive_rumble_allowed(0u, true, true));
static_assert(!drive_rumble_allowed(0u, false, true));
static_assert(!drive_rumble_allowed(1u, true, false));
static_assert(rider_has_fists_selected(1u));
static_assert(!rider_has_fists_selected(2u));
static_assert(should_keep_active_traffic_visible(true, true, true, true, true));
static_assert(!should_keep_active_traffic_visible(false, true, true, true, true));
static_assert(!should_keep_active_traffic_visible(true, true, true, false, true));
static_assert(!should_keep_active_traffic_visible(true, true, true, true, false));
static_assert(is_valid_mode(0x39u));
static_assert(!is_valid_mode(0x3Au));

} // namespace rr64::engine
