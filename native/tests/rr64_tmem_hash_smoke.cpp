#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>
#include <intrin.h>
#include "xxHash/xxh3.h"
#include "common/rt64_load_types.h"
#include "shared/rt64_f3d_defines.h"
#define TMEMHasher OriginalHasher
#define bitScanForward64 originalBitScan
#include "fixtures/rt64_tmem_hasher_r20.h"
#undef TMEMHasher
#undef bitScanForward64
#include "common/rt64_tmem_hasher.h"

volatile uint64_t digestSink = 0;
using HashFn = uint64_t(*)(const uint8_t*, const RT64::LoadTile&, uint16_t, uint16_t, uint32_t, uint32_t);
__declspec(noinline) uint64_t oldHash(const uint8_t* b, const RT64::LoadTile& t, uint16_t w, uint16_t h, uint32_t p, uint32_t v) { return RT64::OriginalHasher::hash(b,t,w,h,p,v); }
__declspec(noinline) uint64_t newHash(const uint8_t* b, const RT64::LoadTile& t, uint16_t w, uint16_t h, uint32_t p, uint32_t v) { return RT64::TMEMHasher::hash(b,t,w,h,p,v); }

double timing(HashFn fn, std::array<uint8_t,4096>& bytes, const RT64::LoadTile& tile) {
    std::vector<double> times;
    uint64_t sum = 0;
    for (int r=0;r<11;r++) {
        auto start=std::chrono::steady_clock::now();
        for (int i=0;i<6000;i++) {
            bytes[2048] = uint8_t(i);
            sum += fn(bytes.data(),tile,64,64,G_TT_RGBA16,5);
        }
        auto end=std::chrono::steady_clock::now();
        if(r>=2) times.push_back(std::chrono::duration<double,std::milli>(end-start).count());
    }
    digestSink=sum;
    std::sort(times.begin(),times.end());
    return times[times.size()/2];
}

int main(int argc, char** argv) {
    std::mt19937 random(0x52617368);
    std::array<uint8_t,4096> bytes{};
    uint64_t checks=0;
    // Versions, sparse/full palettes, wrapping, odd-row tails, RGBA32 and no TLUT.
    for (int n=0;n<100000;n++) {
        const int pattern=n%7;
        for(auto& b:bytes) {
            const auto r=random();
            b=pattern==0?0:pattern==1?uint8_t(r&0x33):pattern==2?uint8_t(r&0x77):uint8_t(r);
        }
        RT64::LoadTile tile{};
        tile.siz=n%4; tile.fmt=(n%5); tile.line=random()%65;
        tile.tmem=random()%512; tile.palette=random()%16;
        uint16_t w=1+random()%129, h=1+random()%129;
        uint32_t tlut=n%3==0?0:n%3==1?G_TT_RGBA16:G_TT_IA16;
        uint32_t version=1+n%5;
        auto old=oldHash(bytes.data(),tile,w,h,tlut,version);
        auto now=newHash(bytes.data(),tile,w,h,tlut,version);
        if(old!=now) { std::printf("FAIL case=%d\n",n); return 1; }
        checks++;
    }
    // Exhaust all byte values in CI4; mutate both used and unused palette entries.
    for(uint32_t v=0;v<256;v++) for(uint32_t p=0;p<16;p++) {
        bytes.fill(uint8_t(v));
        bytes[2048+p*8] ^= 0x55;
        RT64::LoadTile tile{}; tile.siz=G_IM_SIZ_4b; tile.fmt=G_IM_FMT_CI; tile.line=4;
        if(oldHash(bytes.data(),tile,64,64,G_TT_RGBA16,5)!=newHash(bytes.data(),tile,64,64,G_TT_RGBA16,5)) return 2;
        checks++;
    }
    std::printf("equivalence=%llu passed\n",(unsigned long long)checks);
    RT64::LoadTile tile{}; tile.siz=G_IM_SIZ_4b; tile.fmt=G_IM_FMT_CI; tile.line=4;
    for(int pattern=0;pattern<4;pattern++) {
        for(auto& b:bytes) b=pattern==0?0:pattern==1?uint8_t(random()&0x33):pattern==2?uint8_t(random()&0x77):uint8_t(random());
        double old=timing(oldHash,bytes,tile), now=timing(newHash,bytes,tile);
        std::printf("pattern=%d hashes=6000 old_ms=%.6f new_ms=%.6f digest=%llu\n",pattern,old,now,(unsigned long long)digestSink);
    }
    if (argc==3) {
        struct Fixture { std::array<uint8_t,4096> bytes{}; RT64::LoadTile tile{}; uint16_t width{},height{}; uint32_t tlut{}; };
        std::ifstream metaFile(argv[1]), blobFile(argv[2],std::ios::binary);
        json meta; metaFile >> meta;
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(blobFile)),{});
        std::vector<Fixture> fixtures;
        for (const auto& tex: meta.at("textures")) {
            uint32_t width=tex.at("width"),height=tex.at("height"),bits=tex.at("bits");
            uint32_t pixels=tex.at("pixel_bytes"),palette=tex.at("palette_bytes"),frames=tex.at("frames");
            if (pixels>(palette?2048:4096) || width%16 || !height) continue;
            for(uint32_t frame=0;frame<frames;frame++) {
                Fixture f;
                const size_t offset=size_t(tex.at("pixel_offset"))+frame*size_t(tex.at("stride"));
                const size_t paletteOffset=size_t(tex.at("palette_offset"))+frame*size_t(tex.at("stride"));
                if(offset+pixels>blob.size() || paletteOffset+palette>blob.size()) return 3;
                uint32_t rowBytes=width*bits/8;
                for(uint32_t i=0;i<pixels;i++) f.bytes[i^(((i/rowBytes)&1)*4)]=blob[offset+i];
                for(uint32_t i=0;i<palette/2;i++) for(uint32_t j=0;j<8;j++) f.bytes[2048+i*8+j]=blob[paletteOffset+i*2+(j&1)];
                f.tile.siz=bits==4?G_IM_SIZ_4b:G_IM_SIZ_8b;
                f.tile.fmt=palette?G_IM_FMT_CI:G_IM_FMT_I;
                f.tile.line=uint16_t(rowBytes/8); f.width=uint16_t(width); f.height=uint16_t(height); f.tlut=palette?G_TT_RGBA16:0;
                if(oldHash(f.bytes.data(),f.tile,f.width,f.height,f.tlut,5)!=newHash(f.bytes.data(),f.tile,f.width,f.height,f.tlut,5)) return 4;
                fixtures.push_back(f);
            }
        }
        auto realTiming=[&](HashFn fn) {
            std::vector<double> times; uint64_t sum=0;
            for(int r=0;r<11;r++) {
                auto start=std::chrono::steady_clock::now();
                for(int rep=0;rep<4;rep++) for(const auto& f:fixtures) sum+=fn(f.bytes.data(),f.tile,f.width,f.height,f.tlut,5);
                if(r>=2) times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            }
            digestSink=sum; std::sort(times.begin(),times.end()); return times[times.size()/2];
        };
        double old=realTiming(oldHash),now=realTiming(newHash);
        std::printf("real-ROM frames=%zu repeats=4 old_ms=%.6f new_ms=%.6f digest=%llu\n",fixtures.size(),old,now,(unsigned long long)digestSink);
    }
    return 0;
}
