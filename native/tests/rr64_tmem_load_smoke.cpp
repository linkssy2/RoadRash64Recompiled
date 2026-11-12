#include "hle/rt64_state.h"
#include "xxHash/xxh3.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "fixtures/rr64_tmem_load_r20.inc"
using namespace RT64;
extern "C" void rr64_record_authored_sample(unsigned long long,unsigned long long,unsigned long long,unsigned int,unsigned int,unsigned long long,unsigned long long) { std::abort(); }
extern "C" void rr64_record_source_cadence(unsigned int,unsigned int,unsigned int,unsigned int,unsigned int,unsigned int) { std::abort(); }
static void noInterrupts() { std::abort(); }
static uint32_t rng=0x7374ACE2U;
static uint32_t random32() { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
struct Case { unsigned mode; LoadTile tile; LoadTexture texture; };
struct Params { uint32_t start,stride,tmem,tstride,words,rows,dxt; };
static Params parameters(const Case &c) {
    const auto &t=c.tile; const auto &s=c.texture;
    const bool block=c.mode==2 || c.mode==3, tlut=c.mode>=4;
    const uint32_t offset=(block?t.uls:(t.uls>>2))<<s.siz>>1;
    const uint32_t stride=s.width<<s.siz>>1;
    return {s.address+offset+stride*(block?t.ult:(t.ult>>2)),stride,uint32_t(t.tmem)<<3,
        uint32_t(t.line)<<(tlut?5:3),
        block?uint32_t(((t.lrs-t.uls)>>(4-t.siz))+1):tlut?uint32_t((t.lrs>>2)-(t.uls>>2)+1):uint32_t((((t.lrs>>2)-(t.uls>>2))>>(4-t.siz))+1),
        block?1U:uint32_t(1+(t.lrt>>2)-(t.ult>>2)),block?uint32_t(t.lrt):0U};
}
static void actual(RDP &r,const Case &c) {
    if(c.mode<2) r.loadTileOperation(c.tile,c.texture,false);
    else if(c.mode<4) r.loadBlockOperation(c.tile,c.texture,false);
    else r.loadTLUTOperation(c.tile,c.texture,false);
}
template<bool RGBA,bool BLOCK,bool TLUT> static void refLoad(uint8_t *out,const uint8_t *ram,const Params &p) {
    Original::loadToTMEMCommon<RGBA,BLOCK,TLUT>(out,ram,p.start,p.stride,p.tmem,p.tstride,p.words,p.rows,p.dxt);
}
static void reference(uint8_t *out,const uint8_t *ram,const Case &c) {
    const auto p=parameters(c);
    switch(c.mode) {
    case 0: refLoad<false,false,false>(out,ram,p); break;
    case 1: refLoad<true,false,false>(out,ram,p); break;
    case 2: refLoad<false,true,false>(out,ram,p); break;
    case 3: refLoad<true,true,false>(out,ram,p); break;
    case 4: refLoad<false,false,true>(out,ram,p); break;
    case 5: refLoad<true,false,true>(out,ram,p); break;
    }
}
static bool safe(const Case &c) {
    const auto p=parameters(c); const bool rgba=c.mode&1;
    const uint32_t mask=rgba?2047:4095,advance=rgba?4:8;
    // Avoid an existing row-skip destination overflow in malformed inputs; the
    // candidate deliberately leaves that independent production behavior alone.
    return !(p.tstride>0 && p.words*advance<=p.tstride && p.rows>(mask+p.tstride)/p.tstride && (p.tmem&mask)!=0);
}
static double timeLoad(RDP &r,const Case &c,uint64_t &digest) {
    std::vector<double> times;
    for(unsigned n=0;n<70;++n) {
        auto start=std::chrono::steady_clock::now();
        for(unsigned k=0;k<6000;++k) actual(r,c);
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        digest+=r.TMEM[n%512]; if(n>=10) times.push_back(ms);
    }
    std::sort(times.begin(),times.end()); return times[times.size()/2];
}
int main(int argc,char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    std::vector<uint8_t> ram(8*1024*1024); for(auto &v:ram) v=uint8_t(random32());
    uint32_t interrupts=0; auto state=std::make_unique<State>(ram.data(),&interrupts,noInterrupts);
    auto &r=*state->rdp; for(auto &v:r.TMEM) v=uint64_t(random32())|(uint64_t(random32())<<32);
    std::array<uint8_t,RDP_TMEM_BYTES> ref;
    XXH3_state_t hash; XXH3_64bits_reset(&hash);
    unsigned checks=0,skipped=0;
    for(unsigned mode=0;mode<6;++mode) for(unsigned n=0;n<20000;++n) {
        Case c{}; c.mode=mode; c.texture.address=4096+random32()%65536;
        c.texture.siz=uint8_t(random32()%4); c.texture.width=uint16_t(1+random32()%256);
        c.tile.fmt=mode&1?G_IM_FMT_RGBA:G_IM_FMT_CI; c.tile.siz=mode&1?G_IM_SIZ_32b:uint8_t(random32()%4);
        c.tile.tmem=uint16_t(random32()%512); c.tile.line=uint16_t(random32()%65);
        c.tile.uls=uint16_t(random32()%32); c.tile.ult=uint16_t(random32()%32);
        c.tile.lrs=uint16_t(c.tile.uls+random32()%(mode>=2&&mode<4?2048:512));
        c.tile.lrt=uint16_t(mode>=2&&mode<4?random32()%4096:c.tile.ult+random32()%128);
        if(!safe(c)) {++skipped;continue;}
        if(n%7==0) ram[c.texture.address+13]^=uint8_t(n);
        std::memcpy(ref.data(),r.TMEM,ref.size());
        reference(ref.data(),ram.data(),c); actual(r,c); ++checks;
        if(std::memcmp(ref.data(),r.TMEM,ref.size())!=0) {
            std::printf("MISMATCH mode=%u iteration=%u\n",mode,n);return 1;
        }
        XXH3_64bits_update(&hash,r.TMEM,sizeof(r.TMEM));
    }
    std::printf("actual_rdp_checks=%u skipped_preexisting_unsafe_inputs=%u all_bytes_equal=1 digest=%llu\n",checks,skipped,XXH3_64bits_digest(&hash));
    if(argc>1 && std::strcmp(argv[1],"--check-only")==0) return 0;
    uint64_t digest=0;
    for(unsigned mode=0;mode<6;++mode) {
        Case c{};c.mode=mode;c.texture={4096,G_IM_FMT_RGBA,G_IM_SIZ_16b,32};
        c.tile.fmt=(mode&1)?G_IM_FMT_RGBA:G_IM_FMT_CI;c.tile.siz=(mode&1)?G_IM_SIZ_32b:G_IM_SIZ_16b;
        c.tile.tmem=mode==4?256:0;c.tile.line=mode<2?uint16_t((mode&1)?4:8):0;
        c.tile.lrs=mode<2?uint16_t((mode&1)?60:124):mode<4?uint16_t((256-1)<<(4-c.tile.siz)):1020;
        c.tile.lrt=mode<2?124:mode<4?256:0;
        auto p=parameters(c);
        std::printf("mode=%u words=%u rows=%u loads=6000 median_ms=%.6f\n",mode,p.words,p.rows,timeLoad(r,c,digest));
    }
    std::printf("timing_digest=%llu\n",digest);
    return 0;
}
