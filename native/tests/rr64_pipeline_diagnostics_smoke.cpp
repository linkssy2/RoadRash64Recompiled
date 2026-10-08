#include "hle/rt64_rr64_pipeline_diagnostics.h"
#include "hle/rt64_rr64_matching_evidence.h"
#include "hle/rt64_rr64_command_profile.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

static unsigned calls = 0;
static unsigned invalid = 0;
static std::array<uint64_t, static_cast<unsigned>(RT64::RR64PipelineDiagnostics::Stage::Count)> recorded_ns{};
extern "C" void rr64_record_pipeline_stage(unsigned stage, unsigned long long ns) {
    ++calls;
    if (stage >= static_cast<unsigned>(RT64::RR64PipelineDiagnostics::Stage::Count)) ++invalid;
    else recorded_ns[stage] += ns;
}

static bool preparation_trace(bool diagnostics) {
    namespace Trace = RT64::RR64PreparationTrace;
    namespace Pipeline = RT64::RR64PipelineDiagnostics;
    using Stage = Pipeline::Stage;
    unsigned failures = 0;
    const auto check = [&](bool valid, const char *message) {
        if (!valid) { ++failures; std::fprintf(stderr, "FAIL preparation trace: %s\n", message); }
    };
    const char *setting = std::getenv("RR64_PREPARATION_TRACE");
    const bool expected = diagnostics && setting && std::strcmp(setting, "1") == 0;
    check(Trace::enabled() == expected, "both diagnostic switches gate collection");
    for (Stage stage : { Stage::FullSyncUploadResizeRanges, Stage::FullSyncUploadDrawSubmit,
            Stage::FullSyncUploadOutputBuffers, Stage::FullSyncUploadTransformSubmit,
            Stage::BufferUploadSubmitLock, Stage::BufferUploadResourceUpdate }) {
        check(Pipeline::stageEnabled(stage) == expected, "upload detail uses both switches before reading the clock");
    }
    std::vector<Trace::Record> records;
    const auto drain = [&] { Trace::buffer.drain([&](const auto &record) { records.push_back(record); }); };
    const unsigned parameters = static_cast<unsigned>(Stage::FullSyncParameters);
    const auto before_parameters = recorded_ns[parameters];
    uint64_t writer = 100;
    {
        Trace::Scope scope(writer, 0x123450u);
        check(bool(Trace::activeRecord) == expected, "outer scope owns this thread's record");
        Trace::recordStage(Stage::FullSync, 7);
        Trace::recordStage(Stage::FullSyncGpuWait, 3);
        Trace::recordBufferPairGrowth(0, 256);
        Trace::recordOutputBufferGrowth(0, 512);
        {
            Trace::Scope nested(writer, 0xabcdefu);
            Trace::recordStage(Stage::CheckRDRAM, 5);
            Trace::recordBufferPairGrowth(256, 768);
            Trace::recordOutputBufferGrowth(512, 1536);
        }
        check(bool(Trace::activeRecord) == expected, "nested destruction preserves outer scope");
        std::thread worker([] {
            Trace::recordStage(Stage::FullSync, 99999);
            Trace::recordStage(Stage::CheckRDRAM, 88888);
            Trace::recordBufferPairGrowth(99999, 999999);
            Trace::recordOutputBufferGrowth(99999, 999999);
        });
        worker.join();
        Trace::recordStage(Stage::FullSync, 11);
        const auto before_calls = calls;
        {
            Pipeline::Scope measured(Stage::FullSyncParameters);
            measured.finish();
            measured.finish();
        }
        check(calls == before_calls + (diagnostics ? 1u : 0u), "explicit pipeline finish records once");
        const auto before_detail_calls = calls;
        {
            Pipeline::Scope measured(Stage::FullSyncUploadDrawSubmit);
            measured.finish();
            measured.finish();
        }
        check(calls == before_detail_calls + (expected ? 1u : 0u), "upload detail records once only with preparation capture");
        writer = 102; // Two full-sync submissions during one interpreted list.
        scope.finish();
        scope.finish();
        check(Trace::activeRecord == nullptr, "explicit trace finish releases thread record");
        Trace::recordStage(Stage::FullSync, 77777);
        Trace::recordBufferPairGrowth(77777, 777777);
        Trace::recordOutputBufferGrowth(77777, 777777);
    }
    writer = 200;
    { Trace::Scope no_submission(writer, 0x200u); }
    writer = 300;
    { Trace::Scope one_submission(writer, 0x300u); writer = 301; }
    drain();
    drain();
    check(records.size() == (expected ? 3u : 0u), "scope publication is single-shot and disabled mode is empty");
    check(Trace::buffer.attempts.load() == (expected ? 3u : 0u) && !Trace::buffer.dropped.load(),
          "disabled scopes do not consume evidence budget");
    if (records.size() == 3) {
        const auto &record = records[0];
        check(record.firstWriter == 101 && record.lastWriter == 102 && record.displayListAddress == 0x123450u,
              "writer range includes every full-sync submission, with original list address");
        check(record.endNs >= record.startNs && record.totalNs == record.endNs - record.startNs,
              "record interval uses consistent monotonic timestamps");
        check(record.stageNs[static_cast<unsigned>(Stage::FullSync)] == 18 &&
              record.stageNs[static_cast<unsigned>(Stage::FullSyncGpuWait)] == 3 &&
              record.stageNs[static_cast<unsigned>(Stage::CheckRDRAM)] == 5,
              "same-thread nested stage sums exclude other workers and post-finish events");
        check(record.stageNs[parameters] == recorded_ns[parameters] - before_parameters,
              "real pipeline scope contributes exactly its recorded elapsed time once");
        check(record.bufferPairGrowthCount == 2 && record.bufferPairOldCapacityBytes == 256 &&
              record.bufferPairNewCapacityBytes == 1024 && record.outputBufferGrowthCount == 2 &&
              record.outputBufferOldCapacityBytes == 512 && record.outputBufferNewCapacityBytes == 2048,
              "growth counts and capacities exclude other threads and post-finish events");
        check(!records[1].bufferPairGrowthCount && !records[1].bufferPairOldCapacityBytes &&
              !records[1].bufferPairNewCapacityBytes && !records[1].outputBufferGrowthCount &&
              !records[1].outputBufferOldCapacityBytes && !records[1].outputBufferNewCapacityBytes &&
              !records[2].bufferPairGrowthCount && !records[2].outputBufferGrowthCount,
              "new preparation scopes start with zero growth counters");
        check(records[1].firstWriter == 201 && records[1].lastWriter == 200 &&
              records[2].firstWriter == 301 && records[2].lastWriter == 301,
              "empty and single-submission writer ranges remain distinguishable");
    }

    // Selection uses supplied durations, so threshold coverage never sleeps or
    // depends on scheduler timing. Separate local buffers leave the live budget intact.
    auto sampled = std::make_unique<Trace::Buffer>();
    unsigned selected = 0;
    for (unsigned attempt = 0; attempt < 258; ++attempt) {
        const uint64_t elapsed = attempt == 9 ? Trace::SlowThresholdNs :
            attempt == 10 ? Trace::SlowThresholdNs - 1 :
            attempt == 11 ? Trace::SlowThresholdNs + 1 : 0;
        uint64_t ordinal = 0;
        const int slot = sampled->claim(elapsed, ordinal);
        check(ordinal == attempt, "record ordinal preserves every attempted interval, including unsampled ones");
        const bool retained = attempt < 8 || attempt % 128 == 0 || attempt == 9 || attempt == 11;
        check(retained ? slot == int(selected++) : slot == -1,
              "first eight, periodic samples, and inclusive slow threshold are deterministic");
    }
    check(sampled->attempts.load() == 258 && sampled->reserved.load() == selected && !sampled->dropped.load(),
          "ordinary sampling does not count as capacity loss");
    auto bounded = std::make_unique<Trace::Buffer>();
    Trace::Record record{};
    unsigned consumed = 0;
    for (unsigned i = 0; i < Trace::Capacity; ++i) {
        uint64_t ordinal = 0;
        const int slot = bounded->claim(Trace::SlowThresholdNs, ordinal);
        check(slot == int(i), "slow records claim the complete fixed capacity");
        record.firstWriter = i;
        if (i && i + 1 < Trace::Capacity && slot >= 0) bounded->publish(unsigned(slot), record);
    }
    const auto consume = [&](const Trace::Record &r) {
        check(r.firstWriter == consumed++, "published records preserve claim order and payload");
    };
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t ordinal = 0;
        check(bounded->claim(Trace::SlowThresholdNs, ordinal) == -1, "undrained full buffer reports capacity loss");
    }
    bounded->drain(consume);
    check(!consumed, "unpublished head hides later records from delayed readers");
    record.firstWriter = 0;
    bounded->publish(0, record);
    bounded->drain(consume);
    check(consumed == Trace::Capacity - 1, "drain stops at the next unpublished slot");
    for (unsigned i = Trace::Capacity; i < 2 * Trace::Capacity - 1; ++i) {
        uint64_t ordinal = 0;
        const int slot = bounded->claim(Trace::SlowThresholdNs, ordinal);
        check(slot == int(i % Trace::Capacity), "consumed slots are reusable while the last producer is delayed");
        record.firstWriter = i;
        if (slot >= 0) bounded->publish(unsigned(slot), record);
    }
    bounded->drain(consume);
    check(consumed == Trace::Capacity - 1, "wrapped publication cannot replace or skip a delayed producer");
    uint64_t ordinal = 0;
    check(bounded->claim(Trace::SlowThresholdNs, ordinal) == -1, "delayed producer still owns its capacity slot");
    record.firstWriter = Trace::Capacity - 1;
    bounded->publish(Trace::Capacity - 1, record);
    bounded->drain(consume);
    check(consumed == 2 * Trace::Capacity - 1, "delayed publication releases the complete wrapped batch in order");
    for (unsigned batch = 0; batch < 2; ++batch) {
        for (unsigned i = 0; i < Trace::Capacity; ++i) {
            const int slot = bounded->claim(Trace::SlowThresholdNs, ordinal);
            record.firstWriter = (2 + batch) * Trace::Capacity - 1 + i;
            check(slot == int(record.firstWriter % Trace::Capacity), "draining restores the full fixed capacity each time");
            if (slot >= 0) bounded->publish(unsigned(slot), record);
        }
        bounded->drain(consume);
    }
    bounded->drain(consume);
    check(Trace::Capacity == 512 && consumed == 4 * Trace::Capacity - 1 && bounded->dropped.load() == 6 &&
          bounded->reserved.load() == consumed && bounded->attempts.load() == consumed + 6,
          "more than 512 cumulative records are consumed once; only actual capacity pressure reports loss");

    auto concurrent = std::make_unique<Trace::Buffer>();
    constexpr unsigned Producers = 4, PerProducer = 4096;
    std::array<std::thread, Producers> producers;
    std::atomic<unsigned> finished{0};
    std::vector<bool> seen(Producers * PerProducer, false);
    unsigned received = 0;
    for (auto &producer : producers) {
        producer = std::thread([&] {
            for (unsigned i = 0; i < PerProducer; ++i) {
                Trace::Record value{};
                const int slot = concurrent->claim(Trace::SlowThresholdNs, value.ordinal);
                value.firstWriter = value.ordinal;
                value.lastWriter = ~value.ordinal;
                if (slot >= 0) concurrent->publish(unsigned(slot), value);
                if (i % 16 == 0) std::this_thread::yield();
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    const auto receive = [&](const Trace::Record &value) {
        check(value.ordinal < seen.size() && !seen[value.ordinal] &&
              value.firstWriter == value.ordinal && value.lastWriter == ~value.ordinal,
              "concurrent producers publish complete, unique payloads during reuse");
        if (value.ordinal < seen.size()) seen[value.ordinal] = true;
        ++received;
    };
    while (finished.load(std::memory_order_acquire) != Producers) {
        concurrent->drain(receive);
        std::this_thread::yield();
    }
    for (auto &producer : producers) producer.join();
    concurrent->drain(receive);
    check(concurrent->attempts.load() == Producers * PerProducer &&
          received == concurrent->reserved.load() && received + concurrent->dropped.load() == Producers * PerProducer,
          "concurrent recording and draining account for every selected interval");
    std::printf("preparation_trace_enabled=%u checks_failed=%u reusable_records=%u capacity_drops=%llu concurrent_records=%u concurrent_drops=%llu; writer ranges, thread isolation, stage sums, growth counters, sampling and capacity\n",
                expected, failures, consumed, static_cast<unsigned long long>(bounded->dropped.load()),
                received, static_cast<unsigned long long>(concurrent->dropped.load()));
    return failures == 0;
}

int main(int argc, char** argv) {
    using namespace RT64::RR64PipelineDiagnostics;
    static_assert(static_cast<unsigned>(Stage::CheckRDRAM) == 33, "existing stage IDs remain stable");
    static_assert(static_cast<unsigned>(Stage::FullSyncUploadResizeRanges) == 34, "upload detail stages are appended");
    if (argc < 2 || argc > 3) return 2;
    const bool expected = std::strcmp(argv[1], "enabled") == 0;
    if (enabled() != expected) return 3;
    {
        Scope outer(Stage::FullSync);
        { Scope inner(Stage::Matching); }
        if (calls != (expected ? 1u : 0u)) return 4;
        outer.finish();
        outer.finish(); // Explicit finish followed by destruction records once.
    }
    if (calls != (expected ? 2u : 0u) || invalid) return 5;
    const bool packets=argc>2 && std::strcmp(argv[2],"packets")==0;
    const char *preparation_setting = std::getenv("RR64_PREPARATION_TRACE");
    const bool preparation = preparation_setting && std::strcmp(preparation_setting, "1") == 0;
    for (unsigned i = 0; i < static_cast<unsigned>(Stage::Count); ++i) {
        Scope scope(static_cast<Stage>(i));
        if (!Names[i] || !Names[i][0]) return 6;
    }
    if (calls != (expected ? 2u + static_cast<unsigned>(Stage::Count) - (packets ? 0u : 3u) - (preparation ? 0u : 6u) : 0u) || invalid) return 7;
    std::printf("diagnostics_enabled=%u callbacks=%u nested_and_explicit_finish=passed\n", expected, calls);
    if (!preparation_trace(expected)) return 19;
    // A delayed producer must not expose incomplete records or block a different
    // category. Published records are consumed once; full buffers never overwrite.
    RT64::RR64MatchingEvidence::Buffer evidence;
    RT64::RR64MatchingEvidence::Record record;
    struct Sources { std::vector<unsigned> worldTransformPhysicalAddresses, worldTransformSegmentedAddresses; };
    Sources current{{0,0x00123450u},{0x06000040u}}, previous{{0x00765430u},{0x06000800u}};
    RT64::RR64MatchingEvidence::captureSource(record,current,0,false);
    RT64::RR64MatchingEvidence::captureSource(record,previous,0,true);
    if(record.sourceMask!=15 || record.physicalAddress!=0 || record.segmentedAddress!=0x06000040u ||
        record.previousPhysicalAddress!=0x00765430u || record.previousSegmentedAddress!=0x06000800u) return 15;
    record={};
    RT64::RR64MatchingEvidence::captureSource(record,current,1,false);
    RT64::RR64MatchingEvidence::captureSource(record,previous,99,true);
    if(record.sourceMask!=1 || record.physicalAddress!=0x00123450u || record.previousPhysicalAddress!=0) return 16;
    const int first=evidence.claim(0),second=evidence.claim(0),other=evidence.claim(1);
    if(first!=0 || second!=1 || other!=0) return 8;
    record.world=22; evidence.publish(0,second,record);
    record.world=33; evidence.publish(1,other,record);
    unsigned consumed=0;
    evidence.drain([&](const auto &r){if(r.world!=33) ++invalid; ++consumed;});
    if(consumed!=1 || invalid) return 9;
    record.world=11; evidence.publish(0,first,record);
    evidence.drain([&](const auto &r){if(r.world!=11 && r.world!=22) ++invalid; ++consumed;});
    evidence.drain([&](const auto &){++invalid;});
    if(consumed!=3 || invalid) return 10;
    for(unsigned i=0;i<10000;i++) {
        int slot=evidence.claim(2);
        if(slot>=0){record.world=unsigned(slot);evidence.publish(2,unsigned(slot),record);}
    }
    unsigned count=0;
    evidence.drain([&](const auto &r){if(r.world!=count++) ++invalid;});
    if(count!=RT64::RR64MatchingEvidence::Capacity || invalid || evidence.claim(2)!=-1) return 11;
    for(unsigned phase=0;phase<RT64::RR64MatchingEvidence::Capacity;++phase) {
        const int slot=evidence.claim(3);
        if(slot<0) return 17;
        record={};record.category=3;record.cameraMarker=0x484c0000u+phase;
        record.cameraViews=4;record.cameraGenerations={phase,phase+1,phase+2,phase+3};
        evidence.publish(3,unsigned(slot),record);
    }
    unsigned phases=0;
    evidence.drain([&](const auto &r){
        if(r.category!=3 || r.cameraMarker!=0x484c0000u+phases || r.cameraViews!=4 ||
            r.cameraGenerations[3]!=phases+3) ++invalid;
        ++phases;
    });
    if(phases!=RT64::RR64MatchingEvidence::Capacity || invalid || evidence.claim(3)!=-1) return 18;
    std::puts("PASS bounded matching evidence: independent budgets, delayed publication, no overwrite, single consumption");
    // Validate sampling frequency without relying on wall-clock timing values.
    uint32_t random=0x6d2b79f5u;unsigned selected=0;
    for(unsigned i=0;i<1048576;i++)selected+=RT64::RR64CommandProfile::select(random);
    if(selected<800 || selected>1250)return 12;
    const bool commandTiming=RT64::RR64CommandProfile::enabled();
    for(unsigned i=0;i<32768;i++) {
        RT64::RR64CommandProfile::Scope ordinary(0xda000000,0x64);
        RT64::RR64CommandProfile::Scope extended(0x64000030,0x64);
    }
    for(unsigned i=0;i<512;i++) {
        auto count=RT64::RR64CommandProfile::buckets[i].samples.load();
        if(commandTiming && (i==0xda || i==0x130)) {if(!count)return 13;}
        else if(count)return 14;
    }
    std::printf("PASS command sampler enabled=%u distribution=%u per 1048576, bucket routing correct\n",commandTiming,selected);
    return 0;
}
