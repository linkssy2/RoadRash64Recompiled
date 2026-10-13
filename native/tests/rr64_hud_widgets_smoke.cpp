#include "recomp.h"
#include "rr64_native.hpp"
#include <vector>
#include <cstring>
#include <cstdio>

int main() {
    std::vector<unsigned char> memory(8*1024*1024);
    auto* rdram=memory.data(); unsigned failures=0,checks=0;
    auto check=[&](bool ok){++checks;if(!ok) {++failures;std::printf("FAIL %u\n",checks);}};
    auto word=[&](uint32_t p)->int32_t& {return MEM_W(0,static_cast<int32_t>(p));};
    auto half=[&](uint32_t p)->int16_t& {return MEM_H(0,static_cast<int32_t>(p));};
    auto put=[&](uint32_t p,float v){std::memcpy(&word(p),&v,4);};
    auto get=[&](uint32_t p){float v;std::memcpy(&v,&word(p),4);return v;};
    word(0x800B0808)=320;word(0x800B080C)=240;
    word(0x800BC9A0)=0x4650;
    word(0x800AC658)=0x801FFE00;word(0x800AC65C)=0x801FFE00;
    for(unsigned players=1;players<=4;++players) {
        word(0x800A6578)=players;word(0x800A4F24)=players==1?0:players==2?1:2;
        word(0x800A76A0)=1;
        rr64_hud_widgets_begin(rdram);
        // Deliberately long label: its backing and all glyphs must retain the
        // origin set before the native font consumer, regardless of text width.
        for(unsigned i=1;i<=players;++i) {
            const float x=players>2?((i-1)%2)*160.f+20.f:72.f;
            const float y=players>2?((i-1)/2)*120.f+104.f:(i-1)*120.f+104.f;
            put(0x800D9650+i*88+0x40,x);put(0x800D9650+i*88+0x44,y);
        }
        word(0x800A76A0)=players+1;rr64_hud_widgets_end(rdram);
        for(unsigned i=1;i<=players;++i) {
            word(0x800AC650)=0x80200000;
            rr64_hud_label_begin(rdram,0x800D9650+i*88);
            check(word(0x800AC650)==(players==1?0x80200000:0x80200030));
            if(players>1) {
                unsigned origin=players>2 && (i-1)%2?512:0;
                check(uint32_t(word(0x80200000))==0xE0525464);
                check(uint32_t(word(0x8020000C))==(origin|(origin<<12)));
                check(uint32_t(word(0x80200018))==0x64000017); // Push scissor.
                // Decode effective bounds using RDP::movedFromOrigin semantics.
                // Native coordinates must survive anchoring before widescreen math.
                const int x_offset=int16_t(uint32_t(word(0x80200010))>>16);
                check(x_offset+int(origin)*320*4/1024==0);
                const uint32_t origins=word(0x80200024);
                const int clip_left=int16_t(uint32_t(word(0x80200028))>>16)+int((origins>>2)&4095)*320*4/1024;
                const int clip_right=int16_t(uint32_t(word(0x8020002C))>>16)+int((origins>>14)&4095)*320*4/1024;
                const int col=players>2?int((i-1)%2):0;
                check(clip_left==col*160*4);
                check(clip_right==(players>2?(col+1)*160:320)*4);
                // Stand in for an arbitrary number of native glyph/backing draws.
                word(0x800AC650)+=80;
            }
            rr64_hud_label_end(rdram);
            if(players>1) {
                check(uint32_t(word(0x80200084))==0x800800);
                check(uint32_t(word(0x80200090))==0x64000018); // Pop scissor.
                check(word(0x800AC650)==0x80200098);
            }
        }
        for(unsigned b=0;b<2;++b) {
            word(0x8009CBA4)=b;const uint32_t count=0x800BC9D0+b*2;
            const uint32_t record=0x800BCAD8+(b*96+1)*76;
            half(count)=1;rr64_hud_countdown_begin(rdram);
            half(count)=2;half(record+0x10)=99;word(record)=0x12345678;
            put(record+0x14,160);put(record+0x18,120);put(record+0x20,1);put(record+0x24,1);
            rr64_hud_countdown_end(rdram);
            check(half(count)==1+players);
            for(unsigned i=0;i<players;++i) {
                const uint32_t r=record+i*76;
                check(word(r)==0x12345678 && half(r+0x10)==99);
                check(get(r+0x14)==(players>2?(i%2)*160.f+80.f:160.f));
                check(get(r+0x18)==(players==1?120.f:(players>2?i/2:i)*120.f+60.f));
                check(get(r+0x20)==(players==1?1.f:.5f));
            }
            const auto previous=half(count);rr64_hud_countdown_end(rdram);check(half(count)==previous);
            half(count)=95;rr64_hud_countdown_begin(rdram);half(count)=96;
            rr64_hud_countdown_end(rdram);check(half(count)==96);
        }
        rr64_hud_widgets_clear(rdram);word(0x800AC650)=0x80200000;
        rr64_hud_label_begin(rdram,0x800D9650+88);check(word(0x800AC650)==0x80200000);
    }
    // A scope created while online's presentation override is active stays
    // fullscreen even after the shared simulation player count is restored.
    word(0x800A4F24)=0;word(0x800A6578)=1;word(0x800A76A0)=0;
    rr64_hud_widgets_begin(rdram);rr64_hud_widgets_end(rdram);
    word(0x800A4F24)=2;word(0x800A6578)=4;
    word(0x8009CBA4)=0;half(0x800BC9D0)=0;rr64_hud_countdown_begin(rdram);
    half(0x800BC9D0)=1;rr64_hud_countdown_end(rdram);check(half(0x800BC9D0)==1);
    // Right-anchored speed label remains at x=248, rather than x=568.
    word(0x800A6578)=2;word(0x800A4F24)=1;word(0x800A76A0)=0;
    rr64_hud_widgets_begin(rdram);
    put(0x800D9690,248);put(0x800D9694,90);
    word(0x800A76A0)=1;rr64_hud_widgets_end(rdram);word(0x800AC650)=0x80200000;
    rr64_hud_label_begin(rdram,0x800D9650);
    check((word(0x8020000C)&4095)==1024);
    check(248*4+int16_t(uint32_t(word(0x80200010))>>16)+320*4==248*4);
    rr64_hud_label_end(rdram);rr64_hud_widgets_clear(rdram);
    // Quadrant speed readouts sit at different native x per column (view-local
    // 118.5 left, 98.5 right). Each takes one view right-edge anchor: the
    // centre line for the left column, the screen edge for the right.
    word(0x800A6578)=4;word(0x800A4F24)=2;word(0x800A76A0)=0;
    rr64_hud_widgets_begin(rdram);
    put(0x800D9690,118.5f);put(0x800D9694,97.8f);
    put(0x800D9690+88,258.5f);put(0x800D9694+88,97.8f);
    word(0x800A76A0)=2;rr64_hud_widgets_end(rdram);
    for(unsigned i=0;i<2;++i) {
        word(0x800AC650)=0x80200000;
        rr64_hud_label_begin(rdram,0x800D9650+i*88);
        check((word(0x8020000C)&4095)==(i?1024:512));
        rr64_hud_label_end(rdram);
    }
    rr64_hud_widgets_clear(rdram);
    // Pause Options uses the whole screen, even in the lower player's HUD area.
    half(0x800A2192)=1;word(0x800A76A0)=0;rr64_hud_widgets_begin(rdram);
    rr64_hud_widgets_end(rdram);word(0x800A76A0)=1;word(0x800AC650)=0x80200000;
    rr64_hud_label_begin(rdram,0x800D9650);
    check((word(0x8020000C)&4095)==512);
    check(int16_t(uint32_t(word(0x80200010))>>16)==-160*4);
    check((word(0x8020002C)&65535)==240*4);
    rr64_hud_label_end(rdram);rr64_hud_widgets_clear(rdram);half(0x800A2192)=0;
    std::printf("HUD widget scopes / native countdown queues: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
