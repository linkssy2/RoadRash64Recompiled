#pragma once
#include <cstdint>
#include <filesystem>
#include <array>
#include <string_view>

namespace rr64::local_race_options {
// UI-thread persistence; guest hooks never perform disk I/O.
void initialize(const std::filesystem::path &directory);
void flush();
// Session-only online preferences never overwrite this machine's local preset.
void reset_online();
unsigned online_options();
void apply_online_options(unsigned bits);
// Thrash owns a separate offline preset. Never copy it into multiplayer/online.
unsigned thrash_options();
void set_thrash_options(unsigned bits);
// Stock Thrash choices: difficulty [0:2], traffic [3:4], police [5:6].
// Unknown preserves the original game's defaults on the first visit.
constexpr unsigned unknown_thrash_stock = 0xFFFFFFFFu;
constexpr bool valid_thrash_stock(unsigned bits) {
    return bits <= 127u && (bits & 7u) <= 4u;
}
unsigned thrash_stock_options();
void set_thrash_stock_options(unsigned bits);
// Stable donor IDs and the existing bits 11..26 remain compatible with saved
// presets and host setup packets. Music now follows one preference across MK64
// courses: any old enabled bit opts in, while a toggle sets/clears the full mask.
inline constexpr unsigned course_music_mask = 0x07FFF800u;
inline constexpr std::array<std::string_view, 16> music_courses{
    "mario_raceway", "choco_mountain", "bowsers_castle", "banshee_boardwalk",
    "yoshi_valley", "frappe_snowland", "koopa_troopa_beach", "royal_raceway",
    "luigi_raceway", "moo_moo_farm", "toads_turnpike", "kalimari_desert",
    "sherbet_land", "rainbow_road", "wario_stadium", "dks_jungle_parkway"};
constexpr unsigned course_music_bit(std::string_view course) {
    for (unsigned i = 0; i < music_courses.size(); ++i)
        if (music_courses[i] == course) return 1u << (11 + i);
    return 0;
}
constexpr bool valid_online_options(unsigned bits) {
    return bits <= 0x0FFFFFFFu && (bits & 15u) <= 10u;
}
// An unset bit preserves ON for existing presets and setup snapshots.
inline constexpr unsigned mk64_items_disabled_bit = 1u << 27;
// Live UI/gameplay only. Private physics uses its bound historical item state.
bool mk64_items_enabled();
void toggle_mk64_items(std::string_view course);
// Scoped to the current imported-course menu frame; stock row visibility stays native.
void show_mk64_items_row(bool visible);
bool course_music_enabled(std::string_view course);
void toggle_course_music(std::string_view course);
// Candidate roster ceiling, not the size of the engine's actor allocation.
constexpr unsigned max_ai(unsigned humans) {
    return humans >= 1 && humans <= 4 ? 11u - humans : 0u;
}
constexpr unsigned roster(unsigned ai, unsigned humans, unsigned stock) {
    return humans >= 1 && humans <= 4 ? humans + (ai < max_ai(humans) ? ai : max_ai(humans))
                                      : stock;
}
constexpr unsigned next_row(unsigned row, int direction, unsigned mask) {
    for (unsigned attempts = 0; attempts < 11; ++attempts) {
        row = (row + (direction < 0 ? 10u : 1u)) % 11u;
        if (mask & (1u << row))
            return row;
    }
    return 0;
}
}

extern "C" {
void rr64_custom_cop_ai_pool(unsigned char *memory, void *context);
int rr64_local_player_roaming(unsigned char *memory, unsigned actor);
void rr64_local_options_menu_begin(unsigned char *memory);
void rr64_local_options_reset_race();
unsigned rr64_local_spectator_begin(unsigned char *memory, unsigned view);
void rr64_local_spectator_end(unsigned char *memory, unsigned view, unsigned saved);
void rr64_local_bike_profiles_reset();
int rr64_local_options_input(unsigned char *memory);
int rr64_local_options_navigation(unsigned char *memory);
unsigned rr64_local_options_table(unsigned stock);
unsigned rr64_local_options_visible(unsigned row, unsigned stock);
void rr64_local_options_text(unsigned char *memory, unsigned row, unsigned buffer);
void rr64_local_options_finish(unsigned char *memory);
void rr64_local_options_race(unsigned char *memory);
void rr64_local_options_thrash_roster(unsigned char *memory);
void rr64_local_options_thrash_race(unsigned char *memory);
unsigned rr64_local_bike_level(unsigned original);
unsigned rr64_local_bike_menu_level(unsigned original);
void rr64_local_bike_ai_pool(unsigned char *memory, void *context);
unsigned rr64_local_bike_profile(unsigned char *memory, unsigned racer, unsigned profile);
}
