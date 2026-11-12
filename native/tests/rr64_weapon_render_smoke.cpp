#include "rr64_engine_layout.hpp"
#include "rr64_native.hpp"
#include "rr64_weapon_diagnostics.hpp"
#include <vector>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
static unsigned calls=0,expectedSource=2;static bool enabled=true;
// This fixture exercises local attachment matrices. Online presentation has its
// own coherence tests; its offline behavior is an identity transform.
extern "C" void rr64_online_presentation_matrix(unsigned char*,unsigned,unsigned,unsigned,unsigned) {}
static bool preparedAvailable=false;
static unsigned preparedSource=1;
static std::array<unsigned,7> preparedRoot{};
extern "C" int rr64_lod_weapon_root(unsigned char*,unsigned,unsigned* words,unsigned* source){
    if(!preparedAvailable)return 0;
    std::copy(preparedRoot.begin(),preparedRoot.end(),words);*source=preparedSource;return 1;
}
extern "C" int rr64_render_only_max_lod_enabled(){return enabled;}
extern "C" void func_80016A18(unsigned char*,recomp_context* c){if(c->r4!=expectedSource)std::abort();++calls;c->r4=99;c->r8=55;}
int main(){
    using namespace rr64::engine;
    std::vector<unsigned char> memory(8*1024*1024);auto* m=memory.data();unsigned failures=0;
    auto check=[&](bool ok){if(!ok)++failures;};
    constexpr unsigned node=0x80100000,rider=0x80110000,graph=0x80120000,source=0x80130000,matrix=0x80140000,parentModel=0x80150000,parentSource=0x80160000;
    write_u32(m,node,2);write_u32(m,node+4,rider);write_u32(m,rider+0x5BC,graph);
    write_u32(m,node+0x28,parentModel);write_u32(m,parentModel+0x14,parentSource);write_u16(m,parentSource+0x12,2);
    write_u32(m,graph+0x14,source);write_u16(m,source+0x12,1);
    write_u32(m,0x8009DBB0,std::bit_cast<unsigned>(100.f));write_u32(m,0x8009DBB4,std::bit_cast<unsigned>(10.f));
    for(unsigned views=1;views<=4;++views)for(unsigned view=0;view<views;++view){
        write_u32(m,0x8009DB88,views);write_u32(m,globals::active_viewport,view);
        for(unsigned i=0;i<16;++i)write_u32(m,matrix+i*4,std::bit_cast<unsigned>(i==12?47000.f:(i%5==0?1.f:0.f)));
        recomp_context c{};c.r4=123;c.r8=456;const auto before=c;const unsigned prior=calls;
        if(view&1)rr64_weapon_alt_begin(m,node,graph);else rr64_weapon_begin(m,node,graph);
        rr64_weapon_source(m,&c,graph);rr64_weapon_matrix(m,graph,matrix);rr64_weapon_end();
        float x=0,w=0;read_float(m,matrix+48,x);read_float(m,matrix+60,w);
        check(x==47000.f&&w==1&&calls==prior+(views>1));
        check(std::memcmp(&c,&before,sizeof(c))==0);
        rr64_weapon_matrix(m,graph,matrix);float again=0;read_float(m,matrix+48,again);check(again==x);
    }
    // Exercise both sides of a distance transition and unchanged source units.
    for(unsigned ownerSource : {1u,2u,1u})for(unsigned assetSource : {1u,2u}){
        write_u16(m,parentSource+0x12,ownerSource);write_u16(m,source+0x12,assetSource);
        expectedSource=ownerSource;const unsigned prior=calls;recomp_context context{};
        rr64_weapon_begin(m,node,graph);rr64_weapon_source(m,&context,graph);rr64_weapon_end();
        check(calls==prior+(ownerSource!=assetSource));
    }
    write_u16(m,parentSource+0x12,2);write_u16(m,source+0x12,1);expectedSource=2;
    const unsigned before=calls;recomp_context c{};
    rr64_weapon_begin(m,node,graph+32);rr64_weapon_source(m,&c,graph);check(calls==before);
    rr64_weapon_begin(m,node,graph);write_u32(m,globals::active_viewport,0);rr64_weapon_source(m,&c,graph);check(calls==before);
    rr64_weapon_end();enabled=false;rr64_weapon_begin(m,node,graph);rr64_weapon_source(m,&c,graph);check(calls==before);
    enabled=true;
    constexpr unsigned pose=0x80170000,packed=0x80180000;
    write_u32(m,parentModel+0xc,pose);write_u16(m,parentSource+0x12,1);
    write_u32(m,pose,std::bit_cast<unsigned>(47000.f));
    write_u16(m,packed+24,47000);write_u16(m,packed+56,0);
    const auto unchanged=memory;
    rr64_weapon_begin(m,node,graph);rr64_weapon_matrix(m,graph,matrix);
    rr64_weapon_packed(m,graph,packed);rr64_weapon_end();
    auto report=rr64::weapon::take_report();
    const char* diag=std::getenv("RR64_WEAPON_DIAGNOSTICS");
    const bool recording=diag&&std::strcmp(diag,"1")==0;
    check(report.examined==(recording?1u:0u));
    if(recording){check(report.size==1&&report.samples[0].translation[0]==47000.f&&
        report.samples[0].packed[0]==-18536.f&&report.samples[0].parent[0]==47000.f);}
    check(memory==unchanged);check(rr64::weapon::take_report().examined==0);
    rr64_weapon_begin(m,node,graph);rr64_weapon_matrix(m,graph,matrix);
    write_u32(m,globals::active_viewport,1);rr64_weapon_packed(m,graph,packed);
    check(rr64::weapon::take_report().examined==0);rr64_weapon_end();
    // Recorder capacity is fixed; excess identities must be counted, not grow.
    for(unsigned i=0;i<65;++i){
        const unsigned owner=0x80200000+i*0x200;
        write_u32(m,owner,2);write_u32(m,owner+4,rider);write_u32(m,owner+0x28,parentModel);
        rr64_weapon_begin(m,owner,graph);rr64_weapon_matrix(m,graph,matrix);
        rr64_weapon_packed(m,graph,packed);rr64_weapon_end();
    }
    report=rr64::weapon::take_report();
    check(recording?(report.size==64&&report.omitted==1&&report.examined==65):report.examined==0);
    // Reproduce the captured failure: shared weapon still has the near view's
    // position while the current rider root is distant. Bind all seven root
    // words, then restore exactly, including when a new draw interrupts scope.
    constexpr unsigned weaponPose=0x80190000;
    write_u32(m,graph+0xc,weaponPose);
    for(unsigned i=0;i<7;++i){
        write_u32(m,pose+i*4,std::bit_cast<unsigned>(i==0?15238.583f:float(i)));
        write_u32(m,weaponPose+i*4,std::bit_cast<unsigned>(i==0?-408.418f:float(i+10)));
    }
    const auto originalMemory=memory;
    rr64_weapon_begin(m,node,graph);
    for(unsigned i=0;i<7;++i){unsigned actual=0,expected=0;
        read_u32(m,weaponPose+i*4,actual);read_u32(m,pose+i*4,expected);check(actual==expected);}
    rr64_weapon_end();check(memory==originalMemory);
    rr64_weapon_begin(m,node,graph);rr64_weapon_begin(m,node,graph+32);
    check(memory==originalMemory);rr64_weapon_end();
    // Distant stock cop root was zero in the captured failing view. The
    // prepared rider root must win, retaining animation and exact restoration.
    for(unsigned i=0;i<7;++i){write_u32(m,pose+i*4,0);preparedRoot[i]=std::bit_cast<unsigned>(i==0?18000.f:float(i));}
    preparedAvailable=true;preparedSource=2;expectedSource=2;
    const auto beforePrepared=memory;
    for(unsigned view=0;view<4;++view){
        write_u32(m,globals::active_viewport,view);
        const auto beforeView=memory;
        rr64_weapon_begin(m,node,graph);
        for(unsigned i=0;i<7;++i){unsigned actual=0;read_u32(m,weaponPose+i*4,actual);check(actual==preparedRoot[i]);}
        const auto prior=calls;rr64_weapon_source(m,&c,graph);check(calls==prior+1);
        rr64_weapon_end();check(memory==beforeView);
    }
    preparedAvailable=false;
    write_u32(m,globals::active_viewport,1);
    check(memory==beforePrepared);
    write_u32(m,0x8009DB88,1);const auto singleViewMemory=memory;
    rr64_weapon_begin(m,node,graph);rr64_weapon_end();check(memory==singleViewMemory);
    std::printf("Weapon scope/source-transition and read-only packed-matrix capture: %u failures\n",failures);return failures?1:0;
}
