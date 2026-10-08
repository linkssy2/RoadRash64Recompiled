// Bounded, opt-in observations. Producers never format text or perform file I/O.
// Slots are published once and never recycled, so delayed readers cannot observe
// overwritten records. Independent category budgets keep visibility churn from
// exhausting translation evidence. This data never controls rendering.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace RT64::RR64MatchingEvidence {
constexpr unsigned Categories = 4, Capacity = 64;
struct Record {
    uint64_t submission=0, workload=0, topologyHash=0;
    uint32_t category=0, world=UINT32_MAX, previousWorld=UINT32_MAX;
    uint32_t framebuffer=0, projection=0, view=UINT32_MAX;
    uint32_t id=UINT32_MAX, previousId=UINT32_MAX, idOccurrences=0, previousIdOccurrences=0;
    uint32_t positionPolicy=0, ordering=0, vertices=0, indices=0;
    // Submitted source addresses identify producer paths, not object lifetimes.
    // Mask bits distinguish a missing address from a legitimate zero address.
    uint32_t sourceMask=0, physicalAddress=0, segmentedAddress=0;
    uint32_t previousPhysicalAddress=0, previousSegmentedAddress=0;
    uint32_t viewId=UINT32_MAX, previousViewId=UINT32_MAX;
    // Category 3 records an emitted camera phase's immutable key, not a live
    // camera pose or proof that every frame in that phase is continuous.
    uint32_t cameraMarker=0, cameraRound=0, cameraLayout=0, cameraViews=0, cameraReplayKey=0, cameraValidMask=0;
    std::array<uint32_t,4> cameraModes{},cameraOwners{},cameraFlags{},cameraGenerations{},cameraBikes{},cameraRiders{};
    bool mapped=false, worldLerp=false, viewLerp=false;
    std::array<float,3> worldBefore{},worldAfter{},viewBefore{},viewAfter{};
};
template<class DrawData>
inline void captureSource(Record &record, const DrawData &data, uint32_t world, bool previous) {
    if(world<data.worldTransformPhysicalAddresses.size()) {
        (previous ? record.previousPhysicalAddress : record.physicalAddress)=data.worldTransformPhysicalAddresses[world];
        record.sourceMask |= previous ? 4u : 1u;
    }
    if(world<data.worldTransformSegmentedAddresses.size()) {
        (previous ? record.previousSegmentedAddress : record.segmentedAddress)=data.worldTransformSegmentedAddresses[world];
        record.sourceMask |= previous ? 8u : 2u;
    }
}
struct Slot { Record record; std::atomic<bool> ready{false}; };
struct Buffer {
    std::array<std::array<Slot,Capacity>,Categories> slots{};
    std::array<std::atomic<unsigned>,Categories> attempts{}, reserved{};
    std::array<unsigned,Categories> drained{}; // Single periodic log consumer.
    int claim(unsigned category) {
        if(category>=Categories || reserved[category].load(std::memory_order_relaxed)>=Capacity) return -1;
        unsigned n=attempts[category].fetch_add(1,std::memory_order_relaxed);
        // Camera phases are already change-only events. Preserve every early
        // phase up to the same hard cap; sample the per-frame geometry events.
        if(category!=3 && n>=8 && n%128!=0) return -1;
        unsigned index=reserved[category].fetch_add(1,std::memory_order_relaxed);
        return index<Capacity ? int(index) : -1;
    }
    void publish(unsigned category,unsigned index,const Record &record) {
        slots[category][index].record=record;
        slots[category][index].ready.store(true,std::memory_order_release);
    }
    template<class Consumer> void drain(Consumer consume) {
        for(unsigned c=0;c<Categories;c++) {
            while(drained[c]<Capacity && slots[c][drained[c]].ready.load(std::memory_order_acquire)) {
                consume(slots[c][drained[c]].record); ++drained[c];
            }
        }
    }
};
inline Buffer buffer;
inline bool enabled() {
    static const bool value=[] { const char *p=std::getenv("RR64_MATCH_EVIDENCE"); return p && std::strcmp(p,"1")==0; }();
    return value;
}
}
