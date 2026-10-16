#pragma once
#include "rr64_rival_engine.hpp"
#include <array>

namespace recomp::config {
class Config;
}

namespace rr64::rival_engine {
inline constexpr const char *enabled_option = "rr64_rival_engines_enabled";
inline constexpr const char *volume_option = "rr64_rival_engine_volume";
inline constexpr double default_volume_percent = 35.0;
struct BikeVolumeOption { BikeGroup group; const char *id; const char *label; };
inline constexpr std::array<BikeVolumeOption, 4> bike_volume_options{{
    {BikeGroup::Sport, "rr64_rival_sport_volume", "Sport Bikes"},
    {BikeGroup::Chopper, "rr64_rival_chopper_volume", "Choppers"},
    {BikeGroup::Scooter, "rr64_rival_scooter_volume", "Scooters"},
    {BikeGroup::Insanity, "rr64_rival_insanity_volume", "Insanity Bikes"},
}};
void configure_volume(recomp::config::Config &config);
void apply_volume_config(const recomp::config::Config &config);
}
