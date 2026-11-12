#include "rr64_video_mode.hpp"
#include "rr64_world_frustum.hpp"
#include "rr64_actor_render_fixture.hpp"
#include "recomp.h"
#include <cstdio>
#include <cmath>
#include "../lib/rt64/src/common/rt64_rr64_video_aspect.h"
extern "C" void func_8000A310(unsigned char*, recomp_context*);
extern "C" void func_8000A35C(unsigned char*, recomp_context*);
extern "C" void func_8000A3A8(unsigned char*, recomp_context*);
int main() {
    using namespace rr64::engine;
    rr64::lod::test::Fixture f;
    auto* m=f.live.data(); bool passed=true;
    const auto check=[&](bool ok,const char* label){if(!ok){std::printf("FAIL %s\n",label);passed=false;}};
    const auto read=[&](unsigned address){unsigned v=0;read_u32(m,address,v);return v;};
    write_u32(m,0x800bc9c4u,640u);write_u32(m,0x800bc9ccu,240u);
    write_u32(m,0x8009cc68u,0x8000a3a8u);
    for(unsigned selected=0;selected<4;++selected) {
        write_u32(m,0x8009cc54u,selected);
        for(unsigned mode:{9u,10u,11u,18u,19u,20u,23u,24u,25u,28u,29u,30u}) {
            write_u32(m,globals::main_mode,mode);write_u32(m,globals::pending_mode,mode);
            const auto before=f.live;
            check(rr64::video::combined_callback(m,0x8000a310u,true)==0x8000a3a8u,"combined mode admitted in retained one-player race");
            check(rr64::video::combined_callback(m,0x8000a310u,false)==0x8000a310u,"legacy option unchanged");
            check(before==f.live,"callback selection does not alter saved mode or guest memory");
        }
    }
    for(unsigned address:{0x800a6578u,0x8009db88u}) {
        write_u32(m,address,2u);
        check(rr64::video::combined_callback(m,0x8000a310u,true)==0x8000a310u,"inconsistent player/view setup is not admitted");
        write_u32(m,address,1u);
    }
    // A real split-screen setup changes BOTH counts. Changing only one above
    // tests a transition, not multiplayer. All local views must remain eligible
    // for MAX LOD while their stock layout lock protects framebuffer geometry.
    for(unsigned views=1;views<=4;++views) {
        write_u32(m,0x800a6578u,views);write_u32(m,0x8009db88u,views);
        check(rr64::lod::supported_scene(m),"consistent 1-4 player scene supports actor LOD");
        write_u32(m,0x800a4f24u,1u);
        check(rr64::video::combined_callback(m,0x8000a310u,true)==0x8000a310u,"locked native layout retains framebuffer writer");
        write_u32(m,0x800a4f24u,0u);
    }
    write_u32(m,0x800a6578u,1u);write_u32(m,0x8009db88u,1u);
    for(unsigned address:{0x800a4f24u,0x800bc9c4u,0x800bc9ccu,0x8009cc68u}) {
        const unsigned original=read(address);write_u32(m,address,1u);
        check(rr64::video::combined_callback(m,0x8000a310u,true)==0x8000a310u,"capacity, lock and callback checks");
        write_u32(m,address,original);
    }
    write_u32(m,globals::pending_mode,0u);
    check(rr64::video::combined_callback(m,0x8000a310u,true)==0x8000a310u,"scene exit unchanged");
    recomp_context c{};
    func_8000A310(m,&c);
    check(read(0x800b07dcu)==512 && read(0x800b07e0u)==150 && read(0x8009cc40u)==45,"original Wide is 512x150 letterbox");
    func_8000A3A8(m,&c);
    check(read(0x800b07dcu)==512 && read(0x800b07e0u)==240 && read(0x8009cc40u)==0,"combined original High Res writer is full-height 512x240");
    func_8000A35C(m,&c);
    check(read(0x800b07dcu)==640 && read(0x800b07e0u)==120 && read(0x8009cc40u)==60,"legacy Letterbox remains available");
    const float target=RT64::RR64Video::combinedTarget;
    for(unsigned width:{320u,512u}) {
        const float scale=target/RT64::RR64Video::sourceAspect(true,width,240u);
        check(std::abs(scale-4.0f/3.0f)<0.00001f,"one identical widescreen expansion in Normal and High Res");
        check(std::abs((320.0f*scale/240.0f)-target)<0.00001f,"VI television viewport presents at 16:9");
    }
    check(RT64::RR64Video::sourceAspect(false,512u,240u)==512.0f/240.0f,"legacy renderer aspect is unchanged");
    // Exercise the renderer's actual Expand target calculation. 3440x1440 is
    // slightly wider than nominal 21:9, so testing only 16:9 hides a clamp.
    for (const auto size : {std::array<unsigned,2>{3440,1440}, {2560,1080}, {5120,1440}, {1920,1080}}) {
        const float aspect=RT64::RR64Video::expandedAspect(4.f/3.f,size[0],size[1]);
        check(std::abs(aspect*float(size[1])-float(size[0]))<.001f,
            "ultrawide target fills the real output width without a 16:9 cap");
    }
    for (const auto size : {std::array<unsigned,2>{0,1440}, {3440,0}, {0,0}, {800,1200}}) {
        check(RT64::RR64Video::expandedAspect(4.f/3.f,size[0],size[1])==4.f/3.f,
            "minimized and narrow outputs retain the source aspect");
    }
    for (unsigned width : {320u, 512u, 640u}) {
        check(RT64::RR64Video::adjustPairAspect(false, true, float(width), 240, 4.f/3.f,
            width, width, 240), "native menu output uses VI pixel dimensions at every resolution");
        check(!RT64::RR64Video::adjustPairAspect(false, true, float(width)/2, 240, 4.f/3.f,
            width, width, 240), "partial menu pair is not a full output projection");
        check(RT64::RR64Video::adjustPairAspect(true, false, float(width)/2, 120, 4.f/3.f,
            width, width, 240), "live split-screen cameras keep their accepted projection");
    }
    check(!RT64::RR64Video::adjustPairAspect(false,false,512,240,4.f/3.f,512,512,240),
        "legacy presentation comparison is retained");
    check(!RT64::RR64Video::adjustPairAspect(false,true,512,240,4.f/3.f,512,640,240),
        "unrelated framebuffer width keeps generic comparison");
    check(!RT64::RR64Video::adjustPairAspect(false,true,640,0,4.f/3.f,640,640,240),
        "empty pair cannot authorize aspect correction");
    // Changing video size while retaining layout zero must invalidate the
    // native region cache. Stable layouts must not disturb viewport/HUD state.
    for (unsigned layout=0;layout<3;++layout) {
        const unsigned columns=layout==2?2:1, rows=layout?2:1;
        write_u32(m,0x8009db8cu+layout*4,columns);
        write_u32(m,0x8009db98u+layout*4,rows);
        write_u32(m,0x8009db80u,layout);
        write_u32(m,0x800b0808u,320);write_u32(m,0x800b080cu,240);
        write_u32(m,0x800b74a8u,320/columns);write_u32(m,0x800b74acu,240/rows);
        write_u16(m,0x8009dba4u,0);
        const auto stable=f.live;
        check(!rr64::video::refresh_viewport_dimensions(m,layout) && stable==f.live,
            "same-size single/split viewport caches remain byte-identical");
        for (unsigned address:{0x800b0808u,0x800b080cu}) {
            const unsigned original=read(address);
            write_u32(m,address,original*2);
            const auto before=f.live;
            check(rr64::video::refresh_viewport_dimensions(m,layout),"size change invalidates native cached dimensions");
            auto expected=before;
            write_u16(expected.data(),0x8009dba4u,1);
            check(expected==f.live,"refresh changes only the native cache-dirty flag");
            write_u32(m,address,original);write_u16(m,0x8009dba4u,0);
        }
    }
    const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    auto model=identity;model[12]=1.6f;
    const std::array<float,3> low{-.01f,-.01f,-.01f},high{.01f,.01f,.01f};
    rr64::view_width.store(4.0/3.0);
    check(!rr64::world::WorldFrustum(identity,identity).intersects(low,high,model),"outside 16:9 side boundary");
    rr64::view_width.store(7.0/4.0);
    check(rr64::world::WorldFrustum(identity,identity).intersects(low,high,model),"21:9 admits additional side geometry");
    model[12]=1.9f;
    check(!rr64::world::WorldFrustum(identity,identity).intersects(low,high,model),"21:9 still rejects beyond its side boundary");
    rr64::view_width.store(RT64::RR64Video::expandedAspect(4.f/3.f,3440,1440)/(4.f/3.f));
    model[12]=1.77f;
    check(rr64::world::WorldFrustum(identity,identity).intersects(low,high,model),
        "3440x1440 side geometry follows the actual window rather than nominal 21:9");
    model[12]=1.81f;
    check(!rr64::world::WorldFrustum(identity,identity).intersects(low,high,model),
        "3440x1440 retains bounded side culling");
    rr64::view_width.store(4.0/3.0);
    std::puts(passed?"[RR64-VIDEO] PASS":"[RR64-VIDEO] FAIL");return passed?0:1;
}
