//
// Road Rash 64 frame-pacing policy.
//

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace RT64::RR64FramePacing {
    constexpr uint32_t MaximumCadenceFrames = 8;
    constexpr uint32_t ConsoleRefreshRate = 60;

    constexpr uint32_t manualTargetRate(uint32_t requestedRate,
        uint32_t displayRate, bool vsync)
    {
        // Manual tearing output may exceed the monitor's refresh rate.
        return vsync && displayRate > 0 ? std::min(requestedRate, displayRate) : requestedRate;
    }

    constexpr bool requiresFrameMatching(bool stablePresentation,
        bool raytracingEnabled, uint32_t targetRate, uint32_t originalRate)
    {
        // Matching supplies fractional pictures (or ray-traced velocities).
        // Native output does not need it in races, results, or menus. An
        // unresolved source rate cannot authorize fractional pictures either.
        return raytracingEnabled || (stablePresentation ?
            ((originalRate > 0) && (targetRate > originalRate)) :
            (targetRate > 0));
    }

    // Process-start A/B switch. This changes host presentation only and never
    // changes the guest clock. Read once before the queues start using it.
    inline bool stablePresentationEnabled() {
        static const bool enabled = [] {
#ifdef _MSC_VER
            char *value = nullptr;
            std::size_t length = 0;
            _dupenv_s(&value, &length, "RR64_STABLE_PRESENTATION");
            const bool selected = (value == nullptr) || (value[0] != '0');
            std::free(value);
            return selected;
#else
            const char *value = std::getenv("RR64_STABLE_PRESENTATION");
            return (value == nullptr) || (value[0] != '0');
#endif
        }();
        return enabled;
    }

    constexpr bool usePresentWait(bool supported, bool d3d12,
        bool vsync, bool softwarePacing, bool stablePresentation)
    {
        // Present(1) owns D3D12 VSync. The software clock owns tearing output.
        // Neither path may acquire a second DXGI frame-latency gate.
        return supported && !(d3d12 &&
            (softwarePacing || (stablePresentation && vsync)));
    }

    constexpr bool useSoftwarePacing(bool stablePresentation, bool d3d12,
        bool vulkan, bool vsync, uint32_t targetRate, uint32_t displayRate,
        uint32_t originalRate, bool hasPreviousPresent)
    {
        // Preserve the legacy comparison path and D3D12's existing pacing.
        // FIFO owns Vulkan's cadence when the cap reaches the display rate.
        // A lower cap (or unknown refresh) still needs a software deadline.
        if (d3d12 && vsync) return false;
        if (stablePresentation && vulkan && vsync && displayRate > 0 &&
            targetRate >= displayRate) return false;
        return targetRate > 0 &&
            (stablePresentation || (hasPreviousPresent && targetRate > originalRate));
    }

    constexpr bool waitBeforeSoftwarePacing(bool stablePresentation, bool vulkan) {
        // Queue throttling consumes the current frame's budget; it must not
        // be stacked after the software deadline once the image is ready.
        return stablePresentation && vulkan;
    }

    enum class InterpolationTargetSelection : uint32_t {
        Unavailable = 0,
        StockFlag = 1,
        PresentationHistory = 2
    };

    struct PresentationTargetIdentity {
        uint32_t address = 0;
        uint16_t width = 0;
        uint8_t siz = 0;

        constexpr bool valid() const {
            return (address != 0) && (width != 0);
        }

        constexpr bool operator==(const PresentationTargetIdentity &rhs) const {
            return (address == rhs.address) && (width == rhs.width) &&
                (siz == rhs.siz);
        }
    };

    // The VI-selected buffer can differ from the next buffer being drawn.
    // Keep a bounded exact history of VI-selected targets so native and
    // generated images can retain their own pixels. This history cannot
    // authorize an arbitrary off-screen target or establish its source rate.
    class PresentationTargetHistory {
    public:
        static constexpr std::size_t Capacity = 8;

        constexpr void clear() {
            entries = {};
            entryCount = 0;
        }

        constexpr void record(PresentationTargetIdentity target) {
            if (!target.valid()) {
                return;
            }

            std::size_t existing = entryCount;
            for (std::size_t index = 0; index < entryCount; index++) {
                if (entries[index] == target) {
                    existing = index;
                    break;
                }
            }

            if (existing < entryCount) {
                for (std::size_t index = existing + 1; index < entryCount; index++) {
                    entries[index - 1] = entries[index];
                }
                entryCount--;
            }
            else if (entryCount == Capacity) {
                for (std::size_t index = 1; index < entryCount; index++) {
                    entries[index - 1] = entries[index];
                }
                entryCount--;
            }

            entries[entryCount++] = target;
        }

        constexpr void eraseAddress(uint32_t address) {
            std::size_t kept = 0;
            for (std::size_t index = 0; index < entryCount; index++) {
                if (entries[index].address != address) {
                    entries[kept++] = entries[index];
                }
            }
            entryCount = kept;
        }

        constexpr bool contains(PresentationTargetIdentity target) const {
            if (!target.valid()) {
                return false;
            }

            for (std::size_t index = 0; index < entryCount; index++) {
                if (entries[index] == target) {
                    return true;
                }
            }

            return false;
        }

        constexpr std::size_t size() const {
            return entryCount;
        }

    private:
        std::array<PresentationTargetIdentity, Capacity> entries{};
        std::size_t entryCount = 0;
    };

    struct DeadlineDecision {
        int64_t deadlineNanoseconds = 0;
        bool rebased = false;
    };

    // The old presenter derived every wait from whichever frame happened to
    // reach Present last. When its source-rate guess changed, pacing toggled
    // on and off and a missed interval could be followed by an uneven catch-up
    // interval. This coordinator owns one absolute deadline stream. Preserve
    // that phase through tiny wake/submission errors rather than adding each
    // error to every future interval. A real stall still discards timing debt;
    // a phase correction can shorten an interval by at most 1% (and 250 us).
    class StableDeadlinePacer {
    public:
        constexpr void reset() {
            initialized = false;
            scheduledRate = 0;
            deadlineNanoseconds = 0;
        }

        constexpr DeadlineDecision schedule(
            int64_t nowNanoseconds,
            uint32_t targetRate)
        {
            if (targetRate == 0) {
                reset();
                return { nowNanoseconds, true };
            }

            const int64_t periodNanoseconds =
                1'000'000'000LL / static_cast<int64_t>(targetRate);
            if (!initialized || (scheduledRate != targetRate)) {
                initialized = true;
                scheduledRate = targetRate;
                deadlineNanoseconds = nowNanoseconds;
                return { deadlineNanoseconds, true };
            }

            const int64_t plannedDeadline =
                deadlineNanoseconds + periodNanoseconds;
            if (nowNanoseconds - plannedDeadline > phaseCorrectionTolerance()) {
                deadlineNanoseconds = nowNanoseconds;
                return { deadlineNanoseconds, true };
            }

            // A slightly expired deadline is intentional: the caller proceeds
            // immediately, then the next frame resumes the original phase.
            deadlineNanoseconds = plannedDeadline;
            return { deadlineNanoseconds, false };
        }

        // schedule() cannot see a late timer wake or work between the wait and
        // submission. Apply the same small-error bound here; rebasing on every
        // positive nanosecond would turn this back into a drifting relative
        // timer. Larger delays get a full interval before the next submission.
        constexpr void recordPresent(int64_t actualNanoseconds) {
            if (initialized &&
                actualNanoseconds - deadlineNanoseconds > phaseCorrectionTolerance()) {
                deadlineNanoseconds = actualNanoseconds;
            }
        }

    private:
        constexpr int64_t phaseCorrectionTolerance() const {
            // This is a host scheduling allowance, not a new game/frame rate.
            // At 60 Hz at most 166.7 us can be recovered on the next interval;
            // lower rates never accumulate a correction beyond 250 us.
            return std::min<int64_t>(250'000,
                (1'000'000'000LL / static_cast<int64_t>(scheduledRate)) / 100);
        }

        bool initialized = false;
        uint32_t scheduledRate = 0;
        int64_t deadlineNanoseconds = 0;
    };

    // One source interval consumes its output slots even when matching fails.
    // Keep only the fractional remainder: eligibility cannot restart 117/60
    // at one image on every fallback, and long sessions cannot grow the clock.
    class RationalFrameCadence {
    public:
        constexpr void reset() {
            target = source = remainder = previousRemainder = 0;
        }

        constexpr uint32_t advance(uint32_t targetRate, uint32_t sourceRate) {
            if (target != targetRate || source != sourceRate) {
                reset();
                target = targetRate;
                source = sourceRate;
            }
            if (source == 0 || target <= source) {
                remainder = previousRemainder = 0;
                return 1;
            }
            previousRemainder = remainder;
            const uint64_t ticks = uint64_t(remainder) + target;
            remainder = uint32_t(ticks % source);
            return uint32_t(std::min<uint64_t>(ticks / source, MaximumCadenceFrames));
        }

        // Index zero is the previous weight; index one is the first output.
        constexpr float interpolationWeight(uint32_t index) const {
            return target == 0 ? 1.0f : std::clamp(
                float(int64_t(source) * index - previousRemainder) / float(target),
                0.0f, 1.0f);
        }

    private:
        uint32_t target = 0, source = 0;
        uint32_t remainder = 0, previousRemainder = 0;
    };

    // Only immutable owned images can be resampled to the presenter's slots.
    // Include the latest image when a skipped VI leaves fewer slots than images.
    constexpr uint32_t ownedPresentationImageIndex(uint32_t outputIndex,
        uint32_t outputCount, uint32_t imageCount)
    {
        return outputCount == 0 || imageCount == 0 ? 0u :
            std::min(uint32_t((uint64_t(outputIndex + 1u) * imageCount - 1u) /
                outputCount), imageCount - 1u);
    }

    // Rational targets alternate floor/ceil frame counts.
    constexpr bool validGeneratedFrameCount(uint32_t target, uint32_t source, uint32_t count) {
        if (!source || target <= source || !count || count > MaximumCadenceFrames) return false;
        const uint64_t ceiling = (uint64_t(target) + source - 1) / source;
        return ceiling <= MaximumCadenceFrames &&
            (count == target / source || count == ceiling);
    }

    // A known lower-rate source can repeat its selected completed image at an
    // integer output cadence when interpolation is unavailable. Equal or
    // unresolved source rates require only one native image.
    constexpr uint32_t exactCadenceFrameCount(
        uint32_t targetRate,
        uint32_t originalRate)
    {
        if ((targetRate == 0) || (originalRate == 0) ||
            (targetRate <= originalRate) || ((targetRate % originalRate) != 0))
        {
            return 1;
        }

        return std::clamp(
            targetRate / originalRate,
            uint32_t(1),
            MaximumCadenceFrames);
    }

    constexpr uint32_t presentationFrameCount(
        bool interpolationAvailable,
        uint32_t generatedFrameCount,
        uint32_t targetRate,
        uint32_t originalRate)
    {
        if (interpolationAvailable && (generatedFrameCount > 0)) {
            return std::min(generatedFrameCount, MaximumCadenceFrames);
        }

        return exactCadenceFrameCount(targetRate, originalRate);
    }

    // Legacy queue policy: clock predictions do not abandon generated frames.
    // The caller supplies whether another workload supersedes the current one.
    constexpr bool shouldAbandonRemainingGeneratedFrames(
        bool newerWorkloadPending)
    {
        return newerWorkloadPending;
    }

    constexpr bool allowPresentationHistoryTarget(
        bool raceActive,
        uint32_t targetRate,
        uint32_t /* originalRate */,
        bool wasActuallyPresented)
    {
        // Retaining an exact selected target is useful for native images too.
        // Source cadence controls image count separately from target admission.
        return raceActive && (targetRate > 0) && wasActuallyPresented;
    }
}
