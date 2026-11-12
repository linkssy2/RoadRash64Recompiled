#include <cstdlib>
#include <iostream>
#include <limits>

#include "hle/rt64_rr64_authored_frame.h"

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "RR64 authored frame failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using namespace RT64::RR64FramePacing;
    const SceneSnapshot race{7u, true};
    const SceneSnapshot nextRace{8u, true};
    const PresentationTargetIdentity targetA{0x00400000u, 320u, 2u};
    const PresentationTargetIdentity targetB{0x004C0000u, 320u, 2u};
    AuthoredTargetTracker tracker;
    require(tracker.write(targetA, 240u, 401u, race, 30u) &&
        tracker.write(targetB, 240u, 402u, race, 30u), "independent buffers retain their writers");
    const AuthoredTargetStamp requestA = tracker.find(targetA, race);
    const InterpolationBatchMetadata batchA{401u, race, targetA, 30u, 60u};
    const InterpolationBatchMetadata batchB{402u, race, targetB, 30u, 60u};
    require(ownedBatchRequestMatches(batchA, requestA, 402u, 30u, 60u),
        "the VI can select A401 after the queue finishes B402");
    require(!ownedBatchRequestMatches(batchB, requestA, 402u, 30u, 60u),
        "the latest global workload does not own a different selected buffer");
    require(!ownedBatchRequestMatches(batchA, requestA, 400u, 30u, 60u),
        "a target writer beyond the completed watermark is not ready");
    auto wrongWriter = batchA;
    wrongWriter.workloadId = 400u;
    require(!ownedBatchRequestMatches(wrongWriter, requestA, 402u, 30u, 60u),
        "being older than the watermark cannot substitute for exact writer identity");
    require(!ownedBatchRequestMatches(batchA, requestA, 402u, 60u, 60u) &&
        !ownedBatchRequestMatches(batchA, requestA, 402u, 30u, 120u),
        "source and destination rates remain exact");

    // Timing follows the writer of the selected target, not whichever other
    // target was most recently parsed or whichever cadence is now current.
    AuthoredTargetTracker timedTracker;
    require(timedTracker.write(targetA, 240u, 701u, race, 30u,
        1'000'000'000u, 33'333'332u) &&
        timedTracker.write(targetB, 240u, 702u, race, 60u,
        1'016'666'666u, 16'666'666u), "each target stores its writer timing");
    const auto slowStamp = timedTracker.find(targetA, race);
    const auto fastStamp = timedTracker.find(targetB, race);
    require(slowStamp.sourceRate == 30u &&
        slowStamp.authoredTimestampNs == 1'000'000'000u &&
        slowStamp.authoredIntervalNs == 33'333'332u &&
        fastStamp.sourceRate == 60u &&
        fastStamp.authoredTimestampNs == 1'016'666'666u &&
        fastStamp.authoredIntervalNs == 16'666'666u,
        "alternating buffers retain distinct timestamps, intervals and source rates");
    const InterpolationBatchMetadata slowBatch{701u, race, targetA, 30u, 60u};
    const InterpolationBatchMetadata fastBatch{702u, race, targetB, 60u, 60u};
    require(ownedBatchRequestMatches(slowBatch, slowStamp, 702u,
        slowStamp.sourceRate, 60u) &&
        !ownedBatchRequestMatches(slowBatch, slowStamp, 702u,
            fastStamp.sourceRate, 60u),
        "a slow buffer selected after a fast writer uses its own source rate");
    auto wronglyRelabeledSlowBatch = slowBatch;
    wronglyRelabeledSlowBatch.sourceRate = 60u;
    require(!ownedBatchRequestMatches(wronglyRelabeledSlowBatch, slowStamp,
        702u, 60u, 60u),
        "matching explicit and batch rates cannot override the selected writer's source rate");
    require(ownedBatchRequestMatches(fastBatch, fastStamp, 702u,
        fastStamp.sourceRate, 60u) &&
        !ownedBatchRequestMatches(fastBatch, fastStamp, 702u,
            slowStamp.sourceRate, 60u),
        "a fast buffer cannot inherit the older target's slow cadence");
    require(presentationFrameCount(true, 2u, 60u, slowStamp.sourceRate) == 2u &&
        presentationFrameCount(true, 1u, 60u, fastStamp.sourceRate) == 1u &&
        presentationFrameCount(false, 0u, 60u, fastStamp.sourceRate) == 1u,
        "source selection controls generated and native output counts together");
    require(timedTracker.write(targetA, 16u, 703u, race, 0u,
        1'041'666'666u, 25'000'000u), "an uncertain partial writer is still identified");
    const auto uncertainStamp = timedTracker.find(targetA, race);
    require(uncertainStamp.valid() && uncertainStamp.sourceRate == 0u &&
        uncertainStamp.authoredTimestampNs == 1'041'666'666u &&
        uncertainStamp.authoredIntervalNs == 25'000'000u &&
        presentationFrameCount(false, 0u, 60u, uncertainStamp.sourceRate) == 1u,
        "an unresolved new writer replaces timing and uses one native output");
    require(slowStamp.sourceRate == 30u &&
        slowStamp.authoredTimestampNs == 1'000'000'000u &&
        slowStamp.authoredIntervalNs == 33'333'332u,
        "a captured request retains its timing after the target is rewritten");
    timedTracker.invalidateRange(targetA.address + 200u * 640u, 1u);
    require(!timedTracker.find(targetA, race).valid() &&
        timedTracker.find(targetA, race).sourceRate == 0u &&
        timedTracker.find(targetA, race).authoredTimestampNs == 0u &&
        timedTracker.find(targetA, race).authoredIntervalNs == 0u,
        "alias invalidation removes timing as well as target ownership");

    // Reproduce the audit's throughput using production helpers: 600 parsed
    // samples at 60 Hz, alternating target ownership and a newer watermark.
    // This does not simulate the GPU/VI queue or establish distinct visual poses.
    AuthoredCadenceTracker cadence;
    AuthoredTargetTracker streamTargets;
    uint32_t outputSlots = 0;
    for (uint64_t sample = 1; sample <= 600u; ++sample) {
        const auto target = (sample & 1u) ? targetA : targetB;
        const uint64_t timestamp = sample * 16'666'666u;
        const auto observation = cadence.observe(timestamp, race.epoch, true);
        require(streamTargets.write(target, 240u, sample, race,
            observation.sourceRate, timestamp, observation.intervalNs),
            "the cadence fixture must create an authored target for each workload");
        const auto stamp = streamTargets.find(target, race);
        const InterpolationBatchMetadata batch{sample, race, target, stamp.sourceRate, 60u};
        require(ownedBatchRequestMatches(batch, stamp, sample + 1u,
            stamp.sourceRate, 60u), "a later watermark does not change exact source identity");
        outputSlots += presentationFrameCount(false, 0u, 60u, stamp.sourceRate);
    }
    require(outputSlots == 600u,
        "600 authored 60 Hz workloads require 600 output slots, not 1200 then alternate skipping");

    require(tracker.write(targetA, 16u, 403u, race), "partial updates are authored writes");
    require(!ownedBatchRequestMatches(batchA, tracker.find(targetA, race), 403u, 30u, 60u) &&
        tracker.find(targetB, race).writerWorkloadId == 402u,
        "a later partial A write invalidates A401 without invalidating B402");
    require(ownedBatchRequestMatches(batchA, requestA, 402u, 30u, 60u),
        "a captured VI request is an immutable value, not a live ledger alias");
    tracker.invalidateRange(targetA.address + 200u * 640u, 1u);
    require(!tracker.find(targetA, race).valid(),
        "partial writes retain the image extent for later lower-row alias invalidation");

    tracker.write(targetA, 240u, 404u, race);
    const PresentationTargetIdentity alias{targetA.address + 640u, 320u, 2u};
    tracker.write(alias, 1u, 405u, race);
    require(!tracker.find(targetA, race).valid() && tracker.find(alias, race).valid(),
        "an overlapping color write replaces the prior ownership certificate");
    tracker.invalidateRange(alias.address, 640u);
    require(!tracker.find(alias, race).valid(), "CPU or depth writes invalidate the same byte range");
    tracker.write(targetA, 240u, 406u, race);
    tracker.invalidateRange(targetA.address + 240u * 640u, 640u);
    require(tracker.find(targetA, race).valid(), "adjacent intervals do not overlap");
    tracker.invalidateRange(targetA.address, 0u);
    require(tracker.find(targetA, race).valid(), "an empty write does not alter ownership");
    tracker.invalidateAddress(targetA.address);
    require(!tracker.find(targetA, race).valid(), "an address discard invalidates its certificate");

    tracker.write(targetA, 240u, 407u, race);
    const PresentationTargetIdentity wideA{targetA.address, 640u, 2u};
    tracker.write(wideA, 240u, 408u, race);
    require(!tracker.find(targetA, race).valid() && tracker.find(wideA, race).valid(),
        "new dimensions cannot inherit the prior exact target identity");
    const PresentationTargetIdentity rgba32A{targetA.address, 640u, 3u};
    tracker.write(rgba32A, 240u, 409u, race);
    require(!tracker.find(wideA, race).valid() && tracker.find(rgba32A, race).valid(),
        "pixel-size changes replace the old identity");
    require(!tracker.find(rgba32A, nextRace).valid() &&
        !tracker.find(rgba32A, {race.epoch, false}).valid(),
        "both scene epoch and race state belong to authored identity");
    tracker.write(rgba32A, 240u, 410u, nextRace);
    require(!tracker.find(rgba32A, race).valid() && tracker.find(rgba32A, nextRace).valid(),
        "a new scene cannot reuse a prior-scene certificate at the same address");
    tracker.clear();
    require(!tracker.find(rgba32A, nextRace).valid() && !tracker.find(targetB, race).valid(),
        "reset removes every tracked resource");

    for (uint8_t siz = 0; siz <= 3u; ++siz) {
        const PresentationTargetIdentity packed{0x1000u, 3u, siz};
        const uint64_t rowBytes = ((uint64_t{3} << siz) + 1u) / 2u;
        tracker.write(packed, 2u, 500u + siz, race);
        tracker.invalidateRange(packed.address + uint32_t(rowBytes * 2u), 1u);
        require(tracker.find(packed, race).valid(), "pixel-size byte extents stop at their exclusive end");
        tracker.invalidateRange(packed.address + uint32_t(rowBytes * 2u - 1u), 1u);
        require(!tracker.find(packed, race).valid(), "the last covered byte participates in alias checks");
    }
    const PresentationTargetIdentity highTarget{0xFFFFFFF0u, 4u, 2u};
    tracker.write(targetA, 240u, 600u, race);
    require(tracker.write(highTarget, 1u, 601u, race), "a representable high address is bounded correctly");
    tracker.invalidateRange(highTarget.address, std::numeric_limits<uint64_t>::max());
    require(!tracker.find(highTarget, race).valid() && tracker.find(targetA, race).valid(),
        "oversized invalidation saturates instead of wrapping into low memory");
    require(!tracker.write({0xFFFFFFF0u, 16u, 3u}, 1u, 602u, race) &&
        !tracker.find(targetA, race).valid(), "a write overflowing the address space fails closed");
    tracker.write(targetA, 240u, 603u, race);
    require(!tracker.write(targetA, 240u, 0u, race) && !tracker.find(targetA, race).valid(),
        "an unidentified writer invalidates known pixels without minting a certificate");
    tracker.write(targetA, 240u, 604u, race);
    require(!tracker.write(targetA, 240u, 605u, {}) && !tracker.find(targetA, race).valid(),
        "an uninitialized scene cannot preserve or create authored identity");
    require(!AuthoredTargetStamp{}.valid() &&
        !AuthoredTargetStamp{{0x1000u, 320u, 4u}, 1u, race}.valid() &&
        !ownedBatchRequestMatches({}, {}, 0u, 0u, 0u), "empty or invalid requests never match");

    tracker.clear();
    for (std::size_t i = 0; i <= AuthoredTargetTracker::MaximumEntries; ++i) {
        tracker.write({uint32_t(0x1000u + i * 0x100u), 8u, 2u}, 1u, 700u + i, race);
    }
    require(!tracker.find({0x1000u, 8u, 2u}, race).valid(), "capacity overflow evicts bounded history");
    std::size_t retained = 0;
    for (std::size_t i = 0; i <= AuthoredTargetTracker::MaximumEntries; ++i) {
        retained += tracker.find({uint32_t(0x1000u + i * 0x100u), 8u, 2u}, race).valid() ? 1u : 0u;
    }
    require(retained == AuthoredTargetTracker::MaximumEntries, "the ledger holds at most sixteen intervals");
    tracker.clear();
    require(tracker.write(targetA, 240u, 999u, race) && tracker.find(targetA, race).valid(),
        "the ledger can be warmed up after eviction and reset");
    std::cout << "RR64 authored frame passed: exact selected-target writers, queue watermarks, "
        "partial writes, aliases, lifecycle changes, bounded capacity, and overflow. "
        "GPU readiness and complete composition are checked by the batch owner.\n";
}
