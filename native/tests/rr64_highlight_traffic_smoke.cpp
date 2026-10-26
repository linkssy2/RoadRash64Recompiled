#include "rr64_highlight_traffic.hpp"
#include "rr64_world_objects.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_engine_layout.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "recomp.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

extern "C" {
void func_8001A69C(unsigned char*,recomp_context*);
void func_8001A994(unsigned char*,recomp_context*);
void func_80011988(unsigned char*,recomp_context*);
void func_8005EF74(unsigned char*,recomp_context*);
void func_80015A90(unsigned char*,recomp_context*);
void func_8007B254(unsigned char*,recomp_context*);
void rr64_traffic_fixture_retire(unsigned char*,recomp_context*);
}
namespace {
using namespace rr64::engine;
using namespace rr64::highlights;
unsigned failures=0, native_next=0x80280000, extended_next=0x01000000;
struct Block { unsigned bytes=0,pool=0; bool freed=false; };
std::map<unsigned,Block> native_blocks;
std::vector<std::pair<unsigned,unsigned>> extended_blocks;
std::vector<unsigned char> rom;
void check(bool ok,const char*message) {
    if(!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
}
unsigned word(unsigned char*m,unsigned p) { unsigned w; std::memcpy(&w,m+(p&0x1fffffff),4); return w; }
void put(unsigned char*m,unsigned p,unsigned w) { std::memcpy(m+(p&0x1fffffff),&w,4); }
unsigned big(std::span<const unsigned char>b,unsigned p) {
    return unsigned(b[p])<<24|unsigned(b[p+1])<<16|unsigned(b[p+2])<<8|b[p+3];
}
void copy(unsigned char*m,unsigned p,std::span<const unsigned char>b) {
    for(unsigned i=0;i<b.size();++i) m[((p&0x1fffffff)+i)^3]=b[i];
}
unsigned allocate_native(unsigned char*m,unsigned pool,unsigned bytes) {
    const unsigned p=native_next; native_next=(native_next+bytes+15)&~15u;
    if(native_next>=0x80600000) std::abort();
    native_blocks[p]={bytes,pool,false}; std::memset(m+(p&0x1fffffff),0,bytes); return p;
}
void context(recomp_context &c) { c={};c.r29=guest_address(0x807f0000);c.f_odd=&c.f0.u32h; }
void pack(unsigned char*m,unsigned p,const rr64::world::ObjectMatrix&v) {
    for(unsigned i=0;i<16;++i){const auto f=unsigned(int(v[i]*65536.f));
        write_u16(m,p+i*2,f>>16);write_u16(m,p+32+i*2,f);}
}
void frame(unsigned char*m,unsigned slot,unsigned epoch) {
    put(m,globals::main_mode,0x17);put(m,globals::pending_mode,0x17);
    put(m,0x800a6578,1);write_u16(m,0x800a65c4,1);
    put(m,globals::active_viewport,0);put(m,0x8009db88,1);
    put(m,globals::actor_render_buffer_slot,slot);put(m,0x8009cba4,slot);put(m,0x800a1830,epoch);
    put(m,0x800ac658,0x80200000);put(m,0x800ac65c,0x80240000);
    put(m,0x8009cb90,slot?0x80240000:0x80200000);
    put(m,0x800ac650,(slot?0x80240000:0x80200000)+0x200);
    put(m,0x800bc9a0,0x4650);write_float(m,0x8009dbb4,10);
    const rr64::world::ObjectMatrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const rr64::world::ObjectMatrix projection{.001f,0,0,0,0,.001f,0,0,0,0,.001f,.0001f,0,0,0,1};
    pack(m,0x800b6668+slot*64,projection);pack(m,0x800b6ee8+slot*64,identity);
    write_u16(m,0x800b73f0+slot*2,17);
    recomp_context c;context(c);c.r18=2;c.r22=slot;
    c.f4.fl=10;c.f8.fl=10;c.f6.fl=9000;
    rr64_world_camera_far(m,&c);
    check(c.f6.fl==9000,"traffic camera readiness does not enable extended world distance");
    write_float(m,unsigned(c.r29)+0x10,10);
    write_float(m,unsigned(c.r29)+0x14,9000);
    rr64_world_camera_normalization(m,&c);
    check(rr64_world_camera_native_ready(m,2,slot),"native traffic camera is current with extended world objects disabled");
    check(!rr64_world_camera_ready(m,2,slot),"extended world admission remains disabled independently of traffic replay");
}
void native_vehicle(unsigned char*m,unsigned node,unsigned entity,unsigned source,unsigned resource,
                    unsigned next,unsigned previous) {
    recomp_context c;context(c); c.r4=guest_address(source+((word(m,source+0xc)&0xffffu)*8));
    func_8001A994(m,&c);const auto graph=unsigned(c.r2);
    check(valid_guest_range(graph,0x18),"actual native constructor creates model graph");
    put(m,node,4);put(m,node+4,entity);put(m,node+0x28,graph);put(m,node+0x2c,graph);
    put(m,node+0x40,resource);put(m,node+0x3c,next);put(m,node+0x38,previous);
    put(m,entity,1);put(m,entity+4,entity==0x80102000?71:72);put(m,entity+0x28,entity==0x80102000?0:1);
    write_u16(m,entity+0x334,1);write_float(m,entity+0x18c,1);
    context(c);c.r4=guest_address(node);c.r5=guest_address(0x80100000);func_80011988(m,&c);
}
void lifecycle(unsigned char*m,const TrafficAssets&assets) {
    std::puts("native lifecycle: pool and source");std::fflush(stdout);
    // Construct real native graphs and shared compiled materials from the exact
    // original ROM vehicle. Only malloc/free backing and motion update are
    // instrumented; the original sharing/retirement/destructor/expiry run.
    const auto &model=assets.assets.models[assets.models[0xd8]];
    constexpr unsigned resource=300, node0=0x80100000,node1=0x80100400;
    constexpr unsigned entity0=0x80102000,entity1=0x80102400;
    const unsigned source=allocate_native(m,5,big(rom,model.raw_rom_offset+4));
    copy(m,source,std::span(rom).subspan(model.raw_rom_offset,native_blocks[source].bytes));
    put(m,0x8009dc40,0);put(m,0x8009dc44,0);put(m,0x800bc9bc,1024);
    recomp_context c;context(c);func_8001A69C(m,&c);
    std::puts("native lifecycle: first graph/material");std::fflush(stdout);
    write_u16(m,0x8009db54,1);put(m,0x800a145c,node0);put(m,0x8009dc50,2);
    native_vehicle(m,node0,entity0,source,resource,0,0);
    std::puts("native lifecycle: second graph/material");std::fflush(stdout);
    put(m,node0+0x3c,node1);
    native_vehicle(m,node1,entity1,source,resource,0,node0);
    std::puts("native lifecycle: roots");std::fflush(stdout);
    const unsigned shared=word(m,node0+0x50);
    check(shared && shared==word(m,node1+0x50),"real native compile shares equal source materials");
    std::uint16_t owner=9;read_u16(m,node1+0x48,owner);
    check(owner==1,"second native node is a compiled-list borrower");
    put(m,0x800d76e0,entity0);put(m,0x800d76e4,entity1);put(m,0x800a6528,2);put(m,0x800a1448,2);
    put(m,0x800dacd8+resource*16,source);put(m,0x800dace4+resource*16,0x17d78400);
    write_float(m,entity0+0xa8,52.25f);write_float(m,entity0+0xac,-18.5f);write_float(m,entity0+0xb0,7);
    write_float(m,0x800d69f8,1.25f);write_float(m,0x800d69fc,-2.5f);write_float(m,0x800d6a00,3.f);
    write_float(m,0x800a4fdc,40.f);write_float(m,0x800a4fe0,-20.f);write_float(m,0x800a4fe4,2.f);
    context(c);func_8005EF74(m,&c);
    const auto graph=word(m,node0+0x28),pose=word(m,graph+0xc);
    rr64::world::ObjectPlacementAsset placement;placement.position={52.25f,-18.5f,7};placement.quaternion={0,0,0,1};
    rr64::world::ObjectMatrix expected;
    check(rr64::world::object_matrix(placement,{1.25f,-2.5f,3},{40,-20,2},{},expected),"historic rigid root accepted");
    context(c);c.r4=guest_address(pose+0xc);c.r5=guest_address(pose);c.r6=guest_address(0x80610000);
    func_80015A90(m,&c);
    for(unsigned i=0;i<16;++i)
        check(word(m,0x80610000+i*4)==std::bit_cast<unsigned>(expected[i]),"historic root equals exact native root/matrix writers");
    write_u16(m,entity0+0x334,0);write_u16(m,graph+0xa,1);put(m,0x800a1830,100);
    std::puts("native lifecycle: first retirement");std::fflush(stdout);
    context(c);rr64_traffic_fixture_retire(m,&c);
    read_u16(m,node1+0x48,owner);
    check(word(m,0x800a6528)==1 && word(m,0x800a145c)==node1 && owner==0,
          "actual retirement compacts roster and transfers compiled-list ownership");
    check(!native_blocks.at(shared).freed && !native_blocks.at(source).freed,
          "owner retirement preserves shared material and original resource");
    write_u16(m,entity1+0x334,0);write_u16(m,word(m,node1+0x28)+0xa,1);
    std::puts("native lifecycle: last retirement");std::fflush(stdout);
    context(c);rr64_traffic_fixture_retire(m,&c);
    check(word(m,0x800a6528)==0 && word(m,0x800a145c)==0 && native_blocks.at(shared).freed,
          "last native retirement destroys compiled material and removes all traffic");
    check(word(m,0x800dace4+resource*16)==100,"last resource owner marks exact native expiry");
    // The native expiry routine also invalidates the resource-index directory.
    // It is normally installed by the track loader, outside this fixture.
    put(m,0x800dea84,0x80108000);
    put(m,0x800a7714,373);put(m,0x800a4f24,0);put(m,0x800a1830,140);
    std::puts("native lifecycle: resource expiry");std::fflush(stdout);
    context(c);func_8007B254(m,&c);
    std::puts("native lifecycle: expired");std::fflush(stdout);
    check(native_blocks.at(source).freed && word(m,0x800dacd8+resource*16)==0,
          "actual native expiry frees and poisons the original ROM-loaded model");
    // Reuse all old entity/source storage for unrelated bytes. Owned replay
    // geometry must neither read nor resurrect any retired native allocation.
    std::memset(m+(source&0x1fffffff),0x61,native_blocks.at(source).bytes);
    std::memset(m+(entity0&0x1fffffff),0x62,0x800);
    std::memset(m+(node0&0x1fffffff),0x63,0x800);
}
void driver(unsigned char*m,const TrafficAssets&assets) {
    const auto start=std::chrono::steady_clock::now();
    check(prepare_traffic(m),"production traffic preparation uses actual local ROM");
    const auto cold=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    check(extended_blocks.size()==2,"two independent native graphics-slot uploads");
    if(extended_blocks.size()!=2)return;
    auto preserved=[&](unsigned slot){const auto [p,n]=extended_blocks[slot];return std::vector<unsigned char>(m+p,m+p+n);};
    std::array<rr64::world_sync::Traffic,rr64::traffic::capacity> cars{};
    for(unsigned i=0;i<2;++i){auto &v=cars[i];v.active=v.motion_valid=1;v.id=0xabcd0001u+i*0x10000u;
        v.kind=1;v.model=0xd8;v.position={52.25f+float(i)*3,-18.5f,7};v.motion[54]=1;}
    frame(m,0,201);auto other=preserved(1);
    const auto live_before=std::vector<unsigned char>(m,m+kRdramSize);
    check(draw_traffic(m,cars),"retired historic cars draw with no surviving native roster");
    check(traffic_statistics().cars==2,"two historic instances of the same actual vehicle both render");
    std::vector<bool> pointer_bytes(assets.assets.bytes.size());
    for(const auto &r:assets.assets.relocations){
        for(unsigned i=0;i<4;++i)pointer_bytes[r.word_offset+i]=true;
        check(word(m,0x80000000+extended_blocks[0].first+r.word_offset)==
              0x80000000+extended_blocks[0].first+r.target_offset,
              "every vertex, child matrix, pixel and palette pointer owns its original bytes after native expiry");
    }
    bool byte_exact=true;
    for(unsigned i=0;i<assets.assets.bytes.size();++i)if(!pointer_bytes[i])
        byte_exact&=m[(extended_blocks[0].first+i)^3u]==assets.assets.bytes[i];
    check(byte_exact,"all authored geometry, material alpha, child matrices and texture bytes survive native source reuse exactly");
    check(preserved(1)==other,"graphics slot zero cannot overwrite queued slot one");
    const unsigned dl=word(m,0x8020021c);std::vector<unsigned> tags;
    for(unsigned p=dl;p<dl+2048;p+=8){const auto op=word(m,p);if(op==0xdf000000)break;
        if(op==0x6400000c){tags.push_back(word(m,p+4));p+=8;}
        if(op==0x64000030)p+=8;
        if(op==0xde000000){const auto model_list=word(m,p+4);const auto [offset,size]=extended_blocks[0];
            check(model_list>=0x80000000+offset && model_list<0x80000000+offset+size,"vehicle list belongs to owned slot");}}
    check(tags.size()==2 && tags[0]!=tags[1],"full32-bit IDs with equal low16 bits have distinct render identities");
    for(unsigned i=0;i<kRdramSize;++i){const unsigned a=kRdramBegin+i;
        if((a>=0x80200200&&a<0x80200228)||(a>=0x800ac650&&a<0x800ac654)||
           (a>=0x8009db2c&&a<0x8009db30)||(a>=0x800b1a20&&a<0x800b1a24)||(a>=0x8009dbec&&a<0x8009dbf0))continue;
        if(m[i]!=live_before[i]){check(false,"historical draw changes no native gameplay/resource/roster memory");break;}}
    const auto submitted=preserved(0);frame(m,1,202);check(draw_traffic(m,cars,375000),"second graphics slot draws historic frame");
    check(preserved(0)==submitted,"submitted geometry and matrices survive other-slot drawing");
    for(const auto &r:assets.assets.relocations) if(r.texture_index<assets.assets.textures.size()) {
        const auto &t=assets.assets.textures[r.texture_index];
        const auto phase=unsigned(std::fmod(375.0/t.frame_duration_ms,double(t.frame_count)));
        check(word(m,0x80000000+extended_blocks[1].first+r.word_offset)==
              0x80000000+extended_blocks[1].first+r.target_offset+phase*t.frame_stride,
              "original flashing-light frame follows recording time in its own graphics slot");
    }
    frame(m,0,203);check(draw_traffic(m,cars,375000),"paused recording clock draws despite advancing native clock");
    for(const auto &r:assets.assets.relocations) if(r.texture_index<assets.assets.textures.size())
        check(word(m,0x80000000+extended_blocks[0].first+r.word_offset)-extended_blocks[0].first==
              word(m,0x80000000+extended_blocks[1].first+r.word_offset)-extended_blocks[1].first,
              "animated material phase is independent of the live frame epoch");
    auto old=preserved(1);frame(m,1,202);check(!draw_traffic(m,cars),"same-slot epoch reuse refuses");
    check(preserved(1)==old,"rejected same epoch leaves submitted buffers intact");
    for(unsigned bad=0;bad<5;++bad){frame(m,0,210+bad);auto broken=cars;
        if(bad==0)broken[1].model=0;else if(bad==1)broken[1].motion_valid=0;
        else if(bad==2)broken[1].id=broken[0].id;else if(bad==3)broken[1].motion[54]=0;
        else put(m,0x800a1830,word(m,0x800a1830)+1);
        const auto pointer=word(m,0x800ac650);old=preserved(0);
        check(!draw_traffic(m,broken),"invalid historical roster or stale camera cannot own native presentation");
        check(word(m,0x800ac650)==pointer && preserved(0)==old,"refusal is atomic for native and owned graphics");}
    frame(m,0,220);check(draw_traffic(m,{}),"empty historical roster suppresses all terminal cars");
    frame(m,0,221);check(draw_traffic(m,cars,0),"backwards recording seek rewinds animated materials");
    for(const auto &r:assets.assets.relocations) if(r.texture_index<assets.assets.textures.size())
        check(word(m,0x80000000+extended_blocks[0].first+r.word_offset)==
              0x80000000+extended_blocks[0].first+r.target_offset,
              "material clock rewind ignores advanced live epoch");
    for(unsigned i=2;i<cars.size();++i){cars[i]=cars[0];cars[i].id=0xf0000000u+i;}
    frame(m,1,222);check(draw_traffic(m,cars),"expanded forty-car historic roster renders");
    check(traffic_statistics().cars==20,"last traffic slots are not dropped from highlights");
    const auto warm_start=std::chrono::steady_clock::now();for(unsigned i=0;i<1000;++i)check(prepare_traffic(m),"warm prepare remains ready");
    const auto warm=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warm_start).count();
    check(extended_blocks.size()==2,"repeated replay/race preparation does not grow allocations");
    old=preserved(1);check(prepare_traffic(m),"new race resets only CPU presentation identities");
    check(preserved(1)==old,"race preparation retains queued graphics bytes and lifetime");
    std::printf("traffic actual models=%zu bytes=%zu textures=%zu cold_ms=%.3f warm1000_ms=%.3f\n",
        assets.assets.models.size(),assets.assets.bytes.size(),assets.assets.textures.size(),cold,warm);
}
}
namespace recomp {
std::span<const std::uint8_t> get_rom(){return rom;}
void* alloc(std::uint8_t*m,std::size_t n){extended_next=(extended_next+63)&~63u;
    if(n>64u*1024u*1024u-extended_next)return nullptr;const unsigned p=extended_next;extended_next+=unsigned(n);
    extended_blocks.emplace_back(p,unsigned(n));return m+p;}
void free(std::uint8_t*,void*){check(false,"successfully queued replay assets are not prematurely freed");}
}
extern "C" {
void func_8001BDF8(unsigned char*m,recomp_context*c){c->r2=guest_address(allocate_native(m,unsigned(c->r4),unsigned(c->r5)));}
void func_8001C084(unsigned char*m,recomp_context*c){const unsigned p=unsigned(c->r5);auto it=native_blocks.find(p);
    check(it!=native_blocks.end(),"native free has a tracked actual allocation");if(it==native_blocks.end())std::abort();
    check(!it->second.freed && it->second.pool==unsigned(c->r4),"native free pool/ownership is exact and single-use");
    it->second.freed=true;std::memset(m+(p&0x1fffffff),0xa5,it->second.bytes);}
void func_8000CD34(unsigned char*,recomp_context*){check(false,"fixture never performs runtime ROM I/O");std::abort();}
void func_80076290(unsigned char*,recomp_context*){} // physics tick outside retirement fixture scope
int rr64_world_distance_enabled(){return 0;}
void rr64_world_observe_roots(unsigned char*,unsigned){}
void rr64_world_observe_allocation(unsigned char*,unsigned,unsigned,unsigned){}
void rr64_lod_observe_allocation(unsigned char*,unsigned,unsigned,unsigned){}
void rr64_lod_release_node(unsigned char*,unsigned){}
int rr64_traffic_within_draw_distance(unsigned char*,unsigned,unsigned,unsigned,int original){return original;}
}
int main(int argc,char**argv){
    if(argc!=2){std::fputs("Usage: RR64HighlightTrafficSmoke <own supported RR64 ROM>\n",stderr);return 2;}
    std::ifstream input(argv[1],std::ios::binary);rom.assign(std::istreambuf_iterator<char>(input),{});
    if(rom.size()!=0x2000000){std::fputs("complete local RR64 ROM required\n",stderr);return 2;}
    TrafficAssets assets;std::string error;
    const bool built=build_traffic_assets(rom,assets,error);
    check(built,error.c_str());if(failures)return 1;
    check(assets.assets.bytes.size()<1024*1024 && !assets.assets.models.empty(),"all original traffic fits bounded upload");
    check(assets.models[0xf8]==assets.models[0xf7] && assets.models[0x125]!=~0u && assets.models[0x126]!=~0u,
          "special models and native F8 normalization are preserved");
    check(std::count_if(assets.assets.textures.begin(),assets.assets.textures.end(),
        [](const auto&t){return t.frame_count>1 && (t.flags&4u);})==2,
        "both original animated police light materials are retained");
    auto changed=rom;changed[0x1265c40]=0;TrafficAssets independent;
    check(build_traffic_assets(changed,independent,error),"unrelated course placement changes do not invalidate traffic");
    check(independent.assets.bytes==assets.assets.bytes,"traffic assets do not depend on course placement data");
    std::vector<unsigned char> memory(64u*1024u*1024u);auto*m=memory.data();
    copy(m,0x80000000,std::span(rom).subspan(0xc00,0xa0000));
    lifecycle(m,assets);driver(m,assets);
    if(failures)return 1;
    std::puts("PASS: actual native construction/sharing/retirement/expiry; all traffic ROM assets; historic draw after source reuse; full IDs; graphics lifetime and atomic refusals.");
}
