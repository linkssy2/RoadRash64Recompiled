// ROM-free production RSP preparation and address-edit parity against the old
// multimap lookup. No renderer, game loop, or presentation thread is started.
#include "hle/rt64_state.h"
#include "hle/rt64_interpreter.h"
#include "include/rt64_extended_gbi.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <map>
#include <memory>
using namespace RT64;
extern "C" void rr64_record_authored_sample(unsigned long long,unsigned long long,unsigned long long,unsigned,unsigned,unsigned long long,unsigned long long){std::abort();}
extern "C" void rr64_record_source_cadence(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned){std::abort();}
static void noInterrupts(){std::abort();}
static void require(bool condition,const char *message){if(!condition){std::fprintf(stderr,"transform index FAIL: %s\n",message);std::exit(1);}}
static void group(RSP &rsp,uint32_t id,bool projection,bool address,bool editable,unsigned value){
    rsp.matrixId(id,false,projection,value%2,value%3,(value+1)%3,(value+2)%3,value%3,(value+1)%3,
        value%3,(value+1)%3,(value+2)%3,value%3,value%3,(value+1)%3,editable?G_EX_EDIT_ALLOW:G_EX_EDIT_NONE,address,address);
}
int main(int, char **){
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
#endif
    std::vector<uint8_t> ram(4*1024*1024);uint32_t interrupts=0;
    auto state=std::make_unique<State>(ram.data(),&interrupts,noInterrupts);
    auto queue=std::make_unique<WorkloadQueue>();auto interpreter=std::make_unique<Interpreter>();GBI gbi{};
    interpreter->hleGBI=&gbi;state->ext={};state->ext.workloadQueue=queue.get();state->ext.interpreter=interpreter.get();
    auto &rsp=*state->rsp;
    auto *vertices=reinterpret_cast<RSP::Vertex *>(ram.data()+0x1000);
    for(unsigned i=0;i<24;++i){vertices[i]={};vertices[i].x=int16_t(i*31-700);vertices[i].y=int16_t(300-i*9);vertices[i].z=int16_t(20+i);}
    std::array<std::vector<std::pair<uint32_t,uint32_t>>,3> saved;
    std::array<std::vector<TransformGroup>,3> savedGroups;
    for(unsigned slot=0;slot<3;++slot)savedGroups[slot]=queue->workloads[slot].drawData.transformGroups;
    unsigned editedGroups=0;
    const auto fields=[](const TransformGroup &g){return std::tie(g.matrixId,g.decompose,g.positionInterpolation,
        g.rotationInterpolation,g.scaleInterpolation,g.skewInterpolation,g.perspectiveInterpolation,
        g.vertexInterpolation,g.texcoordInterpolation,g.tileInterpolation,g.lookAtInterpolation,g.ordering,g.aspectMode,g.editable);};
    for(unsigned frame=0;frame<9;++frame){
        unsigned slot=frame%3;queue->writeCursor=slot;auto &workload=queue->workloads[slot];
        for(unsigned other=0;other<3;++other){
            const auto &data=queue->workloads[other].drawData;
            require(queue->workloads[other].physicalAddressTransforms==saved[other],"other workload index unchanged");
            require(data.transformGroups.size()==savedGroups[other].size(),"other workload group count unchanged");
            for(size_t i=0;i<savedGroups[other].size();++i)require(fields(data.transformGroups[i])==fields(savedGroups[other][i]),"other workload groups unchanged");
        }
        auto capacity=workload.physicalAddressTransforms.capacity();auto *storage=workload.physicalAddressTransforms.data();
        workload.reset();rsp.reset();
        require(workload.physicalAddressTransforms.empty(),"reset removes retired entries");
        require(workload.physicalAddressTransforms.capacity()==capacity&&workload.physicalAddressTransforms.data()==storage,"reset retains index storage");
        rsp.projectionIndex=0;rsp.extended.modelMatrixIdStackChanged=false;rsp.extended.viewProjMatrixIdStackChanged=false;
        rsp.viewProjMatrixStack[0]=hlslpp::float4x4::identity();rsp.viewMatrixStack[0]=hlslpp::float4x4::identity();rsp.projMatrixStack[0]=hlslpp::float4x4::identity();
        rsp.viewportStack[0].scale={160,120,512};rsp.viewportStack[0].translate={160,120,512};rsp.textureState.sc=49152;rsp.textureState.tc=16384;rsp.geometryModeStack[0]=0;
        std::multimap<uint32_t,uint32_t> reference;
        const unsigned transforms=frame<3?127:33;
        for(unsigned j=0;j<transforms;++j){
            uint32_t address=0x20000+((j+frame)%5)*64;
            rsp.modelMatrixPhysicalAddressStack[0]=address;rsp.modelMatrixSegmentedAddressStack[0]=address;
            rsp.modelMatrixStack[0]=hlslpp::float4x4::identity();rsp.modelMatrixStack[0][3][0]=float(j)*0.25f;
            group(rsp,0x52510000+frame*256+j,false,false,j%7!=0,j%3);rsp.modelViewProjChanged=true;
            // Production appends the world entry first, then any changed projection.
            reference.emplace(address,uint32_t(workload.drawData.worldTransformGroups.size()));
            if(j%7==0){
                rsp.projectionMatrixPhysicalAddressStack[0]=address;group(rsp,0x48550000+j,true,false,j%5!=0,(j+1)%3);rsp.projectionMatrixChanged=true;
                reference.emplace(address,uint32_t(workload.drawData.viewProjTransformGroups.size()));
            }
            rsp.setVertex(0x1000,24,0);
        }
        auto &data=workload.drawData;
        require(data.vertexCount()==transforms*24,"all vertices prepared");
        require(std::multimap<uint32_t,uint32_t>(workload.physicalAddressTransforms.begin(),workload.physicalAddressTransforms.end())==reference,"production entries match original index");
        auto expected=data.transformGroups;
        for(unsigned edit=0;edit<12;++edit){
            uint32_t address=edit%6==5?0x700000:0x20000+(edit%5)*64;bool projection=edit%2!=0;unsigned value=(edit+frame)%3;
            // Obtain the requested property bundle from the actual matrixId setter.
            // This edits only its pending stack, not any submitted group.
            group(rsp,0,false,false,true,value);
            TransformGroup patch=rsp.extended.modelMatrixIdStack[rsp.extended.modelMatrixIdStackSize-1];
            auto range=reference.equal_range(address);
            for(auto it=range.first;it!=range.second;++it){
                uint32_t index=it->second,groupIndex;
                if(projection&&index<data.viewProjTransformGroups.size())groupIndex=data.viewProjTransformGroups[index];
                else if(index<data.worldTransformGroups.size())groupIndex=data.worldTransformGroups[index];
                else continue;
                auto &target=expected[groupIndex];
                if(target.editable==G_EX_EDIT_ALLOW){uint32_t id=target.matrixId;target=patch;target.matrixId=id;++editedGroups;}
            }
            group(rsp,address,projection,true,true,value);
            require(data.transformGroups.size()==expected.size(),"address edit does not add groups");
            for(size_t i=0;i<expected.size();++i)require(fields(expected[i])==fields(data.transformGroups[i]),"address edit equals original lookup incl locked groups");
        }
        saved[slot]=workload.physicalAddressTransforms;
        savedGroups[slot]=data.transformGroups;
        for(unsigned other=0;other<3;++other){
            const auto &groups=queue->workloads[other].drawData.transformGroups;
            require(queue->workloads[other].physicalAddressTransforms==saved[other],"preparation and edits stay workload-local");
            require(groups.size()==savedGroups[other].size(),"preparation retains other workload groups");
            for(size_t i=0;i<groups.size();++i)require(fields(groups[i])==fields(savedGroups[other][i]),"edits retain other workload groups");
        }
    }
    require(editedGroups>0,"address-edit test exercised updates");
    std::printf("transform index PASS: changed models, duplicates, world/projection address edits, missing/locked groups, retained capacity, 3 workloads and 9 resets (%u edits)\n",editedGroups);
    return 0;
}
