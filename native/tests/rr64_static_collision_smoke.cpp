#include "rr64_engine_layout.hpp"
#include "rr64_online_terrain.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_netplay.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <vector>

using namespace rr64::engine;
extern "C" void func_80062594(unsigned char*, recomp_context*) noexcept(false);
extern "C" void rr64_static_collision_legacy(unsigned char*, recomp_context*);
extern "C" void func_8001BD50(unsigned char*, recomp_context*);
extern "C" void func_80034370(unsigned char*, recomp_context*);
extern "C" void func_80014604(unsigned char*, recomp_context*);
extern "C" void func_80014DE4(unsigned char*, recomp_context*);
namespace {
std::vector<unsigned char> rom, captured;
unsigned failures = 0, responses = 0;
unsigned actor_kind=2, body_offset=0x28;
bool imported=false;
rr64::netplay::PhysicsRules rules{};
unsigned imported_header=0x2000000;
constexpr unsigned grid=0x80100000, objects=0x80120000, terrain=0x80200000,
                   placements=0x80220000, descriptors=0x80240000, actor=0x80300000,
                   list=0x80302000, stack=0x807ff000;
using Vec=std::array<float,3>;
unsigned be32(unsigned p){return unsigned(rom[p])<<24|unsigned(rom[p+1])<<16|unsigned(rom[p+2])<<8|rom[p+3];}
unsigned be16(unsigned p){return unsigned(rom[p])<<8|rom[p+1];}
unsigned word(const std::vector<unsigned char>& m,unsigned p){unsigned v=0;if(valid_guest_range(p,4))std::memcpy(&v,m.data()+p-kRdramBegin,4);return v;}
void check(bool v,const char* why){if(!v){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
std::vector<unsigned char> read(const char* p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void copy(std::vector<unsigned char>& m,unsigned p,unsigned source,unsigned bytes){
    for(unsigned i=0;i<bytes;++i)m[((p-kRdramBegin)+i)^3u]=rom[source+i];
}
void vector(std::vector<unsigned char>& m,unsigned p,Vec v){for(unsigned i=0;i<3;++i)write_float(m.data(),p+4*i,v[i]);}
recomp_context context(){recomp_context c{};c.r29=guest_address(stack);return c;}
struct Source{unsigned cell,source,bytes,object_source,object_bytes;};
Source source(unsigned cell){
    const unsigned header=imported?imported_header:0x18d380;
    const unsigned textures=be16(header+20),entry=header+24+(textures+cell)*12,
                   object_entry=0x1265c50+cell*4;
    const unsigned os=be32(object_entry)?object_entry+be32(object_entry):0;
    return {cell,entry+be32(entry),be32(entry+4),os,os?be32(os+4):0};
}
std::vector<unsigned char> image(const Source& s,bool resident){
    std::vector<unsigned char> m(kRdramSize);
    copy(m,0x80000000,0xc00,0xa0000);
    write_u32(m.data(),globals::main_mode,0x17);write_u32(m.data(),globals::pending_mode,0x17);
    write_u32(m.data(),globals::terrain_cell_grid,grid);write_u32(m.data(),0x800dde98,objects);
    write_u32(m.data(),0x800dea8c,70);
    write_u32(m.data(),0x800dacd0,1000);write_u32(m.data(),0x800df080,70000);
    write_u32(m.data(),0x800dea88,3);
    for(unsigned i=0;i<4900;++i){
        const auto c=source(i);const unsigned d=grid+i*16,o=objects+i*16;
        if(c.bytes){write_u32(m.data(),d+4,0xb0000000|c.source);write_u16(m.data(),d+14,c.bytes/8);write_s8(m.data(),d+12,1);}
        if(c.object_bytes){write_u32(m.data(),o+4,0xb0000000|c.object_source);write_u32(m.data(),o+8,1);write_u32(m.data(),o+12,c.object_bytes);}
    }
    if(resident){
        copy(m,terrain,s.source,s.bytes);write_u32(m.data(),grid+s.cell*16,terrain);write_s8(m.data(),grid+s.cell*16+12,5);
        if(s.object_bytes){copy(m,placements,s.object_source,s.object_bytes);write_u32(m.data(),objects+s.cell*16,placements);write_u32(m.data(),objects+s.cell*16+8,5);}
    }
    copy(m,descriptors,0x1303208,0x1304db0-0x1303208);
    unsigned p=0x1303208;
    for(unsigned i=0;i<295;++i){write_u32(m.data(),0x800df210+i*4,descriptors+p-0x1303208);p+=8+16*rom[p]+14*rom[p+1];}
    auto c=context();c.f_odd=&c.f0.u32h;c.r4=0;c.r5=guest_address(0x80500000);c.r6=0x100000;c.r7=8;
    func_8001BD50(m.data(),&c);
    return m;
}
void body(std::vector<unsigned char>& m,Vec a,Vec b,float radius){
    const unsigned original_bike=word(captured,globals::bike_pool_pointer),original_rider=word(captured,original_bike+bike::rider_pointer);
    check(valid_guest_range(original_rider,rider::stride),"retained native rider template");
    body_offset=actor_kind==1?0x108:0x28;
    std::copy_n(captured.data()+(actor_kind==1?original_bike:original_rider)-kRdramBegin,
        actor_kind==1?bike::stride:rider::stride,m.data()+actor-kRdramBegin);
    const unsigned body=actor+body_offset;
    if(actor_kind==2){write_u16(m.data(),actor+0x576,0);write_u16(m.data(),actor+0x57a,0);}
    write_u32(m.data(),body+0x2c,1);write_float(m.data(),body+8,radius);write_float(m.data(),body+0x54,radius);
    write_u16(m.data(),body+0x188,1);
    vector(m,body+0x64,a);vector(m,body+0x8c,b);vector(m,body+0x190,a);vector(m,body+0x1c4,b);
    vector(m,body+0x80,{(b[0]-a[0])*60,(b[1]-a[1])*60,(b[2]-a[2])*60});
    vector(m,body+0x100,{0,0,0});vector(m,body+0x170,{0,0,0});
    write_u32(m.data(),list,actor_kind);write_u32(m.data(),list+4,actor);write_u32(m.data(),list+8,0);
}
struct Result{std::array<unsigned,128> body{};unsigned response=0;};
Result run(std::vector<unsigned char>& m,unsigned cell,unsigned sub,bool legacy){
    write_u32(m.data(),0x800a5364,1);write_u32(m.data(),0x800d6b40,cell);
    write_u32(m.data(),0x800d6b44,sub/8);write_u32(m.data(),0x800d6b48,sub%8);write_u32(m.data(),0x800d6b4c,list);
    responses=0;auto c=context();c.f_odd=&c.f0.u32h;
    if(legacy)rr64_static_collision_legacy(m.data(),&c);else func_80062594(m.data(),&c);
    Result r;for(unsigned i=0;i<r.body.size();++i)r.body[i]=word(m,actor+body_offset+i*4);r.response=responses;return r;
}
void prepare(std::vector<unsigned char>& m){
    rr64::online_terrain::reset();auto c=context();c.f_odd=&c.f0.u32h;
    check(rr64_online_terrain_prepare(m.data(),&c)==1,"production collision bank prepares");
}
bool force_changed(const std::vector<unsigned char>& a,const std::vector<unsigned char>& b){
    for(unsigned o:{0x100u,0x104u,0x108u,0x170u,0x174u,0x178u})
        if(word(a,actor+body_offset+o)!=word(b,actor+body_offset+o))return true;
    return false;
}
void grids_unchanged(const std::vector<unsigned char>& before,const std::vector<unsigned char>& after){
    for(unsigned base:{grid,objects})check(std::equal(before.begin()+base-kRdramBegin,before.begin()+base-kRdramBegin+4900*16,after.begin()+base-kRdramBegin),"render/DMA descriptors remain unchanged");
}
void advance(std::vector<unsigned char>& m){
    auto c=context();c.f_odd=&c.f0.u32h;c.r4=guest_address(actor+body_offset);
    c.r5=std::bit_cast<unsigned>(1.0f/60);func_80034370(m.data(),&c);
}
std::pair<Vec,Vec> building_segment(const Source& s){
    const unsigned p=s.object_source+0x88+8*0x30;
    Vec a{},b{};for(unsigned k=0;k<3;++k)a[k]=b[k]=std::bit_cast<float>(be32(p+16+k*4));
    a[0]-=15;b[0]+=15;a[2]+=1;b[2]+=1;
    return {a,b};
}
void authentication_and_ownership(){
    actor_kind=2;const auto s=source(1096);const auto [a,b]=building_segment(s);
    auto expected=image(s,true);body(expected,a,b,.5f);const auto result=run(expected,s.cell,5,true);
    auto m=image(s,false);body(m,a,b,.5f);prepare(m);
    const auto prepared=m;
    // Alternate cells, including a nested floor lookup, without sharing either
    // reusable payload. The native force oracle must remain bit-identical.
    for(unsigned pass=0;pass<3;++pass){
        run(m,1271,0,false);
        const auto collision_before=m;
        auto c=context();c.r4=guest_address(grid+1271*16);c.r5=1;c.r8=guest_address(0x80400000);
        rr64_online_terrain_lookup(m.data(),&c);
        const unsigned allocation=0x80500008,collision=allocation+48+word(m,allocation+4),
                       end=allocation+word(m,allocation+24);
        check(std::equal(m.begin()+collision-kRdramBegin,m.begin()+end-kRdramBegin,
                         collision_before.begin()+collision-kRdramBegin),"floor lookup cannot overwrite active static collision payload");
        body(m,a,b,.5f);check(run(m,s.cell,5,false).body==result.body,"alternating collision and floor scratch preserves contacts");
    }
    grids_unchanged(prepared,m);
    const auto live=m;
    auto private_image=prepared;
    {
        rr64::prediction::ReplayScope scope;
        check(rr64::online_terrain::bind_replay(private_image.data(),rom),"valid captured bank binds before corruption");
        write_u32(private_image.data(),objects+s.cell*16+4,word(private_image,objects+s.cell*16+4)^8);
        bool refused=false;
        try {run(private_image,s.cell,5,false);}
        catch(const rr64::online_terrain::CollisionUnavailable&){refused=true;}
        check(refused,"changed historical placement source refuses instead of reading stale bytes");
        check(m==live,"invalid private collision leaves live image unchanged");
        check(!rr64::online_terrain::bind_replay(private_image.data(),rom),"corrupt object descriptors fail bank authentication");
        private_image=prepared;
        write_u32(private_image.data(),grid+s.cell*16+4,word(private_image,grid+s.cell*16+4)^8);
        check(!rr64::online_terrain::bind_replay(private_image.data(),rom),"corrupt terrain descriptors fail bank authentication");
        private_image=prepared;
        write_u32(private_image.data(),0x80500008,0x52525443);
        check(!rr64::online_terrain::bind_replay(private_image.data(),rom),"old diagnostic snapshot layout is refused (not a campaign save)");
    }
    // Bank construction and historical authentication are timed separately;
    // these host measurements are diagnostic, not a hardware performance gate.
    using Clock=std::chrono::steady_clock;double preparation_ms=0,binding_ms=0;
    for(unsigned i=0;i<8;++i){
        auto fresh=image(s,false);auto begin=Clock::now();prepare(fresh);
        preparation_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        auto private_copy=fresh;rr64::prediction::ReplayScope scope;begin=Clock::now();
        check(rr64::online_terrain::bind_replay(private_copy.data(),rom),"repeated private authentication succeeds");
        binding_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    }
    std::cout<<"host_mean_ms prepare="<<preparation_ms/8<<" bind="<<binding_ms/8<<'\n';
}
void combined_bank_and_imported_scope(){
    // An installed pack appends a combined bank, preserving stock cells. Copy
    // from the externally supplied ROM only; no game asset enters the fixture.
    unsigned end=0;for(unsigned i=0;i<4900;++i){const auto s=source(i);end=std::max(end,s.source+s.bytes);}
    const auto original_bytes=rom.size();
    std::vector<unsigned char> appended(rom.begin()+0x18d380,rom.begin()+end);
    rom.resize(imported_header+appended.size());std::copy(appended.begin(),appended.end(),rom.begin()+imported_header);
    imported=true;actor_kind=2;
    const auto s=source(1096);const auto [a,b]=building_segment(s);
    auto resident=image(s,true);body(resident,a,b,.5f);const auto expected=run(resident,s.cell,5,true);
    auto fixed=image(s,false);body(fixed,a,b,.5f);prepare(fixed);
    check(run(fixed,s.cell,5,false).body==expected.body,"relocated combined-bank stock cell keeps authored buildings");
    {
        rr64::prediction::ReplayScope scope;
        check(rr64::online_terrain::bind_replay(fixed.data(),rom),"relocated combined bank authenticates historically");
        imported=false;body(fixed,a,b,.5f);
        check(run(fixed,s.cell,5,false).body==expected.body,"private stock identity does not depend on live installed flag");
        imported=true;
    }
    using Clock=std::chrono::steady_clock;double preparation_ms=0,binding_ms=0;
    for(unsigned i=0;i<8;++i){
        auto fresh=image(s,false);auto begin=Clock::now();prepare(fresh);
        preparation_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        auto private_copy=fresh;rr64::prediction::ReplayScope scope;begin=Clock::now();
        check(rr64::online_terrain::bind_replay(private_copy.data(),rom),"combined bank repeatedly authenticates");
        binding_ms+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    }
    std::cout<<"combined_host_mean_ms prepare="<<preparation_ms/8<<" bind="<<binding_ms/8<<'\n';
    // Changing an otherwise valid terrain cell makes it a non-stock imported
    // cell. It must not receive the original building from the same grid slot.
    rom[s.source+s.bytes-1]^=1;
    auto custom=image(s,false);body(custom,a,b,.5f);prepare(custom);const auto before=custom;
    const auto absent=run(custom,s.cell,5,true);
    custom=before;const auto actual=run(custom,s.cell,5,false);
    check(actual.body==absent.body&&!force_changed(before,custom),"imported cell cannot acquire an invisible stock building");
    grids_unchanged(before,custom);
    rom.resize(original_bytes);imported=false;rr64::online_terrain::reset();
    std::cout<<"source_scope combined_stock=pass imported_negative=pass historical_identity=pass\n";
}
void results_collision(){
    // The results dispatcher continues native physics after the reel. A replay
    // camera can leave the terminal actor's cell unloaded; compare that exact
    // condition against the original resident contact and following motion.
    const auto s=source(1096);const auto [a,b]=building_segment(s);
    unsigned cases=0;
    for(unsigned kind:{1u,2u})for(float radius:{.5f,1.5f}){
        actor_kind=kind;
        auto resident=image(s,true);body(resident,a,b,radius);
        const auto untouched=resident;const auto expected=run(resident,s.cell,5,true);
        check(force_changed(untouched,resident),"results oracle has a real authored building contact");
        for(unsigned mode:{0xbu,0x14u,0x19u,0x1eu})for(unsigned owner=0;owner<3;++owner){
            rules={};
            auto fixed=image(s,false);body(fixed,a,b,radius);prepare(fixed);
            write_u32(fixed.data(),globals::main_mode,mode);
            write_u32(fixed.data(),globals::pending_mode,mode);
            if(owner){rules.active=rules.connected=rules.authoritative=true;
                rules.is_host=owner==1;rules.phase=rr64::netplay::Phase::Race;}
            const auto before=fixed;
            const auto actual=run(fixed,s.cell,5,false);
            check(actual.body==expected.body&&actual.response==expected.response,
                  "results collision matches resident contact for offline/host/guest");
            grids_unchanged(before,fixed);
            auto next=resident;advance(next);advance(fixed);
            for(unsigned q=0;q<128;++q)check(word(fixed,actor+body_offset+q*4)==word(next,actor+body_offset+q*4),
                "results next motion retains resident building response");
            ++cases;
        }
    }
    rules={};actor_kind=2;
    auto ready=image(s,false);body(ready,a,b,.5f);prepare(ready);
    for(unsigned refusal=0;refusal<5;++refusal){
        auto m=ready;rules={};
        const unsigned mode=refusal==0?0x4u:refusal==1?0x39u:0x19u;
        write_u32(m.data(),globals::main_mode,mode);write_u32(m.data(),globals::pending_mode,mode);
        if(refusal>=2){rules.active=true;rules.connected=refusal!=2;rules.authoritative=refusal!=3;
            rules.phase=refusal==4?rr64::netplay::Phase::TrackSelect:rr64::netplay::Phase::Race;}
        // Bind the reusable bank to this image before applying the refusal;
        // a mapping mismatch would otherwise hide an overly broad mode gate.
        const auto selected=rules;rules={};write_u32(m.data(),globals::main_mode,0x17);
        prepare(m);rules=selected;write_u32(m.data(),globals::main_mode,mode);
        const auto before=m;const auto actual=run(m,s.cell,5,false);
        auto stock=before;const auto expected=run(stock,s.cell,5,true);
        check(actual.body==expected.body&&!force_changed(before,m),"menus/disconnected/non-authoritative sessions retain stock scope");
        grids_unchanged(before,m);
    }
    rules={};
    std::cout<<"results_collision cases="<<cases<<" all_result_modes=4 ownership_refusals=5\n";
}
void results_floors(){
    struct Floor { unsigned hit,height,surface;bool operator==(const Floor&)const=default; };
    const auto floor=[](std::vector<unsigned char>& m,Vec point){
        constexpr unsigned query=0x80400000;
        std::memset(m.data()+query-kRdramBegin,0,0x6c);
        auto c=context();c.f_odd=&c.f0.u32h;c.r4=guest_address(query);
        func_80014604(m.data(),&c);
        for(unsigned i=0;i<3;++i)write_float(m.data(),query+i*4,point[i]*4);
        c.r4=guest_address(query);func_80014DE4(m.data(),&c);
        std::uint16_t surface=0;read_u16(m.data(),query+0x64,surface);
        return Floor{unsigned(c.r2)!=0,word(m,query+8),surface};
    };
    unsigned supported=0;
    for(unsigned cell:{1096u,1271u,1272u,1341u,1342u}){
        const auto s=source(cell);if(!s.bytes)continue;
        for(float dx:{20.f,125.f,240.f})for(float dy:{20.f,125.f,240.f})for(float z:{25.f,75.f,150.f}){
            const Vec point{float(cell/70)*250-8750+dx,float(cell%70)*250-8750+dy,z};
            auto resident=image(s,true);const auto expected=floor(resident,point);
            if(!expected.hit)continue;
            ++supported;
            for(unsigned mode:{0xbu,0x14u,0x19u,0x1eu}){
                auto fixed=image(s,false);prepare(fixed);
                write_u32(fixed.data(),globals::main_mode,mode);write_u32(fixed.data(),globals::pending_mode,mode);
                const auto before=fixed;
                check(floor(fixed,point)==expected,"all result modes retain exact native floor height/surface after eviction");
                grids_unchanged(before,fixed);
                if(supported==1){
                    auto private_image=before;
                    const auto live=fixed;
                    rr64::prediction::ReplayScope scope;
                    check(rr64::online_terrain::bind_replay(private_image.data(),rom),"results private image owns authenticated terrain scratch");
                    check(floor(private_image,point)==expected,"historical result floor preserves native support");
                    check(fixed==live,"historical floor leaves the live guest unchanged");
                }
            }
        }
    }
    check(supported!=0,"authored terrain supplies actual result floor contacts");
    std::cout<<"results_floor supported_points="<<supported<<" result_modes=4\n";
}
}
namespace recomp{std::span<const std::uint8_t> get_rom(){return rom;}}
namespace rr64::netplay{PhysicsRules get_physics_rules(){return rules;}}
namespace rr64::experimental_course{
bool installed() noexcept{return imported;}
bool active() noexcept{return imported;}
bool cell_allowed(unsigned) noexcept{return true;}
unsigned terrain_rom_offset() noexcept{return imported_header;}
}
extern "C" void func_800784B4(unsigned char*,recomp_context* c){c->r2=1;}
extern "C" void func_80078588(unsigned char*,recomp_context* c){c->r2=1;}
extern "C" void func_8004E754(unsigned char*,recomp_context*){std::abort();}
// Stock floor cases: only the unrelated camera observer/imported indexing are
// boundaries; original floor arithmetic and production terrain bank are real.
extern "C" void rr64_highlight_camera_floor_cell(unsigned char*,void*){}
extern "C" void rr64_experimental_course_floor_indices(unsigned char*,void*){}
extern "C" void rr64_experimental_course_floor_subindices(unsigned char*,void*){}
extern "C" int rr64_experimental_course_floor_cell_allowed(unsigned){return 1;}
#define RESPONSE(name) extern "C" void name(unsigned char*,recomp_context*){++responses;}
RESPONSE(func_80056B04) RESPONSE(func_80056BA8) RESPONSE(func_80056DA8)
RESPONSE(n_alSeqpDelete_copy_80056F10) RESPONSE(n_alSeqpDelete_copy_80056F2C)
RESPONSE(func_800565BC) RESPONSE(func_80061BBC)
extern "C" void do_break(std::uint32_t){std::abort();}
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    check(argc==3,"ROM and retained stock snapshot arguments");if(argc!=3)return 1;
    rom=read(argv[1]);captured=read(argv[2]);check(rom.size()>=0x2000000&&captured.size()>=kRdramSize,"complete fixture inputs");
    if(failures)return 1;
    unsigned cases=0,contact_cases=0;
    // Search authored building placements, preserving their rotations,
    // subcell references and native collision descriptors from the user's ROM.
    for(unsigned cell=0;cell<4900 && contact_cases<4;++cell){
        const auto s=source(cell);if(!s.bytes||!s.object_bytes)continue;
        for(unsigned i=0;i<be32(s.object_source) && contact_cases<4;++i){
            const unsigned p=s.object_source+0x88+i*0x30,d=be16(p+0x28);if(d<197||d>215)continue;
            Vec pos{};for(unsigned k=0;k<3;++k)pos[k]=std::bit_cast<float>(be32(p+16+k*4));
            auto base=image(s,true);
            for(unsigned sub=0;sub<64&&contact_cases<4;++sub){
                for(unsigned axis=0;axis<2&&contact_cases<4;++axis){
                    auto m=base;Vec a=pos,b=pos;a[axis]-=15;b[axis]+=15;a[2]+=1;b[2]+=1;body(m,a,b,.5f);
                    const auto before=m;const auto result=run(m,cell,sub,true);++cases;
                    if(force_changed(before,m)){
                        ++contact_cases;std::cout<<"contact cell="<<cell<<" placement="<<i<<" descriptor="<<d<<" sub="<<sub<<" axis="<<axis<<'\n';
                        auto absent=image(s,false);body(absent,a,b,.5f);const auto unchanged=absent;
                        run(absent,cell,sub,true);check(!force_changed(unchanged,absent),"negative control: unloaded native scene loses the real building contact");
                        for(unsigned mode=0;mode<3;++mode){
                            auto fixed=mode==2?image(s,true):image(s,false);
                            if(mode==2)write_u32(fixed.data(),objects+cell*16,0);
                            body(fixed,a,b,.5f);prepare(fixed);const auto prepared=fixed;
                            const auto actual=run(fixed,cell,sub,false);
                            check(actual.body==result.body&&actual.response==result.response,"production fallback matches native resident force/contact bytes");
                            grids_unchanged(prepared,fixed);
                            auto resident_next=m;advance(resident_next);advance(fixed);
                            for(unsigned q=0;q<128;++q)check(word(fixed,actor+body_offset+q*4)==word(resident_next,actor+body_offset+q*4),"next native motion agrees after collision fallback");
                            if(mode==1){
                                auto private_image=prepared;
                                auto other=image(source(1271),true);prepare(other);
                                const auto live_before=other;
                                rr64::prediction::ReplayScope scope;
                                check(rr64::online_terrain::bind_replay(private_image.data(),rom),"private snapshot authenticates its own collision scratch");
                                const auto historical=run(private_image,cell,sub,false);
                                check(historical.body==result.body,"private replay uses captured collision payload despite different live bank");
                                check(other==live_before,"private collision cannot mutate live bank");
                            }
                        }
                    }
                }
            }
        }
    }
    check(contact_cases>0,"actual native contact forces against authored buildings");
    actor_kind=1;
    {
        const auto s=source(1096);const auto [a,b]=building_segment(s);
        for(float radius:{.5f,1.5f}){
            auto resident=image(s,true);body(resident,a,b,radius);const auto before=resident;
            const auto expected=run(resident,s.cell,5,true);
            check(force_changed(before,resident),"bike contact exists at normal and giant collision radius");
            auto fixed=image(s,false);body(fixed,a,b,radius);prepare(fixed);
            const auto actual=run(fixed,s.cell,5,false);
            check(actual.body==expected.body&&actual.response==expected.response,"bike contact matches resident native path at both radii");
            auto resident_next=resident;advance(resident_next);advance(fixed);
            for(unsigned q=0;q<128;++q)check(word(fixed,actor+body_offset+q*4)==word(resident_next,actor+body_offset+q*4),"bike next-motion agrees at both radii");
        }
    }
    authentication_and_ownership();
    combined_bank_and_imported_scope();
    results_collision();
    results_floors();
    std::cout<<"Static collision smoke: cases="<<cases<<" contacts="<<contact_cases<<" failures="<<failures<<'\n';return failures?1:0;
}
