#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <thread>

#include "hle/rt64_rr64_frame_pacing.h"
#include "hle/rt64_rr64_present_config.h"
#include "common/rt64_timer.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "RR64 frame-pacing smoke failure: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::string compactSource(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.is_open(), "the production source must be available for its wiring contract");
    std::string source((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    source = std::regex_replace(source,
        std::regex(R"(//[^\r\n]*|/\*[\s\S]*?\*/)"), "");
    source.erase(std::remove_if(source.begin(), source.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }), source.end());
    return source;
}

std::size_t blockEnd(const std::string& source, std::size_t opening) {
    require(opening < source.size() && source[opening] == '{',
        "the source contract must identify an opening brace");
    std::size_t depth = 1;
    auto end = opening + 1;
    while (end < source.size() && depth != 0) {
        if (source[end] == '{') { ++depth; }
        if (source[end] == '}') { --depth; }
        ++end;
    }
    require(depth == 0, "the source contract must identify the complete block");
    return end;
}

void verifyProductionVsyncRequest() {
    const auto nativePath = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto frontend = compactSource(nativePath /
        "lib/RecompFrontend/recompui/src/renderer/rt64_render_context.cpp");
    const auto update = frontend.find("renderer::RT64Context::update_config(");
    require(update != std::string::npos, "runtime graphics configuration must be identifiable");
    const auto updateOpen = frontend.find('{', update);
    const auto updateBody = frontend.substr(updateOpen, blockEnd(frontend, updateOpen) - updateOpen);
    const auto windowBlock = updateBody.find("if(window_mode_changed){");
    require(windowBlock != std::string::npos,
        "window changes must keep their separate swapchain synchronization block");
    const auto windowOpen = updateBody.find('{', windowBlock);
    const auto windowEnd = blockEnd(updateBody, windowOpen);
    const auto windowBody = updateBody.substr(windowOpen, windowEnd - windowOpen);
    require(windowBody.find("app->presentQueue->threadMutex") != std::string::npos &&
        windowBody.find("app->setFullScreen(") != std::string::npos,
        "window changes retain the present mutex around their native window mutation");
    constexpr std::string_view request =
        "app->presentQueue->requestVsync(new_config.vsync_enabled);";
    const auto requestAt = updateBody.find(request);
    require(requestAt != std::string::npos && requestAt >= windowEnd &&
        updateBody.find(request, requestAt + request.size()) == std::string::npos &&
        updateBody.find("setVsyncEnabled(") == std::string::npos,
        "runtime VSync must enqueue once outside the window lock, without direct swapchain mutation");

    const auto queue = compactSource(nativePath / "lib/rt64/src/hle/rt64_present_queue.cpp");
    const auto loop = queue.find("voidPresentQueue::threadLoop(){");
    require(loop != std::string::npos, "the presenter loop must be identifiable");
    const auto loopOpen = queue.find('{', loop);
    const auto loopBody = queue.substr(loopOpen, blockEnd(queue, loopOpen) - loopOpen);
    const auto lock = loopBody.find("threadLock(threadMutex);");
    const auto consume = loopBody.find(".consume(", lock);
    const auto apply = loopBody.find("ext.swapChain->setVsyncEnabled(", consume);
    const auto resize = loopBody.find("ext.swapChain->needsResize()", lock);
    require(lock != std::string::npos && consume != std::string::npos &&
        apply != std::string::npos && resize != std::string::npos &&
        lock < consume && consume < apply && apply < resize,
        "the presenter consumes and applies VSync under its mutex before checking for a Vulkan rebuild");
    require(loopBody.substr(consume, apply - consume).find("isVsyncEnabled()") ==
        std::string::npos,
        "a request must update Vulkan's desired mode even if a delayed rebuild still reports the old mode");
}

void verifyManualTarget() {
    using RT64::RR64FramePacing::manualTargetRate;
    for (const auto display : {0u, 60u, 120u, 144u}) {
        for (const auto requested : {30u, 60u, 120u, 144u, 240u}) {
            require(manualTargetRate(requested, display, false) == requested,
                "VSync off keeps the requested FPS even above the detected display rate");
            const auto synchronized = manualTargetRate(requested, display, true);
            require(synchronized <= requested && (!display || synchronized <= display) &&
                (synchronized == requested || synchronized == display),
                "VSync on retains the manual cap and the known display ceiling");
            require(display || synchronized == requested,
                "an unknown display rate cannot impose a zero FPS cap");
        }
    }
    const auto nativePath = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto frontend = compactSource(nativePath /
        "lib/RecompFrontend/recompui/src/renderer/rt64_render_context.cpp");
    require(frontend.find("application->userConfig.rr64VsyncEnabled=config.vsync_enabled;") !=
        std::string::npos, "launcher VSync preference must reach the renderer user configuration");
    const auto workload = compactSource(nativePath / "lib/rt64/src/hle/rt64_workload_queue.cpp");
    const auto update = workload.find("voidWorkloadQueue::threadConfigurationUpdate(");
    require(update != std::string::npos, "workload configuration update must be identifiable");
    const auto opening = workload.find('{', update);
    const auto body = workload.substr(opening, blockEnd(workload, opening) - opening);
    require(body.find("conststd::scoped_locklock(ext.sharedResources->configurationMutex);") !=
        std::string::npos && body.find("caseUserConfiguration::RefreshRate::Manual:"
        "workloadConfig.targetRate=RR64FramePacing::manualTargetRate("
        "ext.sharedResources->userConfig.refreshRateTarget,ext.sharedResources->swapChainRate,"
        "ext.sharedResources->userConfig.rr64VsyncEnabled);break;") != std::string::npos,
        "manual target selection must use the locked VSync configuration and tested policy");
    require(body.find("caseUserConfiguration::RefreshRate::Display:"
        "workloadConfig.targetRate=ext.sharedResources->swapChainRate;break;") != std::string::npos &&
        body.find("caseUserConfiguration::RefreshRate::Original:default:"
        "workloadConfig.targetRate=0;break;") != std::string::npos,
        "Display and Original target selection remain unchanged");
}

