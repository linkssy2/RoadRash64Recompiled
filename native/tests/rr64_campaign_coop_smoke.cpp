#include "rr64_campaign_coop.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <vector>
namespace rr64::netplay {
Status status;
Status get_status() { return status; }
}
namespace {
using namespace rr64::engine;
unsigned checks=0;
void check(bool ok,const char *message) { ++checks; if(!ok){std::fprintf(stderr,"%s\n",message);std::exit(1);} }
unsigned word(unsigned char *m,unsigned a) {unsigned v=0;read_u32(m,a,v);return v;}
unsigned half(unsigned char *m,unsigned a) {std::uint16_t v=0;read_u16(m,a,v);return v;}
constexpr unsigned profile=0x800D6A40, descriptor=0x800D8520, actors=0x800D8570;
void mode(unsigned char *m,unsigned v) {write_u32(m,globals::main_mode,v);write_u32(m,globals::pending_mode,v);}
void enter(unsigned char *m,unsigned level) {
    mode(m,0x20);
    rr64_coop_mode(m,0x20);
    write_u32(m,local_race::menu_cursor,5);write_u32(m,local_race::menu_buttons,4);
    rr64_coop_main_menu(m);
    check(word(m,local_race::menu_cursor)==2,"Co-op must dispatch Big Game, not Thrash");
    write_u32(m,profile+0x3C,level);write_u32(m,profile+0x14,12);write_u32(m,profile+0x18,7);
    write_u32(m,profile+0x38,60545);
    for(unsigned i=0;i<18;++i)write_u32(m,descriptor+i*4,0x100+i);
    write_u32(m,0x800A6680,3);write_u32(m,0x800A6690,4);
    mode(m,0x2F);
    check(rr64_coop_mode(m,0x16)==0x23,"Campaign must visit two-player preparation before loading");
    mode(m,0x23);rr64_coop_selection_begin(m);
    check(word(m,0x8009EF5C)==2 && word(m,0x8009EF28)==2,"Exactly two native selectors");
    check(word(m,0x8009EF2C)==1 && word(m,0x8009EF30)==1,"Both players must confirm");
    check(word(m,0x800A6690)==(level==5?6:level),"Partner uses campaign tier, including Insanity");
    check(rr64_coop_bike_count(0,2)==1 && rr64_coop_bike_count(1,2)==2,"Only owner's bike is locked");
    check(word(m,rr64_coop_bike_entry(m,0,0)+4)==12,"Owner preview uses purchased bike");
}
void commit(unsigned char *m) {
    write_u32(m,0x8009F5DC,1);write_u32(m,0x8009F404,9);write_u32(m,0x8009F400,7);
    check(rr64_coop_mode(m,0x1B)==0x16,"Ready selectors return to campaign loader");
    check(word(m,profile+0x14)==12 && word(m,profile+0x38)==60545,"Partner never buys/replaces owner's bike");
    for(unsigned i=0;i<18;++i)check(word(m,descriptor+i*4)==0x100+i,"Selector must preserve campaign descriptor");
    check(word(m,0x800A6680)==3 && word(m,0x800A6690)==4,"Campaign route restored");
}
}
int main() {
    using namespace rr64::engine;
    std::vector<unsigned char> memory(kRdramSize);auto *m=memory.data();
    auto before=memory;
    check(rr64_coop_mode(m,0x16)==0x16,"Solo routing unchanged");
    rr64_coop_roster(m);rr64_coop_riders(m);
    check(memory==before,"Disabled bridge changes no guest bytes");
    for(unsigned level=0;level<6;++level) {
        enter(m,level);commit(m);
        for(unsigned population=2;population<=14;++population) {
            for(unsigned i=0;i<8;++i)write_u16(m,descriptor+0x2C+i*2,0);
            write_u16(m,descriptor+0x2C,1);write_u16(m,descriptor+0x2E,population-1);
            rr64_coop_roster(m);
            unsigned total=0;for(unsigned i=0;i<8;++i)total+=half(m,descriptor+0x2C+i*2);
            check(word(m,local_race::humans)==2 && total<=14,"Two humans fit native actor pool");
            check(total==std::min(population+1,14u),"Only full rosters replace an AI");
        }
        rr64_coop_riders(m);
        check(word(m,actors+0x118+0x18)==1 && word(m,actors+0x118+0x1C)==9,"Partner has own bike and rider");
        check(word(m,profile+0x14)==12 && word(m,profile+0x38)==60545,"Construction preserves purchases and money");
        const unsigned a=0x800D7810,b=a+0x64;
        write_u32(m,actors+0xE8,a);write_u32(m,actors+0x118+0xE8,b);
        for(unsigned i=0;i<0xC8;++i)write_s8(m,a+i,0);
        write_u32(m,a+0x44,0);write_u32(m,b+0x44,1);
        write_u16(m,a+0x4C,1);write_u16(m,b+0x50,1);
        check(rr64_coop_result_actor(m,actors)==actors+0x118,"Finished partner beats unqualified owner's stale rank");
        check(rr64_coop_result_state(m,a)==b,"Result screen uses same qualifying rider");
        mode(m,0x17);write_u32(m,0x800D7648,1);
        check(rr64_coop_mode(m,0x32)==0x17,"Bust cannot end partner's race");
        check(rr64_coop_mode(m,0x30)==0x17,"Wreck cannot end partner's race");
        write_u32(m,0x800D7648,0);
        check(rr64_coop_mode(m,0x32)==0x19,"All done enters native results");
        mode(m,0x19);
        check(rr64_coop_mode(m,0x1A)==0x1A,"Partner finish follows single campaign award flow");
        write_u16(m,b+0x50,0);write_u16(m,b+0x4E,1);
        check(rr64_coop_mode(m,0x1A)==0x32,"Both eliminated follow failure, no qualification");
        rr64_coop_mode(m,0x2F);check(!rr64_coop_spectating(),"Campaign menu retires spectator ownership");
    }
    enter(m,0);write_u32(m,globals::multiplayer_stage,1);rr64_coop_selection_end(m);
    check(word(m,globals::pending_mode)==0x2F,"Back returns to campaign, not multiplayer settings");
    check(word(m,profile+0x38)==60545,"Cancel costs no money");
    enter(m,0);rr64::netplay::status.active=true;before=memory;
    check(rr64_coop_mode(m,0x23)==0x23,"Online keeps its mode");
    rr64_coop_roster(m);rr64_coop_riders(m);check(memory==before,"Online excludes co-op guest writes");
    check(!rr64::campaign_coop::enabled.load(),"Online retires offline co-op");
    std::printf("Campaign co-op: %u checks passed\n",checks);
}
