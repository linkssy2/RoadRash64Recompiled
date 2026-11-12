// Original sprite queue and RDP rectangle production, without a graphics device.
#include "rr64_mk64_item_hud.hpp"
#include "rr64_mk64_item_render.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_online_flow.hpp"
#include "rr64_course_roulette.hpp"
#include "recomp.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <semaphore>
#include <thread>
#include <vector>

extern "C" void func_8001EB90(unsigned char*,recomp_context*);
extern "C" void func_8001EF8C(unsigned char*,recomp_context*);
extern "C" void fixture_hud_weapon_single(unsigned char*,recomp_context*);
extern "C" void fixture_hud_weapon_split_a(unsigned char*,recomp_context*);
extern "C" void fixture_hud_weapon_split_b(unsigned char*,recomp_context*);
namespace item=rr64::mk64_items;
namespace engine=rr64::engine;
namespace {
std::vector<unsigned char> initial,memory;
unsigned checks=0;
bool assets=true,draw_ok=true;
item::HudDisplay caller_display{};
unsigned caller_view=0;
constexpr unsigned asset=0x80300000,stack=0x807f0000,gfx=0x80600000;
constexpr unsigned pool=0x800BCAD8,counts=0x800BC9D0,sprite=0xA3;
struct Rectangle {unsigned view;item::Item item;float x,y,width,height;};
std::vector<Rectangle> replacements;
void check(bool value,const char*reason){++checks;if(!value){std::fprintf(stderr,"HUD check %u: %s\n",checks,reason);std::exit(1);}}
void w(unsigned a,unsigned v){check(engine::write_u32(memory.data(),a,v),"valid word write");}
void h(unsigned a,unsigned v){check(engine::write_u16(memory.data(),a,std::uint16_t(v)),"valid half write");}
void f(unsigned a,float v){check(engine::write_float(memory.data(),a,v),"valid float write");}
unsigned u(unsigned a){unsigned v=0;check(engine::read_u32(memory.data(),a,v),"valid word read");return v;}
unsigned hu(unsigned a){std::uint16_t v=0;check(engine::read_u16(memory.data(),a,v),"valid half read");return v;}
recomp_context context(){recomp_context c{};c.r29=engine::guest_address(stack);return c;}
void reset(unsigned buffer=0,unsigned width=24,unsigned height=24){
    memory=initial;item::reset_hud_queue();replacements.clear();assets=draw_ok=true;
    w(0x8009CBA4,buffer);w(0x800A1830,1);h(counts,0);h(counts+2,0);
    w(0x800D6870,asset);w(asset,0x100);w(asset+8,0x3000);w(asset+0xC,asset+0x4000);w(asset+0x14,0);
    const unsigned descriptor=asset+0x100+sprite*24;
    w(descriptor,0);h(descriptor+8,height);h(descriptor+0xA,width);
    h(descriptor+0xC,width);h(descriptor+0xE,4);h(descriptor+0x10,4);h(descriptor+0x12,0);
    w(asset+0x3000,0);w(0x800AC650,gfx);w(0x800B0808,640);w(0x800B080C,480);w(0x800C0430,0);
}
unsigned produce(unsigned view,float x,float y,float sx,float sy,item::HudDisplay display,
                 bool arm=true,unsigned flags=0){
    const unsigned buffer=u(0x8009CBA4),record=pool+(buffer*96+hu(counts+buffer*2))*76;
    auto c=context();c.f_odd=&c.f0.u32h;c.r4=engine::guest_address(asset);c.r5=sprite;
    c.r6=std::bit_cast<unsigned>(x);c.r7=std::bit_cast<unsigned>(y);
    for(unsigned offset=0x10;offset<=0x44;offset+=4)w(stack+offset,0);
    f(stack+0x10,1);f(stack+0x1C,sx);f(stack+0x20,sy);
    for(unsigned offset=0x24;offset<=0x3C;offset+=4)f(stack+offset,1);
    w(stack+0x40,flags);w(stack+0x44,0x400);
    if(arm)item::arm_hud_sprite(memory.data(),view,sprite,display);
    func_8001EB90(memory.data(),&c);
    if(arm)item::commit_hud_sprite(memory.data());
    check(unsigned(c.r29)==stack,"native producer restores stack");return record;
}
unsigned produce_original(unsigned variant,unsigned view,float x,float y,float sx,float sy,
                          item::HudDisplay display){
    caller_view=view;caller_display=display;
    const unsigned buffer=u(0x8009CBA4),record=pool+(buffer*96+hu(counts+buffer*2))*76;
    auto c=context();c.f_odd=&c.f0.u32h;c.r3=engine::guest_address(0x80310000);
    w(0x80310000+0x5B0,0);c.r4=engine::guest_address(asset);
    c.r1=engine::guest_address(0x80000000);c.r7=std::bit_cast<unsigned>(y);
    c.r16=engine::guest_address(0x800D0000);c.r17=0;
    c.f0.fl=0;c.f16.fl=sy;c.f20.fl=1;c.f26.fl=y;c.f28.fl=x;c.f30.fl=sx;f(stack+0x7C,sy);
    const auto invoke=variant==0?fixture_hud_weapon_single:
        variant==1?fixture_hud_weapon_split_a:fixture_hud_weapon_split_b;
    invoke(memory.data(),&c);
    check(unsigned(c.r29)==stack,"original weapon caller and producer preserve stack");
    check(hu(record+0x12)==0 && hu(record+0x48)==0x400,
          "actual weapon call writes draw flags zero and placement 0x400 separately");
    return record;
}
struct Bounds {unsigned left=4095,top=4095,right=0,bottom=0,count=0;};
Bounds consume(){
    auto c=context();c.f_odd=&c.f0.u32h;
    func_8001EF8C(memory.data(),&c);
    check(unsigned(c.r29)==stack,"native consumer restores stack");
    Bounds bounds;
    for(unsigned p=gfx;p<u(0x800AC650);p+=8){
        unsigned a=u(p),b=u(p+4);
        if((a>>24)==0xE4){
            bounds.right=std::max(bounds.right,(a>>12)&4095);bounds.bottom=std::max(bounds.bottom,a&4095);
            bounds.left=std::min(bounds.left,(b>>12)&4095);bounds.top=std::min(bounds.top,b&4095);++bounds.count;
        }
    }
    return bounds;
}
void virtual_cycle_cases() {
    reset();
    item::reset_hud_focus();
    const auto before = memory;
    item::RiderState held;
    held.held = item::Item::GreenShell;
    held.charges = 1;
    held.revision = 41;
    const rr64::netplay::CourseWeaponRoll roll{};
    std::array<item::CycleBinding, 4> bindings{};
    for (unsigned profile = 0; profile < bindings.size(); ++profile) {
        bindings[profile] = {memory.data(), 10 + profile, 0x80310000 + profile * 0x1000,
                             8, held};
        item::filter_cycle_input(profile, 0, 0, false);
    }
    std::thread publisher([&] { item::publish_cycle_bindings(bindings); });
    publisher.join();
    const auto draw = [&](unsigned profile, unsigned weapon = 5) {
        const auto &b = bindings[profile];
        return item::hud_display_for_rider(b.mapping,b.canonical,b.rider,
                                          b.item,roll,200,weapon);
    };
    for (unsigned profile = 0; profile < bindings.size(); ++profile) {
        check(draw(profile).owns, "MK starts selected for each independently bound controller");
        const auto snapshot = held;
        check(item::filter_cycle_input(profile, 0x8008, 0x8008, true) == 0x8000,
              "leaving MK consumes only cycle and preserves simultaneous acceleration");
        check(!draw(profile).owns, "consumed cycle reveals current native weapon without advancement");
        for (unsigned poll = 0; poll < 4; ++poll)
            check(item::filter_cycle_input(profile, 0x8008, 0x8008, true) == 0x8000,
                  "entire physical cycle hold remains suppressed across input polls");
        check(item::filter_cycle_input(profile, 0x8008, 0, false) == 0,
              "blocked admitted input does not clear the physical hold latch");
        check(item::filter_cycle_input(profile, 0x8008, 0x8008, true) == 0x8000,
              "resuming while still held cannot create a delayed native cycle edge");
        check(item::filter_cycle_input(profile, 0, 0, true) == 0,
              "physical release rearms the virtual cycle");
        check(item::filter_cycle_input(profile, 8, 8, true) == 8,
              "next normal native cycle is admitted after release");
        const auto &b = bindings[profile];
        item::focus_native_weapon(b.mapping,b.canonical,b.rider,1,b.item,true);
        check(draw(profile,1).owns, "exact native wrap returns to held MK item");
        // The wrapping press was admitted and remains held; it is not a fresh
        // press that could immediately leave the virtual MK slot.
        check(item::filter_cycle_input(profile, 8, 8, true) == 8 && draw(profile,1).owns,
              "wrapping hold does not immediately leave the MK slot");
        item::filter_cycle_input(profile, 0, 0, true);
        check(item::filter_cycle_input(profile, 8, 8, true) == 0 && !draw(profile,1).owns,
              "MK to native one consumes the edge so first native weapon is not skipped");
        check(held == snapshot, "virtual selection never changes held item or revision");
        item::filter_cycle_input(profile, 0, 0, true);
    }
    // A reset/OFF or a rebind during a consumed press must not leak that press
    // into a newly admitted controller identity.
    ++bindings[0].item.revision;
    item::publish_cycle_bindings(bindings);
    check(item::filter_cycle_input(0,8,8,true) == 0, "new use revision restores MK selection");
    item::reset_hud_focus();
    check(item::filter_cycle_input(0,8,8,true) == 0,
          "race reset retains suppression until the physical press releases");
    bindings[0].rider += 0x10000;
    item::publish_cycle_bindings(bindings);
    check(item::filter_cycle_input(0,8,8,true) == 0,
          "new rider cannot inherit an in-flight consumed cycle edge");
    item::filter_cycle_input(0,0,0,true);
    check(item::filter_cycle_input(0,8,8,true) == 0 && !draw(0).owns,
          "rebound rider accepts a new physical virtual-cycle press");
    item::filter_cycle_input(0,0,0,true);
    bindings[0].item = {};
    item::publish_cycle_bindings(bindings);
    check(item::filter_cycle_input(0,8,8,true) == 8 && !draw(0).owns,
          "depleted MK inventory returns the cycle unchanged");
    item::filter_cycle_input(0,0,0,true);
    bindings = {};
    item::publish_cycle_bindings(bindings);
    check(item::filter_cycle_input(0,8,8,true) == 8,
          "missing or disabled native bindings preserve stock controls");
    item::filter_cycle_input(0,0,0,true);
    bindings[0] = {memory.data(),13,0x80310000,0x20,held};
    item::publish_cycle_bindings(bindings);
    check(item::filter_cycle_input(0,8,8,true) == 8,
          "published native mask is used instead of hardcoded C-Up");
    check(item::filter_cycle_input(0,0x28,0x28,true) == 8,
          "alternate native cycle mask consumes only its own button");
    item::filter_cycle_input(0,0,0,true);
    item::reset_hud_focus();
    check(memory == before, "virtual item cycle leaves all native gameplay memory untouched");
}
void cross_thread_focus_cases() {
    reset();
    item::reset_hud_focus();
    const auto before = memory;
    item::RiderState held;
    held.held = item::Item::Banana;
    held.charges = 1;
    held.revision = 7;
    const rr64::netplay::CourseWeaponRoll roll{};
    std::array<item::HudDisplay, item::racer_capacity> displays{};
    std::binary_semaphore frame_ready(0), frame_done(0);
    // Keep the HUD worker alive across notification and reset. The native game
    // uses distinct persistent OS threads for actor control and HUD creation.
    std::thread render_worker([&] {
        for (unsigned frame = 0; frame < 3; ++frame) {
            frame_ready.acquire();
            for (unsigned slot = 0; slot < item::racer_capacity; ++slot)
                displays[slot] = item::hud_display_for_rider(
                    memory.data(), slot, 0x80310000 + slot * 0x1000, held, roll,
                    200 + frame, 5);
            frame_done.release();
        }
    });
    const auto notify = [&](unsigned parity) {
        std::thread gameplay_worker([&] {
            for (unsigned slot = parity; slot < item::racer_capacity; slot += 2)
                item::focus_native_weapon(memory.data(), slot,
                    0x80310000 + slot * 0x1000, 5, held);
        });
        gameplay_worker.join();
        frame_ready.release();
        frame_done.acquire();
    };
    notify(0);
    for (unsigned slot = 0; slot < item::racer_capacity; ++slot)
        check(displays[slot].owns == bool(slot % 2),
              "cross-thread native switch reaches only its canonical HUD rider");
    notify(1);
    for (const auto &display : displays)
        check(!display.owns,
              "cross-thread focus persists while other riders switch weapons");
    std::thread reset_worker([] { item::reset_hud_focus(); });
    reset_worker.join();
    frame_ready.release();
    frame_done.acquire();
    for (const auto &display : displays)
        check(display.owns,
              "cross-thread race reset clears focus in the existing HUD worker");
    render_worker.join();
    check(memory == before, "cross-thread focus never writes native inventory or memory");
}
void focus_cases() {
    reset();
    const auto before = memory;
    constexpr unsigned rider = 0x80310000;
    item::RiderState held;
    held.held = item::Item::TripleGreenShell;
    held.charges = 3;
    held.revision = 7;
    rr64::netplay::CourseWeaponRoll roll{};
    const auto draw = [&](unsigned slot, unsigned clock = 200, bool enabled = true) {
        return item::hud_display_for_rider(memory.data(),slot,rider,held,roll,clock,5,enabled);
    };
    for (unsigned slot = 0; slot < item::racer_capacity; ++slot) {
        item::reset_hud_focus();
        check(draw(slot).owns, "held MK item initially owns this rider's weapon square");
        item::focus_native_weapon(memory.data(),slot,rider,5,held);
        check(!draw(slot).owns, "successful weapon switch releases icon and quantity ownership");
        check(draw((slot+1)%item::racer_capacity).owns, "weapon focus does not cross canonical riders");
        auto event = held;
        event.event_serial = 99; event.cue = item::Cue::Hit;
        check(!item::hud_display_for_rider(memory.data(),slot,rider,event,roll,201,5).owns,
              "unrelated hit/audio event cannot steal weapon focus");
        ++event.revision;
        check(item::hud_display_for_rider(memory.data(),slot,rider,event,roll,202,5).owns,
              "a new item use or grant revision returns MK focus even for the same item");
    }
    item::reset_hud_focus();
    item::focus_native_weapon(memory.data(),0,rider,5,held);
    check(!draw(0).owns, "native switch before first HUD frame retains its captured item revision");
    roll = {12,100,std::uint16_t(item::reward_id(held.held)),1};
    for (unsigned clock = 100; clock < 148; ++clock) {
        // Prime after any clock rollback, then make the actual switch.
        draw(0,clock); item::focus_native_weapon(memory.data(),0,rider,5,held);
        const auto expected = item::hud_display(roll,clock,0,held.held);
        const auto actual = draw(0,clock);
        check(actual.owns == expected.owns && actual.shown == expected.shown,
              "shared roulette spin has priority over both inventory focuses");
    }
    roll.phase = 2;
    for (unsigned clock = 148; clock < 178; ++clock)
        check(!draw(0,clock).owns, "switch dismisses all phases of the MK award blink");
    roll = {};
    check(!draw(0).owns, "native focus persists after roulette retirement");
    item::reset_hud_queue();
    check(!draw(0).owns, "per-frame sprite queue reset does not forget weapon focus");
    check(draw(0,199).owns, "clock rollback clears prior-race focus");
    item::focus_native_weapon(memory.data(),0,rider,5,held);
    check(!draw(0,200,false).owns && draw(0,201).owns, "OFF clears focus before re-enabling items");
    item::focus_native_weapon(memory.data(),0,rider,5,held);
    auto other_mapping = memory;
    check(item::hud_display_for_rider(other_mapping.data(),0,rider,held,roll,202,5).owns,
          "a different guest memory mapping cannot inherit weapon focus");
    item::focus_native_weapon(memory.data(),0,rider,5,held);
    check(item::hud_display_for_rider(memory.data(),0,rider+0x1000,held,roll,203,5).owns,
          "replacement rider identity cannot inherit weapon focus");
    item::focus_native_weapon(memory.data(),0,rider,5,held);
    auto changed = held; changed.held = item::Item::RedShell; changed.charges = 1;
    check(item::hud_display_for_rider(memory.data(),0,rider,changed,roll,204,5).owns,
          "held type change clears focus independently of revision");
    roll = {12,200,std::uint16_t(item::reward_id(held.held)),2};
    item::RiderState empty;
    check(!item::hud_display_for_rider(memory.data(),0,rider,empty,roll,248,5).owns,
          "depleted item never hides the native weapon during old award blink");
    item::focus_native_weapon(memory.data(),0,rider,5,held);
    item::reset_hud_focus();
    check(draw(0,248).owns, "explicit race reset clears all prior focus");
    roll = {};
    item::focus_native_weapon(memory.data(),0,rider,15,held);
    check(draw(0,249).owns, "invalid native weapon tag cannot establish focus");
    check(!item::hud_display_for_rider(nullptr,0,rider,held,roll,250,5).owns,
          "invalid mapping cannot acquire a replacement record");
    check(memory == before, "presentation focus never mutates native memory or inventory");
    item::reset_hud_focus();
}
}
namespace rr64::mk64_items {
bool render_asset_available() noexcept{return assets;}
bool draw_hud_rectangle(unsigned char*,unsigned view,Item shown,float x,float y,float width,float height){
    if(!draw_ok)return false;
    replacements.push_back({view,shown,x,y,width,height});return true;
}
}
extern "C" int rr64_course_items_hud_hide(unsigned){return 0;}
extern "C" unsigned rr64_course_items_hud_weapon(unsigned char*,unsigned,unsigned original){return original;}
extern "C" unsigned rr64_course_items_hud_sprite(unsigned char*m,unsigned original){
    item::arm_hud_sprite(m,caller_view,original,caller_display);return original;
}
extern "C" void rr64_course_items_hud_sprite_end(unsigned char*m){item::commit_hud_sprite(m);}
extern "C" void do_break(std::uint32_t pc){
    std::fprintf(stderr,"Unexpected native HUD break at %08x\n",pc);std::abort();
}
extern "C" void switch_error(const char*function,std::uint32_t pc,std::uint32_t table){
    std::fprintf(stderr,"Unexpected native HUD switch %s %08x %08x\n",function,pc,table);std::abort();
}
extern "C" void fixture_hud_native_stub(unsigned address,unsigned char*,recomp_context*ctx){
    if(address==0x8001677C){ctx->f0.fl=1;return;}
    check(address==0x8001CE80 || address==0x8001CEFC,"only viewport IO can be stubbed");
}
int main(int argc,char**argv){
    check(argc==2,"private supported ROM argument");
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<unsigned char> rom((std::istreambuf_iterator<char>(file)),{});
    check(rom.size()>0xD0C00 && rom[0]==0x80 && rom[1]==0x37,"supported big endian ROM");
    initial.resize(8*1024*1024);
    for(unsigned i=0x400;i<0xD0000;++i)initial[i^3]=rom[i+0xC00];
    virtual_cycle_cases();
    cross_thread_focus_cases();
    focus_cases();
    struct Layout {unsigned width,height;float x,y,sx,sy;};
    const Layout layouts[]={{24,24,100,150,1,1},{64,64,200,250,.5f,.5f},
        {32,20,100.31f,150.73f,1.13f,.83f},{31,23,100.31f,150.73f,1.13f,.83f},
        {24,24,300.8f,350.2f,1.5f,2},{64,64,200.1f,250.9f,1.13f,.83f}};
    for(unsigned variant=0;variant<3;++variant)for(unsigned buffer=0;buffer<2;++buffer)
    for(unsigned view=0;view<4;++view)for(const auto &l:layouts){
        reset(buffer,l.width,l.height);produce_original(variant,view,l.x,l.y,l.sx,l.sy,{});
        const auto native=consume();check(native.count>0,"actual original weapon call emits its native rectangle");
        reset(buffer,l.width,l.height);produce_original(variant,view,l.x,l.y,l.sx,l.sy,{true,item::Item::GreenShell});
        const auto replaced=consume();
        if(replaced.count!=0 || replacements.size()!=1)
            std::fprintf(stderr,"caller %u view %u buffer %u replacements %zu native %u record %08x sprite %u flags %u placement %u\n",
                variant,view,buffer,replacements.size(),replaced.count,u(pool),hu(pool+0x10),hu(pool+0x12),hu(pool+0x48));
        check(replaced.count==0 && replacements.size()==1,
              "actual original weapon call accepts the held MK replacement");
        const auto &r=replacements.front();
        check(std::lround(r.x*4)==native.left && std::lround(r.y*4)==native.top &&
              std::lround((r.x+r.width)*4)==native.right && std::lround((r.y+r.height)*4)==native.bottom,
              "actual native single/split weapon bounds equal MK replacement bounds");
    }
    for(unsigned buffer=0;buffer<2;++buffer)for(unsigned view=0;view<4;++view)for(const auto &l:layouts){
        reset(buffer,l.width,l.height);produce(view,l.x,l.y,l.sx,l.sy,{},false);auto native=consume();
        check(native.count>0,"native original weapon emits an RDP rectangle");
        reset(buffer,l.width,l.height);produce(view,l.x,l.y,l.sx,l.sy,{true,item::Item::GreenShell});auto replaced=consume();
        check(replaced.count==0 && replacements.size()==1,"queued MK item replaces one original weapon rectangle");
        const auto &r=replacements.front();
        const bool same=std::lround(r.x*4)==native.left && std::lround(r.y*4)==native.top &&
            std::lround((r.x+r.width)*4)==native.right && std::lround((r.y+r.height)*4)==native.bottom;
        if(!same)std::fprintf(stderr,"dimensions %ux%u scale %.3f/%.3f native %u,%u..%u,%u replacement %.3f,%.3f..%.3f,%.3f\n",
            l.width,l.height,l.sx,l.sy,native.left,native.top,native.right,native.bottom,r.x*4,r.y*4,(r.x+r.width)*4,(r.y+r.height)*4);
        check(same,"replacement uses the exact native weapon rectangle");
        check(r.view==view && r.item==item::Item::GreenShell,"native local view identity reaches one replacement");
    }
    // More than one local view can enqueue before the shared consumer runs.
    reset();
    for(unsigned view=0;view<4;++view)produce(view,100+view*120.f,100,1,1,{true,item::Item(view+1)});
    check(consume().count==0 && replacements.size()==4,"all four local views replace independently");
    for(unsigned view=0;view<4;++view)check(replacements[view].view==view,"queue does not reuse another local view");
    // Authentication checks do not rewrite another record or consume stale state.
    for(unsigned reason=0;reason<8;++reason){
        reset();const unsigned record=produce(0,100,150,1,1,{true,item::Item::RedShell});
        switch(reason){
        case 0:w(0x800A1830,2);break;
        case 1:w(0x8009CBA4,1);break;
        case 2:w(record+0x14,std::bit_cast<unsigned>(200.f));break;
        case 3:h(record+0x10,sprite+1);break;
        case 4:h(record+0x12,8);break;
        case 5:w(record,asset+16);break;
        case 6:item::reset_hud_queue();break;
        case 7:{auto alternate=memory;check(!rr64_mk64_item_hud_draw_record(alternate.data(),record),"changed mapping rejects record");continue;}
        }
        check(!rr64_mk64_item_hud_draw_record(memory.data(),record),"changed record/buffer/epoch or reset rejects stale replacement");
        check(replacements.empty(),"stale record draws no item icon");
    }
    reset();h(counts,96);produce(0,100,150,1,1,{true,item::Item::RedShell});
    check(hu(counts)==96,"full native allocation remains rejected");
    check(!rr64_mk64_item_hud_draw_record(memory.data(),pool+95*76),"failed allocation cannot replace previous record");
    reset();unsigned record=produce(0,100,150,1,1,{true,item::Item::RedShell});
    check(rr64_mk64_item_hud_draw_record(memory.data(),record),"authenticated record consumed");
    check(rr64_mk64_item_hud_draw_record(memory.data(),record) && replacements.size()==1,"duplicate consumer suppresses stock without duplicate item draw");
    reset();produce(0,100,150,1,1,{true,item::Item::None});
    check(consume().count==0 && replacements.empty(),"blink-off suppresses only original icon with no replacement");
    reset();assets=false;produce(0,100,150,1,1,{true,item::Item::RedShell});
    check(consume().count>0 && replacements.empty(),"missing item assets preserve original weapon");
    reset();draw_ok=false;produce(0,100,150,1,1,{true,item::Item::RedShell});
    check(consume().count>0 && replacements.empty(),"failed replacement falls back to original queued weapon");
    // Interleaved thirty-reward roulette stays deterministic for every winner.
    std::array<bool,32> rewards{},off_rewards{};
    for(unsigned slot=0;slot<14;++slot)for(unsigned winner=2;winner<=31;++winner){
        rr64::netplay::CourseWeaponRoll roll{};roll.generation=123;roll.start_clock=100;roll.weapon=winner;roll.phase=1;
        const auto before=roll;
        for(unsigned age=0;age<rr64::netplay::kCourseRouletteTicks;++age){
            const unsigned clock=100+age;
            const auto native=rr64::course_items::roulette_display(roll,clock,slot,5);
            const auto shown=item::hud_display(roll,clock,slot,item::Item::Star);
            const auto off=rr64::course_items::roulette_display(roll,clock,slot,5,0,false);
            check(off.reward>=2 && off.reward<=16 && off.weapon<=14,
                  "OFF roulette contains only safe native weapons and multipliers");
            off_rewards[off.reward]=true;
            check(!item::hud_display(roll,clock,slot,item::Item::Star,false).owns,
                  "OFF never reserves a native HUD record for an MK item");
            rewards[native.reward]=true;
            check(native.weapon<=14,"roulette never leaks item tags into native weapon index");
            check(shown.owns==item::is_reward(native.reward),"native and MK handlers agree on exclusive rolling ownership");
            if(shown.owns)check(shown.shown==item::reward_item(native.reward),"MK roulette uses the shared deterministic display reward");
            auto other=roll;other.weapon=2+(winner%30);
            const auto alternative=rr64::course_items::roulette_display(other,clock,slot,5);
            check(alternative.reward==native.reward,"rolling sequence does not reveal or depend on eventual winner");
        }
        check(roll==before,"HUD presentation leaves authoritative roulette state unchanged");
        roll.phase=2;
        const auto held=item::is_reward(winner)?item::reward_item(winner):item::Item::Star;
        for(unsigned age=48;age<78;++age){
            const auto native=rr64::course_items::roulette_display(roll,100+age,slot,
                rr64::netplay::is_course_weapon_reward(winner)?winner:5,rr64::netplay::course_reward_effect(winner));
            const auto shown=item::hud_display(roll,100+age,slot,held);
            check(!item::hud_display(roll,100+age,slot,held,false).owns,
                  "OFF suppresses MK winner blink ownership");
            check(shown.owns==item::is_reward(winner),"final winner keeps exclusive ownership during blink");
            if(shown.owns)check(shown.shown==(((age-48)/3)%2?item::Item::None:held),"MK blink uses actual held winner");
            else check(native.reward==winner && native.reward_visible,"native winner keeps original reward display");
        }
        check(item::hud_display(roll,178,slot,held).shown==held,"held MK inventory returns after completed transient");
        check(!item::hud_display(roll,178,slot,held,false).owns,"OFF suppresses retired held-item ownership");
        const unsigned physical=rr64::online_flow::mapped_slot(slot,slot,true);
        check(physical==0 && rr64::online_flow::mapped_slot(physical,slot,true)==slot,"every online local canonical slot maps through physical zero");
    }
    for(unsigned reward=2;reward<=31;++reward)check(rewards[reward],"all thirty real rewards appear in shared roulette across slots");
    for(unsigned reward=2;reward<=16;++reward)check(off_rewards[reward],"OFF still presents every native weapon and multiplier");
    std::printf("MK64 native queued HUD: %u checks passed\n",checks);
}
