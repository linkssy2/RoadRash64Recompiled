#include "rr64_offline_modifiers.hpp"
#include "rr64_netplay.hpp"
#include "recompui/config.h"

#include <array>
#include <string>

namespace rr64::offline_modifiers {
namespace {
constexpr const char *config_id = "cheats";
struct Option {
    Flag flag;
    const char *id;
    const char *label;
    const char *description;
};
constexpr std::array<Option, 6> options{{
    {Flag::RiderHealth, "infinite_rider_health", "Infinite Rider Health",
     "Prevents rider stamina damage for local players. Physical crashes, ejects and busts still happen."},
    {Flag::BikeDurability, "indestructible_bikes", "Indestructible Bikes",
     "Prevents crash damage to local players' bikes. Crashes and normal recovery still happen."},
    {Flag::AllBikes, "all_bikes", "All Bikes Available",
     "Shows all bikes in selection and the bike shop. Big Game purchases still cost money; permanent unlock flags are unchanged."},
    {Flag::AllTracks, "all_tracks", "All Tracks Available",
     "Unlocks every race level, including reversed courses, in Thrash and local multiplayer selection. Saves and Big Game progress are unchanged; switching off relocks only the levels this option opened."},
    {Flag::AllWeapons, "all_weapons", "Start with All Weapons",
     "Gives local racers every weapon at maximum starting quantity. Also grants once when switched on during a race. Use and theft remain normal; cops keep their police equipment."},
    {Flag::FreezeOpponents, "freeze_opponents", "Prevent Opponents from Moving",
     "Holds computer-controlled racers in place while riding. Local players can still race; crashes and recovery continue normally. Switch off to let opponents race again."},
}};
bool initialized = false;
int previous_online = -1;
std::string description(const Option &option, bool online) {
    return std::string(online ? "Unavailable during online play. " : "Offline only. ") +
           option.description + " Local achievements do not unlock while any cheat is on.";
}
}
void register_config_tab() {
    auto &config = recompui::config::create_config_tab("Cheats", config_id, false);
    for (const auto &option : options) {
        config.add_bool_option(option.id, option.label, description(option, false), false);
        config.add_option_change_callback(
            option.id, [flag = option.flag](recomp::config::ConfigValueVariant value,
                                            recomp::config::ConfigValueVariant,
                                            recomp::config::OptionChangeContext) {
                set_requested(flag, std::get<bool>(value));
            });
    }
}
void apply_config() {
    auto &config = recompui::config::get_config(config_id);
    for (const auto &option : options)
        set_requested(option.flag, std::get<bool>(config.get_option_value(option.id)));
    initialized = true;
    previous_online = -1;
}
void update_ui() {
    if (!initialized)
        return;
    const bool online = netplay::get_status().active;
    if (previous_online == static_cast<int>(online))
        return;
    previous_online = online;
    auto &config = recompui::config::get_config(config_id);
    for (const auto &option : options) {
        config.update_option_disabled(option.id, online);
        config.update_option_description(option.id, description(option, online));
    }
}
}
