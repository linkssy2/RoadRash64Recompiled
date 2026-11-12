#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rr64::audio {

// Keep enough queued audio to tolerate normal Windows host scheduling while
// preventing an abnormal producer burst from becoming audible input latency.
constexpr std::uint32_t maximum_queue_latency_frames(std::uint32_t sample_rate,
                                                     std::uint32_t host_period_frames) noexcept {
    const std::uint32_t one_hundred_fifty_ms =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(sample_rate) * 150u) / 1000u);
    const std::uint32_t four_host_periods = host_period_frames * 4u;
    return one_hundred_fifty_ms > four_host_periods ? one_hundred_fifty_ms : four_host_periods;
}

// A short producer burst should shed only the newest block. Clearing and
// restarting the whole SDL queue turns a harmless scheduling spike into an
// audible gap and can amplify a busy race into repeated stutter.
constexpr bool should_drop_overflow_submission(std::uint32_t queued_frames,
                                               std::uint32_t maximum_queue_frames) noexcept {
    return queued_frames > maximum_queue_frames;
}

struct QueueObservation {
    std::uint64_t sequence = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t host_period_frames = 0;
    std::uint32_t submitted_frames = 0;
    std::uint32_t queued_before_frames = 0;
    std::uint32_t queued_after_frames = 0;
    std::uint64_t gap_microseconds = 0;
    std::uint64_t expected_microseconds = 0;
    std::uint32_t discontinuity = 0;
    bool empty_before_submit = false;
    bool below_host_period_before_submit = false;
    bool late_submit = false;
};

struct QueueHealth {
    std::uint64_t submitted_buffers = 0;
    std::uint64_t empty_before_submit = 0;
    std::uint64_t below_host_period_before_submit = 0;
    std::uint64_t late_submits = 0;
    std::uint32_t minimum_queued_before_frames = 0;
    std::uint32_t maximum_queued_before_frames = 0;
    std::uint64_t maximum_gap_microseconds = 0;
    std::uint32_t maximum_discontinuity = 0;
};

class QueueMonitor {
  public:
    using Clock = std::chrono::steady_clock;

    void reset(std::uint32_t sample_rate, std::uint32_t host_period_frames) noexcept;
    QueueObservation observe(std::uint32_t queued_before_frames, std::uint32_t queued_after_frames,
                             std::span<const std::int16_t> interleaved_samples,
                             Clock::time_point now = Clock::now()) noexcept;
    QueueHealth health() const noexcept;

  private:
    std::uint32_t sample_rate_ = 0;
    std::uint32_t host_period_frames_ = 0;
    std::uint32_t previous_submitted_frames_ = 0;
    std::int16_t previous_left_ = 0;
    std::int16_t previous_right_ = 0;
    bool have_previous_samples_ = false;
    Clock::time_point previous_submit_{};
    QueueHealth health_{};
};

} // namespace rr64::audio
