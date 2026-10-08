// Keep native pictures while avoiding optional work already overtaken in the
// same VI interval. This policy never changes the guest clock or source rate.
#pragma once

#include "rt64_rr64_frame_metadata.h"

namespace RT64::RR64FramePacing {
    constexpr bool renderRaceAtNativeLimit(bool canRetainNativeImage,
        bool hasPresentationTarget, uint32_t targetRate)
    {
        // At the console-rate cap, delayed race work must not start extra
        // matching work and prolong the slowdown. Keep the measured source
        // rate: genuine 30 Hz content remains native here, not relabelled 60.
        // Eligibility already excludes non-race, paused, debug and RT work.
        return canRetainNativeImage && hasPresentationTarget &&
            targetRate > 0 && targetRate <= ConsoleRefreshRate;
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
