#include "rr64_prediction_case.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
static void check(bool v){if(!v)std::abort();}
int main(int argc,char**){
    using namespace rr64::prediction;
    if(argc>1){
        std::printf("%zu %zu %zu %zu %zu\n",sizeof(ReplayCase::Header),sizeof(FrameInput),sizeof(FrameOutput),offsetof(FrameInput,visibility),sizeof(VisibilityInputs));
        std::printf("%zu %zu\n",offsetof(FrameInput,terrain),sizeof(TerrainAvailability));
        return 0;
    }
    // Rendering may change bit zero between updates; other flags remain owned
    // by the private simulation. A stale second object must cause no writes.
    {
        std::vector<unsigned char> memory(rr64::engine::kRdramSize);
        auto* m=memory.data();using namespace rr64::engine;
        write_u32(m,0x800a656c,2);
        for(unsigned i=0;i<2;++i){
            unsigned actor=0x800d8570+i*0x118,bike=0x80200000+i*0x1000;
            unsigned object=0x80300000+i*0x100,descriptor=0x80400000+i*0x100;
            write_u16(m,actor+0x24,1);write_u32(m,actor+0xe0,bike);
            write_u32(m,bike+8,object);write_u32(m,object+0x28,descriptor);
            write_u16(m,descriptor+0xa,0x1001);
        }
        VisibilityInputs v;check(capture_visibility(m,v) && v.count==2);
        v.entries[0].distances={11,22,33,44};v.view_shift=2;v.distance_limit=123;
        {VisibilityScope scope(v);
         for(unsigned i=0;i<4;++i)check(historical_visibility_distance(0x80300000,0x80300008+i*4,99)==11*(i+1));
         check(historical_visibility_distance(0x80300900,0x80300908,99)==99);
         check(historical_visibility_parameter(true,0)==2 && historical_visibility_parameter(false,0)==123);
        }
        check(historical_visibility_parameter(false,99)==99);
        write_u16(m,0x8040000a,0x2000);write_u16(m,0x8040010a,0x4000);
        // Descriptor replacement is allowed: read override is bound to object,
        // never written into either old or new descriptor memory.
        write_u32(m,0x80300128,0x80400200);
        auto before=memory;check(validate_visibility(m,v));
        {VisibilityScope scope(v);check(historical_visibility_bit(0x80300100,0)==1);
         check(historical_visibility_bit(0x80300900,0)==0);}
        check(memory==before && historical_visibility_bit(0x80300100,0)==0);
        write_u32(m,0x80201008,0x80300200);
        before=memory;check(!validate_visibility(m,v) && memory==before);
        v.count=15;check(!validate_visibility(m,v) && memory==before);
    }
    auto path=std::filesystem::temp_directory_path()/("rr64-case-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".rrcase");
    {
        std::vector<unsigned char> memory(rr64::engine::kRdramSize);auto* m=memory.data();
        rr64::engine::write_u32(m,0x800dea8c,2);rr64::engine::write_u32(m,0x800ddea4,0x80200000);
        rr64::engine::write_s8(m,0x8020000c,5);
        TerrainAvailability t;check(capture_terrain_availability(m,t));auto before=memory;
        {TerrainAvailabilityScope scope(t);
         check(historical_terrain_state(0x80200010,5)==0); // unloaded live, retained privately
         check(historical_terrain_state(0x80200000,5)==5);
         bool rejected=false;try{historical_terrain_state(0x80200000,1);}catch(const Resources::Invalid&){rejected=true;}check(rejected);
         rejected=false;try{historical_terrain_state(0x80300000,5);}catch(const Resources::Invalid&){rejected=true;}check(rejected);
        }
        check(memory==before && historical_terrain_state(0x80200010,5)==5);
    }
    ReplayCase c;c.header.attempt=42;c.header.humans=1;
    c.before_live.assign(rr64::engine::kRdramSize,3);
    c.before_private.assign(rr64::engine::kRdramSize,5);
    c.after_live.assign(rr64::engine::kRdramSize,7);
    recomp_context context{};context.f_odd=&context.f0.u32h;context.r4=123;
    check(c.input.entry.capture(context));
    // Item effects, exact partial duration, and unscaled collision geometry
    // belong to the same diagnostic ABI as the historical native context.
    c.input.replay_duration_us=8333;
    auto &items=c.input.items;
    items.state.enabled=1;items.state.clock=30;items.state.random=17;
    items.state.riders[13].shrink_until=50;items.elapsed_us=1000000;
    auto &actor=items.actors[13];
    actor.owner={0x800d8570,0x80200000,0x80300000,0x80400000,25,3};
    actor.bike_geometry.body=actor.owner.bike+0x108;
    actor.bike_geometry.count=3;actor.bike_geometry.active=true;
    for(unsigned i=0;i<12;++i){
        actor.bike_geometry.original[i]=std::bit_cast<unsigned>(float(i+1));
        actor.bike_geometry.written[i]=std::bit_cast<unsigned>(float(i+1)*.5f);
    }
    c.expected_native.items=items;c.expected_native.items.elapsed_us+=8333;
    check(write_case(path,c));check(!write_case(path,c));
    ReplayCase read;check(read_case(path,read));
    check(read.header.attempt==42 && read.before_live==c.before_live && read.before_private==c.before_private && read.after_live==c.after_live);
    check(read.input.replay_duration_us==8333 && read.input.items==c.input.items &&
          read.expected_native.items==c.expected_native.items);
    recomp_context restored{};check(read.input.entry.restore(restored));check(restored.r4==123 && restored.f_odd==&restored.f0.u32h);
    {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
     const std::uint32_t old_version=5;file.seekp(offsetof(ReplayCase::Header,version));
     file.write(reinterpret_cast<const char*>(&old_version),sizeof(old_version));}
    check(!read_case(path,read) && read.input.items==c.input.items);
    // Corrupt ABI metadata and truncated images must leave output untouched.
    {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);char bad=0;file.write(&bad,1);}
    check(!read_case(path,read) && read.header.attempt==42 && read.before_private==c.before_private);
    std::filesystem::resize_file(path,24);check(!read_case(path,read));
    std::filesystem::remove(path);
    std::puts("Private replay case: three-image round trip, context rebasing, no overwrite, malformed/truncated rejection passed");
}

