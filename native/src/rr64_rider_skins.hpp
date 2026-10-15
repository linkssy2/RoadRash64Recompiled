#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rr64::rider_skins {
inline constexpr const char *mod_id = "rr64_more_characters";
inline constexpr unsigned maximum_characters = 32;
// Native CI8 indices followed by 256 big-endian RGBA5551 palette entries.
// Headers, geometry, posture and animations continue to come from the ROM.
struct Texture {
    unsigned width{}, height{};
    std::vector<std::uint8_t> bytes;
};
struct Appearance {
    std::string id, name;
    unsigned donor{};
    std::array<Texture, 4> textures; // head, torso, body, distant body
    bool turtle_shell{}; // Small cosmetic attachment; native physics stay unchanged.
    bool dual_head{}; // Two authored half-head strips for asymmetric face detail.
};
bool install_catalog(std::span<const Appearance> appearances);
void unavailable(std::string reason);
// Runtime calls on_init before mod content callbacks and entrypoint afterward.
void prepare_session() noexcept;
void begin_session() noexcept;
bool can_change() noexcept;
bool enabled() noexcept;
inline constexpr const char *random_cpu_option = "rr64_random_cpu_skins";
void set_random_cpu(bool enabled) noexcept;
inline constexpr unsigned stock_characters = 40;
inline constexpr unsigned render_characters = maximum_characters + stock_characters;
inline constexpr const char *cpu_percent_option = "rr64_cpu_skin_percent";
inline constexpr const char *cpu_pool_option = "rr64_cpu_skin_pool";
void set_cpu_percent(double value) noexcept;
void set_cpu_pool(unsigned value) noexcept;
bool cpu_skins_active() noexcept;
void install_stock(unsigned native, Appearance appearance);
bool load_stock(std::span<const std::uint8_t> rom);
unsigned random_cpu_selection(unsigned racer, unsigned donor, unsigned mask = 0x3fff) noexcept;
unsigned count() noexcept;
// Zero means an unmodified original rider; custom indices are never guest IDs.
const Appearance *appearance(unsigned one_based) noexcept;
unsigned native_donor(unsigned one_based) noexcept;
unsigned find(std::string_view id) noexcept;
const char *name(unsigned one_based) noexcept;
std::string status();
unsigned menu_selection(unsigned local_slot) noexcept;
void set_menu_selection(unsigned local_slot, unsigned appearance) noexcept;
unsigned race_selection(unsigned canonical_racer) noexcept;
void set_race_selection(unsigned canonical_racer, unsigned appearance) noexcept;
void clear_menu() noexcept;
void clear_race() noexcept;
}
