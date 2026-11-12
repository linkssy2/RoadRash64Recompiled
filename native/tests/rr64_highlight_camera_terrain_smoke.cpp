#include "rr64_highlight_camera_terrain.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_online_terrain.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_highlight_camera_terrain_inputs.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#include <cmath>

using namespace rr64;
using namespace rr64::engine;
using namespace rr64::highlight_camera;
namespace camera_terrain = rr64::highlight_camera::terrain;
extern "C" void func_80014604(unsigned char*, recomp_context*);
extern "C" void func_80014DE4(unsigned char*, recomp_context*);
namespace {
unsigned checks = 0;
std::vector<unsigned char> rom, live;
std::span<const std::uint8_t> synthetic;
std::shared_ptr<const void> owner = std::make_shared<int>(1);
bool available = true, imported = false;
constexpr unsigned grid = 0x80100000, payload = 0x80200000, query = 0x80600000;
unsigned be32(unsigned p) {
    return unsigned(rom[p]) << 24 | unsigned(rom[p+1]) << 16 | unsigned(rom[p+2]) << 8 | rom[p+3];
}
void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
std::vector<unsigned char> read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void copy(unsigned char* m, unsigned address, std::span<const unsigned char> b) {
    for (unsigned i=0;i<b.size();++i) m[((address-kRdramBegin)+i)^3u]=b[i];
}
std::span<const unsigned char> cell(unsigned index) {
    if (!synthetic.empty()) return index == 35*70+35 ? synthetic : std::span<const unsigned char>{};
    const unsigned table=0x18d398, textures=(unsigned(rom[0x18d394])<<8)|rom[0x18d395];
    const unsigned entry=table+(textures+index)*12, offset=be32(entry), size=be32(entry+4);
    return offset && size ? std::span<const unsigned char>(rom).subspan(entry+offset,size) : std::span<const unsigned char>{};
}
camera_terrain::Floor oracle(const Vec3& point, bool& hit) {
    // Independently reconstruct a complete private native image from the real
    // captured stock snapshot, replacing only one resident source descriptor.
    auto m=live;
    const auto ix=unsigned((point[0]*4+35000)/1000), iy=unsigned((point[1]*4+35000)/1000);
    const unsigned index=ix*70+iy;
    const auto data=cell(index);
    copy(m.data(),payload,data);
    write_u32(m.data(),globals::terrain_cell_grid,grid);
    std::memset(m.data()+grid-kRdramBegin,0,4900*16);
    if (!data.empty()) {
        write_u32(m.data(),grid+index*16,payload);
        write_s8(m.data(),grid+index*16+12,5);
    }
    std::memset(m.data()+query-kRdramBegin,0,0x6c);
    recomp_context c{};c.f_odd=&c.f0.u32h;c.r29=guest_address(0x807f0000);c.r4=guest_address(query);
    func_80014604(m.data(),&c);
    for(unsigned i=0;i<3;++i)write_float(m.data(),query+i*4,point[i]*4);
    c.r4=guest_address(query);func_80014DE4(m.data(),&c);
    float height=0;std::uint16_t surface=0;
    read_float(m.data(),query+8,height);read_u16(m.data(),query+0x64,surface);
    hit=c.r2!=0;return {height*.25f,surface};
}
void compare(const Vec3& point) {
    bool expected=false;const auto a=oracle(point,expected);
    camera_terrain::Floor b;const bool actual=camera_terrain::floor(live.data(),point,b);
    check(actual==expected,"isolated camera hit agrees with actual native image");
    if(actual){check(a.height==b.height,"exact native floor height preserved");check(a.surface==b.surface,"native surface choice preserved");}
}
void use(std::span<const unsigned char> data) { synthetic=data;owner=std::make_shared<int>(1); }
void synthetic_cases() {
    use(cell_flat);
    for(float x:{.0625f,31.1875f,31.25f,31.3125f,125.f,249.9375f})
        for(float y:{.0625f,31.1875f,31.25f,31.3125f,125.f,249.9375f})compare({x,y,50});
    use(cell_steep);
    for(float x:{1.f,50.f,150.f,249.f})compare({x,108.75f,50});
    View v{{150,108.75f,60},{145,108.75f,57},{0,0,1}};const auto target=v.target;
    camera_terrain::Floor floor;check(camera_terrain::floor(live.data(),v.eye,floor),"steep bank is actual collision");
    check(floor.height>v.eye[2],"old cinematic eye is inside steep bank");
    check(camera_terrain::clear_eye(live.data(),v),"camera candidate clears steep native bank");
    check(v.eye[2]==floor.height+.25f && v.target==target,"only eye gains quarter-unit real floor margin");
    const auto original=live;
    View penetrated{{150,108.75f,60},{145,108.75f,57},{0,0,1}};
    std::thread draw([&]{
        if(!highlight_camera::begin(live.data(),penetrated))std::abort();
        rr64_highlight_camera_apply(live.data());
        float actual=0;read_float(live.data(),0x800d6a30,actual);
        check(actual==floor.height+.25f,"production cinematic camera uses corrected floor");
        highlight_camera::end(live.data());
    });
    draw.join();check(live==original,"real cinematic draw scope restores every live byte after corrected eye");
    for(auto bytes:{std::span<const unsigned char>(cell_stacked),std::span<const unsigned char>(cell_stacked_reverse)}) {
        use(bytes);
        for(float z:{24.f,26.f,39.f,41.f,60.f})compare({125,125,z});
        View under{{125,125,28},{125,120,26},{0,0,1}}, before=under;
        check(!camera_terrain::clear_eye(live.data(),under) && under.eye==before.eye,"road below overpass does not lift camera to deck");
    }
    use(cell_tunnel);compare({125,125,28});
    View tunnel{{125,125,28},{125,120,26},{0,0,1}};check(!camera_terrain::clear_eye(live.data(),tunnel),"tunnel ceiling does not become floor");
    use(cell_ceiling);compare({125,125,28});check(!camera_terrain::clear_eye(live.data(),tunnel),"underside alone does not fabricate support");
    use(cell_empty);compare({125,125,28});
    use(cell_surface);compare({125,125,50});
    use(cell_flat);View distant{{125,125,-50},{125,120,-49},{0,0,1}};
    check(!camera_terrain::clear_eye(live.data(),distant),"distant terrain cannot teleport a fallen subject camera");
    const auto snapshot=live;
    std::thread a([&]{for(unsigned i=0;i<100;++i){camera_terrain::Floor f;if(!camera_terrain::floor(live.data(),{125,125,24},f)||f.height!=25)std::abort();}});
    std::thread b([&]{for(unsigned i=0;i<100;++i){camera_terrain::Floor f;if(!camera_terrain::floor(live.data(),{125,125,50},f)||f.height!=25)std::abort();}});
    a.join();b.join();check(live==snapshot,"two worker query streams leave complete live image unchanged");
    available=false;camera_terrain::Floor f{17,9};check(!camera_terrain::floor(live.data(),{125,125,24},f)&&f.height==17,"unavailable bank leaves output unchanged");available=true;
    check(!camera_terrain::floor(live.data(),{INFINITY,0,0},f),"nonfinite eye rejected");
    check(!camera_terrain::floor(live.data(),{-9000,0,0},f),"out-of-bounds eye rejected");
    {prediction::ReplayScope p;check(!camera_terrain::floor(live.data(),{125,125,24},f),"private physics replay cannot invoke presentation query");}
    imported=true;
    for(float x:{0.f,std::nextafter(0.f,-1.f),31.25f,249.9375f})compare({x,125,24});
    imported=false;
}
}
namespace recomp {std::span<const std::uint8_t> get_rom(){return rom;}}
namespace rr64::online_terrain {
bool immutable_cell(unsigned char* m,unsigned index,std::span<const std::uint8_t>& out,std::shared_ptr<const void>* identity) noexcept {
    out={};if(identity)identity->reset();
    if(!available||m!=live.data()||index>=4900)return false;
    out=cell(index);if(out.empty())return false;if(identity)*identity=owner;return true;
}
}
namespace rr64::experimental_course {
bool active() noexcept{return imported;}
bool installed() noexcept{return imported;}
bool cell_allowed(unsigned) noexcept{return true;}
}
extern "C" void rr64_online_terrain_query_begin(unsigned char*,void*){}
extern "C" void rr64_online_terrain_lookup(unsigned char*,void*){}
extern "C" void _nsqrtf(unsigned char*,recomp_context* c){c->f0.fl=std::sqrt(c->f12.fl);}
extern "C" void do_break(std::uint32_t){std::abort();}
int main(int argc,char** argv) {
    check(argc==3,"ROM and retained stock snapshot arguments");rom=read(argv[1]);live=read(argv[2]);
    check(rom.size()>=8*1024*1024 && live.size()>=kRdramSize,"complete fixture sources");
    const auto untouched=live;
    // Original snapshot constants must match the immutable source used by the
    // camera; a changed mapping must fail instead of accepting invented values.
    for(unsigned a=0xd00;a<0xdb0;++a)check(live[a^3u]==rom[a+0xc00],"native floor constants match ROM");
    for(unsigned a=0x9f2b0;a<0x9f2b0+15*20;++a)check(live[a^3u]==rom[a+0xc00],"native material constants match ROM");
    unsigned real_samples=0, real_adjustments=0, largest_cell=0, largest_bytes=0;
    for(unsigned i=0;i<4900;++i)if(cell(i).size()>largest_bytes){largest_cell=i;largest_bytes=unsigned(cell(i).size());}
    Vec3 largest_point{float(largest_cell/70)*250-8625,float(largest_cell%70)*250-8625,100};
    camera_terrain::Floor benchmark;
    const auto first_start=std::chrono::steady_clock::now();
    camera_terrain::floor(live.data(),largest_point,benchmark);
    const auto first_end=std::chrono::steady_clock::now();
    for(unsigned cell_index:{1271u,1272u,1341u,1342u,1411u,1412u}) {
        if(cell(cell_index).empty())continue;
        for(float dx:{20.f,125.f,240.f})for(float dy:{20.f,125.f,240.f})for(float z:{25.f,50.f,58.f,75.f}){
            Vec3 p{float(cell_index/70)*250-8750+dx,float(cell_index%70)*250-8750+dy,z};
            compare(p);++real_samples;
            camera_terrain::Floor contact;
            if(camera_terrain::floor(live.data(),p,contact)){
                View v{{p[0],p[1],contact.height-1},{p[0]+3,p[1]+4,contact.height},{0,0,1}};
                if(camera_terrain::clear_eye(live.data(),v))++real_adjustments;
            }
        }
    }
    check(real_samples && real_adjustments,"real stock terrain supports and clears native floor penetration");
    synthetic_cases();check(live==untouched,"all camera queries preserve every live RDRAM byte");
    use(cell_flat);camera_terrain::Floor f;
    const auto start=std::chrono::steady_clock::now();camera_terrain::floor(live.data(),{125,125,24},f);
    const auto warm=std::chrono::steady_clock::now();
    for(unsigned i=0;i<100000;++i)camera_terrain::floor(live.data(),{125,125,24},f);
    const auto finish=std::chrono::steady_clock::now();
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"stock_samples\":"<<real_samples<<",\"stock_clearances\":"<<real_adjustments
        <<",\"largest_cell_bytes\":"<<largest_bytes<<",\"first_largest_cell_us\":"<<std::chrono::duration<double,std::micro>(first_end-first_start).count()
        <<",\"cold_us\":"<<std::chrono::duration<double,std::micro>(warm-start).count()
        <<",\"warm_us_per_query\":"<<std::chrono::duration<double,std::micro>(finish-warm).count()/100000<<",\"game_launched\":false}\n";
}
