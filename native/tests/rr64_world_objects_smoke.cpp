static bool drawDistanceEnabled=true;
// Driver lifetime/state tests use a small immutable asset fixture. The separate
// object-assets oracle checks all original materials, roots and ROM placements.
#include "rr64_world_objects.hpp"
#include "rr64_world_course_regions.hpp"
#include "rr64_actor_render_fixture.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using namespace rr64::engine;
using namespace rr64::world;
bool enabled=true,current_camera=true,dense_fixture=false;unsigned failures=0,frees=0;
std::vector<unsigned char> rom(512u);
std::vector<std::pair<unsigned,unsigned>> allocations;
unsigned next_allocation=0x1000000u;
void check(bool value,const char* message){if(!value){++failures;std::fprintf(stderr,"[RR64-OBJECT-DRIVER] FAILED: %s\n",message);}}
unsigned get(unsigned char* m,unsigned p){unsigned v=0;std::memcpy(&v,m+(p&0x1fffffffu),4u);return v;}
void put(unsigned char* m,unsigned p,unsigned v){std::memcpy(m+(p&0x1fffffffu),&v,4u);}
void big(std::vector<unsigned char>& v,unsigned p,unsigned w){for(unsigned i=0;i<4;++i)v[p+i]=static_cast<unsigned char>(w>>(24u-i*8u));}
void pack(unsigned char* m,unsigned p,const ObjectMatrix& v){
    for(unsigned i=0;i<16;++i){const unsigned f=static_cast<unsigned>(static_cast<int>(v[i]*65536.0f));write_u16(m,p+i*2u,f>>16u);write_u16(m,p+32u+i*2u,f);}
}
ObjectAssets assets(){
    ObjectAssets a;a.bytes.resize(256u);big(a.bytes,0,0xfd000000u);big(a.bytes,8,0xdf000000u);
    ObjectModelAsset model;model.raw_rom_offset=0;model.root_source_offset=0x40u;model.display_list_offset=0;model.display_list_size=16;
    model.source_bank=2;model.vertices=4;model.triangles=2;model.minimum={-1,-1,-1};model.maximum={1,1,1};a.models.push_back(model);
    for(unsigned i=0;i<(dense_fixture?4257u:3u);++i){ObjectPlacementAsset p;p.descriptor=2;p.model_index=0;p.placement_index=i;p.quaternion={0,0,0,1};
        p.cell_index=i;
        p.position={dense_fixture?float(i%8u)*.01f:(i==2u?1000.0f:float(i)*5.0f),0,0};p.billboard=i==1u;a.placements.push_back(p);}
    ObjectTextureAsset t;t.raw_offset=64;t.raw_size=96;t.frame_stride=16;t.frame_count=2;t.flags=4;t.frame_duration_ms=50;
    t.model_index=0;t.model_local_offset=0x80u;a.textures.push_back(t);a.relocations.push_back({4u,128u,0u});return a;
}
constexpr unsigned stock_record=0x80500000u,live_model=0x80400000u,live_graph=0x80420000u;
void seed_stock(unsigned char* m,unsigned index=0){
    const auto p=assets().placements[index];for(unsigned i=0;i<4;++i)write_float(m,stock_record+i*4u,p.quaternion[i]);
    for(unsigned i=0;i<3;++i)write_float(m,stock_record+0x10u+i*4u,p.position[i]);write_u16(m,stock_record+0x28u,p.descriptor);
}
void seed_resident(unsigned char* m){
    write_u32(m,live_model,0x41u);write_u32(m,live_model+4u,512u);write_u32(m,live_model+0x40u,0x13u);write_u16(m,live_model+0x52u,2u);
    write_u16(m,live_graph+4u,0x13u);write_u32(m,live_graph+0x14u,live_model+0x40u);
    const unsigned t=live_model+0x80u;write_u32(m,t,0x17u);write_u32(m,t+4u,96u);write_u16(m,t+0x20u,2u);
    write_u16(m,t+0x24u,4u);write_float(m,t+0x28u,50);write_u16(m,t+0x34u,1u);write_float(m,t+0x38u,17);write_u32(m,t+0x3cu,16u);
}
std::vector<unsigned> placement_ids(unsigned char* m,unsigned dl){
    std::vector<unsigned> ids;bool group=false;unsigned roots=0,group_pops=0;
    for(unsigned p=dl;p<dl+256u*1024u;p+=8u){const unsigned op=get(m,p);
        if(op==0xdf000000u){check(!group&&ids.size()==roots&&roots==group_pops,"every complete placement has one balanced matrix-ID scope");return ids;}
        if(op==0x6400000cu){
            check(!group,"placement matrix-ID groups do not nest across objects");group=true;ids.push_back(get(m,p+4u));
            const unsigned flags=get(m,p+8u);
            check((flags&7u)==5u,"placement IDs push a decomposed model group without changing the camera group");
            for(unsigned shift:{3u,5u,7u,9u,11u,15u,24u})check(((flags>>shift)&3u)==2u,"root/camera-related interpolation and tile/look-at policies retain AUTO");
            check(((flags>>13u)&3u)==0u&&((flags>>22u)&3u)==0u,"immutable vertex and UV interpolation remains skipped");
            check(((flags>>17u)&3u)==0u&&((flags>>19u)&1u)==0u&&((flags>>20u)&3u)==0u,
                "explicit IDs use LINEAR order and preserve edit/aspect defaults");
            check(get(m,p+12u)==0u,"matrix-ID second command has no extra payload");p+=8u;
        }
        else if(op==0x64000030u){check(group,"every cached root belongs to its authored placement ID");++roots;p+=8u;}
        else if(op==0xde000000u)check(group,"the complete model/child display list retains the placement ID");
        else if(op==0x6400000du){check(group&&get(m,p+4u)==1u,"pop restores exactly one model ID group");group=false;++group_pops;}
    }
    check(false,"object command list ends within its reserved region");return ids;
}
void run(){
    rr64::lod::test::Fixture f;auto memory=std::move(f.live);memory.resize(64u*1024u*1024u);auto* m=memory.data();
    big(rom,0,0x41u);big(rom,4,512u);const auto rom_before=rom;
    write_u32(m,globals::terrain_map_width,70u);write_float(m,0x8009DBB4u,10);write_float(m,0x80000C70u,16.666666f);
    write_u32(m,0x800AC658u,0x80200000u);write_u32(m,0x800AC65Cu,0x80240000u);write_u32(m,0x800BC9A0u,0x4650u);
    const ObjectMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const ObjectMatrix projection{.01f,0,0,0,0,.01f,0,0,0,0,.01f,.001f,0,0,0,1};
    for(unsigned slot=0;slot<2;++slot){pack(m,0x800B6668u+slot*64u,projection);pack(m,0x800B6EE8u+slot*64u,identity);write_u16(m,0x800B73F0u+slot*2u,17u);}
    write_float(m,0x800D6A2Cu,10);seed_stock(m);seed_resident(m);
    auto setup=[&](unsigned slot,unsigned epoch){
        write_u32(m,globals::main_mode,0x17u);write_u32(m,globals::pending_mode,0x17u);write_u32(m,globals::actor_render_buffer_slot,slot);
        write_u32(m,0x8009CBA4u,slot);write_u32(m,0x800A1830u,epoch);write_u32(m,0x800BC9A0u,0x4650u);
        const unsigned base=slot?0x80240000u:0x80200000u;write_u32(m,0x8009CB90u,base);write_u32(m,0x800AC650u,base+0x200u);
        enabled=current_camera=true;
    };
    auto bytes=[&](unsigned slot){const auto [p,n]=allocations[slot];return std::vector<unsigned char>(memory.begin()+p,memory.begin()+p+n);};
    setup(0,1);rr64_world_objects_begin(m);check(allocations.size()==2u,"two graphics-slot asset allocations created once");if(allocations.size()!=2u)return;
    auto other=bytes(1);const auto before=std::vector<unsigned char>(memory.begin(),memory.begin()+kRdramSize);
    rr64_world_objects_draw(m);auto stats=objects_statistics();
    check(stats.frames==1u&&stats.visible_placements==2u&&stats.drawn_triangles==4u,"cached static and billboard placements draw, outside placement culled");
    check(get(m,0x800AC650u)==0x80200228u,"exact40-byte bridge stays inside original graphics arena");
    check(get(m,0x80200200u)==0xe7000000u&&get(m,0x80200208u)==0xe0525464u&&get(m,0x80200210u)==0x6400002cu&&get(m,0x80200218u)==0xde000000u,"bridge synchronizes/enables/calls extended list");
    check(bytes(1)==other,"slot0 draw leaves other graphics buffer immutable");
    const unsigned dl=get(m,0x8020021Cu);unsigned roots=0,pops=0,pushproj=0,popproj=0;
    for(unsigned p=dl;p<dl+4096u;p+=8u){const unsigned op=get(m,p);if(op==0xdf000000u)break;
        if(op==0x64000030u){++roots;check(get(m,p+4u)==2u,"root command is push+load");p+=8u;}
        if(op==0xd8380002u)++pops;if(op==0x6400001du)++pushproj;if(op==0x6400001eu)++popproj;
    }
    check(roots==2u&&pops==2u&&pushproj==1u&&popproj==1u,"model and projection scopes balance");
    check(placement_ids(m,dl)==std::vector<unsigned>{0x52520000u,0x52520001u},"identical-model placements receive distinct immutable authored IDs");
    check(get(m,0x8009DB2Cu)==0u&&get(m,0x800B1A20u)==0xffffffffu&&get(m,0x8009DBECu)==0xffffffffu,"following original material and camera state is invalidated");
    for(unsigned i=0;i<kRdramSize;++i){const unsigned a=kRdramBegin+i;
        if((a>=0x80200200u&&a<0x80200228u)||(a>=0x800AC650u&&a<0x800AC654u)||
            (a>=0x8009DB2Cu&&a<0x8009DB30u)||(a>=0x800B1A20u&&a<0x800B1A24u)||(a>=0x8009DBECu&&a<0x8009DBF0u))continue;
        if(memory[i]!=before[i]){check(false,"cache draw leaves simulation/entity/ROM-owned memory unchanged");break;}
    }
    auto first=bytes(0);setup(1,2);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(objects_statistics().frames==2u&&bytes(0)==first,"other graphics slot preserves previous submitted data");
    check(placement_ids(m,get(m,0x8024021Cu))==std::vector<unsigned>{0x52520000u,0x52520001u},"matrix IDs do not depend on graphics slot or matrix addresses");
    other=bytes(1);setup(1,2);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(objects_statistics().frames==2u&&bytes(1)==other,"same graphics-slot epoch cannot be overwritten");
    setup(0,3);rr64_world_objects_begin(m);rr64_world_objects_observe(m,stock_record);rr64_world_objects_observe(m,stock_record);rr64_world_objects_draw(m);
    check(objects_statistics().stock_placements==1u&&objects_statistics().visible_placements==1u,"exact immutable placement dedup prevents duplicate stock drawing");
    check(placement_ids(m,get(m,0x8020021Cu))==std::vector<unsigned>{0x52520001u},"stock suppression does not renumber the remaining cached placement");
    setup(1,4);rr64_world_objects_begin(m);const auto old_sync=objects_statistics().texture_syncs;
    const auto live_before=std::vector<unsigned char>(memory.begin(),memory.begin()+kRdramSize);
    rr64_world_objects_sample(m,stock_record,live_graph);
    check(objects_statistics().texture_syncs==old_sync+1u,"resident original F594 frame/elapsed copied through certified model and texture");
    check(std::equal(live_before.begin(),live_before.end(),memory.begin()),"resident phase observation never changes original texture or guest state");
    rr64_world_objects_draw(m);
    check(get(m,0x80000000u+allocations[1].first+4u)==0x80000000u+allocations[1].first+144u,"resident frame1 binds matching pixels rather than advancing fallback clock");
    setup(0,5);rr64_world_objects_begin(m);write_u16(m,live_model+0x80u+0x24u,0u);const auto syncs=objects_statistics().texture_syncs;
    rr64_world_objects_sample(m,stock_record,live_graph);check(objects_statistics().texture_syncs==syncs,"changed resident texture certificate cannot inject animation phase");seed_resident(m);
    setup(0,6);seed_stock(m,1);rr64_world_objects_begin(m);rr64_world_objects_observe(m,stock_record);rr64_world_objects_draw(m);
    check(placement_ids(m,get(m,0x8020021Cu))==std::vector<unsigned>{0x52520000u},"alternating stock visibility keeps each surviving object identity");seed_stock(m);
    setup(1,7);write_float(m,0x800D69F8u,1000);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(objects_statistics().visible_placements==1u&&placement_ids(m,get(m,0x8024021Cu))==std::vector<unsigned>{0x52520002u},"frustum entry uses authored index rather than visible ordinal");
    setup(0,8);write_float(m,0x800D69F8u,0);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(placement_ids(m,get(m,0x8020021Cu))==std::vector<unsigned>{0x52520000u,0x52520001u},"reappearing placements recover the same stable IDs");
    for(unsigned bad=0;bad<8;++bad){setup(0,10+bad);
        switch(bad){case 0:current_camera=false;break;case 1:write_u32(m,0x8009CB90u,0x80240000u);break;
        case 2:write_u32(m,0x800BC9A0u,0xffffffffu);break;case 3:write_u32(m,0x800AC650u,0x80223400u);break;
        case 4:enabled=false;break;case 5:write_u32(m,globals::main_mode,0u);break;
        case 6:write_u16(m,0x800B73F0u,0u);break;case 7:write_float(m,0x800D69F8u,std::numeric_limits<float>::infinity());break;}
        const unsigned pointer=get(m,0x800AC650u);const auto count=objects_statistics().frames;
        rr64_world_objects_begin(m);rr64_world_objects_draw(m);
        check(get(m,0x800AC650u)==pointer&&objects_statistics().frames==count,"stale camera/arena/scene/scale state cannot append objects");
        write_u16(m,0x800B73F0u,17u);write_float(m,0x800D69F8u,0);
    }
    write_u32(m,rr64::lod::test::Fixture::race_player_count,4u);write_u32(m,0x8009DB88u,4u);
    drawDistanceEnabled=false;
    const auto priorFrames=objects_statistics().frames;
    for(unsigned count=2;count<=4;++count)for(unsigned camera=0;camera<count;++camera){
        setup(0,50);write_u32(m,rr64::lod::test::Fixture::race_player_count,count);
        write_u32(m,0x8009DB88u,count);write_u32(m,globals::active_viewport,camera);
        const auto pointer=get(m,0x800ac650u);
        rr64_world_objects_begin(m);rr64_world_objects_draw(m);
        check(objects_statistics().frames==priorFrames && get(m,0x800ac650u)==pointer,
            "two to four split screens keep original distant scenery submission");
    }
    write_u32(m,rr64::lod::test::Fixture::race_player_count,1u);write_u32(m,0x8009DB88u,1u);write_u32(m,globals::active_viewport,0u);
    drawDistanceEnabled=true;
    for(unsigned camera=0;camera<4;++camera){
        setup(0,60+camera);write_u32(m,rr64::lod::test::Fixture::race_player_count,4u);
        write_u32(m,0x8009DB88u,4u);write_u32(m,globals::active_viewport,camera);
        pack(m,0x800B6668u+camera*0x180u,projection);pack(m,0x800B6EE8u+camera*0x180u,identity);
        write_u16(m,0x800B73F0u+camera*12u,17u);
        rr64_world_objects_begin(m);rr64_world_objects_draw(m);
        check(objects_statistics().frames==priorFrames+camera+1&&objects_statistics().visible_placements==2,
            "global draw distance adds scenery in every split-screen viewport");
    }
    write_u32(m,rr64::lod::test::Fixture::race_player_count,1u);write_u32(m,0x8009DB88u,1u);write_u32(m,globals::active_viewport,0u);
    check(allocations.size()==2u&&rom==rom_before,"frames/refusals do not grow allocations or modify ROM");
    const auto old_allocations=allocations;objects_reset_session();for(const auto [p,n]:old_allocations)std::fill(memory.begin()+p,memory.begin()+p+n,0xce);
    setup(0,1);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(allocations.size()==4u&&objects_statistics().frames==1u&&frees==0u,"new session allocates fresh buffers without freeing retired heap");
    for(const auto [p,n]:old_allocations)check(std::all_of(memory.begin()+p,memory.begin()+p+n,[](auto v){return v==0xce;}),"session reset never reuses retired allocation bytes");
    ObjectModelAsset strip;strip.minimum={-0.01f,-0.01f,-0.01f};strip.maximum={0.01f,0.01f,0.01f};auto root=identity;root[12]=1.2f;
    check(object_in_frustum(strip,root,identity,identity),"16:9 expansion side strip stays visible");root[12]=1.5f;
    check(!object_in_frustum(strip,root,identity,identity),"beyond expanded side clip remains culled");
    dense_fixture=true;objects_reset_session();setup(0,30);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    const auto dense_stats=objects_statistics();const auto dense_ids=placement_ids(m,get(m,0x8020021Cu));
    check(dense_stats.frames==1u&&dense_stats.visible_placements==4257u&&dense_stats.drawn_triangles==8514u,
        "all 4257 placements fit without visibility reduction after matrix-ID commands");
    check(dense_ids.size()==4257u,"full placement inventory emits one ID per root");
    for(unsigned i=0;i<dense_ids.size();++i)check(dense_ids[i]==0x52520000u+i,"full-inventory IDs are unique, ordered, and outside the terrain namespace");
    CourseRegions::Mask course{};course.fill(true);course[0]=false;
    setup(1,31);course_frame().publish(m,31,course);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(objects_statistics().visible_placements==4256&&objects_statistics().drawn_triangles==8512,
        "scenery uses the same terrain-course mask without changing retained geometry");
    setup(0,32);rr64_world_objects_begin(m);rr64_world_objects_draw(m);
    check(objects_statistics().visible_placements==4257,"scenery rejects a previous-frame course mask");
}
}
namespace recomp {
std::span<const std::uint8_t> get_rom(){return rom;}
void* alloc(std::uint8_t* rdram,std::size_t size){next_allocation=(next_allocation+63u)&~63u;if(size>64u*1024u*1024u-next_allocation)return nullptr;
    allocations.emplace_back(next_allocation,unsigned(size));void* p=rdram+next_allocation;next_allocation+=unsigned(size);return p;}
void free(std::uint8_t*,void*){++frees;}
}
namespace rr64::world {
bool build_object_assets(std::span<const std::uint8_t>,ObjectAssets& out,std::string& error) noexcept{out=assets();error.clear();return true;}
bool build_object_assets(std::span<const std::uint8_t> rom,ObjectAssets& out,std::string& error,bool batch_triangles) noexcept{check(batch_triangles,"production world objects request triangle batching");return build_object_assets(rom,out,error);}
bool build_object_assets(std::span<const std::uint8_t> rom,ObjectAssets& out,std::string& error,bool batch_triangles,bool compile_packets) noexcept{check(compile_packets,"production objects request compiled packets");return build_object_assets(rom,out,error,batch_triangles);}
}
extern "C" int rr64_world_distance_enabled(){return enabled;}
extern "C" int rr64_world_camera_ready(unsigned char*,unsigned source,unsigned slot){return current_camera&&source==2u&&slot<2u;}
int main(){run();std::printf("Object driver smoke: %s (%u failures); cached roots,stable placement IDs,full4257 capacity,stock dedup,phase sync,frame lifetime,graphics bounds,state restoration\n",failures?"FAIL":"PASS",failures);return failures?1:0;}



extern "C" bool rr64_draw_distance_enabled(){return drawDistanceEnabled;}
extern "C" double rr64_draw_distance_percent(){return 100.0;}
