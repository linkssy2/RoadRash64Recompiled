#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

#include "hle/rt64_rr64_authored_cadence.h"
#include "hle/rt64_rr64_frame_pacing.h"

namespace {
    using RT64::RR64FramePacing::AuthoredCadenceObservation;
    using RT64::RR64FramePacing::AuthoredCadenceTracker;
    constexpr uint64_t Ms = AuthoredCadenceTracker::MillisecondNs;

    uint32_t outputSlots(const AuthoredCadenceObservation &observation,
        bool interpolationCompatible = true)
    {
        using namespace RT64::RR64FramePacing;
        const uint32_t generatedCount = exactCadenceFrameCount(60u, observation.sourceRate);
        return presentationFrameCount(interpolationCompatible, generatedCount,
            60u, observation.sourceRate);
    }

    void require(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "RR64 authored cadence failure: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    void requireRate(const AuthoredCadenceObservation &observation,
        uint32_t rate, uint32_t reason, const char *message)
    {
        require((observation.sourceRate == rate) &&
            (observation.reason == reason), message);
    }

    AuthoredCadenceObservation advance(AuthoredCadenceTracker &tracker,
        uint64_t &timestamp, uint64_t interval, uint64_t epoch = 1u)
    {
        timestamp += interval;
        const auto observation = tracker.observe(timestamp, epoch, true);
        require(observation.intervalNs == interval,
            "ordinary observations preserve their actual parsed interval");
        return observation;
    }

    void verifyUniformSamples(uint64_t intervalNs, uint32_t expectedRate) {
        AuthoredCadenceTracker tracker;
        uint64_t timestamp = 0;
        requireRate(tracker.observe(timestamp, 1u, true), 0u, 0u,
            "the first sample does not invent an interval");
        std::vector<uint64_t> representedTimes;
        for (uint32_t sample = 1; sample <= 120; sample++) {
            const uint64_t previous = timestamp;
            const auto observation = advance(tracker, timestamp, intervalNs);
            if (sample < 3u) {
                requireRate(observation, 0u, 0u,
                    "two intervals do not authorize a source rate");
                continue;
            }
            require(observation.sourceRate == expectedRate,
                "a constant actual source stream converges to its rate");
            const uint32_t imageCount = outputSlots(observation);
            for (uint32_t image = 1; image <= imageCount; image++) {
                representedTimes.push_back(previous +
                    (intervalNs * image) / imageCount);
            }
        }
        const uint32_t imagesPerSample = expectedRate == 30u ? 2u : 1u;
        require(representedTimes.size() == 118u * imagesPerSample,
            "60 Hz samples are not doubled and 30 Hz samples supply two images");
        const uint64_t expectedSpacing = intervalNs / imagesPerSample;
        for (std::size_t index = 1; index < representedTimes.size(); index++) {
            require(representedTimes[index] > representedTimes[index - 1u],
                "represented source time progresses without repeated endpoints");
            require(representedTimes[index] - representedTimes[index - 1u] ==
                expectedSpacing,
                "consecutive samples have uniform source-time spacing");
        }
    }

    void verifyRecordedThroughputShape() {
        // The audit observed 600 authored workloads per ten-second window.
        // Exercise production cadence/count helpers with that timing shape;
        // this fixture does not emulate the real GPU queue or prove pose motion.
        for (const uint64_t interval : {16'666'666u, 33'333'332u}) {
            AuthoredCadenceTracker tracker;
            uint32_t slots = 0;
            uint32_t fallbackSlots = 0;
            for (uint64_t sample = 0; sample < 600u; ++sample) {
                const auto observation = tracker.observe(sample * interval, 1u, true);
                slots += outputSlots(observation);
                fallbackSlots += outputSlots(observation, false);
            }
            const uint32_t expected = interval == 16'666'666u ? 600u : 1197u;
            require(slots == expected && fallbackSlots == expected,
                "600 tasks preserve native60 throughput or confirmed30 output including warmup");
        }

        AuthoredCadenceTracker tracker;
        uint64_t timestamp = 0;
        require(outputSlots(tracker.observe(timestamp, 1u, true)) == 1u,
            "warmup has one native output slot");
        for (unsigned sample = 0; sample < 3; ++sample) {
            const auto slow = advance(tracker, timestamp, 33'333'332u);
            require(outputSlots(slow) == (sample == 2 ? 2u : 1u),
                "30 Hz expansion begins only when the current slow sequence confirms");
        }
        require(outputSlots(advance(tracker, timestamp, 16'666'666u), false) == 1u,
            "the first fast transition drops the old extra slot even during geometry fallback");
        require(outputSlots(advance(tracker, timestamp, 25u * Ms), false) == 1u,
            "uncertain timing and fallback cannot retain the old30Hz repetition");
    }
}

