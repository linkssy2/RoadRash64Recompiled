#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include "hle/rt64_rr64_interpolation_pressure.h"
#include "hle/rt64_rr64_authored_cadence.h"

namespace {
    void require(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "RR64 interpolation pressure failure: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char **argv) {
    using namespace RT64::RR64FramePacing;
    const bool oldNativeLimit = argc == 2 && std::string_view(argv[1]) == "--old-native-limit";
    const auto nativeLimit = [&](bool eligible, bool targetKnown, uint32_t target) {
        return !oldNativeLimit && renderRaceAtNativeLimit(eligible, targetKnown, target);
    };
    // Reproduce the measured camera-turn cadence; do not turn its source30
    // observation into source60 just to avoid expensive correspondence work.
    AuthoredCadenceTracker tracker;
    uint64_t time = 0;
    tracker.observe(time, 2u, true);
    for (unsigned i = 0; i < 6; ++i) tracker.observe(time += 16'666'667u, 2u, true);
    for (const uint64_t delay : {32'907'000u, 32'324'000u, 30'870'000u}) {
        const auto observed = tracker.observe(time += delay, 2u, true);
        require(!(requiresFrameMatching(true, false, 60u, observed.sourceRate) &&
            !nativeLimit(true, true, 60u)),
            "delayed60-cap race frames must not enable extra matching");
    }
    for (const uint32_t source : {0u, 15u, 30u, 60u}) {
        for (const uint32_t target : {0u, 30u, 60u, 61u, 90u, 117u, 120u, 144u}) {
            const bool native = nativeLimit(true, true, target);
            require(native == (target > 0 && target <= 60), "native cap is independent of delayed source detection");
            if (native) {
                require(presentationFrameCount(true, 1u, target, source) == 1u,
                    "a retained native image presents once even for a genuine30Hz race");
            } else {
                require((requiresFrameMatching(true, false, target, source) && !native) ==
                    requiresFrameMatching(true, false, target, source),
                    "higher caps and disabled targets retain the existing matching policy");
            }
            require(!nativeLimit(false, true, target) && !nativeLimit(true, false, target),
                "unsupported retention or unknown target cannot select the native-cap path");
            const InterpolationBatchMetadata preserved{10u, {2u, true}, {0x100000u, 320u, 2u}, source, target};
            require(preserved.matches(10u, {2u, true}, preserved.target, source, target),
                "native-cap policy preserves actual source and writer identity");
        }
    }
    // Returning to high refresh must immediately enable the original policy;
    // the cap decision has no cooldown or state carried from earlier frames.
    for (uint32_t target : {60u, 117u, 60u, 120u}) {
        require((requiresFrameMatching(true, false, target, 60u) && !nativeLimit(true, true, target)) == (target > 60),
            "switching60-to117/120 resumes matching without a sticky native flag");
        RationalFrameCadence cadence;
        uint32_t images = 0;
        for (unsigned i = 0; i < 60; ++i) images += cadence.advance(target, 60u);
        require(images == target, "117/120 rational output cadence is unchanged");
    }
    // Exercise the production wiring, including its existing single-native
    // ownership path, so a helper-only fix cannot pass this regression.
    std::ifstream file(std::filesystem::path(__FILE__).parent_path().parent_path() /
        "lib/rt64/src/hle/rt64_workload_queue.cpp");
    require(file.is_open(), "production queue is available for the wiring check");
    std::string queue((std::istreambuf_iterator<char>(file)), {});
    queue = std::regex_replace(queue, std::regex(R"(//[^\r\n]*|/\*[\s\S]*?\*/)"), "");
    queue = std::regex_replace(queue, std::regex(R"(\s+)"), "");
    require(queue.find("constboolrenderNativeAtLimit=RR64FramePacing::renderRaceAtNativeLimit(canRetainWorkload,!interpolationTargetKey.isEmpty(),workloadConfig.targetRate);") != std::string::npos &&
        queue.find("if(!renderNativeAtLimit&&!renderNativeUnderPressure){") < queue.find("curFrame.match("),
        "real matching uses the tested native-cap decision after retention eligibility");
    require(queue.find("uint32_tdisplayFrames=1;if(generateInterpolatedFrames){") != std::string::npos &&
        queue.find("if(generateInterpolatedFrames&&!prevFrame.workloads.empty()){") != std::string::npos,
        "skipping matching retains one endpoint without claiming interpolation history");
    const QueuedAuthoredWorkload current{10u, 7u, {2u, true}};
    const QueuedAuthoredWorkload superseding{11u, 7u, {2u, true}};

    require(renderSupersededBatchNatively(true, true, true, current, superseding),
        "a newer writer before the same VI avoids optional interpolation");
    require(!renderSupersededBatchNatively(false, true, true, current, superseding),
        "unsupported retention, ray tracing, pauses and non-race paths keep their policy");
    require(!renderSupersededBatchNatively(true, false, true, current, superseding),
        "unknown target does not authorize a retained native image");
    require(!renderSupersededBatchNatively(true, true, false, current, superseding),
        "native-rate rendering has no optional work to suppress");

    for (const auto invalid : {
        QueuedAuthoredWorkload{},
        QueuedAuthoredWorkload{9u, 7u, {2u, true}},
        QueuedAuthoredWorkload{10u, 7u, {2u, true}},
        QueuedAuthoredWorkload{11u, 8u, {2u, true}},
        QueuedAuthoredWorkload{11u, 6u, {2u, true}},
        QueuedAuthoredWorkload{11u, 7u, {3u, true}},
        QueuedAuthoredWorkload{11u, 7u, {2u, false}}}) {
        require(!renderSupersededBatchNatively(true, true, true, current, invalid),
            "empty, stale, different-interval or different-scene work is not supersession");
    }
    require(!renderSupersededBatchNatively(true, true, true,
        {0u, 7u, {2u, true}}, superseding), "current writer must be valid");
    require(!renderSupersededBatchNatively(true, true, true,
        {10u, 7u, {0u, true}}, {11u, 7u, {0u, true}}), "scene must be initialized");

    // Exercise the actual presentation count helper: under pressure an owned
    // native image stays one image even with a legitimate slower source. Once
    // the queued writer belongs to the next VI, normal fractional output is
    // permitted again immediately; no sticky rate or cooldown state exists.
    for (const uint32_t source : {15u, 30u, 60u}) {
        for (const uint32_t target : {60u, 90u, 120u, 144u, 240u}) {
            const bool optional = target > source;
            const bool reduce = renderSupersededBatchNatively(true, true,
                optional, current, superseding);
            require(reduce == optional, "only optional pictures may be reduced");
            require(presentationFrameCount(true, 1u, target, source) == 1u,
                "owned native endpoint cannot expand according to a slow source ratio");
            const QueuedAuthoredWorkload nextInterval{12u, 8u, {2u, true}};
            require(!renderSupersededBatchNatively(true, true, optional,
                superseding, nextInterval), "ordinary interpolation returns at next interval");
            const InterpolationBatchMetadata certificate{current.writer,
                current.scene, {0x100000u, 320u, 2u}, source, target};
            require(certificate.matches(current.writer, current.scene,
                certificate.target, source, target),
                "native reduction preserves the writer and actual source-rate certificate");
        }
    }
    std::cout << "RR64 interpolation pressure smoke tests passed\n";
}
