#include "rr64_achievement_audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

namespace rr64::achievement_audio {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kStingDurationSeconds = 1.42f;

struct ChordHit {
    float start;
    float root_frequency;
    float duration;
    float damping;
};

// An original E-E-G-A-E chug and resolving power chord. Each hit excites a
// root, fifth, and octave twice with slight detuning and stereo separation,
// which gives the physical string model the weight of a double-tracked amp.
constexpr std::array<ChordHit, 5> kRiff{{
    {0.000f, 82.407f, 0.145f, 0.9790f},
    {0.115f, 82.407f, 0.165f, 0.9800f},
    {0.255f, 97.999f, 0.185f, 0.9820f},
    {0.405f, 110.000f, 0.210f, 0.9840f},
    {0.575f, 82.407f, 0.790f, 0.9962f},
}};
constexpr std::array<float, 3> kPowerChordRatios{{1.0f, 1.498307f, 2.0f}};
constexpr std::array<float, 3> kStringGains{{0.82f, 0.55f, 0.42f}};

std::atomic_bool g_sting_requested{false};
std::atomic_bool g_sting_cancel_requested{false};
GuitarStingSynth g_sting{};

std::int16_t mix_sample(std::int16_t original, float addition) noexcept {
    const int mixed =
        static_cast<int>(original) +
        static_cast<int>(addition * static_cast<float>(std::numeric_limits<std::int16_t>::max()));
    return static_cast<std::int16_t>(
        std::clamp(mixed, static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                   static_cast<int>(std::numeric_limits<std::int16_t>::max())));
}

} // namespace

void GuitarStingSynth::trigger() noexcept {
    frame_cursor_ = 0;
    initialized_sample_rate_ = 0;
    previous_amp_input_ = {};
    dc_blocker_state_ = {};
    cabinet_high_ = {};
    cabinet_low_ = {};
    active_ = true;
}

void GuitarStingSynth::stop() noexcept {
    active_ = false;
    frame_cursor_ = 0;
}

void GuitarStingSynth::initialize_strings(std::uint32_t sample_rate) noexcept {
    initialized_sample_rate_ = sample_rate;
    noise_state_ = 0xC001D00Du;
    std::size_t voice_index = 0;

    auto next_noise = [this]() noexcept {
        noise_state_ = (noise_state_ * 1664525u) + 1013904223u;
        const float normalized = static_cast<float>((noise_state_ >> 8u) & 0x00FFFFFFu) /
                                 static_cast<float>(0x00800000u);
        return normalized - 1.0f;
    };

    for (const ChordHit &chord : kRiff) {
        for (std::size_t track = 0; track < 2; ++track) {
            const float detune = track == 0 ? 0.9962f : 1.0038f;
            const float pan = track == 0 ? 0.20f : 0.80f;
            for (std::size_t string_index = 0; string_index < kPowerChordRatios.size();
                 ++string_index) {
                StringVoice &voice = voices_[voice_index++];
                const float frequency =
                    chord.root_frequency * kPowerChordRatios[string_index] * detune;
                voice.delay_length = static_cast<std::uint32_t>(
                    std::clamp(std::lround(static_cast<float>(sample_rate) / frequency), 8l,
                               static_cast<long>(kMaximumDelaySamples)));
                voice.delay_cursor = 0;
                voice.start_frame = static_cast<std::uint64_t>(chord.start * sample_rate);
                voice.stop_frame =
                    static_cast<std::uint64_t>((chord.start + chord.duration) * sample_rate);
                voice.gain = kStringGains[string_index] * (track == 0 ? 0.96f : 0.92f);
                voice.pan = pan;
                voice.damping = chord.damping - (0.0007f * static_cast<float>(string_index));

                float smoothed_noise = 0.0f;
                for (std::uint32_t sample = 0; sample < voice.delay_length; ++sample) {
                    const float noise = next_noise();
                    smoothed_noise = (0.64f * noise) + (0.36f * smoothed_noise);
                    // A virtual pick-position notch prevents the excitation
                    // from sounding like undifferentiated white noise.
                    const float position =
                        static_cast<float>(sample) / static_cast<float>(voice.delay_length);
                    const float pick_shape =
                        0.72f + (0.28f * std::sin(kPi * std::clamp(position * 3.1f, 0.0f, 1.0f)));
                    voice.delay[sample] = smoothed_noise * pick_shape;
                }
            }
        }
    }
}