int main() {
    verifyUniformSamples(16'666'666u, 60u);
    verifyUniformSamples(33'333'332u, 30u);
    verifyRecordedThroughputShape();

    AuthoredCadenceTracker tracker;
    uint64_t timestamp = 500u * Ms;
    requireRate(tracker.observe(timestamp, 1u, true), 0u, 0u,
        "startup is native until cadence is proven");

    const std::array<uint64_t, 6> slowJitter{28u, 38u, 31u, 35u, 29u, 37u};
    for (std::size_t index = 0; index < slowJitter.size(); index++) {
        const auto observation = advance(tracker, timestamp, slowJitter[index] * Ms);
        requireRate(observation, index < 2u ? 0u : 30u,
            index < 2u ? 0u : 2u,
            "three consecutive in-range slow intervals authorize30Hz");
    }

    requireRate(advance(tracker, timestamp, 16u * Ms), 0u, 0u,
        "the first fast sample after30Hz must not be doubled");
    requireRate(advance(tracker, timestamp, 21u * Ms), 0u, 0u,
        "fast cadence still warms up without retaining old30Hz");
    requireRate(advance(tracker, timestamp, 13u * Ms), 60u, 1u,
        "three fast intervals establish60Hz including its boundaries");
    requireRate(advance(tracker, timestamp, 18u * Ms), 60u, 1u,
        "ordinary60Hz jitter keeps one image per workload");

    requireRate(advance(tracker, timestamp, 33u * Ms), 0u, 0u,
        "one delayed60Hz workload does not become30Hz");
    requireRate(advance(tracker, timestamp, 32u * Ms), 0u, 0u,
        "two slow intervals do not yet authorize expansion");
    requireRate(advance(tracker, timestamp, 34u * Ms), 30u, 2u,
        "a sustained60-to30 transition is recognized");

    // Every interval outside both accepted bands immediately revokes a proven
    // slow rate. The next slow run must start over, including burst intervals.
    const std::array<uint64_t, 7> uncertainMs{1u, 12u, 22u, 27u, 39u, 67u, 100u};
    for (uint64_t uncertain : uncertainMs) {
        requireRate(advance(tracker, timestamp, uncertain * Ms), 0u, 0u,
            "ambiguous timing never holds an older30Hz classification");
        requireRate(advance(tracker, timestamp, 33u * Ms), 0u, 0u,
            "slow confirmation restarts after ambiguous timing");
        requireRate(advance(tracker, timestamp, 33u * Ms), 0u, 0u,
            "a second interval remains unproven after an interruption");
        requireRate(advance(tracker, timestamp, 33u * Ms), 30u, 2u,
            "a fresh complete slow run can recover");
    }

    // A late workload followed by a queued burst must never form a synthetic
    //30Hz lock; neither elapsed total time nor old state grants expansion.
    for (uint32_t index = 0; index < 20u; index++) {
        requireRate(advance(tracker, timestamp, 1u * Ms), 0u, 0u,
            "queued burst clears slow permission");
        requireRate(advance(tracker, timestamp, 33u * Ms), 0u, 0u,
            "isolated slow intervals separated by bursts never confirm30Hz");
    }

    requireRate(advance(tracker, timestamp, 101u * Ms), 0u, 3u,
        "a long pause is a discontinuity rather than a low source rate");
    requireRate(advance(tracker, timestamp, 33u * Ms), 0u, 0u,
        "the first interval after a pause starts fresh");
    requireRate(tracker.observe(timestamp, 1u, true), 0u, 3u,
        "equal timestamps cannot produce a zero-time source sample");
    requireRate(tracker.observe(timestamp - 1u, 1u, true), 0u, 3u,
        "backward timestamps reset without unsigned underflow");
    timestamp += 17u * Ms;
    requireRate(tracker.observe(timestamp, 2u, true), 0u, 3u,
        "a new scene never inherits previous cadence");
    requireRate(advance(tracker, timestamp, 33u * Ms, 2u), 0u, 0u,
        "new-scene timing requires its own confirmation");
    requireRate(tracker.observe(timestamp, 2u, false), 0u, 3u,
        "leaving the race clears all timing evidence");
    requireRate(tracker.observe(timestamp, 2u, true), 0u, 0u,
        "race re-entry cannot reuse an earlier interval");
    requireRate(tracker.observe(timestamp, 0u, true), 0u, 3u,
        "an invalid scene cannot authorize interpolation");

    tracker.reset();
    requireRate(tracker.observe(std::numeric_limits<uint64_t>::max(), 4u, true),
        0u, 0u, "large timestamps need no signed conversion");
    requireRate(tracker.observe(0u, 4u, true), 0u, 3u,
        "timestamp wrap is a discontinuity");
    requireRate(tracker.observe(16u * Ms, 4u, true), 0u, 0u,
        "a wrapped clock begins a new run without timing debt");

    std::cout << "RR64 authored cadence smoke tests passed\n";
    return EXIT_SUCCESS;
}
