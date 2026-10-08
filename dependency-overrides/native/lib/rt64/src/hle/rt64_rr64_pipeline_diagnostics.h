// Optional observations only: never use these probes to authorize rendering.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" void rr64_record_pipeline_stage(unsigned int stage,
    unsigned long long nanoseconds);
extern "C" void rr64_record_pipeline_resources(unsigned long long targets,
    unsigned long long targetBytes, unsigned long long batches,
    unsigned long long batchBytes, unsigned long long textures,
    unsigned long long retiredTextures, unsigned long long reuseHits,
    unsigned long long reuseMisses, unsigned long long recycledBytes, unsigned long long recycledImages);

namespace RT64::RR64PipelineDiagnostics {
    inline bool enabled() {
        // Match the application's process-start opt-in. Disabled probes must
        // not read the clock or prepare expensive observation data.
        static const bool value = [] {
            const char* setting = std::getenv("RR64_DIAGNOSTICS");
            return setting && std::strcmp(setting, "1") == 0;
        }();
        return value;
    }

    inline bool preparationTraceEnabled() {
        static const bool optIn = [] {
            const char *setting = std::getenv("RR64_PREPARATION_TRACE");
            return setting && std::strcmp(setting, "1") == 0;
        }();
        return enabled() && optIn;
    }

    enum class Stage : unsigned int {
        FullSync, ProducerWaitPresent, PresentWaitProducer, Matching,
        RenderTotal, RenderLock, WorkerLock, UploadWait, SubmitWait,
        GpuCommands, CopyTotal, CopyAllocation, PresentTotal, DisplayList,
        GuestUpdate, FullSyncTiles, FullSyncParameters, FullSyncUpload,
        FullSyncUploadWait, FullSyncGpuWait, FullSyncTextureWait,
        FullSyncAdvance, RspVertices, RspTriangles, RspTriangleBatch, GeometryCollect, GeometryIntern,
        GeometryCertify, GeometryCoherence, MatchScene, GeometryHash, GeometryReuse, PresentThrottleWait, CheckRDRAM,
        FullSyncUploadResizeRanges, FullSyncUploadDrawSubmit, FullSyncUploadOutputBuffers,
        FullSyncUploadTransformSubmit, BufferUploadSubmitLock, BufferUploadResourceUpdate, Count
    };

    inline bool stageEnabled(Stage stage) {
        if (!enabled()) { return false; }
        switch (stage) {
        case Stage::FullSyncUploadResizeRanges:
        case Stage::FullSyncUploadDrawSubmit:
        case Stage::FullSyncUploadOutputBuffers:
        case Stage::FullSyncUploadTransformSubmit:
        case Stage::BufferUploadSubmitLock:
        case Stage::BufferUploadResourceUpdate:
            return preparationTraceEnabled();
        default:
            break;
        }
        if (stage != Stage::RspVertices && stage != Stage::RspTriangles && stage != Stage::RspTriangleBatch) { return true; }
        // Per-packet probes can execute millions of times per report. Keep them
        // separate from ordinary frame-stage timing to avoid skewing CPU tests.
        static const bool packets = [] {
            const char *value = std::getenv("RR64_PACKET_TIMING");
            return value && std::strcmp(value, "1") == 0;
        }();
        return packets;
    }

    inline constexpr const char* Names[] = {
        "full-sync", "producer-wait-present", "present-wait-producer", "matching",
        "render-total", "render-lock", "worker-lock", "upload-wait", "submit-wait",
        "gpu-commands", "copy-total", "copy-allocation", "present-total",
        "display-list", "guest-update", "full-sync-tiles", "full-sync-parameters",
        "full-sync-upload", "full-sync-upload-wait", "full-sync-gpu-wait",
        "full-sync-texture-wait", "full-sync-advance",
        "rsp-vertices", "rsp-triangles-inclusive", "rsp-triangle-batch-inclusive",
        "geometry-collect", "geometry-intern", "geometry-certify", "geometry-coherence",
        "match-scene-inclusive", "geometry-hash", "geometry-reuse-copy", "present-throttle-wait", "check-rdram",
        "full-sync-upload-resize-ranges", "full-sync-upload-draw-submit", "full-sync-upload-output-buffers",
        "full-sync-upload-transform-submit", "buffer-upload-submit-lock", "buffer-upload-resource-update"
    };
    static_assert(sizeof(Names) / sizeof(Names[0]) == static_cast<unsigned int>(Stage::Count));
}

namespace RT64::RR64PreparationTrace {
    constexpr unsigned Capacity = 512;
    constexpr uint64_t SlowThresholdNs = 12000000;
    struct Record {
        uint64_t firstWriter, lastWriter, startNs, endNs, totalNs, ordinal;
        uint32_t displayListAddress;
        std::array<uint64_t, static_cast<unsigned>(RR64PipelineDiagnostics::Stage::Count)> stageNs;
        // Pair capacities count once per pair; each pair owns two GPU resources.
        uint64_t bufferPairGrowthCount, bufferPairOldCapacityBytes, bufferPairNewCapacityBytes;
        uint64_t outputBufferGrowthCount, outputBufferOldCapacityBytes, outputBufferNewCapacityBytes;
    };
    struct Slot { Record record{}; std::atomic<bool> ready{false}; };
    struct Buffer {
        // Multiple producers, one periodic log consumer. Reuse a slot only after
        // its consumer returns; a delayed producer keeps its place in the ring.
        // No producer locks, allocation or file formatting.
        std::array<Slot, Capacity> slots{};
        std::atomic<uint64_t> attempts{0}, dropped{0};
        std::atomic<uint64_t> reserved{0}, consumed{0};

