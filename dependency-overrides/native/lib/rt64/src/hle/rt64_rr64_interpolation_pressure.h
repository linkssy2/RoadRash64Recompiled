// Keep native pictures while avoiding optional work already overtaken in the
// same VI interval. This policy never changes the guest clock or source rate.
#pragma once

#include "rt64_rr64_authored_cadence.h"
#include "rt64_rr64_frame_metadata.h"

namespace RT64::RR64FramePacing {
    inline bool renderRaceAtNativeLimit(bool canRetainNativeImage,
        bool hasPresentationTarget, uint32_t targetRate, uint32_t sourceRate)
    {
        // At the console-rate cap, delayed race work must not start extra
        // matching work and prolong the slowdown. Keep the measured source
        // rate: genuine 30 Hz content remains native here, not relabelled 60.
        // Eligibility already excludes non-race, paused, debug and RT work.
        // A paced simulation step (the game-reported rate, or a measured 24
        // or 20 Hz) needs fractional pictures to reach the console rate.
        const uint32_t paced = pacedSourceRate.load(std::memory_order_relaxed);
        const bool pacedSource = sourceRate != 0u &&
            (sourceRate == paced || sourceRate == 24u || sourceRate == 20u);
        return canRetainNativeImage && hasPresentationTarget &&
            targetRate > 0 && targetRate <= ConsoleRefreshRate && !pacedSource;
    }

    struct QueuedAuthoredWorkload {
        uint64_t writer = 0;
        uint64_t presentId = 0;
        SceneSnapshot scene;
    };

    constexpr bool renderSupersededBatchNatively(bool canRetainNativeImage,
        bool hasPresentationTarget, bool interpolationRequested,
        QueuedAuthoredWorkload current, QueuedAuthoredWorkload queued)
    {
        // The next VI interval can legitimately be queued early and still
        // needs this interval's intermediate pictures. A different scene or an
        // invalid/non-newer writer is not evidence that this work is obsolete.
        return canRetainNativeImage && hasPresentationTarget &&
            interpolationRequested && current.writer != 0 &&
            queued.writer > current.writer && current.scene.epoch != 0 &&
            current.scene.raceActive && current.scene == queued.scene &&
            queuedWorkloadSupersedesBatch(true, current.presentId, queued.presentId);
    }
}
