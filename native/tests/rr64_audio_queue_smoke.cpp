#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "rr64_audio_output.hpp"
#include "ultramodern/audio_policy.hpp"

namespace {
bool check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[RR64-AUDIO-TEST] FAILED: %s\n", message);
    }
    return condition;
}
} // namespace

int main() {
    bool passed = true;
    passed &= check(rr64::audio::maximum_queue_latency_frames(44100u, 1024u) == 6615u,
        "44.1 kHz latency ceiling is 150 ms");
    passed &= check(rr64::audio::maximum_queue_latency_frames(32000u, 2048u) == 8192u,
        "large host periods determine the safe latency ceiling");
    passed &= check(!rr64::audio::should_drop_overflow_submission(6615u, 6615u),
        "audio at the latency ceiling is retained");
    passed &= check(rr64::audio::should_drop_overflow_submission(6616u, 6615u),
        "only a new block beyond the latency ceiling is shed");
    passed &= check(ultramodern::audio_reserve_frames(44100u) == 2205u, "three-interval 44.1 kHz reserve");
    passed &= check(ultramodern::audio_reserve_frames(44100u) > 2048u, "reserve exceeds two observed WASAPI periods");
    passed &= check(ultramodern::audio_startup_prebuffer_frames(44100u, 1024u) == 2205u,
        "startup prebuffer covers reserve and two host periods");
    passed &= check(ultramodern::audio_startup_prebuffer_frames(32000u, 2048u) == 4096u,
        "large host periods determine startup prebuffer");
    passed &= check(ultramodern::audio_remaining_bytes_for_game(2205u, 44100u) == 0u, "reserve is hidden from guest");
    passed &= check(ultramodern::audio_remaining_bytes_for_game(2941u, 44100u) == 2944u,
        "one 736-frame source block remains reportable");

    rr64::audio::QueueMonitor monitor{};
    monitor.reset(44100u, 1024u);
    const std::array<std::int16_t, 4> first = {10, -10, 20, -20};
    const std::array<std::int16_t, 4> second = {25, -25, 30, -30};
    const auto start = rr64::audio::QueueMonitor::Clock::time_point{} + std::chrono::seconds(1);
    const auto observation_1 = monitor.observe(0u, 2u, first, start);
    const auto observation_2 = monitor.observe(0u, 2u, second, start + std::chrono::milliseconds(50));
    const auto health = monitor.health();
    passed &= check(!observation_1.empty_before_submit, "initial empty queue is not an underrun");
    passed &= check(observation_2.empty_before_submit, "later empty queue is detected");
    passed &= check(observation_2.late_submit, "late producer submission is detected");
    passed &= check(observation_2.discontinuity == 5u, "stereo boundary discontinuity");
    passed &= check(health.empty_before_submit == 1u && health.late_submits == 1u, "health counters");

    if (passed) {
        std::puts("[RR64-AUDIO-TEST] queue policy and telemetry passed.");
        return 0;
    }
    return 1;
}
