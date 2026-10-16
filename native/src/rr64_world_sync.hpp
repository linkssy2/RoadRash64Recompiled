#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace rr64::world_sync {
// Complete motion/basis state needs several sub-MTU batches. Commit only full
// snapshots; partial receipt must never retire half the visible traffic roster.
// Two records leave room for the fourteen timed-input cursors in the shared
// authority stamp while keeping every UDP fragment below the 1200-byte limit.
constexpr unsigned capacity=20, batch_size=2, batches=(capacity+batch_size-1)/batch_size;
// Pointer-free traffic observations. Native +4 is a race-scoped identity;
// allocation addresses and the compacted route roster index are not identities.
struct Traffic {
    std::uint32_t active=0,id=0,model=0,kind=0;
    std::array<float,3> position{},velocity{},angles{};
    std::uint32_t motion_valid=0;
    float road_distance=0;
    // Native A8..1A4: current/previous translation, velocity, angular state,
    // basis vectors and rotation matrices. 7731C initializes these as scalars.
    std::array<float,64> motion{};
    std::array<float,6> directions{}; // 310..324, no resource/scene pointers.
    bool operator==(const Traffic&) const = default;
};
template<std::size_t N> struct TrafficSnapshot {
    std::uint32_t round=0;
    std::uint64_t tick=0;
    std::array<Traffic,N> traffic{};
};
using Snapshot = TrafficSnapshot<capacity>;
struct Batch {
    std::uint32_t round=0,index=0;
    std::uint64_t tick=0;
    std::array<Traffic,batch_size> traffic{};
};
inline bool valid(const Traffic &v) {
    if(v.active>1) return false;
    if(!v.active) return true;
    if(v.motion_valid>1 || !std::isfinite(v.road_distance)) return false;
    for(float f:v.motion) if(!std::isfinite(f)) return false;
    for(float f:v.directions) if(!std::isfinite(f)) return false;
    if(v.kind<1 || v.kind>5 || v.model<0xD8 || (v.model>0x101 && v.model!=0x125 && v.model!=0x126)) return false;
    for(const auto &a:{v.position,v.velocity,v.angles})
        for(float f:a) if(!std::isfinite(f)) return false;
    return true;
}
template<std::size_t N> inline bool valid(const TrafficSnapshot<N> &s) {
    if(!s.round || !s.tick) return false;
    for(unsigned i=0;i<N;++i) {
        if(!valid(s.traffic[i])) return false;
        if(s.traffic[i].active) for(unsigned j=0;j<i;++j)
            if(s.traffic[j].active && s.traffic[i].id==s.traffic[j].id) return false;
    }
    return true;
}
// Commit complete rosters only. Missing packets retain the previous complete
// state; an old active record can never resurrect a removed traffic identity.
class Receiver {
    struct Pending {Snapshot state{};unsigned mask=0;};
    // Jitter can interleave several frames. A newer first batch must not
    // discard an older nearly complete frame and starve world presentation.
    std::array<Pending,8> pending_{};
    Snapshot committed_{};
    std::uint32_t round_=0;
public:
    void reset() {pending_={};committed_={};round_=0;}
    const Snapshot &snapshot() const {return committed_;}
    bool accept(const Batch &b,std::uint32_t round) {
        if(!round || b.round!=round || !b.tick || b.index>=batches) return false;
        for(const auto &v:b.traffic) if(!valid(v)) return false;
        for(unsigned i=0;i<batch_size;++i)
            if(b.index*batch_size+i>=capacity && b.traffic[i].active) return false;
        if(round_!=round) {reset();round_=round;}
        if(b.tick<=committed_.tick) return false;
        Pending *p=nullptr;
        for(auto &candidate:pending_) if(candidate.state.tick==b.tick) {p=&candidate;break;}
        if(!p) {
            p=&pending_[0];
            for(auto &candidate:pending_) if(candidate.state.tick<p->state.tick) p=&candidate;
            if(p->state.tick>b.tick) return false;
            *p={};p->state.round=round;p->state.tick=b.tick;
        }
        const unsigned bit=1u<<b.index,first=b.index*batch_size;
        if(p->mask&bit) return false;
        for(unsigned i=0;i<batch_size && first+i<capacity;++i) p->state.traffic[first+i]=b.traffic[i];
        p->mask|=bit;
        if(p->mask!=(1u<<batches)-1) return false;
        if(!valid(p->state)) {*p={};return false;}
        committed_=p->state;
        for(auto &old:pending_) if(old.state.tick<=committed_.tick) old={};
        return true;
    }
};
}
