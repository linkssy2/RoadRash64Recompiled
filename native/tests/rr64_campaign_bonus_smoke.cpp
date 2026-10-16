// Native campaign control and arithmetic, with rendering/model/audio IO stubbed.
// ROM metadata is read privately at execution and never embedded in the test.
#include "rr64_engine_layout.hpp"
#include "rr64_native.hpp"
#include "rr64_campaign_bonus_save.hpp"
#include "rr64_ai_bike_selection.hpp"
#include "rr64_netplay.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

extern "C" {
void bonus_native_route(unsigned char*, recomp_context*);
void bonus_native_postrace(unsigned char*, recomp_context*);
void bonus_native_ai_pool(unsigned char*, recomp_context*);
void stock_bonus_native_route(unsigned char*, recomp_context*);
void stock_bonus_native_postrace(unsigned char*, recomp_context*);
void stock_func_80073658(unsigned char*, recomp_context*);
void stock_func_80072E74(unsigned char*, recomp_context*);
void func_80073658(unsigned char*, recomp_context*);
void func_80073728(unsigned char*, recomp_context*);
void func_80072E74(unsigned char*, recomp_context*);
void func_80072A14(unsigned char*, recomp_context*);
void func_800516B8(unsigned char*, recomp_context*);
void stock_func_800516B8(unsigned char*, recomp_context*);
void bonus_load_8002C3D8(unsigned char*, recomp_context*);
void bonus_load_8002C718(unsigned char*, recomp_context*);
void bonus_load_8007290C(unsigned char*, recomp_context*);
void bonus_load_800729B4(unsigned char*, recomp_context*);
void bonus_load_8002DE20(unsigned char*, recomp_context*);
void bonus_load_8002DE4C(unsigned char*, recomp_context*);
void bonus_load_8002DE84(unsigned char*, recomp_context*);
void bonus_load_80073994(unsigned char*, recomp_context*);
void bonus_load_80073A34(unsigned char*, recomp_context*);
}
namespace {
using namespace rr64::engine;
constexpr unsigned profile=0x800D6A40, descriptor=0x800D8520, actor=0x800D8570;
unsigned checks=0, completed=0, advanced=0, preview=0, mode=0;
unsigned allocated_ai=0;
unsigned expected_ai_tier=7;
std::vector<unsigned> allocated_rows;
bool online_active=false;
unsigned confirmed_network_bike=~0u;
unsigned word(unsigned char* m, unsigned a) { unsigned v=0; read_u32(m,a,v); return v; }
unsigned half(unsigned char* m, unsigned a) { std::uint16_t v=0; read_u16(m,a,v); return v; }
unsigned byte(unsigned char* m, unsigned a) { std::uint8_t v=0; read_u8(m,a,v); return v; }
void check(bool b,const char* message) { ++checks; if(!b) { std::fprintf(stderr,"FAIL %s\n",message); std::exit(1); } }
std::vector<unsigned char> bytes(unsigned char* m,unsigned a,unsigned n) {
    std::vector<unsigned char> v(n); for(unsigned i=0;i<n;++i) read_u8(m,a+i,v[i]); return v;
}
recomp_context context() { recomp_context c{}; c.r29=S32(0x807F0000); c.r31=0x12345678; return c; }
void postrace_context(recomp_context& c) { c=context(); c.r19=1; c.r21=S32(profile); c.r22=S32(0x800D0000); }
}
extern "C" void func_80072880(unsigned char*,recomp_context*) {}
// Full startup scanning is exercised by RR64CampaignBonusSaveSmoke.
extern "C" void func_800207DC(unsigned char*,recomp_context* c) { c->r2 = 0; }
extern "C" void func_80057F88(unsigned char*,recomp_context*) {}
extern "C" void func_80057FC0(unsigned char*,recomp_context*) {}
extern "C" void func_80057FF8(unsigned char*,recomp_context*) {}
extern "C" void func_80058CF0(unsigned char*,recomp_context*) {}
extern "C" void func_8002FAB8(unsigned char*,recomp_context*) {}
extern "C" void func_800796F8(unsigned char*,recomp_context*) {}
extern "C" void func_8006CB8C(unsigned char*,recomp_context*) {}
extern "C" void func_8005F3B4(unsigned char*,recomp_context*) {}
extern "C" void func_8006CF60(unsigned char*,recomp_context*) {}
extern "C" void func_800727A0(unsigned char*,recomp_context* c) { c->r2=7; }
extern "C" void func_800696E0(unsigned char*,recomp_context* c) { preview=unsigned(c->r4); c->f0.fl=1.0f; }
extern "C" void func_8001A46C(unsigned char* m,recomp_context* c) {
    for(unsigned i=0;i<unsigned(c->r6);++i) { std::uint8_t b; read_u8(m,unsigned(c->r5)+i,b); write_s8(m,unsigned(c->r4)+i,b); }
}
extern "C" void func_8001A440(unsigned char*,recomp_context*) {}
extern "C" unsigned rr64_local_bike_level(unsigned original) { return original; }
extern "C" int rr64_custom_cop_active() { return 0; }
extern "C" void rr64_custom_cop_ai_pool(unsigned char*,void*) {}
extern "C" unsigned rr64_online_bike_profile(unsigned char*,unsigned,unsigned donor) { return donor; }
extern "C" unsigned rr64_online_race_choice(unsigned,unsigned original,unsigned) { return confirmed_network_bike==~0u ? original:confirmed_network_bike; }
namespace rr64::netplay { Status get_status() { Status s{}; s.active=online_active; return s; } }
extern "C" void rr64_prediction_verify_random(unsigned char*,unsigned) {}
extern "C" void func_80051E24(unsigned char* m,recomp_context* c) {
    const unsigned row=unsigned(c->r5), racer=unsigned(c->r4);
    check(row>=0x800A3460 && row<0x800A3460+160*16 && (row-0x800A3460)%16==0,"native AI assignment must have valid donor");
    check(racer>=actor && racer<actor+14*0x118,"native AI destination within14 actors");
    check(half(m,racer+0x24)==0 || half(m,racer+0x26)!=0,"native AI producer overwrote reserved human");
    check(byte(m,row+8)==expected_ai_tier,"native AI construction stays in effective bike tier");
    if(expected_ai_tier==7) check(byte(m,row+10)==25 || byte(m,row+10)==26,"full native AI construction uses Insanity model");
    allocated_rows.push_back(row);
    ++allocated_ai;
}
extern "C" void rr64_local_options_reset_race() {}
extern "C" void rr64_achievement_campaign_completed(unsigned char*) { ++completed; }
extern "C" void rr64_achievement_campaign_level_advanced(unsigned char*,unsigned) { ++advanced; }
extern "C" unsigned rr64_online_postrace_route_mode(unsigned char*,unsigned m) { mode=m; return m; }
extern "C" void rr64_trace_race_end(unsigned char*,unsigned,unsigned) {}
extern "C" void rr64_thrash_options_mode(unsigned) {}
extern "C" void rr64_online_menu_mode_changed(unsigned) {}
extern "C" void rr64_rival_engine_mode(unsigned char*,void*,unsigned) {}
extern "C" unsigned rr64_offline_bikes_count(unsigned char*,unsigned original,unsigned) { return original; }
extern "C" unsigned rr64_offline_bikes_shop_table(unsigned char*,unsigned original,unsigned) { return original; }

