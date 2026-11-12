#include "recomp.h"
#include "rr64_native.hpp"
#include <vector>
#include <cstdio>
int main() {
    std::vector<unsigned char> memory(8*1024*1024);
    auto* rdram=memory.data(); unsigned failures=0;
    auto check=[&](bool ok){ if(!ok) ++failures; };
    auto word=[&](uint32_t p)->int32_t& {return MEM_W(0,static_cast<int64_t>(static_cast<int32_t>(p)));};
    auto count=[&](uint32_t b)->int16_t& {return MEM_H(0,static_cast<int64_t>(static_cast<int32_t>(0x800BC9D0+b*2)));};
    auto record=[&](unsigned b,unsigned i,unsigned id,unsigned marker){
        auto p=0x800BCAD8+(b*96+i)*76;
        word(p)=marker; MEM_H(0x10,static_cast<int64_t>(static_cast<int32_t>(p)))=id;
    };
    // Both buffers, all stock cloud IDs, preexisting HUD and mixed records.
    for(unsigned b=0;b<2;++b) {
        word(0x8009CBA4)=b; count(b)=2;
        record(b,0,0x2D,123); record(b,1,5,456);
        rr64_sky_queue_begin(rdram);
        for(unsigned i=0;i<16;++i) record(b,i+2,0x2D+i,789);
        record(b,18,7,987); count(b)=19;
        rr64_sky_queue_end(rdram);
        check(count(b)==3);
        check(word(0x800BCAD8+b*96*76)==123); // Not this producer's span.
        check(word(0x800BCAD8+(b*96+1)*76)==456);
        check(word(0x800BCAD8+(b*96+2)*76)==987);
        // A later consumer or repeated end cannot trim another producer's data.
        count(b)=4; rr64_sky_queue_end(rdram); check(count(b)==4);
        rr64_sky_queue_begin(rdram); count(b)=97;
        rr64_sky_queue_end(rdram); check(count(b)==97);
        count(b)=96; rr64_sky_queue_begin(rdram); rr64_sky_queue_end(rdram); check(count(b)==96);
        count(b)=1; rr64_sky_queue_begin(rdram); count(b)=0;
        rr64_sky_queue_end(rdram); check(count(b)==0);
    }
    word(0x8009CBA4)=0; count(0)=0; rr64_sky_queue_begin(rdram);
    word(0x8009CBA4)=1; count(1)=5; rr64_sky_queue_end(rdram); check(count(1)==5);
    word(0x8009CBA4)=3; rr64_sky_queue_begin(rdram); rr64_sky_queue_end(rdram);
    std::printf("Cloud removal, both queues, HUD preservation and invalid spans: %u failures\n",failures);
    return failures?1:0;
}