void verifyPendingVsyncChanges() {
    using RT64::RR64FramePacing::PendingVsyncChange;
    PendingVsyncChange pending;
    bool selected = true;
    require(!pending.consume(selected) && selected,
        "startup has no pending VSync request and must preserve the caller's value");
    pending.request(false);
    require(pending.consume(selected) && !selected,
        "VSync off is a real request, not an empty sentinel");
    require(!pending.consume(selected) && !selected,
        "a consumed off request must not be replayed");
    pending.request(true);
    require(pending.consume(selected) && selected,
        "VSync can be re-enabled after an earlier request was consumed");
    pending.request(false);
    pending.request(true);
    pending.request(false);
    require(pending.consume(selected) && !selected,
        "rapid toggles apply only their latest pending state");
    pending.request(true);
    pending.reset();
    require(!pending.consume(selected) && !selected,
        "reset discards a pending change without changing the caller's value");

    // Exercise overlapping stores/exchanges with bounded work. Coalescing is
    // intentional, so intermediate requests need not all be observed. After
    // joining the producer, however, its final request must either have been
    // consumed already or still be pending; it cannot vanish in a clear race.
    for (const bool finalSelection : {false, true}) {
        pending.reset();
        constexpr unsigned requests = 100'000;
        bool applied = !finalSelection;
        unsigned consumed = 0;
        std::thread producer([&] {
            for (unsigned index = 0; index < requests; ++index) {
                pending.request((index & 1u) != 0);
            }
            pending.request(finalSelection);
        });
        for (unsigned attempt = 0; attempt < requests; ++attempt) {
            bool value = applied;
            if (pending.consume(value)) {
                applied = value;
                ++consumed;
            }
            else {
                require(value == applied, "empty concurrent consume must not mutate its output");
            }
        }
        producer.join();
        if (pending.consume(applied)) { ++consumed; }
        require(consumed > 0 && consumed <= requests + 1 && applied == finalSelection,
            "concurrent coalescing must preserve the final posted VSync selection");
        require(!pending.consume(applied) && applied == finalSelection,
            "draining the final request leaves the mailbox empty without altering the selection");
        pending.request(!finalSelection);
        require(pending.consume(applied) && applied != finalSelection,
            "a new request after consumption must remain independently deliverable");
    }
}

