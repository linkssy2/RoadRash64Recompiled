#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
namespace recomp::config { class Config; }

namespace rr64::voice_chat {

void set_enabled(bool enabled);
bool enabled();
void configure(recomp::config::Config&);
void apply_config(recomp::config::Config&);

// Listener-relative equal-power panning. Degenerate heading stays centered.
inline float direction_pan(float dx,float dz,float forward_x,float forward_z){
    const float length=std::hypot(dx,dz), heading=std::hypot(forward_x,forward_z);
    if(!std::isfinite(length) || !std::isfinite(heading) || length<0.01f || heading<0.01f)return 0;
    return std::clamp((dx/length)*(forward_z/heading)-(dz/length)*(forward_x/heading),-1.f,1.f);
}
inline float gate_level(std::span<const std::int16_t> samples,float gain){
    double sum=0;for(auto v:samples){const double s=v/32768.0;sum+=s*s;}
    return samples.empty()?0.f:float(std::sqrt(sum/samples.size()))*gain;
}
// A bounded presentation effect in native world units, not a physics change.
// Ignore long gaps and teleport-sized corrections instead of pitching a respawn.
inline float flyby_pitch(float previous,float current,float seconds,float strength){
    if(!std::isfinite(previous) || !std::isfinite(current) || !std::isfinite(seconds) ||
       !std::isfinite(strength) || seconds<0.01f || seconds>0.5f || std::abs(current-previous)>300.f)return 1;
    const float radial=(current-previous)/seconds;
    return 1.f-std::clamp(radial/2500.f,-0.12f,0.12f)*std::clamp(strength,0.f,1.f);
}

// Called by the direct-connect service thread. Capture and decoding are kept
// away from the game's render and audio producer threads.
void update();
void shutdown();

// Adds decoded proximity speech to the game's interleaved stereo stream.
void mix(std::span<std::int16_t> stereo_samples, std::uint32_t output_rate);

// Public for deterministic ROM-free validation of the spatial policy.
inline float proximity_gain(float distance) {
    constexpr float full_volume_distance = 90.0f;
    constexpr float silent_distance = 1000.0f;
    if (!std::isfinite(distance) || distance >= silent_distance) {
        return 0.0f;
    }
    if (distance <= full_volume_distance) {
        return 1.0f;
    }
    const float t = std::clamp(
        (distance - full_volume_distance) / (silent_distance - full_volume_distance), 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 1.0f - smooth;
}

} // namespace rr64::voice_chat
