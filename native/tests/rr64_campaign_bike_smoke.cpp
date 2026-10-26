#include "rr64_engine_layout.hpp"
#include "rr64_native.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_local_players.hpp"
#include "rr64_character_preferences.hpp"
#include "rr64_netplay.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
extern "C" int rr64_highlights_presenting() { return 0; }

extern "C" void func_800728B8(unsigned char*, recomp_context*);
extern "C" void func_80073658(unsigned char*, recomp_context*);
extern "C" void func_8005F420(unsigned char*, recomp_context*);
extern "C" void test_thrash_bike_selection(unsigned char*, recomp_context*);
extern "C" void test_campaign_ending_exit(unsigned char*, recomp_context*);
extern "C" void test_validated_campaign_save(unsigned char*, recomp_context*);
extern "C" void test_campaign_buy(unsigned char*, recomp_context*);
extern "C" void test_campaign_join(unsigned char*, recomp_context*);
namespace { unsigned requested_mode = 0, menu_audio = 0, race_audio = 0, preview_bike = 0, promotions = 0; }
extern "C" void func_80072880(unsigned char*, recomp_context*) { ++menu_audio; }
extern "C" void func_80058084(unsigned char*, recomp_context*) { ++race_audio; }
extern "C" unsigned rr64_online_postrace_route_mode(unsigned char*, unsigned mode) { requested_mode = mode; return mode; }
extern "C" void rr64_online_menu_mode_changed(unsigned) {}
extern "C" void rr64_rival_engine_mode(unsigned char*, void*, unsigned) {}
extern "C" void rr64_trace_race_end(unsigned char*, unsigned, unsigned) {}
extern "C" void rr64_achievement_campaign_level_advanced(unsigned char*, unsigned) { ++promotions; }
extern "C" void func_8006B740(unsigned char*, recomp_context* c) { c->r2 = 0; }
extern "C" void func_8006B9E8(unsigned char*, recomp_context* c) { c->r2 = 0; }
extern "C" void func_80056084(unsigned char*, recomp_context*) {}
extern "C" void func_80047D68(unsigned char*, recomp_context* c) { c->r2=1; }
extern "C" void func_8001BAF8(unsigned char*, recomp_context*) {}
extern "C" void func_800470F0(unsigned char*, recomp_context*) {}
extern "C" void func_80046F68(unsigned char*, recomp_context* c) { preview_bike=unsigned(c->r6); c->r2=0; }
extern "C" void func_80024EB8(unsigned char*, recomp_context*) {}
// Full startup scanning is exercised by RR64CampaignBonusSaveSmoke.
extern "C" void func_800207DC(unsigned char*, recomp_context* c) { c->r2 = 0; }
extern "C" int rr64_offline_bikes_active(unsigned char*, unsigned) { return 0; }
extern "C" unsigned rr64_offline_bikes_entry(unsigned char*, unsigned, unsigned stock) { return stock; }
extern "C" unsigned rr64_offline_bikes_count(unsigned char*, unsigned stock, unsigned) { return stock; }
// The opt-in live probe is dormant in this independent native selector fixture.
namespace rr64::offline_modifiers { bool enabled(Flag) { return false; } }
namespace rr64::netplay {
Status get_status() { return {}; }
PhysicsRules get_physics_rules() { return {}; }
bool authority_get_outcome(unsigned,authority::Outcome&,bool&) { return false; }
}
namespace recomp {
void* alloc(unsigned char* m, size_t n) {
    static unsigned next = 0x700000;
    const unsigned address = next;
    next += unsigned(n + 15) & ~15u;
    return m + address;
}
}
namespace {
using namespace rr64::engine;
constexpr unsigned profile = 0x800D6A40, cache = 0x800C07C8, unlocks = 0x800A5334;
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
unsigned word(unsigned char* m, unsigned address) {
    unsigned v = 0; read_u32(m, address, v); return v;
}
void checksum(unsigned char* m, unsigned record) {
    unsigned sum = 0;
    for (unsigned offset = 0; offset < 0xF8; offset += 4)
        if (offset != 4) sum += word(m, record + offset);
    write_u32(m, record + 4, sum);
}
std::vector<unsigned char> bytes(unsigned char* m, unsigned address, unsigned size) {
    std::vector<unsigned char> result(size);
    for (unsigned i = 0; i < size; ++i) read_u8(m, address+i, result[i]);
    return result;
}
}
int main(int argc, char** argv) {
    check(argc == 2, "Pass private USA ROM path");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> rom((std::istreambuf_iterator<char>(file)), {});
    check(rom.size() >= 0x100000, "Private ROM is unavailable");
    std::vector<unsigned char> memory(kRdramSize);
    auto* m = memory.data();
    const auto seed = [&](unsigned address, unsigned size) {
        const unsigned offset = address - 0x80000400u + 0x1000u;
        check(offset + size <= rom.size(), "ROM table range");
        for (unsigned i = 0; i < size; ++i) write_s8(m, address+i, static_cast<std::int8_t>(rom[offset+i]));
    };
    seed(0x800A3460, 160*16);
    seed(0x800A174C, 36*4);
    seed(0x800A66B8, 0x1DC);
    seed(0x800A73F8, 5*4);
    seed(0x800A6620, 0x80);
    recomp_context ctx{};
    ctx.r29 = S32(0x807FF000);
    ctx.r31 = 0x12345678;
    // Native ending exit retains a complete, legal Level 5 profile. Returning
    // to its Save row must not charge another race or replay credits itself.
    write_u32(m, profile+0x3C, 4);
    write_u32(m, profile+0x20, 1);
    write_u32(m, profile+0x50, 0x12312312);
    write_u32(m, profile+0x30, 50);
    write_u32(m, profile+0x34, 900);
    write_u32(m, profile+0x38, 60000);
    write_u32(m, 0x800A68B0, 1000);
    write_u32(m, 0x800D8598, 55);
    const auto completed_profile = bytes(m, profile, 0xF8);
    ctx.r4 = 1; ctx.r30 = S32(0x800A0000);
    std::puts("Checking native ending and campaign menu"); std::fflush(stdout);
    test_campaign_ending_exit(m, &ctx);
    check(requested_mode == 0x2F, "Ending did not return to campaign Save menu");
    write_u32(m, 0x800A6680, 7);
    func_80073658(m, &ctx);
    check(menu_audio == 1 && race_audio == 0, "Original campaign menu initialization changed");
    check(bytes(m, profile, 0xF8) == completed_profile, "Credits return changed campaign progress/money");
    check(word(m, 0x8009EF68) == 0 && word(m, 0x8009E128) == 3, "Save Game is not highlighted");
    check(unsigned(ctx.r29) == 0x807FF000 && ctx.r31 == 0x12345678, "Menu stack/return address changed");
    write_u32(m, 0x8009E128, 2);
    func_80073658(m, &ctx);
    check(word(m, 0x8009E128) == 2, "Save highlight leaked to ordinary campaign menu entry");
    func_800728B8(m, &ctx);
    check(word(m, profile+0x30) == 51 && word(m, profile+0x34) == 955 &&
          word(m, profile+0x38) == 59000, "Ordinary failed-race entry changed");
    write_u32(m, 0x800A6680, 7);
    func_80073658(m, &ctx);
    check(word(m, 0x800A6680) == 0 && word(m, profile+0x3C) == 4,
          "Completed profile selected an invalid next level/track");

    // Drive the stock purchase and Join blocks. Both $60,000 Insanity variants
    // enter the optional bonus chapter without changing prior qualifications,
    // bike ownership, money or the original ending's rewards.
    unsigned shop_cases = 0;
    const auto join = [&] {
        ctx.r18 = ctx.r30 = S32(0x800A0000);
        ctx.r23 = S32(0x800D0000);
        ctx.r29 = S32(0x807FF000);
        write_u32(m, unsigned(ctx.r29) + 0x140, 1);
        write_u32(m, 0x8009E238, 1); // Accept.
        write_u32(m, 0x8009E128, 1); // Join.
        write_u32(m, globals::main_mode, 0x30); // Current mode stays unchanged until dispatch.
        write_u32(m, globals::pending_mode, 0x30);
        promotions = requested_mode = 0;
        test_campaign_join(m, &ctx);
    };
    std::puts("Checking native completed-campaign purchase and Join"); std::fflush(stdout);
    for (unsigned bike : {25u, 26u}) {
        write_u32(m, profile + 0x3C, 4);
        write_u32(m, profile + 0x50, 0x12312312);
        write_u32(m, profile + 0x38, 60545);
        write_u32(m, profile + 0x1C, 6);
        write_u32(m, 0x800D8510, 60000);
        write_u32(m, 0x800D8514, bike);
        write_u32(m, 0x8009E238, 1);
        test_campaign_buy(m, &ctx);
        check(word(m, profile + 0x14) == bike && word(m, profile + 0x38) == 545,
              "Native Insanity purchase did not preserve bike/correct $60,000 cost");
        auto bought = bytes(m, profile, 0xF8);
        bought[0x3F] = 5; // The only native profile change is the bonus chapter index.
        write_u16(m, unlocks + 6 * 2, 1);
        write_u16(m, unlocks + 14 * 2, 1);
        const auto unlocked = bytes(m, unlocks, 30);
        join();
        check(word(m, profile + 0x3C) == 5,
              "Completed shop Join did not enter the optional Insanity bonus");
        check(bytes(m, profile, 0xF8) == bought && bytes(m, unlocks, 30) == unlocked && promotions == 0,
              "Completed shop Join changed purchase, cash, qualifications or rewards");
        check(requested_mode == 0x2F && word(m, globals::pending_mode) == 0x2F &&
              word(m, globals::main_mode) == 0x30,
              "Completed Join did not request the native campaign-menu transition");
        write_u32(m, 0x800A6680, 7);
        func_80073658(m, &ctx);
        check(word(m, 0x800A6680) == 7 && bytes(m, profile, 0xF8) == bought,
              "Bonus menu did not preserve its valid unqualified track without changing the profile");
        ++shop_cases;
    }
    for (unsigned level = 0; level < 4; ++level) {
        write_u32(m, profile + 0x3C, level);
        write_u32(m, profile + 0x40 + level * 4, 0x11111111);
        join();
        check(word(m, profile + 0x3C) == level + 1 && requested_mode == 0x37 && promotions == 1,
              "Ordinary qualified shop promotion changed");
        write_u32(m, profile + 0x3C, level);
        write_u32(m, profile + 0x40 + level * 4, 0x11111110);
        join();
        check(word(m, profile + 0x3C) == level && requested_mode == 0x2F && promotions == 0,
              "Incomplete ordinary qualification promoted");
        shop_cases += 2;
    }
    for (unsigned track = 0; track < 8; ++track) {
        write_u32(m, profile + 0x3C, 4);
        write_u32(m, profile + 0x50, 0x11111111u & ~(15u << (track * 4)));
        join();
        check(word(m, profile + 0x3C) == 4 && requested_mode == 0x2F && promotions == 0,
              "Incomplete final qualification changed level or granted a reward");
        ++shop_cases;
    }
    for (unsigned track = 0; track < 8; ++track) {
        write_u32(m, profile+0x50, 0x11111111u & ~(15u << (track*4)));
        check(rr64_campaign_ending_exit(m, 1) == 1, "Incomplete campaign was treated as completed");
        check(!rr64_campaign_finish_menu(m), "Incomplete ending armed a Save return");
    }
    // Compare restored rewards to the actual native unlock helper for every
    // original gang category and slot. Saved bytes remain identical.
    unsigned saves = 0;
    std::puts("Checking native accepted saves and rewards"); std::fflush(stdout);
    for (unsigned slot = 0; slot < 6; ++slot) for (unsigned gang = 0; gang < 5; ++gang) {
        const unsigned record = cache + slot*0xF8;
        write_u32(m, record, 4); write_u32(m, record+0x3C, 4);
        write_u32(m, record+0x20, gang); write_u32(m, record+0x50, 0x11111111);
        checksum(m, record);
        auto expected = memory;
        for (unsigned i=0;i<15;++i) { write_u16(m, unlocks+i*2, 0); write_u16(expected.data(), unlocks+i*2, 0); }
        ctx.r4=4; ctx.r5=1; func_8005F420(expected.data(), &ctx);
        ctx.r4=gang==1 || gang==2 ? 6 : gang>=3 ? 5 : 7;
        ctx.r5=0; func_8005F420(expected.data(), &ctx);
        const auto saved = bytes(m, record, 0xF8);
        ctx.r16=S32(record); ctx.r18=ctx.r17=word(m,record+4);
        test_validated_campaign_save(m,&ctx);
        check(bytes(m,unlocks,30)==bytes(expected.data(),unlocks,30), "Restored reward differs from native gang reward");
        check(bytes(m,record,0xF8)==saved,"Reward restoration changed saved profile");
        ++saves;
    }
    for (unsigned reason = 0; reason < 5; ++reason) {
        for (unsigned i=0;i<15;++i) write_u16(m,unlocks+i*2,0);
        write_u32(m,cache,4); write_u32(m,cache+0x3C,4);
        write_u32(m,cache+0x20,1); write_u32(m,cache+0x50,0x11111111);
        if(reason==0) write_u32(m,cache+0x3C,3);
        if(reason==1) write_u32(m,cache+0x50,0x01111111);
        if(reason==2) write_u32(m,cache,3);
        checksum(m,cache);
        if(reason==3) write_u32(m,cache+4,0);
        auto expected = memory;
        if(reason < 2) {
            ctx.r4 = reason == 0 ? 3 : 4;
            ctx.r5 = 1;
            func_8005F420(expected.data(), &ctx);
        }
        ctx.r16=S32(cache); ctx.r18=5; ctx.r17=reason==4 ? 6 : 5;
        test_validated_campaign_save(m,&ctx);
        check(bytes(m,unlocks,30)==bytes(expected.data(),unlocks,30),
              "Ordinary progress must unlock earned levels; incomplete/corrupt saves must not grant completion rewards");
    }
    // Drive the original solo left/right selector, not an imitation of its
    // count or pointer lookup. The same committed bike drives the race donor.
    rr64_thrash_options_begin(m);
    std::puts("Checking native solo Insanity selector"); std::fflush(stdout);
    rr64::character_preferences::remember(0, 9);
    write_u32(m,0x800D13C8,0x80600000);
    rr64::local_players::active.store(false);
    unsigned selections = 0;
    for (unsigned map = 0; map < 15; ++map) for (unsigned cop : {0u,512u}) {
        rr64::local_race_options::set_thrash_options((7u<<6)|cop|10u);
        write_u32(m,0x800A6690,map);
        write_u32(m,0x800A66B0,0);
        write_u16(m,0x8009EAE8,0);
        write_u32(m,0x8009F670,0);
        for (unsigned bike : {25u,26u,25u}) {
            write_u32(m,0x8009E238,bike==26 ? 0x40 : selections%3 ? 0x20 : 0);
            write_u32(m,0x8009ECA4,1);
            ctx.r16=1;ctx.r21=S32(0x800A0000);
            test_thrash_bike_selection(m,&ctx);
            check(word(m,0x8009F660)==bike && word(m,0x8009F5D8)==bike,
                  "Native solo selector failed to preview/commit both Insanity models");
            check(preview_bike==bike,"Preview constructor received a different Insanity model");
            check(word(m,0x8009F670)==9,"Remembered character changed during bike selection");
            write_u32(m,0x800D7594,1);
            rr64_local_options_thrash_race(m);
            write_u32(m,0x800D8588,bike);
            const unsigned donor=rr64_local_bike_profile(m,0x800D8570,0x800A3460);
            std::uint8_t model=0;read_u8(m,donor+10,model);
            check(model==bike && word(m,0x800D8588)==bike,"Race profile substituted the other Insanity model");
            check(word(m,0x800A6690)==map,"Bike choice changed track tier");
            ++selections;
        }
    }
    check(word(m,0x800A174C+25*4)+0x41 == 66 && word(m,0x800A174C+26*4)+0x41 == 67,
          "Insanity models share the same native preview resource");
    std::printf("Campaign/bikes: ending Save return, native safe replay track, %u native shop purchase/Join cases, %u saved rewards, corrupt/incomplete negatives, %u native Insanity selector/race profiles passed.\n",shop_cases,saves,selections);
}
