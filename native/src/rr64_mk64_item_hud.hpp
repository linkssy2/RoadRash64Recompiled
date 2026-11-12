#pragma once
#ifdef __cplusplus
#include "rr64_mk64_item_state.hpp"
#include "rr64_netplay.hpp"

namespace rr64::mk64_items {
struct HudDisplay {
    bool owns = false;
    Item shown = Item::None;
};
// Native reward roulettes temporarily take priority over a held MK item.
HudDisplay hud_display(const netplay::CourseWeaponRoll &, unsigned clock, unsigned slot, Item held,
                       bool mk64_enabled = true) noexcept;
// A completed native weapon switch reveals that weapon without consuming the
// separate MK item. A new item revision returns focus to its icon. These focus
// operations synchronize the native gameplay and HUD creation threads.
void reset_hud_focus() noexcept;
void focus_native_weapon(unsigned char *mapping, unsigned canonical, unsigned rider,
                         unsigned weapon, const RiderState &, bool wrapped = false) noexcept;
HudDisplay hud_display_for_rider(unsigned char *mapping, unsigned canonical, unsigned rider,
                                 const RiderState &, const netplay::CourseWeaponRoll &,
                                 unsigned clock, unsigned equipped,
                                 bool mk64_enabled = true) noexcept;
// Published on the native thread; host input only reads this bounded copy.
struct CycleBinding {
    unsigned char *mapping = nullptr;
    unsigned canonical = racer_capacity, rider = 0, mask = 0;
    RiderState item{};
};
void publish_cycle_bindings(const std::array<CycleBinding, 4> &) noexcept;
std::uint16_t filter_cycle_input(unsigned profile, std::uint16_t raw_buttons, std::uint16_t buttons,
                                 bool gameplay_allowed) noexcept;
void reset_hud_queue() noexcept;
void arm_hud_sprite(unsigned char *, unsigned view, unsigned sprite, HudDisplay) noexcept;
void commit_hud_sprite(unsigned char *) noexcept;
} // namespace rr64::mk64_items
extern "C" {
#endif
// Replaces only an authenticated weapon record at its original draw position.
int rr64_mk64_item_hud_draw_record(unsigned char *, unsigned record);
#ifdef __cplusplus
}
#endif