void verifyRationalCadence(bool oldPolicyControl) {
    using namespace RT64::RR64FramePacing;
    uint64_t testedIntervals = 0;
    for (const uint32_t source : {60u, 15u, 30u, 50u}) {
        for (const uint32_t target : {117u, 61u, 75u, 90u, 100u, 119u, 120u, 144u, 165u, 240u}) {
            if (target <= source || !validGeneratedFrameCount(target, source, target / source)) continue;
            for (unsigned pattern = 0; pattern < 4; ++pattern) {
                RationalFrameCadence producer, presenter;
                uint64_t totalSlots = 0, oldTotal = 0, oldRemainder = 0;
                for (uint32_t update = 0; update < source * 12u; ++update) {
                    const bool eligible = pattern == 0 || (pattern == 1 && update % 2 == 0) ||
                        (pattern == 2 && update % 23 < 4);
                    const uint32_t generatedSlots = producer.advance(target, source);
                    const uint32_t outputSlots = presenter.advance(target, source);
                    const uint64_t before = uint64_t(update) * target / source;
                    const uint32_t expected = uint32_t(uint64_t(update + 1u) * target / source - before);
                    require(generatedSlots == expected && outputSlots == expected,
                        "producer and presenter retain rational phase through alternating eligibility");
                    const uint32_t images = eligible ? generatedSlots : 1u;
                    for (uint32_t slot = 0; slot < outputSlots; ++slot) {
                        require(ownedPresentationImageIndex(slot, outputSlots, images) < images,
                            "every repeated or generated owned output remains in its leased batch");
                    }
                    if (eligible) {
                        for (uint32_t slot = 0; slot < generatedSlots; ++slot) {
                            const double expectedWeight = std::clamp(
                                (double((before + slot + 1u) * source) - double(uint64_t(update) * target)) /
                                target, 0.0, 1.0);
                            require(std::abs(producer.interpolationWeight(slot + 1u) - expectedWeight) < 0.000001,
                                "generated weights stay on the source timeline after rejected intervals");
                        }
                    }
                    // Exact pre-fix policies: generation restarted after any
                    // rejected match, and native repeats required an integer ratio.
                    if (!eligible) oldRemainder = 0;
                    uint32_t oldGenerated = 1;
                    if (eligible) {
                        oldRemainder += target;
                        oldGenerated = uint32_t(oldRemainder / source);
                        oldRemainder %= source;
                    }
                    const uint32_t oldSlots = presentationFrameCount(eligible, oldGenerated, target, source);
                    oldTotal += oldSlots;
                    totalSlots += oldPolicyControl ? oldSlots : outputSlots;
                    require(totalSlots == uint64_t(update + 1u) * target / source,
                        "mixed native/interpolated output must meet the rational target without eligibility-dependent drops");
                    ++testedIntervals;
                }
                require(totalSlots == uint64_t(target) * 12u,
                    "twelve source seconds produce exactly twelve target seconds without overspeed");
                if (target == 117 && source == 60 && (pattern == 1 || pattern == 3)) {
                    require(oldTotal == 60u * 12u && totalSlots == 117u * 12u,
                        "the saved 117 FPS failure reproduces 60 output slots per source second");
                }
            }
        }
    }
    // Workloads and VIs are not one-to-one. A VI may select an older exact
    // writer or be skipped altogether. Its output clock must not accumulate
    // catch-up debt, and resampling must stay inside that selected batch.
    struct CompletedBatch { uint32_t source, images; };
    std::array<CompletedBatch, 120> completed{};
    RationalFrameCadence authoring, output;
    uint32_t priorSelectedRate = 0, rateIntervals = 0, skipped = 0, mismatches = 0;
    bool selectedOlderRate = false;
    for (uint32_t writer = 0; writer < completed.size(); ++writer) {
        const uint32_t source = writer >= 50 && writer < 65 ? 30u : 60u;
        const auto generated = authoring.advance(117, source);
        completed[writer] = {source, generated};
        if (writer == 0 || writer % 17 == 0 || (writer >= 10 && writer < 14)) {
            ++skipped;
            continue;
        }
        const auto &selected = completed[writer - 1u];
        selectedOlderRate |= selected.source != source;
        if (selected.source != priorSelectedRate) {
            rateIntervals = 0;
            priorSelectedRate = selected.source;
        }
        const auto slots = output.advance(117, selected.source);
        require(slots == uint64_t(rateIntervals + 1u) * 117 / selected.source -
            uint64_t(rateIntervals) * 117 / selected.source,
            "skipped VIs incur no catch-up debt and an older writer owns its source rate");
        ++rateIntervals;
        mismatches += slots != selected.images;
        uint32_t previousImage = 0;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const auto image = ownedPresentationImageIndex(slot, slots, selected.images);
            require(image >= previousImage && image < selected.images,
                "resume resamples only the selected older batch, never current producer images");
            previousImage = image;
        }
        require(previousImage == selected.images - 1,
            "phase divergence still includes the selected writer's latest image");
    }
    require(skipped > 0 && mismatches > 0 && selectedOlderRate,
        "the asynchronous fixture must actually exercise skips, divergent phase, and older source rates");

    RationalFrameCadence cadence;
    for (const auto rates : {std::array<uint32_t, 2>{117, 60}, {144, 60}, {120, 60},
        {60, 60}, {117, 0}, {117, 60}, {90, 30}, {0, 60}, {30, 60}}) {
        RationalFrameCadence fresh;
        require(cadence.advance(rates[0], rates[1]) == fresh.advance(rates[0], rates[1]),
            "rate changes discard the old fractional phase");
        require(cadence.advance(rates[0], rates[1]) == fresh.advance(rates[0], rates[1]),
            "source and target changes restart at the current rate");
    }
    cadence.reset();
    require(cadence.advance(117, 60) == 1 && cadence.advance(117, 60) == 2,
        "scene reset starts a fresh rational cadence");
    cadence.reset();
    uint32_t pressureTotal = 0, suppressedSlots = 0;
    for (uint32_t update = 0; update < 60; ++update) {
        const auto slots = cadence.advance(117, 60);
        const bool nativePressureBatch = update % 7 == 0;
        pressureTotal += nativePressureBatch ? 1u : slots;
        suppressedSlots += nativePressureBatch ? slots - 1u : 0u;
    }
    require(pressureTotal + suppressedSlots == 117 && cadence.advance(117, 60) == 1,
        "native pressure remains one slot and never creates catch-up debt");
    for (uint32_t images = 1; images <= MaximumCadenceFrames; ++images) {
        for (uint32_t slots = 1; slots <= MaximumCadenceFrames; ++slots) {
            uint32_t previous = 0;
            for (uint32_t slot = 0; slot < slots; ++slot) {
                const auto index = ownedPresentationImageIndex(slot, slots, images);
                require(index >= previous && index < images,
                    "owned sampling is monotonic and cannot escape its immutable lease");
                previous = index;
            }
            require(previous == images - 1u, "owned sampling includes the latest available image");
        }
    }
    require(cadence.advance(UINT32_MAX, 1) == MaximumCadenceFrames,
        "extreme configured ratios retain the existing output bound");

    const auto nativePath = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto workload = compactSource(nativePath / "lib/rt64/src/hle/rt64_workload_queue.cpp");
    const auto present = compactSource(nativePath / "lib/rt64/src/hle/rt64_present_queue.cpp");
    const auto advance = workload.find("retainedFrameCadence.advance(workloadConfig.targetRate,workload.viOriginalRate)");
    require(advance != std::string::npos && advance < workload.find("if(requiresFrameMatching){") &&
        workload.find("displayFrames=retainedCadenceFrames;") != std::string::npos &&
        workload.find("curFrameWeight=retainedFrameCadence.interpolationWeight(frame+1u);") != std::string::npos,
        "production generation consumes its cadence before matching can reject it");
    require(present.find("outputFrameCadence.advance(targetRate,viOriginalRate)") <
        present.find("ownedFrameBatches.find(") &&
        present.find("((ownedFrames&&ownedFrames->previousWriterWorkloadId==0)?1u:cadenceFrames)") != std::string::npos &&
        present.find("ownedFrames->images[RR64FramePacing::ownedPresentationImageIndex(") != std::string::npos,
        "production presentation shares rational slots while retaining native pressure and bounded owned indexing");
    const auto snapshot = present.find("commandList->copyTexture(repeatTexture,sourceTexture);");
    const auto unlock = present.find("ext.sharedResources->workloadMutex.unlock();", snapshot);
    require(snapshot != std::string::npos && unlock != std::string::npos && snapshot < unlock &&
        present.find("notifyPresentId(present);", unlock) != std::string::npos,
        "native repeats copy before releasing the workload or allowing its next writer");
    std::cout << "RR64 rational cadence: " << testedIntervals << " mixed-eligibility intervals passed; "
        "117/60 old native/alternating policy = 60 FPS, current = 117 output slots per source second.\n";
}

