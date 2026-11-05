// Presentation cadence measured at the parsed authored-workload boundary.
// This never changes guest simulation time or infers duplicate visual content.
#pragma once

#include <atomic>
#include <cstdint>

namespace RT64::RR64FramePacing {
    // The game's paced simulation rate (10-59 Hz), or zero when the game is
    // not pacing a fixed step. Arrival intervals near 1/rate are then that
    // rate rather than the nearest stock cadence.
    inline std::atomic<uint32_t> pacedSourceRate{0u};

    struct AuthoredCadenceObservation {
        // Zero is deliberately unproven: callers render one native image.
        uint32_t sourceRate = 0;
        // Parsed arrival cadence, not proof of distinct poses:
        // 0: warm-up/ambiguous; 1: observed60; 2: observed30; 3: discontinuity;
        // 4: observed24; 5: observed20; 6: game-reported paced rate.
        uint32_t reason = 0;
        uint64_t intervalNs = 0;
    };

    class AuthoredCadenceTracker {
    public:
        static constexpr uint64_t MillisecondNs = 1'000'000u;
        static constexpr uint64_t Native60MinimumNs = 13u * MillisecondNs;
        static constexpr uint64_t Native60MaximumNs = 21u * MillisecondNs;
        static constexpr uint64_t Interpolate30MinimumNs = 28u * MillisecondNs;
        static constexpr uint64_t Interpolate30MaximumNs = 38u * MillisecondNs;
        // The original game's own step: a fixed 1/24 s simulation paced to real
        // time authors every 41.67 ms, presented at 60 as 3, 2, 3, 2 slots.
        static constexpr uint64_t Interpolate24MinimumNs = 39u * MillisecondNs;
        static constexpr uint64_t Interpolate24MaximumNs = 46u * MillisecondNs;
        // Stock frames take whole VI counts; three per update is 20 Hz.
        static constexpr uint64_t Interpolate20MinimumNs = 47u * MillisecondNs;
        static constexpr uint64_t Interpolate20MaximumNs = 56u * MillisecondNs;
        // Above the slowest paced step (10 Hz, 100 ms).
        static constexpr uint64_t MaximumContinuousIntervalNs = 125u * MillisecondNs;
        static constexpr uint32_t RequiredConsecutiveIntervals = 3u;

        constexpr void reset() {
            previousTimestampNs = 0;
            previousSceneEpoch = 0;
            consecutive60 = 0;
            consecutive30 = 0;
            consecutive24 = 0;
            consecutive20 = 0;
            consecutivePaced = 0;
            hasPrevious = false;
        }

