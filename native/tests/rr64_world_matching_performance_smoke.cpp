#include "hle/rt64_workload_queue.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <cstring>
using namespace RT64;
static void mix(std::uint64_t &digest, std::uint64_t value) { digest=(digest^value)*1099511628211ull; }
static void real(std::uint64_t &digest,float f) { unsigned b;std::memcpy(&b,&f,sizeof(b));mix(digest,b); }
template<class Mapping> static void mapped_digest(std::uint64_t &digest,const Mapping &m,const hlslpp::float4x4 &identity) {
    mix(digest,m.mapped);mix(digest,m.prevTransformIndex);
    const auto &r=m.rigidBody;
    for(unsigned i=0;i<3;++i)real(digest,r.linearVelocity[i]);
    real(digest,r.angularVelocity);mix(digest,r.transformIndex);
    mix(digest,r.lerpTranslation);mix(digest,r.lerpRotation);mix(digest,r.lerpScale);mix(digest,r.lerpSkew);mix(digest,r.lerpPerspective);mix(digest,r.lerpDecompose);
    for(const auto &t:r.transforms){mix(digest,t.valid);mix(digest,t.coordinateFlip);}
    const auto midpoint=r.lerp(0.5f,identity,identity,false);
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)real(digest,midpoint[row][col]);
}
#include "fixtures/rr64_matching_workload_fixture.inc"
int main(int argc,char **argv){
    unsigned count=argc>1?std::strtoul(argv[1],nullptr,10):2000;
    unsigned triangles=argc>2?std::strtoul(argv[2],nullptr,10):80;
    unsigned mode=argc>3?std::strtoul(argv[3],nullptr,10):0;
    // 0: repeated immutable submissions; 1: uncached control; 2: a new current
    // submission per pair, with the previous certificate retained (steady race).
    unsigned reuse=argc>4?std::strtoul(argv[4],nullptr,10):0;
    auto q=std::make_unique<WorkloadQueue>();seed(q->workloads[0],count,triangles,mode);seed(q->workloads[1],count,triangles,mode);
    if(mode==5)std::swap(q->workloads[1].drawData.faceIndices[1],q->workloads[1].drawData.faceIndices[2]);
    if(mode==7||mode==14)for(float &p:q->workloads[1].drawData.posFloats)if(p==0.0f)p=-0.0f;
    if(mode==13)q->workloads[1].drawData.posFloats[1]+=1.0f;
    std::vector<double> times;unsigned reasons=0;std::uint64_t digest=0;
    for(unsigned repeat=0;repeat<35;++repeat){
        if(reuse==3) {
            seed(q->workloads[0],count,triangles,mode);
            seed(q->workloads[1],count,triangles,mode);
            // Same queue slots and dimensions, different connectivity. Final
            // iteration corresponds to mode 5, interleaved with valid frames.
            if(repeat%2==0)std::swap(q->workloads[1].drawData.faceIndices[1],q->workloads[1].drawData.faceIndices[2]);
        }
        if(mode>=19&&mode<=22){
            auto &data=q->workloads[1].drawData;
            for(auto &tile:data.rdpTiles){tile.uls=1;tile.lrs=17;}
            for(auto &look:data.rspLookAt)look.x={0.8f,0.2f,0};
        }
        if(reuse) for(auto &certificate:q->workloads[1].rr64GeometryCache)certificate.reset();
        if(reuse==1) for(auto &certificate:q->workloads[0].rr64GeometryCache)certificate.reset();
        GameFrame previous,current;previous.workloads={0};current.workloads={1};
        previous.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);current.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);
        GameScene ps,cs;for(unsigned p=0;p<q->workloads[0].fbPairs[0].projectionCount;++p){ps.projections.push_back({0,0,p});cs.projections.push_back({1,0,p});}
        previous.perspectiveScenes.push_back(ps);current.perspectiveScenes.push_back(cs);
        bool velocity=false,tiles=false,look=false;auto start=std::chrono::steady_clock::now();
        current.match(nullptr,*q,previous,nullptr,velocity,tiles,look);
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(repeat>=5)times.push_back(ms);
        if(velocity||(mode<19&&(tiles||look)))return 2;
        if((mode==19||mode==22)&&!tiles)return 4;
        if(mode==20&&!look)return 4;
        if(mode==21&&(!tiles||!look))return 4;
        reasons=current.rr64GeometryRejectionReasons;digest=1469598103934665603ull;
        const auto identity=hlslpp::float4x4::identity();
        for(const auto &t:current.frameMap.workloads[1].transforms)mapped_digest(digest,t,identity);
        for(const auto &t:current.frameMap.workloads[1].viewProjections)mapped_digest(digest,t,identity);
        for(bool mapped:current.frameMap.workloads[1].prevTransformsMapped)mix(digest,mapped);
        for(const auto &t:current.frameMap.workloads[1].tiles){
            mix(digest,t.mapped);real(digest,t.deltaUls);real(digest,t.deltaUlt);real(digest,t.deltaLrs);real(digest,t.deltaLrt);
            real(digest,t.prevUls);real(digest,t.prevUlt);real(digest,t.prevLrs);real(digest,t.prevLrt);
        }
        for(const auto &l:current.frameMap.workloads[1].lookAt){
            mix(digest,l.mapped);for(unsigned i=0;i<3;++i){real(digest,l.deltaX[i]);real(digest,l.deltaY[i]);}
        }
        for(bool mapped:current.frameMap.workloads[1].prevTilesMapped)mix(digest,mapped);
        for(bool mapped:current.frameMap.workloads[1].prevLookAtMapped)mix(digest,mapped);
        mix(digest,current.rr64InterpolationCompatible);mix(digest,current.matched);mix(digest,velocity);mix(digest,tiles);mix(digest,look);
    }
    std::sort(times.begin(),times.end());
    std::printf("transforms=%u triangles-per-transform=%u mode=%u median_ms=%.6f p90_ms=%.6f reasons=%u digest=%llu\n",count,triangles,mode,times[times.size()/2],times[times.size()*9/10],reasons,(unsigned long long)digest);
    // Reusing the same workload/allocations must retire both certificate kinds.
    // Weak ownership also detects an accidental global cache retaining evidence.
    for(unsigned w=0;w<2;++w) {
        std::weak_ptr<const RR64WorkloadGeometry> weak=q->workloads[w].rr64GeometryCache[0];
        q->workloads[w].resetDrawData();
        if(q->workloads[w].rr64GeometryCache[0] || q->workloads[w].rr64GeometryCache[1] || !weak.expired())return 3;
    }
    return mode==0&&reuse!=3&&reasons?1:0;
}
