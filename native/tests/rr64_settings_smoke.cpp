#include "../src/rr64_master_volume.hpp"
#include "../src/rr64_presentation_options.hpp"
#include <array>
#include <cstdio>
#include <limits>
int main() {
    using namespace rr64;
    std::array<std::int16_t,4> samples{-32768,-100,100,32767};
    const auto original=samples;
    audio::apply_master_volume(samples,audio::volume_gain(100));
    if(samples!=original)return 1;
    audio::apply_master_volume(samples,audio::volume_gain(50));
    if(samples!=std::array<std::int16_t,4>{-16384,-50,50,16383})return 2;
    audio::apply_master_volume(samples,audio::volume_gain(0));
    if(samples!=std::array<std::int16_t,4>{})return 3;
    if(audio::volume_gain(-20)!=0 || audio::volume_gain(150)!=1 ||
        audio::volume_gain(std::numeric_limits<double>::quiet_NaN())!=1)return 4;
    for(auto* setting:{&presentation_options::max_lod,&presentation_options::world_distance}) {
        setting->store(-1); if(!presentation_options::enabled(*setting,true) || presentation_options::enabled(*setting,false))return 5;
        setting->store(0); if(presentation_options::enabled(*setting,true))return 6;
        setting->store(1); if(!presentation_options::enabled(*setting,false))return 7;
    }
    std::puts("Settings: full/half/muted audio, invalid range and saved-setting precedence passed.");
}
