#include <cmath>
#include <cstdio>
#include <array>
#include <limits>

#include "rr64_voice_chat.hpp"

int main() {
    using namespace rr64::voice_chat;
    if(direction_pan(1,0,0,1)!=1 || direction_pan(-1,0,0,1)!=-1 ||
       direction_pan(0,1,0,1)!=0 || direction_pan(1,0,0,0)!=0)return 2;
    const std::array<std::int16_t,4> silence{},signal{16384,-16384,16384,-16384};
    if(gate_level(silence,1)!=0 || std::abs(gate_level(signal,1)-0.5f)>0.0001f)return 3;
    if(!(flyby_pitch(100,90,.1f,1)>1 && flyby_pitch(90,100,.1f,1)<1) ||
       flyby_pitch(100,90,.1f,0)!=1 || flyby_pitch(0,1000,.1f,1)!=1 ||
       flyby_pitch(100,90,0,1)!=1 || flyby_pitch(100,90,1,1)!=1 ||
       flyby_pitch(100,std::numeric_limits<float>::quiet_NaN(),.1f,1)!=1)return 4;
    const float near_gain = rr64::voice_chat::proximity_gain(20.0f);
    const float middle_gain = rr64::voice_chat::proximity_gain(500.0f);
    const float far_gain = rr64::voice_chat::proximity_gain(1200.0f);
    if (std::abs(near_gain - 1.0f) > 0.001f ||
        !(middle_gain > 0.0f && middle_gain < near_gain) ||
        std::abs(far_gain) > 0.001f) {
        std::fprintf(stderr, "Invalid proximity curve: %.3f %.3f %.3f\n",
            near_gain, middle_gain, far_gain);
        return 1;
    }
    std::puts("RR64 proximity voice attenuation smoke test passed.");
    return 0;
}
