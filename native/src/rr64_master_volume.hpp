#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
namespace rr64::audio {
inline float volume_gain(double percent) {
    return std::isfinite(percent) ? static_cast<float>(std::clamp(percent, 0.0, 100.0) / 100.0)
                                  : 1.0f;
}
inline void apply_master_volume(std::span<std::int16_t> samples, float gain) {
    if (gain >= 1.0f)
        return;
    for (auto &sample : samples)
        sample = static_cast<std::int16_t>(sample * gain);
}
}