        AuthoredCadenceObservation observe(uint64_t timestampNs,
            uint64_t sceneEpoch, bool raceActive)
        {
            if (!raceActive || (sceneEpoch == 0u)) {
                reset();
                return {0u, 3u, 0u};
            }

            if (!hasPrevious) {
                begin(timestampNs, sceneEpoch);
                return {};
            }

            if ((sceneEpoch != previousSceneEpoch) ||
                (timestampNs <= previousTimestampNs)) {
                begin(timestampNs, sceneEpoch);
                return {0u, 3u, 0u};
            }

            // Subtract only after the ordering check, so backward clocks cannot
            // wrap into an apparently valid interval.
            const uint64_t intervalNs = timestampNs - previousTimestampNs;
            previousTimestampNs = timestampNs;
            if (intervalNs > MaximumContinuousIntervalNs) {
                begin(timestampNs, sceneEpoch);
                return {0u, 3u, intervalNs};
            }

            const uint32_t paced = pacedSourceRate.load(std::memory_order_relaxed);
            if ((paced >= 10u) && (paced < 60u)) {
                // Within a quarter of the paced step: 4/5 to 5/4 of 1/rate.
                const uint64_t expectedNs = 1'000'000'000u / paced;
                if ((intervalNs * 5u >= expectedNs * 4u) && (intervalNs * 4u <= expectedNs * 5u)) {
                    consecutive60 = 0;
                    consecutive30 = 0;
                    consecutive24 = 0;
                    consecutive20 = 0;
                    if (consecutivePaced < RequiredConsecutiveIntervals) {
                        consecutivePaced++;
                    }
                    return consecutivePaced == RequiredConsecutiveIntervals ?
                        AuthoredCadenceObservation{paced, 6u, intervalNs} :
                        AuthoredCadenceObservation{0u, 0u, intervalNs};
                }
            }
            consecutivePaced = 0;

            if ((intervalNs >= Native60MinimumNs) &&
                (intervalNs <= Native60MaximumNs)) {
                // The first fast interval immediately revokes a previous30Hz
                // classification, even before the new60Hz run is confirmed.
                consecutive30 = 0;
                consecutive24 = 0;
                consecutive20 = 0;
                if (consecutive60 < RequiredConsecutiveIntervals) {
                    consecutive60++;
                }
                return consecutive60 == RequiredConsecutiveIntervals ?
                    AuthoredCadenceObservation{60u, 1u, intervalNs} :
                    AuthoredCadenceObservation{0u, 0u, intervalNs};
            }

            if ((intervalNs >= Interpolate30MinimumNs) &&
                (intervalNs <= Interpolate30MaximumNs)) {
                consecutive60 = 0;
                consecutive24 = 0;
                consecutive20 = 0;
                if (consecutive30 < RequiredConsecutiveIntervals) {
                    consecutive30++;
                }
                return consecutive30 == RequiredConsecutiveIntervals ?
                    AuthoredCadenceObservation{30u, 2u, intervalNs} :
                    AuthoredCadenceObservation{0u, 0u, intervalNs};
            }

            if ((intervalNs >= Interpolate24MinimumNs) &&
                (intervalNs <= Interpolate24MaximumNs)) {
                consecutive60 = 0;
                consecutive30 = 0;
                consecutive20 = 0;
                if (consecutive24 < RequiredConsecutiveIntervals) {
                    consecutive24++;
                }
                return consecutive24 == RequiredConsecutiveIntervals ?
                    AuthoredCadenceObservation{24u, 4u, intervalNs} :
                    AuthoredCadenceObservation{0u, 0u, intervalNs};
            }

            if ((intervalNs >= Interpolate20MinimumNs) &&
                (intervalNs <= Interpolate20MaximumNs)) {
                consecutive60 = 0;
                consecutive30 = 0;
                consecutive24 = 0;
                if (consecutive20 < RequiredConsecutiveIntervals) {
                    consecutive20++;
                }
                return consecutive20 == RequiredConsecutiveIntervals ?
                    AuthoredCadenceObservation{20u, 5u, intervalNs} :
                    AuthoredCadenceObservation{0u, 0u, intervalNs};
            }

            // Short bursts, uncertain rates, and isolated stalls never retain
            // permission to expand a workload into multiple presentation slots.
            consecutive60 = 0;
            consecutive30 = 0;
            consecutive24 = 0;
            consecutive20 = 0;
            return {0u, 0u, intervalNs};
        }

    private:
        constexpr void begin(uint64_t timestampNs, uint64_t sceneEpoch) {
            previousTimestampNs = timestampNs;
            previousSceneEpoch = sceneEpoch;
            consecutive60 = 0;
            consecutive30 = 0;
            consecutive24 = 0;
            consecutive20 = 0;
            consecutivePaced = 0;
            hasPrevious = true;
        }

        uint64_t previousTimestampNs = 0;
        uint64_t previousSceneEpoch = 0;
        uint32_t consecutive60 = 0;
        uint32_t consecutive30 = 0;
        uint32_t consecutive24 = 0;
        uint32_t consecutive20 = 0;
        uint32_t consecutivePaced = 0;
        bool hasPrevious = false;
    };
}