        int claim(uint64_t elapsedNs, uint64_t &ordinal) {
            ordinal = attempts.fetch_add(1, std::memory_order_relaxed);
            // Retain slow intervals, the first eight, and a normal sample every
            // 128 intervals. dropped counts selected records lost to capacity.
            if (elapsedNs < SlowThresholdNs && ordinal >= 8 && ordinal % 128 != 0) { return -1; }
            for (;;) {
                // Read consumed first: acquiring the released slot also orders
                // its previous reservation before the following reserved load.
                const uint64_t drained = consumed.load(std::memory_order_acquire);
                uint64_t index = reserved.load(std::memory_order_relaxed);
                if (index - drained >= Capacity) {
                    dropped.fetch_add(1, std::memory_order_relaxed);
                    return -1;
                }
                if (reserved.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {
                    return static_cast<int>(index % Capacity);
                }
            }
        }
        void publish(unsigned index, const Record &record) {
            slots[index].record = record;
            slots[index].ready.store(true, std::memory_order_release);
        }
        template<class Consumer> void drain(Consumer consume) {
            uint64_t drained = consumed.load(std::memory_order_relaxed);
            const uint64_t end = reserved.load(std::memory_order_relaxed);
            while (drained != end) {
                auto &slot = slots[drained % Capacity];
                if (!slot.ready.load(std::memory_order_acquire)) { break; }
                consume(slot.record);
                slot.ready.store(false, std::memory_order_relaxed);
                consumed.store(++drained, std::memory_order_release);
            }
        }
    };
    inline Buffer buffer;
    inline thread_local Record *activeRecord = nullptr;

    inline bool enabled() {
        return RR64PipelineDiagnostics::preparationTraceEnabled();
    }
    inline void recordStage(RR64PipelineDiagnostics::Stage stage, uint64_t ns) {
        const unsigned index = static_cast<unsigned>(stage);
        if (activeRecord && index < activeRecord->stageNs.size()) {
            activeRecord->stageNs[index] += ns;
        }
    }
    inline void recordBufferPairGrowth(uint64_t oldCapacity, uint64_t newCapacity) {
        if (!activeRecord) { return; }
        ++activeRecord->bufferPairGrowthCount;
        activeRecord->bufferPairOldCapacityBytes += oldCapacity;
        activeRecord->bufferPairNewCapacityBytes += newCapacity;
    }
    inline void recordOutputBufferGrowth(uint64_t oldCapacity, uint64_t newCapacity) {
        if (!activeRecord) { return; }
        ++activeRecord->outputBufferGrowthCount;
        activeRecord->outputBufferOldCapacityBytes += oldCapacity;
        activeRecord->outputBufferNewCapacityBytes += newCapacity;
    }

    class Scope {
    public:
        Scope(const uint64_t &workloadId, uint32_t displayListAddress)
            : workloadId(workloadId), active(enabled() && activeRecord == nullptr) {
            if (!active) { return; }
            record = {};
            record.firstWriter = workloadId + 1;
            record.displayListAddress = displayListAddress;
            record.startNs = nowNs();
            activeRecord = &record;
        }
        ~Scope() { finish(); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

        void finish() {
            if (!active) { return; }
            active = false;
            record.endNs = nowNs();
            record.totalNs = record.endNs - record.startNs;
            record.lastWriter = workloadId;
            activeRecord = nullptr;
            const int index = buffer.claim(record.totalNs, record.ordinal);
            if (index >= 0) { buffer.publish(static_cast<unsigned>(index), record); }
        }
    private:
        static uint64_t nowNs() {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
        const uint64_t &workloadId;
        bool active;
        Record record;
    };
    // Writer IDs advance at each fullSync. firstWriter > lastWriter means the
    // display list submitted none; otherwise the inclusive range can span more
    // than one workload. Stage sums overlap and include waits, not just CPU work.
    // Only scopes ending on the interpreter thread contribute to this interval.
}

namespace RT64::RR64PipelineDiagnostics {
    class Scope {
    public:
        explicit Scope(Stage stage) : stage(stage), active(stageEnabled(stage)),
            start(active ? Clock::now() : Clock::time_point{}) { }
        ~Scope() { finish(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        void finish() {
            if (!active) { return; }
            active = false;
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            if (ns >= 0) {
                RR64PreparationTrace::recordStage(stage, static_cast<uint64_t>(ns));
                rr64_record_pipeline_stage(static_cast<unsigned int>(stage), static_cast<unsigned long long>(ns));
            }
        }
    private:
        using Clock = std::chrono::steady_clock;
        Stage stage;
        bool active;
        Clock::time_point start;
    };
}