int main(int argc,char** argv) {
    check(argc==2,"private USA ROM path required");
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<unsigned char> rom((std::istreambuf_iterator<char>(file)),{});
    check(rom.size()>=0x100000,"private ROM unavailable");
    std::vector<unsigned char> memory(kRdramSize);
    auto* m=memory.data();
    const auto seed=[&](unsigned a,unsigned size) {
        const unsigned off=a-0x80000000u+0xC00u;
        check(off+size<=rom.size(),"private metadata range");
        for(unsigned i=0;i<size;++i) write_s8(m,a+i,rom[off+i]);
    };
    seed(0x800A0000,0x7800); seed(0x80007000,0x400); seed(0x80004E80,0x30);
    seed(0x80000ED8,0x20); // Constants used by the actual native RNG.
    write_u32(m,0x800A6524,0x80600000); write_u32(m,0x800A64E8,0x80610000);
    write_u32(m,profile+0x20,1); write_u32(m,profile+0x14,25);
    write_u32(m,profile+0x50,0x11111111); write_u32(m,profile+0x54,0x00010002);
    const auto original_tables=bytes(m,0x800A68C8,0x740C-0x68C8);
    const unsigned routes[]={43,44,45,47,48,54,37,46};

    // Drive the native clamp, full 72-byte donor copy, route ID and preview
    // pointer construction for every ordinary and bonus selection.
    for(unsigned level=0;level<=5;++level) {
        write_u32(m,profile+0x3C,level);
        const unsigned count=level==0 ? 6:8;
        for(unsigned selected=0;selected<count;++selected) {
            write_u32(m,0x800A6680,selected); write_u32(m,0x800A6684,~0u);
            auto expected=memory;
            auto c=context(), stock=context();
            if(level<5) stock_bonus_native_route(expected.data(),&stock);
            bonus_native_route(m,&c);
            const unsigned donor=word(m,0x800A73E4+std::min(level,4u)*4)+selected*0x48;
            if(level<5) {
                check(bytes(m,descriptor,0x48)==bytes(expected.data(),descriptor,0x48),"ordinary native descriptor changed");
                check(word(m,0x800A64D8)==word(expected.data(),0x800A64D8),"ordinary preview changed");
            } else {
                check(word(m,descriptor)==6 && word(m,descriptor+4)==1 && word(m,descriptor+0x28)==7,"bonus descriptor tier/type/AI donor pool");
                check(half(m,descriptor+8)==routes[selected] && word(m,0x800A64D8)==routes[selected] && word(m,0x800A64DC)==routes[selected],"bonus actual route IDs");
                check(preview==0x80600000+routes[selected]*76,"bonus native preview/name record");
                check(bytes(m,descriptor+0xC,0x1C)==bytes(m,donor+0xC,0x1C) && bytes(m,descriptor+0x2C,0x1C)==bytes(m,donor+0x2C,0x1C),"bonus inherited Level5 economics/population");
                check(word(m,0x800A6690)==6,"bonus native track tier");
            }
        }
        for(unsigned invalid : {~0u,count,100u}) {
            write_u32(m,0x800A6680,invalid); write_u32(m,0x800A6684,~0u);
            auto c=context(); bonus_native_route(m,&c);
            check(word(m,0x800A6680)==(invalid==~0u ? count-1:0),"native selection wraps within chapter");
        }
    }

    // Model changes keep the closest available native physics/skill profile.
    // A copied RNG context must not clobber the producer's live registers;
    // police, reserved humans and invalid profiles must not consume a draw.
    const auto donor_table=bytes(m,0x800A3460,160*16);
    const unsigned ai_actor=actor+4*0x118;
    for(unsigned i=0;i<0x118;++i) write_s8(m,ai_actor+i,0);
    for(unsigned i=4;i<160;++i) {
        const unsigned original=0x800A3460+i*16;
        const unsigned tier=byte(m,original+8), family=byte(m,original+9);
        if(tier==12) break;
        if(tier<1 || tier>7) continue;
        for(unsigned sample=0;sample<8;++sample) {
            auto call=context(); call.r4=S32(ai_actor); call.r5=S32(original);
            call.r16=0x13579; call.f12.fl=0.375f;
            const auto before=call;
            write_u32(m,0x8009DC30,0x9E3779B9u*(sample+1));
            const unsigned rng_before=word(m,0x8009DC30);
            const unsigned selected=rr64_ai_bike_profile(m,&call,original);
            check(std::memcmp(&before,&call,sizeof(call))==0,"AI model selection clobbered native registers");
            if(family==7) {
                check(selected==original && word(m,0x8009DC30)==rng_before,"police profiles must bypass randomization");
                continue;
            }
            check(selected>=0x800A34A0 && selected<0x800A3460+160*16 && (selected-0x800A3460)%16==0,"AI selected malformed donor row");
            check(byte(m,selected+8)==tier && byte(m,selected+9)>=1 && byte(m,selected+9)<=4,"AI selected wrong tier or role");
            const unsigned model=byte(m,selected+10), rank=byte(m,original+13);
            unsigned nearest=256;
            for(unsigned j=4;j<160;++j) {
                const unsigned candidate=0x800A3460+j*16;
                if(byte(m,candidate+8)==12) break;
                if(byte(m,candidate+8)!=tier || byte(m,candidate+10)!=model ||
                   byte(m,candidate+9)<1 || byte(m,candidate+9)>4) continue;
                const unsigned other=byte(m,candidate+13);
                nearest=std::min(nearest,other>rank ? other-rank:rank-other);
            }
            const unsigned selected_rank=byte(m,selected+13);
            check((selected_rank>rank ? selected_rank-rank:rank-selected_rank)==nearest,"AI model change altered native difficulty unnecessarily");
        }
    }
    check(bytes(m,0x800A3460,160*16)==donor_table,"AI randomization modified ROM donor metadata");
    auto protected_call=context(); protected_call.r4=S32(ai_actor);
    const unsigned valid_profile=0x800A34A0;
    write_u16(m,ai_actor+0x24,1); write_u16(m,ai_actor+0x26,0);
    const unsigned protected_rng=word(m,0x8009DC30);
    check(rr64_ai_bike_profile(m,&protected_call,valid_profile)==valid_profile,"AI helper changed a reserved human profile");
    write_u16(m,ai_actor+0x24,0);
    confirmed_network_bike=25;
    check(rr64_ai_bike_profile(m,&protected_call,valid_profile)==valid_profile,"AI helper changed a confirmed network human profile");
    confirmed_network_bike=~0u;
    for(unsigned invalid:{0u,0x800A3461u,0x80800000u})
        check(rr64_ai_bike_profile(m,&protected_call,invalid)==invalid,"invalid AI donor must be rejected unchanged");
    check(rr64_ai_bike_profile(nullptr,&protected_call,valid_profile)==valid_profile &&
          rr64_ai_bike_profile(m,nullptr,valid_profile)==valid_profile,"null AI helper inputs must be rejected");
    check(word(m,0x8009DC30)==protected_rng,"rejected AI selection consumed random state");

    // Exercise the real producer and private ROM donor data, including Level
    // 1's unusual menu order. Every human choice must leave all four ordinary
    // models reachable. The former allocation path is a negative control;
    // police and reserved humans keep their original roles in both paths.
    unsigned ordinary_ai_cases=0, restricted_stock_pools=0;
    for(unsigned level=0;level<5;++level) {
        const unsigned tier=level+1;
        std::set<unsigned> expected_models;
        std::vector<unsigned> human_profiles;
        for(unsigned i=4;i<160;++i) {
            const unsigned row=0x800A3460+i*16;
            if(byte(m,row+8)==12) break;
            if(byte(m,row+8)!=tier || byte(m,row+9)==7) continue;
            if(expected_models.insert(byte(m,row+10)).second) human_profiles.push_back(row);
        }
        check(expected_models.size()==4,"private ordinary tier must contain four distinct bike models");
        const unsigned tracks=level==0 ? 6:8;
        for(unsigned track=0;track<tracks;++track) for(unsigned human_profile:human_profiles)
        for(unsigned humans:{1u,4u}) {
            std::set<unsigned> selected_models, stock_models;
            const auto produce=[&](bool stock,unsigned rng_seed) {
                write_u32(m,globals::main_mode,0x18); write_u32(m,globals::pending_mode,0x18);
                write_u32(m,profile+0x3C,level); write_u32(m,0x800A6680,track);
                write_u32(m,0x800A6684,~0u);
                auto call=context(); bonus_native_route(m,&call);
                check(word(m,descriptor+0x28)==tier,"native chapter selected unexpected donor tier");
                for(unsigned i=0;i<14*0x118;++i) write_s8(m,actor+i,0);
                for(unsigned i=0;i<humans;++i) {
                    const unsigned racer=actor+i*0x118;
                    write_u32(m,racer+0x18,byte(m,human_profile+10));
                    write_u32(m,racer+0x20,byte(m,human_profile+9));
                    write_u16(m,racer+0x24,1); write_u16(m,racer+0x26,0);
                }
                const auto reserved=bytes(m,actor,humans*0x118);
                write_u32(m,0x8009DC30,rng_seed);
                call=context(); call.r4=S32(descriptor);
                call.r5=0x3E800000; call.r6=0x3F400000; call.r7=0x3F000000;
                write_u32(m,unsigned(call.r29)+0x10,2);
                allocated_ai=0; allocated_rows.clear(); expected_ai_tier=tier;
                (stock ? stock_func_800516B8:func_800516B8)(m,&call);
                unsigned population=0; for(unsigned i=0;i<8;++i) population+=half(m,descriptor+0x2C+i*2);
                check(allocated_ai==population-humans,"ordinary AI count must preserve native human reservation");
                check(bytes(m,actor,humans*0x118)==reserved,"ordinary AI choice changed a human actor");
                check(unsigned(call.r29)==0x807F0000 && call.r31==0x12345678,"ordinary AI producer corrupted caller context");
                return allocated_rows;
            };
            for(unsigned sample=0;sample<16;++sample) {
                const unsigned rng_seed=0x9E3779B9u*(sample+1);
                const auto stock=produce(true,rng_seed), selected=produce(false,rng_seed);
                unsigned stock_cops=0, selected_cops=0;
                for(unsigned row:stock) {
                    if(byte(m,row+9)==7) ++stock_cops;
                    else stock_models.insert(byte(m,row+10));
                }
                for(unsigned row:selected) {
                    if(byte(m,row+9)==7) ++selected_cops;
                    else {
                        check(expected_models.count(byte(m,row+10))==1,"AI model is outside actual current-level pool");
                        selected_models.insert(byte(m,row+10));
                    }
                }
                check(selected_cops==stock_cops,"AI randomization changed native police count");
                if(sample==0) check(produce(false,rng_seed)==selected,"native seeded AI roster must be reproducible");
                ++ordinary_ai_cases;
            }
            if(stock_models!=expected_models) ++restricted_stock_pools;
            check(selected_models==expected_models,"human bike selection excluded a valid AI bike model");
        }
    }
    check(restricted_stock_pools>0,"negative control did not reproduce the former restricted AI pool");
    expected_ai_tier=7; allocated_rows.clear();

    // Native rider-pool filter must select the actual Insanity model donors,
    // not ordinary Level 5 racers accidentally carried in the donor profile.
    write_u32(m,profile+0x3C,5); write_u32(m,0x800A6680,0); write_u32(m,0x800A6684,~0u);
    auto c=context(); bonus_native_route(m,&c);
    c=context(); c.r23=S32(descriptor);
    for(unsigned i=0;i<0x200;++i) write_s8(m,unsigned(c.r29)+0x48+i,0);
    bonus_native_ai_pool(m,&c);
    unsigned donors=0;
    for(unsigned gang=0;gang<8;++gang) {
        const unsigned count=word(m,0x807F0048+gang*4);
        check(count<=15,"native donor group bound");
        for(unsigned i=0;i<count;++i) {
            const unsigned row=word(m,0x807F0098+(gang*15+i)*4);
            std::uint8_t tier=0,bike=0; read_u8(m,row+8,tier); read_u8(m,row+10,bike);
            check(tier==7 && (bike==25 || bike==26),"native AI pool uses Insanity models"); ++donors;
        }
    }
    check(donors>0,"native Insanity donor pool is populated");
    // The demand repair is scoped to the offline bonus race, including its
    // pending initialization. A saved bonus profile alone is insufficient.
    for(unsigned scenario=0;scenario<6;++scenario) {
        write_u32(m,profile+0x3C,scenario==2 ? 4:5);
        write_u32(m,descriptor+0x28,scenario==3 ? 4:7);
        write_u32(m,globals::main_mode,scenario==1 || scenario==4 ? 0x2F:0x18);
        write_u32(m,globals::pending_mode,scenario==1 ? 0x18:0x2F);
        online_active=scenario==5; c=context();
        for(unsigned i=0;i<0x50;++i) write_s8(m,0x807F0048+i,0);
        write_u32(m,0x807F0048+4,3); write_u32(m,0x807F0070+8,2);
        const auto before=bytes(m,0x807F0048,0x50);
        rr64_local_bike_ai_pool(m,&c);
        if(scenario<2) check(word(m,0x807F0074)==2 && word(m,0x807F0078)==0,"offline bonus redirects unsupported donor demands");
        else check(bytes(m,0x807F0048,0x50)==before,"bonus AI guard changed ordinary/menu/online demands");
    }
    online_active=false;
    unsigned ai_cases=0;
    for(unsigned track=0;track<8;++track) for(unsigned bike:{25u,26u}) for(unsigned family=1;family<=6;++family) {
        write_u32(m,globals::main_mode,0x18); write_u32(m,globals::pending_mode,0x18);
        write_u32(m,profile+0x3C,5); write_u32(m,0x800A6680,track); write_u32(m,0x800A6684,~0u);
        c=context(); bonus_native_route(m,&c);
        for(unsigned i=0;i<14*0x118;++i) write_s8(m,actor+i,0);
        write_u32(m,actor+0x18,bike); write_u32(m,actor+0x20,family);
        write_u16(m,actor+0x24,1); write_u16(m,actor+0x26,0);
        c=context(); c.r4=S32(descriptor); c.r5=0x3E800000; c.r6=0x3F400000; c.r7=0x3F000000;
        write_u32(m,unsigned(c.r29)+0x10,2); allocated_ai=0;
        func_800516B8(m,&c);
        unsigned population=0; for(unsigned i=0;i<8;++i) population+=half(m,descriptor+0x2C+i*2);
        check(allocated_ai==population-1 && allocated_ai>0 && allocated_ai<14,"native donor demands cover whole campaign population");
        check(word(m,actor+0x18)==bike,"native AI assignment preserved selected human model");
        check(unsigned(c.r29)==0x807F0000 && c.r31==0x12345678,"native full AI producer preserves context");
        ++ai_cases;
    }

    // Execute each actual fee/shop load with its real address register. This
    // catches correct helper logic wired to a wrong native register or table.
    struct Load { void(*call)(unsigned char*,recomp_context*); unsigned base,in,out,bonus; };
    const Load loads[]={
        {bonus_load_8002C3D8,0x800A68B4,2,6,4}, {bonus_load_8002C718,0x800A68A0,2,6,4},
        {bonus_load_8007290C,0x800A68B4,5,8,4}, {bonus_load_800729B4,0x800A68A0,4,8,4},
        {bonus_load_8002DE20,0x800A66B8,3,2,6}, {bonus_load_8002DE4C,0x800A66B8,3,2,6},
        {bonus_load_8002DE84,0x800A6854,3,5,6}, {bonus_load_80073994,0x800A6854,2,4,6},
        {bonus_load_80073A34,0x800A6854,3,4,6}};
    const auto reg=[](recomp_context& v,unsigned r)->gpr& {
        switch(r) { case 2:return v.r2; case 3:return v.r3; case 4:return v.r4;
            case 5:return v.r5; case 6:return v.r6; default:return v.r8; }
    };
    for(unsigned level=0;level<=5;++level) for(const auto& load:loads) {
        write_u32(m,profile+0x3C,level); c=context();
        reg(c,load.in)=S32(load.base+level*4); load.call(m,&c);
        const unsigned index=level==5 ? load.bonus:level;
        check(unsigned(reg(c,load.out))==word(m,load.base+index*4),"actual native fee/shop load bounded to correct tier");
    }
    write_u32(m,profile+0x3C,4); c=context(); c.r2=S32(0x800A6854+5*4);
    bonus_load_80073994(m,&c);
    check(unsigned(c.r4)==word(m,0x800A6854+6*4),"completed Level5 next shop initializes Insanity");
    write_u32(m,profile+0x3C,5);

    // Both purchased variants pass unchanged through the native race setup.
    for(unsigned bike : {25u,26u}) {
        write_u32(m,profile+0x14,bike); c=context(); func_80073728(m,&c);
        check(word(m,actor+0x18)==bike && word(m,0x800A6418)==6,"native human bike and race tier");
        check(word(m,0x800A656C)>1 && word(m,0x800A656C)<=14,"native campaign population stays within14");
        check(unsigned(c.r29)==0x807F0000,"native race stack preserved");
    }

    const auto set_results=[&](unsigned level,unsigned value) {
        write_u32(m,profile+0x3C,level);
        if(level==5) {
            rr64_campaign_begin_bonus(m);
            rr64_campaign_qualification_store(m,profile+0x54,value);
        } else write_u32(m,profile+0x40+level*4,value);
    };
    const auto results=[&](unsigned level) {
        const unsigned address=profile+0x40+level*4;
        return rr64_campaign_qualification_word(m,address,word(m,address));
    };
    // Native menu scans all eight nibbles, including the most significant one.
    for(unsigned level=0;level<=5;++level) {
        const unsigned count=level==0 ? 6:8;
        for(unsigned missing=0;missing<=count;++missing) {
            const unsigned value=missing<count ? 0x11111111u & ~(15u<<(missing*4)):0x11111111u;
            set_results(level,value); write_u32(m,0x800A6680,(missing+1)%count);
            auto before=bytes(m,profile,0xF8); c=context(); func_80073658(m,&c);
            check(word(m,0x800A6680)==(missing<count ? missing:0),"native first unqualified/replay selection");
            check(bytes(m,profile,0xF8)==before && results(level)==value,"menu changed progress or inventory");
        }
    }
    // Stock ordinary behavior is byte-identical through real prize and writes.
    write_u32(m,actor+0xE8,0x80620000); write_u32(m,0x80620044,1);
    write_u32(m,actor+0x28,13);
    for(unsigned level=0;level<5;++level) {
        set_results(level,0x87654321); write_u32(m,0x800A6680,2);
        write_u32(m,0x800A6684,~0u); c=context(); bonus_native_route(m,&c);
        auto expected=memory; auto stock=context(); stock_func_80072E74(expected.data(),&stock);
        c=context(); func_80072E74(m,&c);
        check(bytes(m,profile,0xF8)==bytes(expected.data(),profile,0xF8),"ordinary prize/qualification differs from stock");
    }
    // Every repeat counter, finish branch and course passes through the actual
    // native load/clear/store/OR/store sequence, preserving profile+54 aliases.
    unsigned prize_cases=0;
    for(unsigned track=0;track<8;++track) for(unsigned old=0;old<16;++old) for(unsigned place=0;place<4;++place) {
        const unsigned shift=track*4;
        const unsigned before=(0x87654321u & ~(15u<<shift)) | (old<<shift);
        set_results(5,before);
        write_u32(m,profile+0x54,0x00120034); write_u32(m,profile+0x38,1000);
        write_u32(m,profile+0x28,10); write_u32(m,profile+0x34,20);
        const auto inventory=bytes(m,profile+0x54,0xA2);
        const auto base_progress=bytes(m,profile+0x40,0x14);
        write_u32(m,0x800A6680,track); write_u32(m,0x800A6684,~0u);
        c=context(); bonus_native_route(m,&c);
        const unsigned prize=half(m,descriptor+0xC+place*2)/(old+1);
        write_u32(m,0x80620044,place);
        c=context(); func_80072E74(m,&c);
        const unsigned after=place<3 ? (before&~(15u<<shift))|(std::min(old+1,15u)<<shift):before;
        check(results(5)==after,"native bonus qualification load/store and saturation");
        check(bytes(m,profile+0x54,0xA2)==inventory,"bonus qualification corrupted existing inventory/statistics");
        check(bytes(m,profile+0x40,0x14)==base_progress,"bonus qualification changed original chapters");
        check(word(m,profile+0x38)==1000+prize+35 && word(m,0x800A66A0)==prize,"bonus Level5 prize/repeat divisor/combat economics");
        check(word(m,profile+0x28)==11 && word(m,profile+0x34)==33,"native race/distance statistics");
        check(word(m,profile+0x3C)==5 && unsigned(c.r29)==0x807F0000 && c.r31==0x12345678,"result kept chapter and context");
        // Alternate result routine awards the same divided money but never
        // qualifies. Its separate qualification read hook must also be correct.
        set_results(5,before); write_u32(m,profile+0x38,1000);
        c=context(); func_80072A14(m,&c);
        check(results(5)==before && word(m,profile+0x38)==1000+prize+35,"alternate native result prize and unchanged qualification");
        check(word(m,profile+0x54)==0x00120034,"alternate result preserves aliased word");
        ++prize_cases;
    }
    // Run all eight first-time qualifications, accept each result and replay
    // after the eighth. No synthetic achievement or second ending is emitted.
    set_results(5,0); write_u32(m,0x800A1810,0x35); write_u16(m,0x8009CDA8,0x9000);
    write_u32(m,0x80620044,0); completed=advanced=0;
    for(unsigned track=0;track<8;++track) {
        write_u32(m,0x800A6680,track); write_u32(m,0x800A6684,~0u);
        c=context(); bonus_native_route(m,&c);
        c=context(); func_80072E74(m,&c);
        postrace_context(c); mode=0; bonus_native_postrace(m,&c);
        check(mode==0x2F && word(m,0x800A1814)==0x2F && word(m,profile+0x3C)==5,"bonus result returns to campaign through native mode table");
        check(completed==0 && advanced==0,"bonus emitted original completion/advancement achievement");
        c=context(); func_80073658(m,&c);
        check(word(m,0x800A6680)==(track==7 ? 0:track+1),"bonus sequence selects next course or replay");
    }
    check(results(5)==0x11111111,"all eight bonus qualifications retained");
    // Finish the original campaign from each possible final unqualified track.
    // The real prize initializer must decide from PRE-award saved progress, not
    // the newly qualified record later displayed by 73054 each frame.
    unsigned ending_cases=0;
    for(unsigned final_track=0;final_track<8;++final_track) {
        set_results(4,0x11111111u&~(15u<<(final_track*4)));
        write_u32(m,profile+0x14,20); write_u32(m,profile+0x38,425);
        write_u32(m,0x800A6680,final_track); write_u32(m,0x800A6684,~0u);
        c=context(); bonus_native_route(m,&c);
        const unsigned prize=half(m,descriptor+0xC);
        write_u32(m,0x80620044,0); c=context(); func_80072E74(m,&c);
        check(word(m,profile+0x50)==0x11111111,"first final qualification stored natively");
        check(word(m,profile+0x38)==425+prize+35,"first completion prize remains native");
        // Several idle result frames must not consume the first ending.
        write_u16(m,0x8009CDA8,0); completed=advanced=mode=0;
        for(unsigned frame=0;frame<3;++frame) { postrace_context(c); bonus_native_postrace(m,&c); }
        check(mode==0 && completed==0,"first ending waits for the player's acceptance");
        write_u16(m,0x8009CDA8,0x9000); postrace_context(c); bonus_native_postrace(m,&c);
        check(mode==0x38 && completed==1 && advanced==0 && word(m,profile+0x3C)==4,"first original ending and achievement preserved");
        check(rr64_campaign_ending_exit(m,1)==0x2F,"original ending returns to save-capable menu");
        c=context(); func_80073658(m,&c);
        check(word(m,0x8009E128)==3,"first ending still highlights Save Game");
        ++ending_cases;
    }
    // Farm any original finale race, including failed qualifications and the
    // saturated counter. Below $60,000 must not suppress its payout or charge
    // an Insanity bike. The completion route alone changes to the normal menu.
    for(unsigned track=0;track<8;++track) for(unsigned old:{1u,2u,15u}) for(unsigned place=0;place<4;++place) {
        const unsigned before=(0x11111111u&~(15u<<(track*4)))|(old<<(track*4));
        set_results(4,before); write_u32(m,profile+0x14,20); write_u32(m,profile+0x38,425);
        write_u32(m,0x800A6680,track); write_u32(m,0x800A6684,~0u);
        c=context(); bonus_native_route(m,&c);
        write_u32(m,0x80620044,place);
        auto expected=memory; auto stock=context(); stock_func_80072E74(expected.data(),&stock);
        c=context(); func_80072E74(m,&c);
        check(bytes(m,profile,0xF8)==bytes(expected.data(),profile,0xF8),"replay prize, cash, bike, inventory and counters exactly match native");
        const auto paid=bytes(m,profile,0xF8); completed=advanced=mode=0;
        postrace_context(c); bonus_native_postrace(m,&c);
        check(mode==0x2F && completed==0 && advanced==0,"completed original campaign replay must not run ending again");
        check(bytes(m,profile,0xF8)==paid,"replay routing changed native earnings or inventory");
        ++ending_cases;
    }
    for(unsigned missing=0;missing<8;++missing) {
        set_results(4,0x11111111u&~(15u<<(missing*4))); completed=advanced=mode=0;
        postrace_context(c); bonus_native_postrace(m,&c);
        check(mode==0x2F && completed==0 && advanced==0,"incomplete original campaign triggered ending");
    }
    // Exercise the production co-op hooks inside native construction and award
    // routines, including a busted owner whose partner qualifies the campaign.
    for(unsigned level=0;level<6;++level) {
        rr64::campaign_coop::enabled.store(true);
        set_results(level,0);
        write_u32(m,0x800A6680,0); write_u32(m,0x800A6684,~0u);
        c=context(); bonus_native_route(m,&c);
        write_u32(m,profile+0x14,level==5?25:12);
        write_u32(m,profile+0x18,7); write_u32(m,profile+0x38,60545);
        write_u32(m,0x800A1810,0x2F);
        check(rr64_coop_mode(m,0x16)==0x23,"native campaign enters co-op preparation");
        write_u32(m,0x800A1810,0x23);
        write_u32(m,0x8009F5DC,level==5?26:1); write_u32(m,0x8009F404,9);
        check(rr64_coop_mode(m,0x1B)==0x16,"native campaign resumes after confirmation");
        c=context(); func_80073728(m,&c);
        check(word(m,0x800A6578)==2,"native constructor keeps two humans");
        check(word(m,actor+0x18)==(level==5?25:12) && word(m,actor+0x118+0x18)==(level==5?26:1),"native constructor keeps separate bike models");
        check(word(m,0x800A656C)<=14,"native constructor bounds total roster");
        write_u32(m,actor+0xE8,0x80620000); write_u32(m,actor+0x118+0xE8,0x80620100);
        write_u16(m,0x8062004C,1); write_u16(m,0x80620050,0);
        write_u16(m,0x8062014C,0); write_u16(m,0x8062014E,0); write_u16(m,0x80620150,1);
        write_u32(m,0x80620044,0); write_u32(m,0x80620144,1);
        write_u32(m,actor+0x118+0x28,17);
        auto expected=memory;
        write_u32(expected.data(),actor+0xE8,0x80620100);
        write_u32(expected.data(),actor+0x28,17);
        // The existing bonus hooks also apply in the reference single-player
        // award; only the co-op actor redirection is disabled for this control.
        rr64::campaign_coop::enabled.store(false);
        auto stock=context(); func_80072E74(expected.data(),&stock);
        rr64::campaign_coop::enabled.store(true);
        c=context(); func_80072E74(m,&c);
        check(bytes(m,profile,0xF8)==bytes(expected.data(),profile,0xF8),"team award must equal exactly one native payout and qualification");
        rr64_coop_mode(m,0x20);
    }
    check(bytes(m,0x800A68C8,0x740C-0x68C8)==original_tables,"shared campaign route tables mutated");
    std::printf("{\"passed\":true,\"checks\":%u,\"nativeDonors\":%u,\"nativeAiCases\":%u,\"ordinaryAiCases\":%u,\"restrictedStockPools\":%u,\"nativePrizeCases\":%u,\"nativeEndingCases\":%u,\"gameLaunched\":false}\n",checks,donors,ai_cases,ordinary_ai_cases,restricted_stock_pools,prize_cases,ending_cases);
}