std::array<float, 2> GuitarStingSynth::render_frame(std::uint32_t sample_rate) noexcept {
    std::array<float, 2> strings{};
    for (StringVoice &voice : voices_) {
        if (frame_cursor_ < voice.start_frame || frame_cursor_ >= voice.stop_frame ||
            voice.delay_length < 2u) {
            continue;
        }

        const std::uint32_t next = (voice.delay_cursor + 1u) % voice.delay_length;
        const float current = voice.delay[voice.delay_cursor];
        const float adjacent = voice.delay[next];
        voice.delay[voice.delay_cursor] = voice.damping * (0.505f * current + 0.495f * adjacent);
        voice.delay_cursor = next;

        const std::uint64_t local_frame = frame_cursor_ - voice.start_frame;
        const std::uint64_t remaining = voice.stop_frame - frame_cursor_;
        const float attack = std::clamp(static_cast<float>(local_frame) /
                                            (0.0025f * static_cast<float>(sample_rate)),
                                        0.0f, 1.0f);
        const float release = std::clamp(
            static_cast<float>(remaining) / (0.038f * static_cast<float>(sample_rate)), 0.0f, 1.0f);
        const float sample = current * voice.gain * attack * release;
        strings[0] += sample * (1.0f - voice.pan);
        strings[1] += sample * voice.pan;
    }

    std::array<float, 2> output{};
    const float high_cut = 1.0f - std::exp(-2.0f * kPi * 5200.0f / static_cast<float>(sample_rate));
    const float low_cut = 1.0f - std::exp(-2.0f * kPi * 2800.0f / static_cast<float>(sample_rate));
    for (std::size_t channel = 0; channel < output.size(); ++channel) {
        const float coupled = strings[channel] + (0.12f * strings[1u - channel]);
        // Asymmetric preamp saturation, a DC blocker, and two cabinet poles
        // turn the plucked strings into a compact high-gain guitar recording.
        const float biased = (coupled * 5.8f) + 0.10f;
        const float saturated = std::tanh(biased) - std::tanh(0.10f);
        const float blocked =
            saturated - previous_amp_input_[channel] + (0.994f * dc_blocker_state_[channel]);
        previous_amp_input_[channel] = saturated;
        dc_blocker_state_[channel] = blocked;
        cabinet_high_[channel] += high_cut * (blocked - cabinet_high_[channel]);
        cabinet_low_[channel] += low_cut * (cabinet_high_[channel] - cabinet_low_[channel]);
        output[channel] =
            ((0.62f * cabinet_high_[channel]) + (0.38f * cabinet_low_[channel])) * 0.31f;
    }
    return output;
}

bool GuitarStingSynth::mix(std::span<std::int16_t> interleaved_stereo,
                           std::uint32_t sample_rate) noexcept {
    if (!active_ || sample_rate == 0u || interleaved_stereo.size() < 2u) {
        return false;
    }

    if (initialized_sample_rate_ != sample_rate) {
        initialize_strings(sample_rate);
    }

    bool mixed_any = false;
    const std::size_t stereo_samples = interleaved_stereo.size() & ~std::size_t{1};
    for (std::size_t sample = 0; sample < stereo_samples; sample += 2u) {
        const float time = static_cast<float>(frame_cursor_) / static_cast<float>(sample_rate);
        if (time >= kStingDurationSeconds) {
            active_ = false;
            break;
        }

        const std::array<float, 2> value = render_frame(sample_rate);
        interleaved_stereo[sample] = mix_sample(interleaved_stereo[sample], value[0]);
        interleaved_stereo[sample + 1u] = mix_sample(interleaved_stereo[sample + 1u], value[1]);
        ++frame_cursor_;
        mixed_any = true;
    }

    return mixed_any;
}

bool GuitarStingSynth::active() const noexcept {
    return active_;
}

void request_guitar_sting() noexcept {
    g_sting_requested.store(true, std::memory_order_release);
}

void cancel_guitar_sting() noexcept {
    g_sting_cancel_requested.store(true, std::memory_order_release);
}

bool mix_requested_guitar_sting(std::span<std::int16_t> interleaved_stereo,
                                std::uint32_t sample_rate) noexcept {
    if (g_sting_cancel_requested.exchange(false, std::memory_order_acq_rel)) {
        g_sting_requested.store(false, std::memory_order_release);
        g_sting.stop();
        return false;
    }
    if (g_sting_requested.exchange(false, std::memory_order_acq_rel)) {
        g_sting.trigger();
    }
    return g_sting.mix(interleaved_stereo, sample_rate);
}

} // namespace rr64::achievement_audio
