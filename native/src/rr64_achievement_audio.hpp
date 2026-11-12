#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace rr64::achievement_audio {

// A short, original procedural guitar sting. It is synthesized into the
// existing host audio stream so an achievement cannot desynchronize or open a
// second audio device.
class GuitarStingSynth {
  public:
    void trigger() noexcept;
    void stop() noexcept;
    bool mix(std::span<std::int16_t> interleaved_stereo, std::uint32_t sample_rate) noexcept;
    [[nodiscard]] bool active() const noexcept;

  private:
    static constexpr std::size_t kVoiceCount = 30;
    static constexpr std::size_t kMaximumDelaySamples = 2048;

    struct StringVoice {
        std::array<float, kMaximumDelaySamples> delay{};
        std::uint32_t delay_length = 0;
        std::uint32_t delay_cursor = 0;
        std::uint64_t start_frame = 0;
        std::uint64_t stop_frame = 0;
        float gain = 0.0f;
        float pan = 0.5f;
        float damping = 0.99f;
    };

    void initialize_strings(std::uint32_t sample_rate) noexcept;
    std::array<float, 2> render_frame(std::uint32_t sample_rate) noexcept;

    std::array<StringVoice, kVoiceCount> voices_{};
    std::uint64_t frame_cursor_ = 0;
    std::uint32_t initialized_sample_rate_ = 0;
    std::uint32_t noise_state_ = 0xC001D00Du;
    std::array<float, 2> previous_amp_input_{};
    std::array<float, 2> dc_blocker_state_{};
    std::array<float, 2> cabinet_high_{};
    std::array<float, 2> cabinet_low_{};
    bool active_ = false;
};

void request_guitar_sting() noexcept;
void cancel_guitar_sting() noexcept;
bool mix_requested_guitar_sting(std::span<std::int16_t> interleaved_stereo,
                                std::uint32_t sample_rate) noexcept;

} // namespace rr64::achievement_audio