void verifyProductionMatchingGate() {
    // This source contract deliberately checks the real queue wiring as well
    // as the pure policy below. It rejects an unchanged R18 queue even when
    // that queue is compiled alongside the new helper.
    const auto queuePath = std::filesystem::path(__FILE__).parent_path()
        .parent_path() / "lib/rt64/src/hle/rt64_workload_queue.cpp";
    std::ifstream input(queuePath);
    require(input.is_open(), "the production workload queue must be available for its gate contract");
    std::string source((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    source = std::regex_replace(source,
        std::regex(R"(//[^\r\n]*|/\*[\s\S]*?\*/)"), "");
    source.erase(std::remove_if(source.begin(), source.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }), source.end());
    const std::string_view expected =
        "constboolrequiresFrameMatching=RR64FramePacing::requiresFrameMatching("
        "stablePresentation,workloadConfig.raytracingEnabled,"
        "workloadConfig.targetRate,workload.viOriginalRate);";
    const auto decision = source.find(expected);
    require(decision != std::string::npos &&
        source.find(expected, decision + expected.size()) == std::string::npos,
        "the actual queue must use the tested scene-independent matching policy once");
    const auto guardedBlock = source.find("if(requiresFrameMatching){", decision);
    require(guardedBlock != std::string::npos,
        "the production matching call must have its policy guard");
    auto blockEnd = guardedBlock + std::string_view("if(requiresFrameMatching){").size();
    std::size_t depth = 1;
    while (blockEnd < source.size() && depth != 0) {
        if (source[blockEnd] == '{') { ++depth; }
        if (source[blockEnd] == '}') { --depth; }
        ++blockEnd;
    }
    const auto matchCall = source.find("curFrame.match(");
    require(depth == 0 && matchCall > guardedBlock && matchCall < blockEnd &&
        source.find("curFrame.match(", matchCall + 1) == std::string::npos,
        "the sole production frame match must stay inside the tested guard");
    require(source.find("constboolretainedRacePath=stablePresentation&&raceActive;") !=
        std::string::npos,
        "native matching policy must not broaden retained race image ownership");
}
}

