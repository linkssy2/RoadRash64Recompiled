#include "rr64_audio_output.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace rr64::audio {

void QueueMonitor::reset(std::uint32_t sample_rate, std::uint32_t host_period_frames) noexcept {
    sample_rate_ = sample_rate;
    host_period_frames_ = host_period_frames;
    previous_submitted_frames_ = 0;
    previous_left_ = 0;
    previous_right_ = 0;
    have_previous_samples_ = false;
    previous_submit_ = {};
    health_ = {};
    health_.minimum_queued_before_frames = std::numeric_limits<std::uint32_t>::max();
}

QueueObservation QueueMonitor::observe(std::uint32_t queued_before_frames,
                                       std::uint32_t queued_after_frames,
                                       std::span<const std::int16_t> samples,
                                       Clock::time_point now) noexcept {
    QueueObservation observation{};
    observation.sequence = ++health_.submitted_buffers;
    observation.sample_rate = sample_rate_;
    observation.host_period_frames = host_period_frames_;
    observation.submitted_frames = static_cast<std::uint32_t>(samples.size() / 2u);
    observation.queued_before_frames = queued_before_frames;
    observation.queued_after_frames = queued_after_frames;
    observation.empty_before_submit = observation.sequence > 1u && queued_before_frames == 0u;
    observation.below_host_period_before_submit =
        observation.sequence > 1u && queued_before_frames < host_period_frames_;

    if (previous_submit_ != Clock::time_point{} && sample_rate_ != 0u) {
        observation.gap_microseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - previous_submit_).count());
        observation.expected_microseconds =
            static_cast<std::uint64_t>(previous_submitted_frames_) * 1000000ull / sample_rate_;
        const std::uint64_t host_period_microseconds =
            static_cast<std::uint64_t>(host_period_frames_) * 1000000ull / sample_rate_;
        observation.late_submit = observation.gap_microseconds >
                                  observation.expected_microseconds + host_period_microseconds;
    }

    if (have_previous_samples_ && samples.size() >= 2u) {
        const std::uint32_t left_delta = static_cast<std::uint32_t>(
            std::abs(static_cast<int>(samples[0]) - static_cast<int>(previous_left_)));
        const std::uint32_t right_delta = static_cast<std::uint32_t>(
            std::abs(static_cast<int>(samples[1]) - static_cast<int>(previous_right_)));
        observation.discontinuity = std::max(left_delta, right_delta);
    }

    if (samples.size() >= 2u) {
        previous_left_ = samples[samples.size() - 2u];
        previous_right_ = samples[samples.size() - 1u];
        have_previous_samples_ = true;
    }
    previous_submitted_frames_ = observation.submitted_frames;
    previous_submit_ = now;

    health_.empty_before_submit += observation.empty_before_submit ? 1u : 0u;
    health_.below_host_period_before_submit +=
        observation.below_host_period_before_submit ? 1u : 0u;
    health_.late_submits += observation.late_submit ? 1u : 0u;
    health_.minimum_queued_before_frames =
        std::min(health_.minimum_queued_before_frames, queued_before_frames);
    health_.maximum_queued_before_frames =
        std::max(health_.maximum_queued_before_frames, queued_before_frames);
    health_.maximum_gap_microseconds =
        std::max(health_.maximum_gap_microseconds, observation.gap_microseconds);
    health_.maximum_discontinuity =
        std::max(health_.maximum_discontinuity, observation.discontinuity);

    return observation;
}

QueueHealth QueueMonitor::health() const noexcept {
    QueueHealth result = health_;
    if (result.submitted_buffers == 0u) {
        result.minimum_queued_before_frames = 0u;
    }
    return result;
}

} // namespace rr64::audio
