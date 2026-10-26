#include "rr64_traffic_roster.hpp"
#include "rr64_highlight_traffic.hpp"
#include "rr64_world_objects.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_actor_render_snapshot.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"

#include <bit>
#include <cstdio>
#include <memory>
#include <mutex>

namespace rr64::highlights {
namespace {
using namespace engine;
constexpr unsigned command_bytes = 2048, frame_bytes = 4096;
constexpr unsigned identity_base = 0x52540000u, group_flags = 0x02011555u;
static_assert(rr64::traffic::capacity * 56u + 200u <= command_bytes);
static_assert(command_bytes + rr64::traffic::capacity * 64u <= frame_bytes);
struct Frame {
    unsigned char *allocation = nullptr;
    unsigned base = 0, commands = 0, matrices = 0, epoch = 0;
    bool issued = false;
};
struct Cache {
    unsigned char *memory = nullptr;
    const unsigned char *rom = nullptr;
    TrafficAssets data;
    std::array<Frame,2> frames{};
    // Full native identities never become truncated renderer identities.
    std::array<unsigned,4096> identities{};
    unsigned identity_count=0;
    TrafficStatistics stats{};
    bool attempted = false, ready = false;
    std::mutex mutex;
};
Cache &cache() { static auto c = std::make_unique<Cache>(); return *c; }
void word(unsigned char *rdram, unsigned p, unsigned v) { MEM_W(0,guest_address(p))=v; }
void command(unsigned char *m, unsigned &p, unsigned a, unsigned b) {
    word(m,p,a); word(m,p+4,b); p+=8;
}
// Same graphics-slot ownership used by the stock world-object renderer: each
// queued graphics task retains its own assets/list/matrices until slot reuse.
struct Context {
    unsigned slot=0, gfx=0, epoch=0, pointer=0, view=0;
    std::uint16_t norm=0;
    world::ObjectMatrix projection{}, camera_matrix{};
    std::array<float,3> camera{}, sector{};
};
bool context(unsigned char *m, Context &c) {
    unsigned base=0, active_base=0, count=0, views=0, scale=0;
    if (!m || !read_u32(m,globals::active_viewport,c.view) || c.view != 0 ||
        !read_u32(m,0x8009db88,views) || views != 1 ||
        !read_u32(m,globals::actor_render_buffer_slot,c.slot) || c.slot>1 ||
        !rr64_world_camera_native_ready(m,2,c.slot) ||
        !read_u32(m,0x8009cba4,c.gfx) || c.gfx>1 ||
        !read_u32(m,0x800a1830,c.epoch) || !read_u32(m,0x800ac650,c.pointer) ||
        !read_u32(m,0x800ac658+c.gfx*4,base) ||
        !read_u32(m,0x8009cb90,active_base) || base!=active_base ||
        !read_u32(m,0x800bc9a0,count) || (count!=0x4650 && count!=0x36b0) ||
        !read_u32(m,0x8009dbb4,scale) || scale!=std::bit_cast<unsigned>(10.f) ||
        !read_u16(m,0x800b73f0+c.slot*2,c.norm) || !c.norm) return false;
    const unsigned bytes=0x140+count*8;
    if (!valid_guest_range(base,bytes) || c.pointer<base+0x148 ||
        c.pointer>base+bytes-1064 || (c.pointer&7)) return false;
    for (unsigned i=0;i<3;++i)
        if (!read_float(m,0x800d69f8+i*4,c.camera[i]) || !std::isfinite(c.camera[i]) ||
            !read_float(m,0x800a4fdc+i*4,c.sector[i]) || !std::isfinite(c.sector[i])) return false;
    Matrix4x4Snapshot p{},v{};
    if (!decode_n64_matrix(m,0x800b6668+c.slot*64,p) ||
        !decode_n64_matrix(m,0x800b6ee8+c.slot*64,v) ||
        p.values[0]==0 || p.values[5]==0 || p.values[11]==0 || v.values[15]!=1) return false;
    c.projection=p.values; c.camera_matrix=v.values;
    return true;
}
}
bool prepare_traffic(unsigned char *m) noexcept {
    auto &c=cache(); std::lock_guard lock(c.mutex);
    try {
        const auto rom=recomp::get_rom();
        if (c.memory!=m || c.rom!=rom.data()) {
            // A different mapping/ROM belongs to a reset guest heap. Never
            // free or reuse addresses from the retired mapping's submissions.
            c.memory=m; c.rom=rom.data(); c.data={}; c.frames={}; c.stats={};
            c.attempted=c.ready=false;
            c.identities={}; c.identity_count=0;
        }
        // Preparation is a race-entry boundary. Identities are CPU-only lookup
        // state: clearing them never changes either submitted graphics buffer.
        // The caller establishes the new race's camera/interpolation boundary.
        c.identity_count=0;
        if (c.attempted) return c.ready;
        c.attempted=true;
        if (!m) return false;
        std::string error;
        if (!build_traffic_assets(rom,c.data,error)) {
            std::fprintf(stderr,"[RR64-HIGHLIGHTS] traffic assets refused: %s\n",error.c_str());
            return false;
        }
        const unsigned bytes=(unsigned(c.data.assets.bytes.size())+63u)&~63u;
        bool allocated=true;
        for (auto &f:c.frames) {
            f.allocation=static_cast<unsigned char*>(recomp::alloc(m,bytes+frame_bytes));
            if (!f.allocation) { allocated=false; break; }
            const auto offset=f.allocation-m;
            if (offset<0x800000 || std::uint64_t(offset)+bytes+frame_bytes>recomp::mem_size) {
                allocated=false; break;
            }
            f.base=0x80000000u+unsigned(offset);
            f.commands=f.base+bytes; f.matrices=f.commands+command_bytes;
            for (unsigned i=0;i<c.data.assets.bytes.size();++i)
                m[(unsigned(offset)+i)^3u]=c.data.assets.bytes[i];
            for (const auto &r:c.data.assets.relocations)
                word(m,f.base+r.word_offset,f.base+r.target_offset);
        }
        if (!allocated) {
            // Nothing has been submitted; partial preparation can free now.
            for (auto &f:c.frames) if (f.allocation) recomp::free(m,f.allocation);
            c.frames={}; return false;
        }
        c.stats.models=unsigned(c.data.assets.models.size());
        c.stats.cached_bytes=2*(bytes+frame_bytes);
        c.ready=true;
        return true;
    } catch (...) { return false; }
}
bool draw_traffic(unsigned char *m,
                  const std::array<world_sync::Traffic,rr64::traffic::capacity> &cars,
                  std::uint64_t recording_time_us) noexcept {
    auto &c=cache(); std::lock_guard lock(c.mutex);
    // No compilation or allocation is allowed in this callback.
    const auto reject=[&] { ++c.stats.refusals; return false; };
    if (!c.ready || c.memory!=m) return reject();
    Context ctx;
    if (!context(m,ctx)) return reject();
    auto &f=c.frames[ctx.gfx];
    if (f.issued && f.epoch==ctx.epoch) return reject();
    std::array<world::ObjectMatrix,rr64::traffic::capacity> matrices{};
    std::array<unsigned,rr64::traffic::capacity> models{},tags{},new_ids{};
    unsigned count=0,new_count=0;
    // Validate the complete recording before touching a submitted list or
    // suppressing native traffic. Unavailable models preserve the old path.
    for (unsigned i=0;i<cars.size();++i) {
        const auto &car=cars[i];
        if (!world_sync::valid(car)) return reject();
        if (!car.active) continue;
        if (!car.motion_valid || car.model>=c.data.models.size()) return reject();
        for (unsigned j=0;j<i;++j) if (cars[j].active && cars[j].id==car.id) return reject();
        const unsigned model=c.data.models[car.model];
        if (model>=c.data.assets.models.size()) return reject();
        world::ObjectPlacementAsset placement;
        placement.position=car.position;
        // Native 5EF74 copies entity +180, not Euler +F8, to root quaternion.
        for (unsigned q=0;q<4;++q) placement.quaternion[q]=car.motion[(0x180-0xa8)/4+q];
        world::ObjectMatrix root;
        if (!world::object_matrix(placement,ctx.camera,ctx.sector,{},root)) return reject();
        if (!world::object_in_frustum(c.data.assets.models[model],root,
                                      ctx.camera_matrix,ctx.projection)) continue;
        unsigned tag=0;
        for (;tag<c.identity_count;++tag) if (c.identities[tag]==car.id) break;
        if (tag==c.identity_count) {
            // Duplicate full IDs were rejected above, including the new set.
            if (c.identity_count+new_count==c.identities.size()) return reject();
            tag=c.identity_count+new_count; new_ids[new_count++]=car.id;
        }
        matrices[count]=root; models[count]=model; tags[count]=tag; ++count;
    }
    c.stats.cars=count;
    if (!count) { ++c.stats.frames; return true; }
    // Native light materials contain all authored pixels/alpha/stride. Follow
    // the recording clock (including replay slow motion and backwards seeks),
    // never the live clock. Only starting phase is approximate: the recorder
    // does not retain the original visibility-dependent material clock.
    for (const auto &r:c.data.assets.relocations) {
        if (r.texture_index>=c.data.assets.textures.size()) continue;
        const auto &t=c.data.assets.textures[r.texture_index];
        const unsigned phase=unsigned(std::fmod(double(recording_time_us)/1000.0 /
                                               t.frame_duration_ms,double(t.frame_count)));
        const std::uint64_t offset=std::uint64_t(r.target_offset)+std::uint64_t(phase)*t.frame_stride;
        if (offset>=std::uint64_t(t.raw_offset)+t.raw_size) return reject();
    }
    for (const auto &r:c.data.assets.relocations) {
        if (r.texture_index>=c.data.assets.textures.size()) continue;
        const auto &t=c.data.assets.textures[r.texture_index];
        const unsigned phase=unsigned(std::fmod(double(recording_time_us)/1000.0 /
                                               t.frame_duration_ms,double(t.frame_count)));
        word(m,f.base+r.word_offset,f.base+r.target_offset+phase*t.frame_stride);
    }
    unsigned dl=f.commands;
    for (unsigned op:{0x19u,0x1bu,0x29u,0x1du,0x1fu,0x21u,0x23u,0x25u,0x27u})
        command(m,dl,0x64000000u|op,0);
    command(m,dl,0xdb0e0000u,ctx.norm);
    command(m,dl,0xda380007u,0x800b6668+ctx.slot*64);
    command(m,dl,0xda380005u,0x800b6ee8+ctx.slot*64);
    for (unsigned i=0;i<count;++i) {
        const unsigned address=f.matrices+i*64;
        for (unsigned k=0;k<16;++k) word(m,address+k*4,std::bit_cast<unsigned>(matrices[i][k]));
        command(m,dl,0x6400000cu,identity_base+tags[i]);
        command(m,dl,group_flags,0);
        command(m,dl,0x64000030u,2);
        command(m,dl,0,address);
        command(m,dl,0xde000000u,f.base+c.data.assets.models[models[i]].display_list_offset);
        command(m,dl,0xd8380002u,64);
        command(m,dl,0x6400000du,1);
    }
    command(m,dl,0xe7000000u,0);
    for (unsigned op:{0x28u,0x26u,0x24u,0x22u,0x20u,0x1eu,0x2au,0x1cu,0x1au})
        command(m,dl,0x64000000u|op,0);
    command(m,dl,0x6400002cu,0);
    command(m,dl,0xe0525464u,0x20000000u);
    command(m,dl,0xdf000000u,0);
    unsigned bridge=ctx.pointer;
    command(m,bridge,0xe7000000u,0);
    command(m,bridge,0xe0525464u,0x10000064u);
    command(m,bridge,0x6400002cu,1);
    command(m,bridge,0xde000000u,f.commands);
    command(m,bridge,0xe7000000u,0);
    word(m,0x800ac650,bridge);
    write_u32(m,0x8009db2c,0); write_u32(m,0x800b1a20,~0u); write_u32(m,0x8009dbec,~0u);
    f.issued=true; f.epoch=ctx.epoch; ++c.stats.frames;
    for (unsigned i=0;i<new_count;++i) c.identities[c.identity_count++]=new_ids[i];
    return true;
}
void reset_traffic_session() noexcept {
    auto &c=cache(); std::lock_guard lock(c.mutex);
    c.memory=nullptr; c.rom=nullptr; c.data={}; c.frames={}; c.stats={};
    c.attempted=c.ready=false;
    c.identities={}; c.identity_count=0;
}
TrafficStatistics traffic_statistics() noexcept {
    auto &c=cache(); std::lock_guard lock(c.mutex); return c.stats;
}
}