int main(int argc, char** argv) {
    using namespace RT64::RR64FramePacing;

    static_assert(RT64::Timestamp::clock::is_steady);
    const auto timerStart = RT64::Timer::current();
    RT64::Timer::preciseSleepUntil(timerStart - std::chrono::seconds(1));
    const auto timerAfter = RT64::Timer::current();
    require(timerAfter >= timerStart, "presentation clock is monotonic");
    require(RT64::Timer::deltaMicroseconds(timerStart, timerStart + std::chrono::milliseconds(3)) == 3000,
        "presentation time deltas remain in microseconds");
    verifyProductionMatchingGate();
    verifyProductionVsyncRequest();
    verifyManualTarget();
    verifyPendingVsyncChanges();
    verifyRationalCadence(argc == 3 && std::string_view(argv[2]) == "--old-rational-cadence");
    require(!requiresFrameMatching(true, false, 60, 60) &&
        !requiresFrameMatching(true, false, 60, 0) &&
        !requiresFrameMatching(true, false, 30, 60) &&
        !requiresFrameMatching(true, false, 0, 60),
        "stable native, unresolved, lower-rate, and disabled output skip correspondence in every scene");
    require(requiresFrameMatching(true, false, 60, 30) &&
        requiresFrameMatching(true, false, 120, 60) &&
        requiresFrameMatching(true, false, 144, 60) &&
        requiresFrameMatching(true, false, 60, 50),
        "known lower-rate sources retain both integral and non-integral interpolation matching");
    for (const std::uint32_t source : {0u, 15u, 30u, 60u, 120u}) {
        for (const std::uint32_t target : {0u, 30u, 60u, 120u, 144u}) {
            require(requiresFrameMatching(false, false, target, source) == (target > 0),
                "the disabled stable-presentation control keeps its original matching policy");
            require(requiresFrameMatching(true, true, target, source) &&
                requiresFrameMatching(false, true, target, source),
                "ray tracing keeps required velocity matching at every cadence");
        }
    }
    // Race -> unresolved results -> native results -> higher-refresh output
    // must not leave a stale scene-dependent matching policy behind.
    const std::uint32_t transitionSource[] = {60, 0, 60, 60, 0, 30, 60};
    const std::uint32_t transitionTarget[] = {60, 60, 60, 120, 120, 60, 60};
    const bool transitionMatching[] = {false, false, false, true, false, true, false};
    for (std::size_t i = 0; i < std::size(transitionSource); ++i) {
        require(requiresFrameMatching(true, false, transitionTarget[i],
            transitionSource[i]) == transitionMatching[i],
            "cadence transitions must enable matching only for the current interpolation requirement");
    }

    if (argc == 2) {
        const std::string_view expected(argv[1]);
        require(expected == "enabled" || expected == "disabled",
            "policy expectation must be enabled or disabled");
        require(stablePresentationEnabled() == (expected == "enabled"),
            "the process-start presentation switch must match the requested policy");
    }

    require(exactCadenceFrameCount(60, 30) == 2,
        "a known 30 Hz source has two output slots at 60 Hz");
    for (uint32_t target : {61u, 75u, 90u, 100u, 119u, 120u, 144u, 165u, 240u}) {
        uint64_t remainder=0, frames=0;
        for (unsigned update=0; update<60; ++update) {
            remainder+=target;
            const auto count=uint32_t(remainder/60);remainder%=60;
            require(validGeneratedFrameCount(target,60,count), "fractional batch accepted");
            frames+=count;
        }
        require(frames==target, "one second of source updates yields requested output count");
    }
    require(!validGeneratedFrameCount(144,60,4), "reject invalid fractional batch size");
    require(!validGeneratedFrameCount(144,0,2), "reject unknown source rate");
    require(exactCadenceFrameCount(120, 30) == 4,
        "a known 30 Hz source has four output slots at 120 Hz");
    require(exactCadenceFrameCount(60, 15) == 4,
        "a known 15 Hz source has four output slots at 60 Hz");
    require(exactCadenceFrameCount(30, 30) == 1,
        "native-rate presentation must remain single-frame");
    require(exactCadenceFrameCount(60, 50) == 1,
        "non-integral fallback cadence must not over-present");
    require(exactCadenceFrameCount(60, 60) == 1 &&
        exactCadenceFrameCount(60, 0) == 1,
        "equal and unresolved source rates require one native output slot");

    require(presentationFrameCount(true, 2, 60, 30) == 2,
        "generated interpolation count must be preserved");
    require(presentationFrameCount(false, 1, 60, 30) == 2,
        "temporary interpolation loss must repeat at 60 Hz");
    require(!shouldAbandonRemainingGeneratedFrames(false),
        "a predicted deadline miss must not discard generated frames");
    require(shouldAbandonRemainingGeneratedFrames(true),
        "a genuinely newer workload may supersede stale interpolation");

    require(!usePresentWait(true, true, true, false, true),
        "D3D12 VSync must use Present(1) without a second wait");
    require(!usePresentWait(true, true, false, true, true),
        "D3D12 software pacing must not stack a DXGI wait");
    require(usePresentWait(true, false, true, false, true) &&
        !usePresentWait(false, false, true, false, true),
        "other backends retain their supported present-wait policy");
    require(usePresentWait(true, true, true, false, false),
        "the disabled candidate retains the maintenance control wait");

    // Full-rate FIFO must not wait on a second software clock. Preserve caps
    // below refresh and the legacy comparison path, including startup.
    for (uint32_t hz : {30u, 60u, 75u, 120u, 144u, 165u, 240u}) {
        require(!useSoftwarePacing(true, false, true, true, hz, hz, 30, false),
            "Vulkan FIFO at refresh has one clock from the first frame");
        require(!useSoftwarePacing(true, false, true, true, hz * 2, hz, 30, true),
            "Vulkan FIFO above refresh does not add software sleeps");
        require(useSoftwarePacing(true, false, true, true, hz / 2, hz, 30, true),
            "Vulkan lower frame cap is preserved");
        require(useSoftwarePacing(true, false, true, false, hz, hz, 30, true),
            "Vulkan immediate mode remains capped");
        require(useSoftwarePacing(true, false, true, true, hz, 0, 30, true),
            "unknown display rate retains software cap");
        require(!useSoftwarePacing(true, true, false, true, hz, hz, 30, true),
            "D3D12 VSync path unchanged");
        require(useSoftwarePacing(true, true, false, false, hz, hz, 30, false),
            "D3D12 tearing path remains capped on cold start");
    }
    require(!useSoftwarePacing(false, false, true, true, 60, 60, 60, true) &&
        useSoftwarePacing(false, false, true, true, 120, 60, 60, true),
        "disabled stable presentation retains legacy Vulkan timing");
    require(!useSoftwarePacing(true, false, true, true, 0, 60, 30, true),
        "native output does not invent a software cap");
    require(waitBeforeSoftwarePacing(true, true) &&
        !waitBeforeSoftwarePacing(false, true) && !waitBeforeSoftwarePacing(true, false),
        "only stable Vulkan moves its queue throttle before the deadline");

    for (bool stable : {false, true}) for (bool d3d : {false, true}) {
        if (stable && !d3d) continue; // Only stable Vulkan intentionally changes.
        for (bool vsync : {false, true}) for (bool previous : {false, true}) {
            for (uint32_t target : {0u, 30u, 60u, 120u, 144u}) {
                for (uint32_t source : {0u, 30u, 60u}) {
                    const bool oldPolicy = !(d3d && vsync) && target > 0 &&
                        (stable || (previous && target > source));
                    require(useSoftwarePacing(stable, d3d, !d3d, vsync,
                        target, 60, source, previous) == oldPolicy,
                        "D3D12 and disabled stable control preserve previous pacing");
                }
            }
        }
    }

    // A queue wait can consume the whole cap interval. It must be included in
    // schedule(now) so a ready late image is submitted immediately, not delayed
    // for another interval. No real display or claimed FPS gain is simulated.
    for (int64_t queueWait : {0LL, 8'000'000LL, 16'666'667LL, 33'333'334LL, 100'000'000LL}) {
        StableDeadlinePacer cap;
        cap.schedule(0, 60);
        const int64_t ready = 2'000'000 + queueWait;
        const auto afterWait = cap.schedule(ready, 60);
        require(afterWait.deadlineNanoseconds == std::max<int64_t>(16'666'666, ready),
            "queue wait consumes cap budget without extra post-deadline wait");
    }

    StableDeadlinePacer deadlinePacer;
    DeadlineDecision deadline = deadlinePacer.schedule(1'000'000'000LL, 60);
    require(deadline.deadlineNanoseconds == 1'000'000'000LL && deadline.rebased,
        "the first presentation must establish an immediate deadline");
    deadline = deadlinePacer.schedule(1'005'000'000LL, 60);
    require(deadline.deadlineNanoseconds == 1'016'666'666LL && !deadline.rebased,
        "an early frame must retain the absolute 60 Hz deadline");
    deadline = deadlinePacer.schedule(1'040'000'000LL, 60);
    require(deadline.deadlineNanoseconds == 1'040'000'000LL && deadline.rebased,
        "a late frame must discard timing debt instead of bursting");
    deadline = deadlinePacer.schedule(1'041'000'000LL, 120);
    require(deadline.deadlineNanoseconds == 1'041'000'000LL && deadline.rebased,
        "a refresh-rate change must begin a new deadline stream");

    // Real timers can oversleep after schedule() returns. The old test injected
    // all lateness before schedule(), so it never exercised this burst path.
    deadlinePacer.reset();
    deadline = deadlinePacer.schedule(0, 60);
    deadlinePacer.recordPresent(0);
    require(deadline.deadlineNanoseconds == 0,
        "the first frame must establish the clock, not leave the next unpaced");
    deadline = deadlinePacer.schedule(1'000'000, 60);
    require(deadline.deadlineNanoseconds == 16'666'666,
        "the second cold-start frame must wait a complete interval");
    deadlinePacer.recordPresent(25'000'000);
    deadline = deadlinePacer.schedule(26'000'000, 60);
    require(deadline.deadlineNanoseconds == 41'666'666,
        "an overslept frame must not be followed by a catch-up burst");
    deadline = deadlinePacer.schedule(10'000'000'000LL, 60);
    deadlinePacer.recordPresent(10'000'000'000LL);
    require(deadline.rebased && deadline.deadlineNanoseconds == 10'000'000'000LL,
        "a pause or focus stall must discard all accumulated timing debt");
    deadline = deadlinePacer.schedule(10'001'000'000LL, 60);
    require(deadline.deadlineNanoseconds == 10'016'666'666LL,
        "the frame after a pause must resume with a full interval");
    deadlinePacer.schedule(10'002'000'000LL, 0);
    deadline = deadlinePacer.schedule(10'003'000'000LL, 60);
    require(deadline.rebased && deadline.deadlineNanoseconds == 10'003'000'000LL,
        "disabling the target clock must reset its next activation");

    // Tiny positive timer or submission errors are unavoidable. The previous
    // implementation added each one to the next deadline, so even a constant
    // 1 us error accumulated 36 ms of phase drift in ten minutes at 60 Hz.
    // Exercise both pre-schedule readiness and post-sleep submission jitter,
    // including varying errors, instead of only exact wakes and large stalls.
    for (uint32_t rate : {30u, 60u, 120u, 144u, 240u, 1000u}) {
        const int64_t period = 1'000'000'000LL / rate;
        const int64_t maximumJitter = std::min<int64_t>(100'000, period / 200);
        for (bool jitterBeforeSchedule : {false, true}) {
            StableDeadlinePacer jitterPacer;
            constexpr int64_t initialPhase = 4'000'000;
            int64_t previousActual = initialPhase;
            for (uint64_t frame = 0; frame <= 36'000; ++frame) {
                const int64_t idealDeadline = initialPhase + int64_t(frame) * period;
                const int64_t jitter = int64_t((frame * 13u) % 17u) * maximumJitter / 16;
                const int64_t ready = frame == 0 ? initialPhase :
                    (jitterBeforeSchedule ? idealDeadline + jitter : previousActual + period / 4);
                const auto scheduled = jitterPacer.schedule(ready, rate);
                const int64_t actual = std::max(ready, scheduled.deadlineNanoseconds) +
                    (jitterBeforeSchedule ? 0 : jitter);
                jitterPacer.recordPresent(actual);
                require(scheduled.deadlineNanoseconds == idealDeadline,
                    "bounded readiness/wake jitter must not accumulate into the deadline phase");
                require(frame == 0 || !scheduled.rebased,
                    "bounded scheduler jitter must not rebase the stable phase");
                require(actual - idealDeadline <= maximumJitter,
                    "actual presentation phase error must remain bounded over a long run");
                if (frame > 0) {
                    require((actual - previousActual) * 100 >= period * 99 &&
                        actual - previousActual >= period - 250'000,
                        "phase recovery must retain at least 99% of a frame interval and recover at most 250 us");
                }
                previousActual = actual;
            }
        }

        for (bool missBeforeSchedule : {false, true}) {
            StableDeadlinePacer stallPacer;
            stallPacer.schedule(0, rate);
            stallPacer.recordPresent(0);
            const int64_t stalledPresent = period + 1'000'000;
            const auto stalled = stallPacer.schedule(missBeforeSchedule ? stalledPresent : period / 4, rate);
            stallPacer.recordPresent(stalledPresent);
            const auto resumed = stallPacer.schedule(stalledPresent + period / 4, rate);
            require(stalled.rebased == missBeforeSchedule &&
                resumed.deadlineNanoseconds == stalledPresent + period,
                "a real readiness/wake delay must discard debt and leave a full following interval");
        }
    }

    // Exercise the host deadline stream for 24 hours at 60 Hz. Inject a
    // scheduler miss once a minute and prove that the following deadline is a
    // full interval after the late frame rather than a catch-up burst.
    deadlinePacer.reset();
    constexpr std::int64_t sixtyHzPeriodNanoseconds =
        1'000'000'000LL / 60LL;
    constexpr std::uint64_t presentCount24Hours =
        24ULL * 60ULL * 60ULL * 60ULL;
    std::int64_t hostNowNanoseconds = 10'000'000'000LL;
    std::int64_t previousDeadlineNanoseconds = hostNowNanoseconds;
    for (std::uint64_t presentIndex = 0;
         presentIndex < presentCount24Hours;
         presentIndex++)
    {
        const bool injectMiss =
            presentIndex > 0 && ((presentIndex % (60ULL * 60ULL)) == 0);
        if (injectMiss) {
            hostNowNanoseconds = previousDeadlineNanoseconds +
                (sixtyHzPeriodNanoseconds * 2);
        }
        else {
            hostNowNanoseconds = previousDeadlineNanoseconds;
        }

        deadline = deadlinePacer.schedule(hostNowNanoseconds, 60);
        if (injectMiss) {
            require(deadline.rebased &&
                deadline.deadlineNanoseconds == hostNowNanoseconds,
                "a long-session scheduler miss must discard timing debt");
        }
        else if (presentIndex > 0) {
            require(!deadline.rebased &&
                deadline.deadlineNanoseconds - previousDeadlineNanoseconds ==
                    sixtyHzPeriodNanoseconds,
                "an on-time deadline stream must remain exactly periodic");
        }

        previousDeadlineNanoseconds = deadline.deadlineNanoseconds;
        // A different miss occurs after the timer was scheduled, once every
        // minute between the pre-schedule misses. Neither may carry debt.
        const bool oversleep = (presentIndex % 3600u) == 1800u;
        if (oversleep) {
            previousDeadlineNanoseconds += 7'000'000LL;
        }
        deadlinePacer.recordPresent(previousDeadlineNanoseconds);
    }

    // Generic synthetic identities exercise bounded history. Three fixtures do
    // not assert that the game uses three rotating presentation buffers.
    const PresentationTargetIdentity targetA{ 0x00100000u, 320, 2 };
    const PresentationTargetIdentity targetB{ 0x00125800u, 320, 2 };
    const PresentationTargetIdentity targetC{ 0x0014B000u, 320, 2 };
    PresentationTargetHistory history;
    require(!history.contains(targetA),
        "an unpresented framebuffer must not be authorized");
    history.record(targetA);
    history.record(targetB);
    history.record(targetC);
    require(history.contains(targetA) && history.contains(targetB) &&
        history.contains(targetC),
        "all three synthetic targets must survive in presentation history");
    require(!history.contains({targetA.address, 640, targetA.siz}) &&
        !history.contains({targetA.address, targetA.width, 3}),
        "matching addresses with different width or pixel size are not identities");
    history.record({0, 320, 2});
    history.record({targetA.address, 0, 2});
    require(history.size() == 3,
        "invalid framebuffer identities must never enter presentation history");
    history.record(targetA);
    require(history.size() == 3,
        "re-presenting a framebuffer must refresh rather than duplicate it");
    require(allowPresentationHistoryTarget(true, 60, 30, history.contains(targetB)),
        "a recorded target remains eligible for a known lower-rate source");
    require(allowPresentationHistoryTarget(true, 60, 60, history.contains(targetB)),
        "a recorded equal-rate target can retain its native image");
    require(allowPresentationHistoryTarget(true, 60, 0, history.contains(targetB)),
        "a recorded target can retain a native image during timing warm-up");
    require(allowPresentationHistoryTarget(true, 30, 60, history.contains(targetB)),
        "target admission also permits native images above the output rate");
    require(!allowPresentationHistoryTarget(false, 60, 30, history.contains(targetB)),
        "menu framebuffers must not use the race presentation history");
    require(!allowPresentationHistoryTarget(true, 0, 30, history.contains(targetB)),
        "disabled output must not authorize a history target");
    require(!allowPresentationHistoryTarget(true, 60, 30, false) &&
        !allowPresentationHistoryTarget(true, 60, 60, false) &&
        !allowPresentationHistoryTarget(true, 60, 0, false),
        "unrecorded targets remain unauthorized at every source rate");
    history.record({targetA.address, 640, 3});
    history.eraseAddress(targetA.address);
    require(!history.contains(targetA) &&
        !history.contains({targetA.address, 640, 3}) &&
        history.contains(targetB) && history.contains(targetC),
        "discarding an allocation expires every identity at that address only");

    PresentationTargetHistory boundedHistory;
    for (std::uint32_t index = 0;
         index < PresentationTargetHistory::Capacity + 1;
         index++)
    {
        boundedHistory.record({ 0x00200000u + (index * 0x1000u), 320, 2 });
    }
    require(boundedHistory.size() == PresentationTargetHistory::Capacity,
        "presentation history must remain bounded");
    require(!boundedHistory.contains({ 0x00200000u, 320, 2 }),
        "the oldest identity must be evicted at capacity");

    // Simulate a known 30 Hz input for 24 hours, with three synthetic target
    // identities and periodic scene resets. This checks history and output
    // count arithmetic; it does not measure the game's source rate or execute
    // the real guest, workload queue, rendering, or presentation backend.
    constexpr std::uint32_t simulatedHours = 24;
    constexpr std::uint32_t sourceUpdatesPerSecond = 30;
    constexpr std::uint32_t workloadCount =
        simulatedHours * 60u * 60u * sourceUpdatesPerSecond;
    constexpr std::uint32_t workloadsPerScene =
        30u * 60u * sourceUpdatesPerSecond;
    constexpr std::uint32_t backlogPeriod = 10'007u;

    std::uint64_t presentedFrames = 0;
    std::uint64_t interpolatedWorkloads = 0;
    std::uint64_t unavailableWarmupWorkloads = 0;
    std::uint64_t backlogEvents = 0;
    std::uint32_t consecutiveUnavailable = 0;
    std::uint32_t worstUnavailableRun = 0;
    PresentationTargetHistory rotatingHistory;
    const PresentationTargetIdentity rotatingTargets[] = {
        targetA,
        targetB,
        targetC
    };
    for (std::uint32_t workload = 0; workload < workloadCount; workload++) {
        if ((workload % workloadsPerScene) == 0) {
            rotatingHistory.clear();
        }

        const PresentationTargetIdentity target = rotatingTargets[workload % 3];
        const bool interpolate = allowPresentationHistoryTarget(
            true,
            60,
            sourceUpdatesPerSecond,
            rotatingHistory.contains(target));

        const bool newerWorkloadPending =
            (workload != 0u) && ((workload % backlogPeriod) == 0u);
        if (newerWorkloadPending) {
            require(shouldAbandonRemainingGeneratedFrames(true),
                "a real producer backlog must supersede only its stale batch");
            backlogEvents++;
        }
        else {
            require(!shouldAbandonRemainingGeneratedFrames(false),
                "an on-time workload must never inherit an earlier backlog");
        }

        // The synthetic source is known to be 30 Hz. Missing history changes
        // interpolation eligibility, not the two-slot count for this fixture.
        presentedFrames += presentationFrameCount(interpolate, 2, 60,
            sourceUpdatesPerSecond);
        interpolatedWorkloads += interpolate ? 1u : 0u;
        unavailableWarmupWorkloads += interpolate ? 0u : 1u;
        consecutiveUnavailable = interpolate ? 0u : consecutiveUnavailable + 1u;
        worstUnavailableRun = std::max(
            worstUnavailableRun,
            consecutiveUnavailable);
        rotatingHistory.record(target);
    }

    require(presentedFrames == std::uint64_t(workloadCount) * 2,
        "a synthetic known 30 Hz stream retains its two-slot output count");
    const std::uint64_t simulatedScenes = workloadCount / workloadsPerScene;
    require(unavailableWarmupWorkloads == simulatedScenes * 3u,
        "each scene reset requires one warm-up visit per synthetic target");
    require(interpolatedWorkloads + unavailableWarmupWorkloads == workloadCount,
        "every workload must remain accounted for across history transitions");
    require(worstUnavailableRun == 3u,
        "history warm-up lasts three visits for the three-target fixture");

    std::cout
        << "RR64 synthetic frame-pacing stress passed: "
        << simulatedHours << " simulated hours, "
        << workloadCount << " source updates, "
        << presentedFrames << " modeled 60 Hz output slots, "
        << interpolatedWorkloads << " interpolated workloads, "
        << backlogEvents << " transient backlogs, worst warm-up "
        << worstUnavailableRun << " workloads, "
        << presentCount24Hours << " deadline decisions.\n";
    return EXIT_SUCCESS;
}
