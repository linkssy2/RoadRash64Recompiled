#include "hle/rt64_rr64_matching_preflight.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <regex>
using namespace RT64;
#include "fixtures/rr64_matching_workload_fixture.inc"
static void require(bool value, const char* message) { if(!value) { std::fprintf(stderr,"preflight FAIL: %s\n",message); std::exit(1); } }
static GameFrame frameFor(const WorkloadQueue& q, uint32_t slot) {
    GameFrame frame; frame.matched=false; frame.workloads={slot}; frame.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);
    GameScene scene;
    for(uint32_t p=0;p<q.workloads[slot].fbPairs[0].projectionCount;p++) scene.projections.push_back({slot,0,p});
    frame.perspectiveScenes.push_back(scene); return frame;
}
static void match(GameFrame& cur,const GameFrame& prev,WorkloadQueue& q) {
    bool v=false,t=false,l=false;cur.match(nullptr,q,prev,nullptr,v,t,l);
    require(!v&&!t&&!l,"static fixtures must not invoke GPU uploaders");
}
int main(int argc, char **argv) {
    auto q=std::make_unique<WorkloadQueue>();unsigned proven=0,deferred=0;
    for(unsigned a: {1u,2u,7u,32u}) for(unsigned b: {1u,2u,7u,32u}) for(unsigned mode: {0u,3u,4u,10u}) {
        seed(q->workloads[0],a,8,mode);seed(q->workloads[1],b,8,mode);
        auto prev=frameFor(*q,0),cur=frameFor(*q,1);
        auto skip=RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q);
        require(!RR64MatchingPreflight::unequalSingleSceneMembership(false,cur,prev,*q),"ineligible path must always defer");
        match(cur,prev,*q);
        if(skip) { require(!cur.rr64InterpolationCompatible && (cur.rr64GeometryRejectionReasons&4),"preflight must imply actual matcher membership rejection");proven++; }
        else deferred++;
    }
    seed(q->workloads[0],7,8,0);seed(q->workloads[1],8,8,0);
    auto prev=frameFor(*q,0),cur=frameFor(*q,1);
    require(RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"different membership must reject");
    // Allocated but unused matrix slots do not belong to the certificate.
    q->workloads[0].drawData.worldTransforms.resize(8);
    require(RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"unused matrix allocation must not equalize used membership");
    cur.perspectiveScenes.push_back(cur.perspectiveScenes.front());
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"multiple scenes must defer");
    cur=frameFor(*q,1);cur.workloads.push_back(0);
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"multiple workloads must defer");
    cur=frameFor(*q,1);cur.perspectiveScenes[0].projections[0].projectionIndex=999;
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"unknown projection must defer");
    cur=frameFor(*q,1);q->workloads[1].drawData.faceIndices[0]=UINT32_MAX;
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"invalid nondegenerate vertex must defer");
    // Whole-scene union: repeated projections must not double count transforms.
    seed(q->workloads[0],8,8,0);seed(q->workloads[1],8,8,4);prev=frameFor(*q,0);cur=frameFor(*q,1);
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"projection duplicates must form one union");
    // RDP degenerate tests are discarded before even checking their vertex bounds.
    q->workloads[1].drawData.faceIndices[0]=q->workloads[1].drawData.faceIndices[1]=UINT32_MAX;
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"degenerate test must not change membership");
    seed(q->workloads[0],0,8,0);seed(q->workloads[1],1,8,0);prev=frameFor(*q,0);cur=frameFor(*q,1);
    require(RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"empty versus used scene must reject");
    match(cur,prev,*q);require(!cur.rr64InterpolationCompatible&&(cur.rr64GeometryRejectionReasons&4),"empty-scene rejection must agree with full matcher");
    seed(q->workloads[1],0,8,0);cur=frameFor(*q,1);
    require(!RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q),"two empty scenes must defer");
    // Visibility rejection must still advance surviving actors' motion history.
    // Movement above AUTO's small-motion bound exposes the old skipped match.
    unsigned historyCases=0;
    for(unsigned views:{1u,2u,4u}) for(bool leaving:{false,true}) {
        for(unsigned slot=0;slot<4;++slot) {
            const unsigned actors=slot<2?(leaving?3:2):(leaving?2:3);
            auto &work=q->workloads[slot];seed(work,actors*views,8,0);
            auto &data=work.drawData;auto &pair=work.fbPairs[0];
            const auto original=pair.projections[0];
            pair.projections.resize(views);pair.projectionCount=views;
            const auto identity=hlslpp::float4x4::identity();
            data.viewTransforms.assign(views,identity);
            data.projTransforms.assign(views,identity);
            data.viewProjTransforms.assign(views,identity);
            data.viewProjTransformGroups.assign(views,0);
            for(unsigned view=0;view<views;++view) {
                auto &projection=pair.projections[view];projection.reset();
                projection.type=Projection::Type::Perspective;projection.transformsIndex=view;
                data.viewTransforms[view][3][0]=-float(slot)*2.0f;
                data.viewTransforms[view][3][1]=float(view)*1000.0f;
                data.viewProjTransforms[view]=data.viewTransforms[view];
                for(unsigned actor=0;actor<actors;++actor) {
                    const unsigned t=view*actors+actor;
                    auto &group=data.transformGroups[data.worldTransformGroups[t]];
                    group.matrixId=0x11110000u+view*256u+actor;
                    // The appearing/disappearing third actor is static; the two
                    // surviving actors keep a known, independent velocity per view.
                    data.worldTransforms[t][3][0]=actor<2?float(slot)*float(20+view):2000.0f;
                    for(unsigned material=0;material<4;++material)
                        projection.addGameCall(original.gameCalls[t]);
                }
            }
        }
        auto make=[&](unsigned slot) {
            auto result=frameFor(*q,slot);result.perspectiveScenes.clear();
            for(unsigned view=0;view<views;++view)
                result.perspectiveScenes.push_back(GameScene{{{slot,0,view}}});
            return result;
        };
        auto start=make(0),warm=make(1),churn=make(2),next=make(3);
        match(warm,start,*q);match(churn,warm,*q);match(next,churn,*q);
        require(churn.matched && !churn.rr64InterpolationCompatible &&
            (churn.rr64GeometryRejectionReasons&4),"visibility churn must retain history but reject interpolation");
        require(next.rr64InterpolationCompatible,"stable membership after churn must recover interpolation");
        const unsigned actors=leaving?2:3;
        for(unsigned view=0;view<views;++view) for(unsigned actor=0;actor<2;++actor) {
            const auto &mapping=next.frameMap.workloads[3].transforms[view*actors+actor];
            require(mapping.mapped && mapping.prevTransformIndex==view*actors+actor &&
                mapping.rigidBody.lerpTranslation && mapping.rigidBody.linearVelocity[0]==float(20+view),
                "surviving actors must retain their own camera's motion history");
        }
        if(views==1) {
            auto skipped=make(2),afterSkipped=make(3);
            require(RR64MatchingPreflight::unequalSingleSceneMembership(true,skipped,warm,*q),
                "old queue shortcut must be reached by the regression");
            match(afterSkipped,skipped,*q);
            require(!afterSkipped.rr64InterpolationCompatible &&
                (afterSkipped.rr64GeometryRejectionReasons&32) &&
                !afterSkipped.frameMap.workloads[3].transforms[0].rigidBody.lerpTranslation,
                "skipping churn matching must reproduce the subsequent translation rejection");
        }
        ++historyCases;
    }
    std::printf("moving_native_history: sequences=%u entering_leaving=1 views=1,2,4 old_skip_rejected=1 PASS\n",historyCases);
    // Verify actual queue eligibility and native-state wiring, including the
    // pause/debugger exclusions that matter even when no extra frame is rendered.
    auto sourcePath=argc>1?std::filesystem::path(argv[1]):
        std::filesystem::path(__FILE__).parent_path().parent_path()/"lib/rt64/src/hle/rt64_workload_queue.cpp";
    std::ifstream file(sourcePath);require(file.is_open(),"queue source must be available");
    std::string source((std::istreambuf_iterator<char>(file)),{});
    source=std::regex_replace(source,std::regex(R"(//[^\r\n]*|/\*[\s\S]*?\*/)"),"");
    source.erase(std::remove_if(source.begin(),source.end(),[](unsigned char c){return std::isspace(c);}),source.end());
    require(source.find("boolcanRetainWorkload=retainedRacePath&&!workloadConfig.raytracingEnabled&&!workload.paused&&(workload.debuggerRenderer.globalDrawCallIndex<0)&&!curFrame.isDebuggerCameraEnabled(*this)")!=std::string::npos,"preflight eligibility must exclude paused, debugger and ray tracing");
    require(source.find("RR64MatchingPreflight::unequalSingleSceneMembership")==std::string::npos,
        "membership proof must not skip required motion-history updates");
    require(source.find("matchingProfiler.start();curFrame.match(")!=std::string::npos &&
        source.find("if(retainedRacePath){canRetainWorkload=canRetainWorkload&&curFrame.rr64InterpolationCompatible;")!=std::string::npos,
        "required matching must advance history while retaining the strict interpolation certificate");
    seed(q->workloads[0],2000,80,0);seed(q->workloads[1],2001,80,0);prev=frameFor(*q,0);cur=frameFor(*q,1);
    for(bool equal: {false,true}) {
        if(equal) {seed(q->workloads[1],2000,80,0);cur=frameFor(*q,1);}
        std::vector<double> times;unsigned hits=0;
        for(unsigned r=0;r<70;r++) {
            auto start=std::chrono::steady_clock::now();
            hits+=RR64MatchingPreflight::unequalSingleSceneMembership(true,cur,prev,*q);
            if(r>=10)times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        require(hits==(equal?0u:70u),"timed decision must match membership");std::sort(times.begin(),times.end());
        std::printf("preflight equal=%u median_ms=%.6f hits=%u\n",equal,times[times.size()/2],hits);
    }
    std::printf("preflight proven_rejections=%u deferred_oracles=%u native_recovery=1 queue_gate=1 PASS\n",proven,deferred);
}
