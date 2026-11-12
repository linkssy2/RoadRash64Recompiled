#include <cstdlib>
#include <initializer_list>
#include <iostream>

#include "hle/rt64_rr64_frame_metadata.h"
#include "../src/rr64_engine_layout.hpp"

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "RR64 frame metadata failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using namespace RT64::RR64FramePacing;
    // Reproduce live -> finish handoff -> orbit -> Continue for each family.
    for(unsigned live : {0x0Au,0x13u,0x18u,0x1Du}) {
        SceneTracker finishTracker;
        const auto driving=finishTracker.observe(rr64::engine::is_race_shortcut_scene_transition(live,live));
        const auto crossing=finishTracker.observe(rr64::engine::is_race_shortcut_scene_transition(live,live+1));
        const auto orbit=finishTracker.observe(rr64::engine::is_race_shortcut_scene_transition(live+1,live+1));
        require(driving.raceActive && driving==crossing && driving==orbit,
            "finish camera retains authored cadence epoch and target ownership");
        const auto menu=finishTracker.observe(rr64::engine::is_race_shortcut_scene_transition(live+1,0x20u));
        require(!menu.raceActive && menu.epoch>orbit.epoch,"Continue invalidates retained scene");
        require(!rr64::engine::is_live_race_transition(live+1,live+1),
            "render-only finish eligibility does not mark gameplay active");
    }
    const PresentationTargetIdentity targetA{0x00100000u, 320u, 2u};
    const PresentationTargetIdentity targetB{0x00125800u, 320u, 2u};
    const PresentationTargetIdentity otherWidth{targetA.address, 640u, 2u};
    const PresentationTargetIdentity otherSize{targetA.address, 320u, 3u};

    SceneTracker tracker;
    const SceneSnapshot initialMenu = tracker.observe(false);
    require(initialMenu.epoch != 0 && !initialMenu.raceActive,
        "the first scene has a distinct initialized epoch");
    require(tracker.observe(false) == initialMenu,
        "successive observations do not reset the same scene");
    const SceneSnapshot firstRace = tracker.observe(true);
    const InterpolationBatchMetadata batch{17u, firstRace, targetA, 30u, 60u};
    require(batch.matches(17u, firstRace, targetA, 30u, 60u),
        "the matching workload and target can consume its generated images");
    require(!batch.matches(18u, firstRace, targetA, 30u, 60u),
        "another workload cannot consume an older batch");
    require(!batch.matches(17u, firstRace, targetB, 30u, 60u) &&
        !batch.matches(17u, firstRace, otherWidth, 30u, 60u) &&
        !batch.matches(17u, firstRace, otherSize, 30u, 60u),
        "address, width and pixel size all belong to the batch identity");
    require(!batch.matches(17u, firstRace, targetA, 60u, 60u) &&
        !batch.matches(17u, firstRace, targetA, 30u, 120u),
        "source or host rate changes cannot relabel a prepared batch");

    const SceneSnapshot otherScene{firstRace.epoch + 1u, true};
    require(batch.rejectionReasons(17u, firstRace, targetA, 30u, 60u) == 0u,
        "an exact batch has no rejection reason");
    require(batch.rejectionReasons(18u, firstRace, targetA, 30u, 60u) == 1u,
        "a workload mismatch reports only the workload reason");
    require(batch.rejectionReasons(17u, otherScene, targetA, 30u, 60u) == 2u &&
        batch.rejectionReasons(17u, {firstRace.epoch, false}, targetA, 30u, 60u) == 2u,
        "scene epoch and race-state mismatches report the scene reason");
    require(batch.rejectionReasons(17u, firstRace, targetB, 30u, 60u) == 4u &&
        batch.rejectionReasons(17u, firstRace, otherWidth, 30u, 60u) == 4u &&
        batch.rejectionReasons(17u, firstRace, otherSize, 30u, 60u) == 4u,
        "address, width and pixel-size mismatches report the target reason");
    require(batch.rejectionReasons(17u, firstRace, targetA, 60u, 60u) == 8u,
        "a source-rate mismatch reports only the source reason");
    require(batch.rejectionReasons(17u, firstRace, targetA, 30u, 120u) == 16u,
        "a host-rate mismatch reports only the host reason");
    require(batch.rejectionReasons(18u, otherScene, targetB, 60u, 120u) == 31u,
        "simultaneous mismatches retain every rejection reason");
    auto invalidBatch = batch;
    invalidBatch.workloadId = 0;
    require(invalidBatch.rejectionReasons(0u, firstRace, targetA, 30u, 60u) == 1u &&
        !invalidBatch.matches(0u, firstRace, targetA, 30u, 60u),
        "an uninitialized workload cannot match merely because both ids are zero");
    invalidBatch = batch;
    invalidBatch.target = {};
    require(invalidBatch.rejectionReasons(17u, firstRace, {}, 30u, 60u) == 4u &&
        !invalidBatch.matches(17u, firstRace, {}, 30u, 60u),
        "an invalid target cannot match another invalid target");

    // R1's live log showed two render workloads per presentation event,
    // successful history target recovery, and zero interpolated consumers.
    // Exercise the production admission/supersession predicates together for
    // that schedule. This models queue decisions, not GPU execution or fences.
    constexpr uint64_t currentPresentGroup = 39u;
    const InterpolationBatchMetadata firstWorkload{401u, firstRace, targetA, 30u, 60u};
    const InterpolationBatchMetadata finalWorkload{402u, firstRace, targetB, 30u, 60u};
    ScenePresentationHistory selectedHistory;
    selectedHistory.activate(firstRace);
    selectedHistory.record(firstRace, targetA);
    selectedHistory.record(firstRace, targetB);
    require(allowPresentationHistoryTarget(true, 60u, 30u,
        selectedHistory.contains(firstRace, targetB)),
        "the final workload can prepare an exactly recorded history target");
    require(queuedWorkloadSupersedesBatch(true, currentPresentGroup, currentPresentGroup),
        "the second workload in the same presentation group may supersede the first");
    require(!queuedWorkloadSupersedesBatch(true, currentPresentGroup, currentPresentGroup + 1u),
        "a queued workload awaiting the next presentation must not cancel this group's images");
    require(!queuedWorkloadSupersedesBatch(false, currentPresentGroup, currentPresentGroup) &&
        !queuedWorkloadSupersedesBatch(true, currentPresentGroup, currentPresentGroup - 1u),
        "an empty queue or a different presentation group cannot supersede this batch");
    const bool finalBatchMatches = finalWorkload.matches(402u, firstRace, targetB, 30u, 60u);
    require(admitGeneratedPresentationBatch(true, false, false, 2u, finalBatchMatches),
        "the final history-selected batch must reach the consumer with its stock flag cleared");
    require(!admitGeneratedPresentationBatch(true, true, false, 2u,
        firstWorkload.matches(402u, firstRace, targetA, 30u, 60u)),
        "sharing a presentation group cannot relabel the earlier workload as the final batch");
    require(!admitGeneratedPresentationBatch(true, true, false, 2u,
        finalWorkload.matches(402u, firstRace, targetA, 30u, 60u)),
        "a stock flag cannot authorize the wrong target of the final workload");
    require(!admitGeneratedPresentationBatch(true, false, false, 2u,
        finalWorkload.matches(402u, otherScene, targetB, 30u, 60u)),
        "a history-selected batch cannot cross a scene change");
    const InterpolationBatchMetadata highRateBatch{403u, firstRace, targetB, 30u, 120u};
    require(admitGeneratedPresentationBatch(true, true, false, 4u,
        highRateBatch.matches(403u, firstRace, targetB, 30u, 120u)),
        "an exact 120 Hz batch also admits four generated images with a stock flag");
    for (uint32_t count : {0u, 1u}) {
        require(!admitGeneratedPresentationBatch(true, true, false, count, true) &&
            !admitGeneratedPresentationBatch(false, true, false, count, true),
            "zero or one planned batch image is never an interpolated presentation");
    }
    require(!admitGeneratedPresentationBatch(true, true, true, 2u, true) &&
        !admitGeneratedPresentationBatch(true, false, true, 2u, true),
        "an explicit debugger framebuffer view does not borrow the normal race batch");
    require(admitGeneratedPresentationBatch(false, true, false, 2u, false) &&
        admitGeneratedPresentationBatch(false, true, true, 2u, false),
        "the disabled comparison retains stock admission without the new metadata/debugger policy");
    require(!admitGeneratedPresentationBatch(false, false, false, 2u, true),
        "the disabled comparison does not acquire history-based admission");

    ScenePresentationHistory history;
    history.activate(firstRace);
    history.record(firstRace, targetA);
    history.record(firstRace, targetB);
    require(history.contains(firstRace, targetA),
        "a selected target is authorized only in its recorded scene");
    history.eraseAddress(targetA.address);
    require(!history.contains(firstRace, targetA) && history.contains(firstRace, targetB),
        "framebuffer discard invalidates its address without losing unrelated buffers");
    history.record(firstRace, targetA);

    SceneSnapshot previousRace = firstRace;
    for (uint32_t transition = 0; transition < 10000u; transition++) {
        // Queue a menu and a new race before an old race present finishes.
        // The shared history must neither admit that old present nor roll its
        // epoch back when the old producer is observed late.
        const SceneSnapshot menu = tracker.observe(false);
        const SceneSnapshot nextRace = tracker.observe(true);
        history.activate(menu);
        history.activate(nextRace);
        require(!history.contains(nextRace, targetA),
            "every race starts without inherited presentation identities");
        history.record(nextRace, targetB);
        history.record(previousRace, targetA);
        history.activate(previousRace);
        history.record(menu, targetA);
        require(!history.contains(nextRace, targetA) && history.contains(nextRace, targetB),
            "late queue events cannot contaminate or reset the current race history");
        require(!batch.matches(17u, nextRace, targetA, 30u, 60u),
            "a reused address and workload count cannot cross a scene epoch");
        previousRace = nextRace;
    }
    history.clear();
    require(!history.contains(previousRace, targetB),
        "resource reset invalidates presented targets in the current scene");
    history.record(previousRace, targetB);
    require(history.contains(previousRace, targetB),
        "the active scene can warm up again after resource reset");

    std::cout << "RR64 frame metadata passed: exact rejection reasons, two-workload group "
        "admission and supersession predicates, framebuffer history, and 10000 delayed "
        "scene transitions. GPU scheduling is not simulated.\n";
}
