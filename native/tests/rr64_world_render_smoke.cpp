// Original world root writers and 18668 animation are the oracle. Rendering
// hooks are invoked directly; this does not emulate terrain streaming or GPU.
#include "rr64_world_render.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_actor_render_snapshot.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
void func_80018668(unsigned char*, recomp_context*);
void func_8005ED6C(unsigned char*, recomp_context*);
void func_8005EF74(unsigned char*, recomp_context*);
void func_8005F070(unsigned char*, recomp_context*);
void func_80015A90(unsigned char*, recomp_context*);
void func_800167BC(unsigned char*, recomp_context*);
void guPerspective(unsigned char*, recomp_context*);
// This producer oracle keeps the native distance decision. Presentation-only
// traffic range extension is exercised separately by its own fixture.
int rr64_traffic_within_draw_distance(unsigned char*, unsigned, unsigned, unsigned, int original) {
    return original;
}
}
namespace {
using namespace rr64::engine;
constexpr unsigned node = 0x80100000u, entity = 0x80110000u;
constexpr unsigned graph = 0x80200000u, poses = 0x80300000u, sources = 0x80310000u;
constexpr unsigned buffers = 0x80400000u, channels = 0x80500000u, keys = 0x80510000u;
constexpr unsigned stack = 0x807f0000u, matrix = 0x80600000u;
bool passed = true;
unsigned cases = 0;
void check(bool ok, const char* message) {
    if (!ok) { passed = false; std::fprintf(stderr, "[RR64-WORLD] FAILED: %s\n", message); }
}
template<std::size_t N> void floats(unsigned char* m, unsigned a, const std::array<float, N>& v) {
    for (unsigned i = 0; i < N; ++i) write_float(m, a + 4u * i, v[i]);
}
template<std::size_t N> void words(unsigned char* m, unsigned a, const std::array<unsigned, N>& v) {
    for (unsigned i = 0; i < N; ++i) write_u32(m, a + 4u * i, v[i]);
}
void byte(unsigned char* m, unsigned a, int v) { m[(a - kRdramBegin) ^ 3u] = static_cast<unsigned char>(v); }
float get(unsigned char* m, unsigned a) { float v = 0; check(read_float(m, a, v), "read finite guest float address"); return v; }
unsigned word(unsigned char* m, unsigned a) { unsigned v = 0; check(read_u32(m, a, v), "read guest word address"); return v; }
unsigned pos(unsigned type) { return type == 3u ? 0x7cu : type == 4u ? 0xa8u : 0x18u; }
unsigned quat(unsigned type) { return type == 3u ? 0x154u : type == 4u ? 0x180u : 8u; }
void bind_context(recomp_context& ctx, bool fr1) {
    ctx.mips3_float_mode = fr1; ctx.f_odd = fr1 ? &ctx.f1.u32l : &ctx.f0.u32h;
    ctx.r29 = guest_address(stack);
}
void constants(unsigned char* m) {
    words(m, 0x80000D08u, std::array<unsigned, 8>{0x3C010203u,0x3F800000u,0x3F800000u,0x3F800000u,0x3C010203u,0x3F800000u,0x3F800000u,0x3C010203u});
    words(m, 0x80000DC0u, std::array<unsigned, 4>{0xBF800000u,0x3F800000u,0x3F800000u,0x3A83126Fu});
    words(m, 0x80000E70u, std::array<unsigned, 9>{0x3F800000u,0x3ADA740Eu,0x3ADA740Eu,0x3F800000u,0x3ADA740Eu,0x3ADA740Eu,0x3F800000u,0x3ADA740Eu,0x3ADA740Eu});
    words(m, 0x80007D30u, std::array<unsigned, 15>{0x3FC90FDBu,0xBFC90FDBu,0x3F800000u,0x41600000u,0x3F800000u,0x41600000u,0x3F800000u,0xBFC90FDBu,0x3FC90FDBu,0x40490FDBu,0x40490FDBu,0x6C616773u,0x3F800000u,0x40490FDBu,0x3FC90FDBu});
    words(m, 0x80008058u, std::array<unsigned, 20>{0xBFC55554u,0xBC83656Du,0x3F8110EDu,0x3804C2A0u,0xBF29F6FFu,0xEEA56814u,0x3EC5DBDFu,0x0E314BFEu,0x3FD45F30u,0x6DC9C883u,0x400921FBu,0x50000000u,0x3E6110B4u,0x611A6263u,0u,0u,0x3FE00000u,0u,0x3FE00000u,0u});
    // __cosf's distinct ROM table has the same initialized polynomial words.
    for(unsigned i=0;i<20u;++i)write_u32(m,0x80007F88u+i*4u,word(m,0x80008058u+i*4u));
    constexpr std::array<unsigned,8> projection{0x3F91DF46u,0x9D353918u,0x40000000u,0u,0x41000000u,0u,0x41E00000u,0u};
    words(m,0x80008010u,projection);words(m,0x80008030u,projection);
    words(m,0x80008000u,std::array<unsigned,4>{0xBFF00000u,0u,0x3FF00000u,0u});
    write_float(m,0x80000E08u,32767.0f);write_float(m,0x80000E0Cu,1.0f);
    write_float(m, 0x80000DE0u, 1.0f);
    floats(m, 0x80005FB4u, std::array<float,5>{9000000.0f,56250000.0f,900000000.0f,268400000.0f,268400000.0f});
    floats(m, 0x8009DBACu, std::array<float,3>{4,100,10});
}
void packed(unsigned char* m, unsigned address, bool projection) {
    const std::array<float,16> v = projection ? std::array<float,16>{1,0,0,0,0,1,0,0,0,0,-1,-1,0,0,-20,0}
        : std::array<float,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned bits = static_cast<unsigned>(static_cast<int>(v[i] * 65536.0f));
        write_u16(m, address + i * 2u, bits >> 16); write_u16(m, address + 32u + i * 2u, bits);
    }
}
void banks(unsigned char* m, bool fr1 = false, float world_far = 700.0f) {
    const unsigned slot=word(m,globals::actor_render_buffer_slot);
    floats(m,0x8009DBC4u,std::array<float,4>{55.0f,1.0f,world_far,4.0f/3.0f});
    constexpr float scales[]{4,100,10};
    for(unsigned source=0;source<3;++source) {
        const unsigned bank=0x800B6B68u+word(m,globals::active_viewport)*0x9cu+source*0x34u; const float s=scales[source];
        floats(m,bank,std::array<float,3>{1.25f*s,2.5f*s,0.5f*s});
        floats(m,bank+12u,std::array<float,3>{1.25f*s,3.5f*s,0.5f*s});
        floats(m,bank+24u,std::array<float,3>{0,0,1});
        write_u16(m,bank+0x30u,1u);write_float(m,0x8009DBD8u+source*4u,1.0f);
    }
    write_u32(m,0x800A1830u,16u+slot);
    recomp_context ctx{};bind_context(ctx,fr1);func_800167BC(m,&ctx);
}
void animation(unsigned char* m, unsigned mode, unsigned motion, unsigned previous) {
    const unsigned clip = mode == 3u ? (motion == 1u ? 0x53u : 0x52u) : 0x54u;
    write_u32(m, entity + 0xcu, mode); write_u32(m, entity + 0x304u, motion);
    write_float(m, entity + 0x310u, 0.375f); write_u32(m, 0x8009DC2Cu, 2u);
    for (unsigned bone = 0; bone < 11; ++bone) {
        const unsigned channel = channels + bone * 0x40u, r = keys + bone * 0x80u, t = r + 0x40u;
        const unsigned table = 0x800B74B0u + clip * 132u + 2u * 11220u + bone * 12u;
        words(m, table, std::array<unsigned,3>{channel,r,t});
        byte(m, channel + 0xeu, bone == 3u || bone == 8u ? 0 : 2);
        byte(m, channel + 0xfu, bone == 5u || bone == 8u ? 0 : 2);
        write_float(m, channel + 0x14u, 0.23f); write_float(m, channel + 0x1cu, 0.41f);
        for (unsigned k = 0; k < 2; ++k) {
            byte(m, r + k * 8u + (k ? bone % 3u : 3u), bone % 2 ? -127 : 127);
            write_u16(m, r + k * 8u + 4u, k * 100u);
            write_u16(m, t + k * 8u, 100 + bone * 17u + k * 43u);
            write_u16(m, t + k * 8u + 2u, static_cast<unsigned short>(-200 - int(bone) * 13 + int(k) * 31));
            write_u16(m, t + k * 8u + 4u, 300 + bone * 19u - k * 53u);
            write_u16(m, t + k * 8u + 6u, k * 100u);
        }
        const float p = float(bone + 1u);
        floats(m, poses + (bone + 1u) * 32u, previous ? std::array<float,8>{-70-p,90+p,-110-p,0,1,0,0,77+p}
            : std::array<float,8>{10+p,-20-p,30+p,0,0,0,1,77+p});
    }
}
std::vector<unsigned char> seed(unsigned type, float distance, unsigned slot, unsigned previous = 0, unsigned mode = 3, unsigned motion = 0) {
    std::vector<unsigned char> memory(kRdramSize); auto* m = memory.data(); constants(m);
    write_u32(m, globals::main_mode, 0x17u); write_u32(m, globals::pending_mode, 0x17u);
    write_u32(m, 0x800A6578u, 1u); write_u32(m, 0x8009DB88u, 1u); write_u16(m, 0x800A65C4u, 1u);
    write_u32(m, globals::actor_render_buffer_slot, slot); write_u32(m, 0x800A1458u + (type-3u)*4u, node);
    write_u32(m, node, type); write_u32(m, node+4u, entity); write_u32(m,node+0x28u,graph);
    if (type != 5u) write_u16(m, entity + (type == 3u ? 8u : 0x334u), 1u);
    floats(m, 0x800D69F8u, std::array<float,3>{1.5f,-2.25f,3.125f});
    floats(m, 0x800A4FDCu, std::array<float,3>{4096,-2048,1024});
    floats(m, entity + pos(type), std::array<float,3>{4096+1.5f+7.0f,-2048-2.25f+distance,1024+3.125f+2.5f});
    floats(m, entity + quat(type), std::array<float,4>{0.5f,-0.5f,0.5f,0.5f});
    for (unsigned lod = 0; lod < (type == 3u ? 2u : 1u); ++lod) {
        const unsigned model = graph + lod * 0x1000u, count = type == 3u && !lod ? 12u : 1u;
        write_u32(m,node+0x2cu+lod*4u,model); write_u32(m,node+0x50u+lod*4u,sources+0x2000u+lod*0x100u);
        write_u32(m,node+0x13cu+lod*4u,sources+0x2008u+lod*0x100u);
        write_u16(m,node+0x4au+lod*2u,1u); write_u32(m,node+0x7cu+lod*0x20u,sources+0x2800u+lod*0x100u);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned record=model+i*32u, source=sources+lod*0x1000u+i*0x80u;
            write_u16(m,record+4u,i?0x12u:0x13u); write_u16(m,record+2u,i+1u==count?0u:4u);
            write_u16(m,record+8u,i+1u==count?0u:4u); write_u32(m,record+0xcu,poses+lod*0x1000u+i*32u);
            write_u32(m,record+0x14u,source); write_u16(m,source+0x12u,type==3u&&!lod?1u:2u);
            floats(m,poses+lod*0x1000u+i*32u,std::array<float,8>{0,0,0,0,0,0,1,123});
        }
    }
    for (unsigned b=0;b<2;++b) write_u32(m,node+0x5cu+b*4u,buffers+b*0x400u);
    if (type==3u) animation(m,mode,motion,previous);
    banks(m); return memory;
}
void roots(unsigned char* m, unsigned type, bool fr1) {
    recomp_context ctx{}; bind_context(ctx,fr1);
    if(type==3u) func_8005ED6C(m,&ctx); else if(type==4u) func_8005EF74(m,&ctx); else func_8005F070(m,&ctx);
    rr64_world_observe_roots(m,type);
}
void register_actor(unsigned char* m, unsigned bytes=0x400u) { rr64_world_invalidate(m); rr64_world_observe_allocation(m,node,0u,bytes); }
void direct(std::vector<unsigned char>& memory, bool fr1) {
    auto* m=memory.data(); recomp_context ctx{}; bind_context(ctx,fr1);
    const unsigned mode=word(m,entity+0xcu), motion=word(m,entity+0x304u);
    ctx.r4=guest_address(graph); ctx.r5=mode==3u?(motion==1u?0x53u:0x52u):0x54u;
    ctx.r6=0x100u; ctx.r7=word(m,entity+0x310u); write_float(m,stack+0x14u,1.0f); func_80018668(m,&ctx);
}
void positive(unsigned type, float distance, unsigned slot, bool fr1, unsigned previous, unsigned mode, unsigned motion) {
    auto memory=seed(type,distance,slot,previous,mode,motion); auto* m=memory.data(); register_actor(m);
    // Deliberately leave banks unavailable until after the root producer.
    write_u16(m,0x800B6C00u,0u); roots(m,type,fr1); banks(m);
    const auto before=memory; auto oracle=memory; if(type==3u) direct(oracle,fr1);
    recomp_context caller{}; bind_context(caller,fr1); caller.r20=0x12345678u;
    const auto caller_before=caller; const unsigned lod=type==3u&&distance>=30.0f?1u:0u;
    rr64_world_begin_draw(m);
    check(rr64_world_actor_hidden(m,node,1u,&caller)==0u,"fresh active world actor survives distance/frustum hidden bit");
    check(rr64_world_select(m,node,lod)==0u,"prepared pedestrian selects detailed graph");
    check(std::memcmp(&caller,&caller_before,sizeof(caller))==0,"private animation preserves caller/FPU alias");
    for(unsigned i=0;i<(type==3u?12u:1u);++i) for(unsigned j=0;j<8u;++j) {
        const auto address=poses+i*32u+j*4u;
        check(word(m,address)==word(oracle.data(),address),"all eleven current bones equal exact original 18668 including zero channels");
    }
    for(unsigned axis=0;axis<3;++axis) {
        const float expected=(get(m,entity+pos(type)+axis*4u)-get(m,0x800D69F8u+axis*4u)-get(m,0x800A4FDCu+axis*4u))*(type==3u?100.0f:10.0f);
        check(get(m,poses+axis*4u)==expected,"original world root preserves both camera terms and actor anchor");
    }
    check(rr64_world_root_source(m,node,graph,type==3u?1u:2u)==2u,"all world models use long-range bank 2");
    auto matrix_memory=memory; recomp_context ctx{}; bind_context(ctx,fr1);
    ctx.r4=guest_address(poses+12u); ctx.r5=guest_address(poses); ctx.r6=guest_address(matrix);
    func_80015A90(matrix_memory.data(),&ctx);
    std::array<unsigned,16> old_matrix{};
    for(unsigned i=0;i<16;++i) {old_matrix[i]=word(m,matrix+i*4u);write_u32(m,matrix+i*4u,word(matrix_memory.data(),matrix+i*4u));}
    rr64_world_scale_root_matrix(m,node,graph,matrix);
    for(unsigned i=0;i<16;++i) {
        const float expected=get(matrix_memory.data(),matrix+i*4u)*(type==3u&&i%4u<3u?0.1f:1.0f);
        check(get(m,matrix+i*4u)==expected,"source conversion scales root basis and translation together, preserving homogeneous column");
        write_u32(m,matrix+i*4u,old_matrix[i]);
    }
    rr64_world_end_actor(); check(memory==before,"renderer completion restores all live memory, channels and entity state");
    rr64_world_end_draw(m); ++cases;
}
enum class Bad { Capacity, PeerBuffer, Scene, Views, Inactive, Unlinked, Cycle, ReplacedEntity, Root, Camera, Phase, SourceScale, Animation, Generation, Bank, SlotBank, NoDraw, Range, Count };
void negative(Bad bad, unsigned type) {
    auto memory=seed(type,900.0f,1u); auto* m=memory.data(); register_actor(m,bad==Bad::Capacity?64u:0x400u); roots(m,type,false);
    rr64_world_begin_draw(m);
    switch(bad) {
    case Bad::PeerBuffer: write_u32(m,node+0x5cu,0x80410000u); break;
    case Bad::Scene: write_u32(m,globals::main_mode,0u); break;
    case Bad::Views: write_u32(m,0x8009DB88u,2u); break;
    case Bad::Inactive: if(type!=5u)write_u16(m,entity+(type==3u?8u:0x334u),0u);else write_u32(m,node+4u,0u);break;
    case Bad::Unlinked: write_u32(m,0x800A1458u+(type-3u)*4u,0u);break;
    case Bad::Cycle: write_u32(m,node+0x3cu,node);break;
    case Bad::ReplacedEntity: write_u32(m,node+4u,entity+0x1000u);break;
    case Bad::Root: write_float(m,poses,11.0f);break;
    case Bad::Camera: write_float(m,0x800D69F8u,19.0f);break;
    case Bad::Phase: if(type==3u)write_float(m,entity+0x310u,0.6f);else write_float(m,entity+pos(type),99.0f);break;
    case Bad::SourceScale: write_float(m,0x8009DBB4u,1.0f);break;
    case Bad::Animation: if(type==3u)write_u16(m,graph+2u,0u);else write_u16(m,sources+0x12u,1u);break;
    case Bad::Generation: rr64_world_invalidate(m);break;
    case Bad::Bank: write_u16(m,0x800B6C00u,0u);break;
    case Bad::SlotBank: write_u16(m,0x800B73F2u,0u);break;
    case Bad::NoDraw: rr64_world_end_draw(m);break;
    case Bad::Range: write_float(m,entity+pos(type)+4u,100000.0f);roots(m,type,false);break;
    default:break;
    }
    const auto before=memory; recomp_context ctx{};bind_context(ctx,false);
    check(rr64_world_actor_hidden(m,node,1u,&ctx)==1u,"invalid lifecycle/resource/range/animation keeps original visibility");
    check(memory==before,"refused world actor never mutates guest state");
    rr64_world_end_draw(m);++cases;
}
void camera_case(unsigned slot,bool fr1,bool enabled,bool unsupported=false,unsigned camera_index=0) {
    auto memory=seed(4u,100,slot);auto* m=memory.data();
    write_u32(m,0x800A6578u,4u);write_u32(m,0x8009DB88u,4u);write_u32(m,globals::active_viewport,camera_index);
    if(unsupported)write_u32(m,globals::main_mode,0u);
    banks(m,fr1,900.0f);
    const bool extended=enabled&&!unsupported;
    check(get(m,0x8009DBCCu)==900.0f,"extended camera preserves stock far-distance global");
    constexpr float scales[]{4,100,10};
    std::array<float,3> depth{};
    for(unsigned source=0;source<3;++source) {
        const float scale=scales[source];auto oracle=memory;auto* o=oracle.data();
        recomp_context ctx{};bind_context(ctx,fr1);ctx.r4=guest_address(matrix);ctx.r5=guest_address(matrix+64u);
        ctx.r6=std::bit_cast<unsigned>(55.0f);ctx.r7=std::bit_cast<unsigned>(4.0f/3.0f);
        write_float(o,stack+0x10u,scale);
        write_float(o,stack+0x14u,extended?30000.0f*scale:std::fmin(900.0f*scale,32767.0f));
        write_float(o,stack+0x18u,1.0f);guPerspective(o,&ctx);
        const unsigned offset=camera_index*0x180u+source*128u+slot*64u, projection=0x800B6568u+offset, view=0x800B6DE8u+offset;
        for(unsigned i=0;i<16;++i)check(word(m,projection+i*4u)==word(o,matrix+i*4u),"actual167BC packed projection equals exact intended original guPerspective inputs");
        std::uint16_t actual_norm=0,expected_norm=0;read_u16(m,0x800B73E8u+camera_index*12u+source*4u+slot*2u,actual_norm);read_u16(o,matrix+64u,expected_norm);
        if(extended&&expected_norm==0)expected_norm=1;
        check(actual_norm==expected_norm&&actual_norm!=0,"large far-plane perspective normalization stays usable");
        check(bool(rr64_world_camera_ready(m,source,slot))==extended,"camera readiness requires an accepted current producer");
        check(bool(rr64_world_camera_native_ready(m,source,slot))==!unsupported,"native camera readiness accepts the actual producer independently of extended-world admission");
        Matrix4x4Snapshot p{},v{};check(decode_n64_matrix(m,projection,p)&&decode_n64_matrix(m,view,v),"camera banks decode");
        const std::array<float,4> point{4*scale,80*scale,2*scale,1};std::array<float,4> eye{},clip{};
        for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)eye[col]+=point[row]*v.values[row*4+col];
        for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)clip[col]+=eye[row]*p.values[row*4+col];
        check(std::isfinite(clip[2]/clip[3]),"camera produces finite projected depth");depth[source]=clip[2]/clip[3];
    }
    if(extended) {
        check(std::abs(depth[0]-depth[1])<0.00002f&&std::abs(depth[0]-depth[2])<0.00002f,"all three extended banks agree on world-point depth");
    }
    write_u32(m,0x800A1830u,18u+slot);
    check(!rr64_world_camera_ready(m,2u,slot),"a later visual epoch cannot reuse old long-range camera readiness");
    check(!rr64_world_camera_native_ready(m,2u,slot),"a later visual epoch cannot reuse old native camera readiness");
    ++cases;
}
void camera_freshness_case(bool enabled) {
    auto memory=seed(4u,100,0);auto* m=memory.data();
    check(rr64_world_camera_native_ready(m,2u,0u),"original producer establishes source-2 camera readiness");
    auto other=memory;
    check(!rr64_world_camera_native_ready(other.data(),2u,0u),"copied guest bytes do not transfer camera producer identity");
    check(!rr64_world_camera_native_ready(m,3u,0u)&&!rr64_world_camera_native_ready(m,2u,2u),"invalid camera bank or slot refuses readiness");
    for(unsigned bad=0;bad<6;++bad) {
        write_u32(m,0x800A1830u,100u+bad);
        recomp_context ctx{};bind_context(ctx,false);ctx.r18=2u;ctx.r22=0u;
        ctx.f4.fl=bad==0?1.0f:10.0f;ctx.f8.fl=bad==1?0.0f:10.0f;ctx.f6.fl=9000.0f;
        rr64_world_camera_far(m,&ctx);
        check(!rr64_world_camera_native_ready(m,2u,0u),"far arguments alone never publish a finished camera");
        write_float(m,stack+0x10u,bad==2?11.0f:10.0f);
        write_float(m,stack+0x14u,bad==3?ctx.f6.fl+1.0f:ctx.f6.fl);
        write_u16(m,0x800b73f0u,1u);
        if(bad==4)write_u32(m,0x800A1830u,200u+bad);
        if(bad!=5)rr64_world_camera_normalization(m,&ctx);
        check(!rr64_world_camera_native_ready(m,2u,0u)&&!rr64_world_camera_ready(m,2u,0u),"invalid, mismatched, stale or unfinished producer cannot publish either readiness");
    }
    if(!enabled) {
        write_u32(m,0x800A1830u,300u);
        recomp_context ctx{};bind_context(ctx,false);ctx.r18=2u;ctx.r22=0u;
        ctx.f4.fl=10;ctx.f8.fl=10;ctx.f6.fl=9000;
        write_float(m,stack+0x10u,10);write_float(m,stack+0x14u,9000);write_u16(m,0x800b73f0u,0u);
        const auto before=memory;
        rr64_world_camera_far(m,&ctx);rr64_world_camera_normalization(m,&ctx);
        check(!rr64_world_camera_native_ready(m,2u,0u),"native zero normalization is refused without changing camera policy");
        check(memory==before&&ctx.f6.fl==9000,"disabled world camera observation never changes guest memory or far distance");
    }
    ++cases;
}
}
int main(int argc,char** argv) {
    const bool disabled=argc>1&&std::strcmp(argv[1],"--disabled")==0;
#ifdef _WIN32
    _putenv_s("RR64_WORLD_DISTANCE",disabled?"0":"1");
#else
    setenv("RR64_WORLD_DISTANCE",disabled?"0":"1",1);
#endif
    camera_freshness_case(!disabled);
    if(disabled) {
        auto memory=seed(3u,900,0);auto* m=memory.data();register_actor(m);roots(m,3u,false);const auto before=memory;
        recomp_context ctx{};bind_context(ctx,false);rr64_world_begin_draw(m);
        check(rr64_world_actor_hidden(m,node,1u,&ctx)==1u&&rr64_world_select(m,node,1u)==1u,"world option is independent and disabled by default");
        rr64_world_end_draw(m);check(memory==before,"disabled world hooks preserve memory");
        for(unsigned slot:{0u,1u})for(bool fr1:{false,true})camera_case(slot,fr1,false);
    } else {
        for(unsigned type:{3u,4u,5u})for(unsigned slot:{0u,1u})for(bool fr1:{false,true})for(float d:{10.0f,40.0f,180.0f,900.0f,2200.0f})
            positive(type,d,slot,fr1,slot,3u,slot);
        for(unsigned mode:{1u,3u})for(unsigned motion:{0u,1u})for(unsigned previous:{0u,1u})positive(3u,450,previous,false,previous,mode,motion);
        for(unsigned type:{3u,4u,5u})for(unsigned i=0;i<unsigned(Bad::Count);++i)negative(Bad(i),type);
        for(unsigned slot:{0u,1u})for(bool fr1:{false,true})for(bool unsupported:{false,true})for(unsigned camera_index=0;camera_index<4;++camera_index)camera_case(slot,fr1,true,unsupported,camera_index);
    }
    if(passed)std::printf("[RR64-WORLD] PASS: %u original root/animation, unit-bank, allocation, lifecycle and rollback cases%s.\n",cases,disabled?" (disabled)":"");
    return passed?EXIT_SUCCESS:EXIT_FAILURE;
}
