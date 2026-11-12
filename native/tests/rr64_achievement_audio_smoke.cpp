#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "rr64_achievement_audio.hpp"

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[RR64-ACH-AUDIO] FAILED: %s\n", message);
    }
    return condition;
}

} // namespace

int main() {
    constexpr std::uint32_t sample_rate = 48000u;
    rr64::achievement_audio::GuitarStingSynth synth;
    std::array<std::int16_t, 2048> silence{};

    bool passed = true;
    passed &= check(!synth.mix(silence, sample_rate), "idle synth leaves audio untouched");
    passed &= check(std::all_of(silence.begin(), silence.end(), [](std::int16_t value) {
        return value == 0;
    }), "idle buffer remains silent");

    synth.trigger();
    std::int16_t maximum = 0;
    std::uint64_t nonzero_samples = 0;
    std::uint64_t stereo_differences = 0;
    for (unsigned block = 0; block < 70u; ++block) {
        silence.fill(0);
        synth.mix(silence, sample_rate);
        for (const std::int16_t value : silence) {
            maximum = std::max<std::int16_t>(maximum, static_cast<std::int16_t>(
                value < 0 ? -static_cast<int>(value) : value));
            nonzero_samples += value != 0 ? 1u : 0u;
        }
        for (std::size_t sample = 0; sample < silence.size(); sample += 2u) {
            stereo_differences += silence[sample] != silence[sample + 1u] ? 1u : 0u;
        }
    }

    passed &= check(maximum > 2500, "guitar sting has audible level");
    passed &= check(nonzero_samples > 10000u, "guitar sting spans the intended riff");
    passed &= check(stereo_differences > 1000u, "double-tracked guitar has stereo width");
    passed &= check(!synth.active(), "guitar sting terminates without a stuck voice");

    synth.trigger();
    synth.stop();
    silence.fill(0);
    passed &= check(!synth.mix(silence, sample_rate), "disabled guitar sting stops immediately");
    passed &= check(std::all_of(silence.begin(), silence.end(), [](std::int16_t value) {
        return value == 0;
    }), "stopped guitar sting leaves the audio buffer silent");

    if (!passed) {
        return 1;
    }
    std::puts("RR64 achievement guitar-sting smoke test passed.");
    return 0;
}
