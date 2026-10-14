// Actual original crash/contact functions with private saved native memory.
// No graphics device, input device, audio device or game process is opened.
#include "rr64_mk64_items.hpp"
#include "rr64_mk64_item_replay.hpp"
#include "rr64_mk64_item_kernel.hpp"
#include "rr64_mk64_item_dimensions.hpp"
#include "rr64_mk64_item_audio.hpp"
#include "rr64_mk64_item_render.hpp"
#include "rr64_course_items.hpp"
#include "rr64_course_impact.hpp"
#include "rr64_course_item_render.hpp"
#include "rr64_course_hazards.hpp"
#include "rr64_mk64_item_hud.hpp"
#include "rr64_course_roulette.hpp"
#include "rr64_online_flow.hpp"
#include "rr64_local_race_options.hpp"
#include "rr64_course_walls.hpp"
#include "rr64_experimental_course.hpp"
#include "rr64_highlights.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_popup_input.hpp"
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fixture {
using namespace rr64;
using namespace engine;
namespace item = mk64_items;
std::vector<unsigned char> original, memory;
netplay::Status status{};
experimental_course::RouteData route{};
std::shared_ptr<const course_walls::World> surface_mesh;
std::vector<course_items::ItemBoxDefinition> box_definitions;
unsigned native_rewards = 0;
bool items_enabled = true;
bool course_active = true, assets = true, audio = true, presenting = false;
unsigned checks = 0, native_calls = 0, crashes = 0, audio_calls = 0;
unsigned hud_draws = 0, world_draws = 0;
unsigned switch_cues = 0, switch_weapon = 0, quantity_text = 0, quantity_draws = 0;
std::array<std::uint16_t,4> physical_buttons{};
bool input_disabled=false, input_focused=true, shortcuts=true, single_player=true;
unsigned input_publications=0;
std::uint16_t published_buttons=0;
struct HudDraw {unsigned view;item::Item shown;float x,y,w,h;};
std::vector<HudDraw> hud_records;
item::Snapshot recorded{};
std::array<float, 3> recorded_bike{}, recorded_rider{}, recorded_origin{};
bool recorded_attached = true;
constexpr unsigned actor_base = 0x800D8570, bike_base = 0x80600000;
recomp_context context{};
unsigned char *m() { return memory.data(); }
void check(bool value, const char *text) {
    ++checks;
    if (!value) throw std::runtime_error(text);
}
void w(unsigned a, unsigned v) { write_u32(m(), a, v); }
void h(unsigned a, unsigned v) { write_u16(m(), a, std::uint16_t(v)); }
void f(unsigned a, float v) { write_float(m(), a, v); }
unsigned u(unsigned a) { return item::native::word(m(), a); }
unsigned hu(unsigned a) { return item::native::half(m(), a); }
float fl(unsigned a) { return item::native::scalar(m(), a); }
void vec(unsigned a, float x, float y, float z) { f(a,x); f(a+4,y); f(a+8,z); }
void timing(float delta) {
    // Native 0B224..0B25C derives these together from one accepted step.
    f(globals::physics_delta,delta);
    f(0x8009CBAC,delta*delta);
    f(0x8009CBB0,delta*fl(0x800005A0));
    f(0x8009CBB4,fl(0x8000059C)/delta);
}
void body(unsigned a, float mass, float x, float speed) {
    f(a,mass);
    for (unsigned p : {0xCu,0x10u,0x14u}) f(a+p,100);
    for (unsigned p : {0x18u,0x1Cu,0x20u}) f(a+p,.01f);
    for (unsigned p : {0x64u,0x8Cu}) vec(a+p,x,100,10);
    for (unsigned p : {0x70u,0x98u}) vec(a+p,speed,0,0);
    for (unsigned p : {0xC0u,0x118u}) {
        vec(a+p,1,0,0); vec(a+p+12,0,0,1); vec(a+p+24,0,1,0);
    }
    f(a+0x13C+12,1); f(a+0xE4+12,1);
    w(a+0x2C,1); f(a+0x54,.45f);
}
void pair(unsigned slot, float x, float speed = 10) {
    const unsigned actor=actor_base+slot*0x118, bike=bike_base+slot*0x4000,
                   rider=bike+0x2000, state=bike+0x3000;
    std::memset(m()+actor-kRdramBegin,0,0x118);
    std::memset(m()+bike-kRdramBegin,0,0x4000);
    h(actor+0x24,1); h(actor+0x26,0); w(actor+8,slot<4?slot:~0u);
    w(actor+0x18,0); w(actor+0x1C,slot);
    w(actor+0xE0,bike); w(actor+0xE4,rider); w(actor+0xE8,state);
    // Authored drivetrain inputs, consumed independently by native3A000.
    f(bike+0xC,16000); f(bike+0x4C,4.6f); w(bike+0x54,6);
    f(bike+0x70,.8f); f(bike+0x3C8,.31f);
    w(bike+4,actor); w(rider+4,actor); w(bike+0x800,rider); w(rider+0x584,bike);
    h(bike+0x7F8,1); h(rider+0x57C,1);
    body(bike+0x108,300,x,speed); body(rider+0x28,80,x,speed);
    f(bike+0x4F8,100); f(bike+0x4FC,100); f(bike+0x40,.4f); f(bike+0x48,.5f);
    f(rider+0x30C,100); f(rider+0x310,100); w(rider+0x20,1); w(rider+0x24,1);
    f(state+0x20,x); f(state+0xC,1000);
}
void prepare(unsigned count = 2) {
    course_items::reset_runtime();
    memory=original;
    status={}; course_active=assets=audio=true; presenting=false;
    route=experimental_course::RouteData{}; surface_mesh.reset();
    box_definitions.clear(); native_rewards=0; items_enabled=true;
    prediction::replay_active=false;
    for (unsigned slot=0;slot<14;++slot) h(actor_base+slot*0x118+0x24,0);
    for (unsigned slot=0;slot<count;++slot) pair(slot,100+slot*10);
    context={}; context.f_odd=&context.f0.u32h; context.r29=guest_address(0x807FF000);
    context.r4=0x1234; context.f24.fl=12.25f;
    w(globals::main_mode,0x1C); w(globals::pending_mode,0x1C);
    h(globals::gameplay_pause_state,0); timing(1.f/60);
    f(0x800D7670,0); w(globals::random_state,12345);
    w(0x800A656C,1); w(0x800A6578,1); w(0x800A4F24,0); w(0x800A657C,0);
    w(0x800B0808,320); w(0x800B080C,240); w(0x807FF0D8,1);
    crashes=native_calls=audio_calls=hud_draws=world_draws=0;
    rr64_mk64_items_step(m(),&context);
}
void tick(float elapsed, float delta = 1.f/60) {
    f(0x800D7670,elapsed); timing(delta);
    rr64_mk64_items_before_physics(m());
    rr64_mk64_items_step(m(),&context);
}
void authority(item::Snapshot s, unsigned tick_number = 1) {
    if (!status.active) {
        status.active=status.connected=status.authoritative=true;
        status.phase=netplay::Phase::Race; status.local_slot=0; status.authority_humans=1;
    }
    check(item::apply_state(s,1,tick_number),"accept valid authority state");
    rr64_mk64_items_before_physics(m());
}
} // namespace fixture

namespace rr64::netplay {
Status get_status() { return fixture::status; }
void set_local_input(std::uint16_t buttons,float,float) {
    ++fixture::input_publications; fixture::published_buttons=buttons;
}
bool get_player_input(std::uint8_t,std::uint16_t&,float&,float&) { return false; }
bool host_request_race_start() { throw std::runtime_error("unexpected setup input"); }
PhysicsRules get_physics_rules() {
    PhysicsRules r; const auto &s=fixture::status;
    r.active=s.active; r.connected=s.connected; r.authoritative=s.authoritative;
    r.is_host=s.is_host; r.phase=s.phase; r.local_slot=s.local_slot;
    r.replicated_riders=s.replicated_riders; r.authority_humans=s.authority_humans;
    return r;
}
}
// Device/window services for the complete production main input callbacks.
// The filter, native bindings, inventory state, and publication order are real.
namespace recompinput {
enum class GameInput {RR64_WEAPON_TRICK,RR64_MK64_USE_ITEM,RR64_EJECT,RR64_SPOKE_JAM};
bool game_input_disabled(){return fixture::input_disabled;}
bool game_window_focused(){return fixture::input_focused;}
void set_road_rumble(int,float){}
namespace profiles {
bool get_n64_input(int profile,std::uint16_t *buttons,float*x,float*y){
    if(profile<0||profile>=4)return false;
    if(buttons)*buttons=fixture::physical_buttons[profile];
    if(x)*x=0;if(y)*y=0;return true;
}
bool get_action_input(int,GameInput){return false;}
}
namespace players {
bool is_single_player_mode(){return fixture::single_player;}
bool get_player_is_assigned(int p){return p>=0&&p<4;}
}
}
namespace rr64::online_menu {
bool controls_online_players(){return fixture::status.active;}
bool host_controls_game_setup(){return false;}
}
namespace rr64 {std::atomic<double> view_width{1};}
void *window=nullptr;
std::atomic_bool rr64_dynamic_ultrawide{false};
void SDL_GetWindowSize(void*,int*,int*){throw std::runtime_error("unexpected window access");}
void apply_responsive_menu_navigation(int,std::uint16_t,float*,float*){}
bool rr64_are_gameplay_shortcuts_active(){return fixture::shortcuts;}
bool rr64_custom_cop_active(){return false;}
constexpr std::uint16_t rr64_cop_weapon_trick_button=0x20;
void rr64_request_rider_eject(unsigned){throw std::runtime_error("unexpected eject action");}
bool rr64_local_rider_has_fists_selected(unsigned){return false;}
bool rr64_is_rumble_enabled(){return false;}
bool rr64_is_road_rumble_allowed(){return false;}
bool rr64_online_host_pause_active(){return false;}
void rr64_log(const char*,...){}
#include "rr64_mk64_item_native_input_fixture.inc"
namespace rr64::experimental_course {
bool active() noexcept { return fixture::course_active; }
const RouteData *route_data() noexcept { return &fixture::route; }
float source_to_world_scale() noexcept { return .05f; }
}
namespace rr64::course_walls {
const World *surface_world() noexcept { return fixture::surface_mesh.get(); }
}
namespace rr64::course_items {
std::span<const ItemBoxDefinition> definitions() noexcept { return fixture::box_definitions; }
bool draw_boxes(unsigned char *, std::span<const ItemBoxDrawState>) { return true; }
}
namespace rr64::course_hazards {
netplay::CourseHazardState capture_state() noexcept { return {}; }
}
namespace rr64::local_race_options {
bool mk64_items_enabled() { return fixture::items_enabled; }
}
namespace rr64::mk64_items {
bool render_asset_available() noexcept { return fixture::assets; }
bool audio_available() noexcept { return fixture::audio; }
void reset_audio() noexcept {}
bool draw_world(unsigned char*,const Snapshot&) { ++fixture::world_draws; return true; }
bool draw_hud_rectangle(unsigned char*,unsigned view,Item shown,float x,float y,float w,float h) {
    ++fixture::hud_draws;fixture::hud_records.push_back({view,shown,x,y,w,h});return true;
}
}
namespace rr64::highlights {
const mk64_items::Snapshot *render_items() noexcept {
    return fixture::presenting ? &fixture::recorded : nullptr;
}
bool render_rider_anchors(unsigned,std::array<float,3>&bike,std::array<float,3>&rider,bool&attached,std::array<float,3>&origin) noexcept {
    bike=fixture::recorded_bike; rider=fixture::recorded_rider;
    attached=fixture::recorded_attached; origin=fixture::recorded_origin; return fixture::presenting;
}
}
extern "C" {
void func_80036B78(unsigned char*,recomp_context*);
void func_80037554(unsigned char*,recomp_context*);
void func_800616BC(unsigned char*,recomp_context*);
void func_80061224(unsigned char*,recomp_context*);
void func_8005F7A4(unsigned char*,recomp_context*);
void func_8005FA18(unsigned char*,recomp_context*);
void func_80060370(unsigned char*,recomp_context*);
void func_8004E754(unsigned char*,recomp_context*);
void item_native_rpm(unsigned char*,recomp_context*);
void item_native_render_anchors(unsigned char*,recomp_context*);
void item_native_render_matrix(unsigned char*,recomp_context*);
void func_80015A90(unsigned char*,recomp_context*);
void rr64_online_presentation_matrix(unsigned char*,unsigned,unsigned,unsigned,unsigned) {}
void rr64_lod_scale_root_matrix(unsigned char*,unsigned,unsigned,unsigned) {}
void rr64_world_scale_root_matrix(unsigned char*,unsigned,unsigned,unsigned) {}
void item_native_bike_contact(unsigned char*,recomp_context*);
void item_native_cycle_mounted(unsigned char*,recomp_context*);
void item_native_cycle_rider(unsigned char*,recomp_context*);
void item_native_quantity(unsigned char*,recomp_context*);
void rr64_online_audio_weapon_source(unsigned char*,void*,unsigned) {}
void sprintf_recomp(unsigned char*,recomp_context *c) { fixture::quantity_text=unsigned(c->r6); }
void rr64_engine_capture_dynamics_boundary(unsigned char*,unsigned,unsigned,unsigned) {}
void rr64_prediction_verify_random(unsigned char*,unsigned) {}
void rr64_highlights_crash(unsigned char*,unsigned) { ++fixture::crashes; }
int rr64_highlights_presenting() { return fixture::presenting; }
int rr64_offline_bike_protected(unsigned char*,unsigned) { return 0; }
int rr64_offline_rider_protected(unsigned char*,unsigned) { return 0; }
unsigned rr64_offline_combat_multiplier(unsigned char*,unsigned) { return 0; }
void rr64_quadrant_hud_sprite_begin(unsigned char*,unsigned) {}
void rr64_quadrant_hud_sprite_end(unsigned char*) {}
int rr64_valid_combat_statistics(unsigned a) { return rr64::engine::valid_combat_statistics(a); }
int rr64_online_hit(unsigned char*,void*,unsigned) { return 0; }
void rr64_combat_impact_rumble(unsigned char*,unsigned,unsigned,unsigned) {}
void rr64_mk64_item_audio_step(unsigned char*) { ++fixture::audio_calls; }
void func_80056DA8(unsigned char*,recomp_context*) { ++fixture::native_calls; }
void func_800565BC(unsigned char*,recomp_context*) { ++fixture::native_calls; }
void func_80037920(unsigned char*,recomp_context *c) { ++fixture::native_rewards; c->r2=1; }
void func_80037960(unsigned char*,recomp_context *c) { ++fixture::native_rewards; c->r2=1; }
void func_8001EF8C(unsigned char*,recomp_context*);
void fixture_hud_weapon_single(unsigned char*,recomp_context*);
void fixture_hud_weapon_split_a(unsigned char*,recomp_context*);
void fixture_hud_weapon_split_b(unsigned char*,recomp_context*);
void fixture_hud_native_stub(unsigned address,unsigned char*,recomp_context*c) {
    if(address==0x8001677C){c->f0.fl=1;return;}
    fixture::check(address==0x8001CE80 || address==0x8001CEFC,"joint HUD stubs only viewport matrix setup");
}
void _nsqrtf(unsigned char*,recomp_context*c) { c->f0.fl=std::sqrt(c->f12.fl); }
void fixture_native_stub(unsigned id,recomp_context*c) {
    ++fixture::native_calls;
    if (id==0x80056000) { ++fixture::switch_cues; fixture::switch_weapon=unsigned(c->r4); }
    if (id==0x800795AC) ++fixture::quantity_draws;
    if (id==0x8001A5D8 || id==0x80063B14) c->r2=0;
}
void do_break(std::uint32_t pc) { std::fprintf(stderr,"Native break %08x\n",pc); std::abort(); }
void switch_error(const char *function,std::uint32_t pc,std::uint32_t table) {
    std::fprintf(stderr,"Unexpected native switch %s %08x %08x\n",function,pc,table);std::abort();
}
}

namespace fixture {
void render_held_hud(unsigned views=1, unsigned native_focus_mask=0) {
    const auto before=item::capture_state();
    const auto boxes=course_items::capture_state();
    constexpr unsigned asset=0x80300000,gfx=0x80500000,pool=0x800BCAD8;
    w(0x800D6870,asset);w(asset,0x100);w(asset+8,0x3000);w(asset+0xC,asset+0x4000);w(asset+0x14,0);
    for(unsigned icon=0xA3;icon<=0xC0;++icon){
        const unsigned d=asset+0x100+icon*24;w(d,0);h(d+8,24);h(d+10,24);
        h(d+12,24);h(d+14,4);h(d+16,4);h(d+18,0);
    }
    w(asset+0x3000,0);w(0x800AC650,gfx);w(0x800C0430,0);
    w(0x8009CBA4,0);w(0x800A1830,u(0x800A1830)+1);h(0x800BC9D0,0);h(0x800BC9D2,0);
    w(0x800A6578,views);w(0x800A4F24,views>2?2:views-1);
    w(0x800B0808,640);w(0x800B080C,480);
    hud_records.clear();hud_draws=0;
    unsigned expected=0;
    for(unsigned view=0;view<views;++view){
        w(0x800A657C+view*4,view);
        const unsigned canonical=status.active?online_flow::mapped_slot(view,status.local_slot,status.replicated_riders):view;
        const auto display=item::hud_display(boxes.roulette[canonical],boxes.clock,canonical,before.riders[canonical].held,items_enabled);
        if(display.owns && display.shown!=item::Item::None && !(native_focus_mask&(1u<<view)))++expected;
    }
    rr64_course_items_hud_begin();
    if(views<=2){
        for(unsigned view=0;view<views;++view){
            const auto p=item::native::pair(m(),view);
            auto c=context;c.f_odd=&c.f0.u32h;
            c.r1=guest_address(0x80000000);c.r3=guest_address(p.rider);c.r4=guest_address(asset);
            c.r7=std::bit_cast<unsigned>(150.f+view*200);c.r16=guest_address(0x800D0000);
            c.r17=rr64_course_items_hud_weapon(m(),p.rider,u(p.rider+0x5B0));
            c.f0.fl=0;c.f16.fl=1;c.f20.fl=1;c.f26.fl=150.f+view*200;c.f28.fl=100;c.f30.fl=1;
            f(unsigned(c.r29)+0x7C,1);
            (views==1?fixture_hud_weapon_single:view==0?fixture_hud_weapon_split_a:fixture_hud_weapon_split_b)(m(),&c);
        }
    }else rr64_course_items_hud_quadrants(m(),&context);
    rr64_course_items_hud_end();
    const unsigned queued=hu(0x800BC9D0);
    check(queued==views,"joint held inventory produces one original weapon record per local view");
    for(unsigned index=0;index<queued;++index)
        check(hu(pool+index*76+0x12)==0 && hu(pool+index*76+0x48)==0x400,
              "joint native caller and quadrant producer retain original weapon record contract");
    auto c=context;c.f_odd=&c.f0.u32h;func_8001EF8C(m(),&c);
    check(hud_records.size()==expected,"real awarded inventory reaches the actual queued HUD consumer");
    if(!items_enabled){
        bool emitted=false;for(unsigned p=gfx;p<u(0x800AC650);p+=8)emitted|=(u(p)>>24)==0xE4;
        check(emitted,"OFF retains the original native weapon rectangle");
    }
    for(const auto &draw:hud_records){
        check(!(native_focus_mask&(1u<<draw.view)),"native-focused view does not retain an MK override");
        const unsigned canonical=status.active?online_flow::mapped_slot(draw.view,status.local_slot,status.replicated_riders):draw.view;
        const auto display=item::hud_display(boxes.roulette[canonical],boxes.clock,canonical,before.riders[canonical].held,items_enabled);
        check(draw.shown==display.shown && draw.w>0 && draw.h>0,"joint HUD preserves canonical item and valid native icon rectangle");
    }
    check(item::capture_state()==before && course_items::capture_state()==boxes,
          "joint HUD draw does not change real inventory or roulette state");
}

void weapon_switch_focus() {
    auto inventory = [](unsigned slot) {
        const auto p=item::native::pair(m(),slot);
        w(p.rider+0x5B0,2); h(p.bike+0x838+2,0xffff);
        h(p.bike+0x838+4,3); h(p.bike+0x838+10,7); w(0x800A54D8,8);
    };
    auto cycle = [](unsigned slot, bool mounted, bool edge=true) {
        const auto p=item::native::pair(m(),slot);
        auto c=context; c.f_odd=&c.f0.u32h;
        c.r2=guest_address(0x800A0000); c.r18=guest_address(p.bike);
        c.r16=guest_address(p.rider); c.r19=edge?8:0; c.r22=8; c.r30=8;
        switch_cues=switch_weapon=0;
        (mounted?item_native_cycle_mounted:item_native_cycle_rider)(m(),&c);
        check(u(p.rider+0x5B0)==5 && switch_cues==unsigned(edge),
              "actual native cycle selects weapon5; missing input edge leaves it unchanged");
        if(edge) check(switch_weapon==5,"native sound delay slot receives completed selection");
        check(hu(p.bike+0x838+4)==3 && hu(p.bike+0x838+10)==7,
              "native cycling preserves both inventory quantities");
    };
    auto quantity = [](unsigned slot, unsigned expected) {
        w(0x800A657C,slot);
        quantity_text=quantity_draws=0;
        rr64_course_items_hud_begin();
        auto c=context; c.f_odd=&c.f0.u32h; c.r30=guest_address(actor_base);
        item_native_quantity(m(),&c);
        rr64_course_items_hud_end();
        check(quantity_text==expected && quantity_draws==unsigned(expected>1),
              "original quantity lookup and text-submission branch preserve native count");
    };
    for(bool mounted:{true,false}) for(bool held:{false,true}) {
        prepare(1); inventory(0);
        if(held)check(item::grant_item(0,item::Item::Banana),"grant held item before native switch");
        const auto before=item::capture_state();
        cycle(0,mounted); // No earlier HUD frame: the callback must establish focus itself.
        check(item::capture_state()==before,"native switch does not consume or discard MK inventory");
        render_held_hud(1,held?1:0);
        check(hu(0x800BCAD8+0x10)==0xA8 && hud_records.empty(),
              "real queued HUD renders selected weapon5 after either native switch path");
        bool rectangle=false;
        for(unsigned a=0x80500000;a<u(0x800AC650);a+=8)rectangle|=(u(a)>>24)==0xE4;
        check(rectangle,"actual native HUD consumer emits selected weapon rectangle");
        quantity(0,7); cycle(0,mounted,false);
    }
    // One focused player must not suppress another view's held item.
    for(unsigned views:{2u,4u}) {
        prepare(views); w(0x800A656C,views);
        for(unsigned view=0;view<views;++view) {
            inventory(view); check(item::grant_item(view,item::Item::Banana),"local held inventory");
        }
        cycle(0,true); render_held_hud(views,1);
        check(hud_records.size()==views-1,"native focus stays within its local viewport");
    }
    prepare(1); inventory(0);
    auto remote=item::capture_state();
    check(item::grant(remote,13,item::Item::Banana),"canonical guest held inventory");
    status.active=status.connected=status.authoritative=true; status.is_host=false;
    status.phase=netplay::Phase::Race; status.local_slot=13; status.replicated_riders=14;
    status.authority_humans=2;
    check(item::apply_state(remote,1,1),"guest receives held inventory at canonical13");
    cycle(0,false); render_held_hud(1,1); quantity(0,7);
    check(item::capture_state()==remote,"guest view0 switch leaves canonical13 inventory intact");

    prepare(1); inventory(0); check(item::grant_item(0,item::Item::Banana),"private guard item");
    {
        prediction::ReplayScope scope({true,true,true,false,false,0,1});
        cycle(0,true);
    }
    render_held_hud(); quantity(0,0);
    check(hud_records.size()==1,"private native selection cannot change live HUD focus");

    prepare(1); inventory(0); check(item::grant_item(0,item::Item::Banana),"reset collision item");
    const auto original_item=item::capture_state().riders[0];
    cycle(0,true); render_held_hud(1,1);
    items_enabled=false; rr64_mk64_items_before_physics(m());
    check(!item::capture_state().enabled,"actual OFF path retires native item runtime");
    inventory(0); cycle(0,false); render_held_hud(); quantity(0,7);
    items_enabled=true; rr64_mk64_items_step(m(),&context);
    check(item::grant_item(0,item::Item::Banana),"re-enabled runtime accepts same item");
    check(item::capture_state().riders[0].revision==original_item.revision,
          "reset regression really reuses the previous held revision");
    render_held_hud(); quantity(0,0);
    check(hud_records.size()==1,"actual OFF reset clears focus before same-revision re-enable");
}

void weapon_switch_worker_ownership() {
    // Native OSThread3 updates selection; OSThread19 builds the HUD. Keep both
    // workers alive across a third-thread reset so thread-local storage cannot
    // accidentally make this appear to pass through a fresh worker lifetime.
    for (bool mounted : {true, false}) for (bool guest : {false, true}) {
        prepare(1);
        const auto p = item::native::pair(m(), 0);
        w(p.rider + 0x5B0, 2);
        h(p.bike + 0x838 + 2, 0xffff);
        h(p.bike + 0x838 + 4, 3);
        h(p.bike + 0x838 + 10, 7);
        w(0x800A54D8, 8);
        const unsigned canonical = guest ? 13 : 0;
        if (guest) {
            auto remote = item::capture_state();
            check(item::grant(remote, canonical, item::Item::Banana), "worker guest inventory");
            status.active = status.connected = status.authoritative = true;
            status.is_host = false;
            status.phase = netplay::Phase::Race;
            status.local_slot = canonical;
            status.replicated_riders = 14;
            status.authority_humans = 2;
            check(item::apply_state(remote, 1, 1), "worker guest remaps canonical13 to native0");
        } else {
            check(item::grant_item(canonical, item::Item::Banana), "worker local inventory");
        }
        const auto before = item::capture_state();
        std::latch switched(1), first_drawn(1), reset_done(1);
        std::exception_ptr update_error, hud_error, reset_error;
        auto read_quantity = [&](unsigned expected) {
            quantity_text = quantity_draws = 0;
            rr64_course_items_hud_begin();
            auto c = context;
            c.f_odd = &c.f0.u32h;
            c.r30 = guest_address(actor_base);
            item_native_quantity(m(), &c);
            rr64_course_items_hud_end();
            check(quantity_text == expected && quantity_draws == unsigned(expected > 1),
                  "separate HUD worker receives native quantity through original consumer");
        };
        std::thread update_worker([&] {
            try {
                auto c = context;
                c.f_odd = &c.f0.u32h;
                c.r2 = guest_address(0x800A0000);
                c.r18 = guest_address(p.bike);
                c.r16 = guest_address(p.rider);
                c.r19 = c.r22 = c.r30 = 8;
                switch_cues = switch_weapon = 0;
                (mounted ? item_native_cycle_mounted : item_native_cycle_rider)(m(), &c);
                check(u(p.rider + 0x5B0) == 5 && switch_cues == 1 && switch_weapon == 5,
                      "update worker completes original native cycle and sound argument");
                check(item::capture_state() == before,
                      "update worker changes presentation without consuming MK inventory");
            } catch (...) {
                update_error = std::current_exception();
            }
            switched.count_down();
            reset_done.wait();
        });
        std::thread hud_worker([&] {
            switched.wait();
            try {
                if (!update_error) {
                    render_held_hud(1, 1);
                    check(hu(0x800BCAD8 + 0x10) == 0xA8 && hud_records.empty(),
                          "distinct HUD worker renders selected native weapon rather than held MK item");
                    bool rectangle = false;
                    for (unsigned a = 0x80500000; a < u(0x800AC650); a += 8)
                        rectangle |= (u(a) >> 24) == 0xE4;
                    check(rectangle, "distinct HUD worker submits original native rectangle");
                    read_quantity(7);
                }
            } catch (...) {
                hud_error = std::current_exception();
            }
            first_drawn.count_down();
            reset_done.wait();
            try {
                if (!update_error && !hud_error && !reset_error) {
                    render_held_hud();
                    read_quantity(0);
                    check(hud_records.size() == 1 && hud_records[0].shown == item::Item::Banana,
                          "same HUD worker observes third-thread reset despite reused item revision");
                }
            } catch (...) {
                hud_error = std::current_exception();
            }
        });
        first_drawn.wait();
        try {
            if (!update_error && !hud_error) {
                check(update_worker.get_id() != hud_worker.get_id() &&
                          update_worker.get_id() != std::this_thread::get_id() &&
                          hud_worker.get_id() != std::this_thread::get_id(),
                      "cycle, HUD and reset really occupy three distinct live host threads");
                items_enabled = false;
                rr64_mk64_items_before_physics(m());
                check(!item::capture_state().enabled, "third-thread OFF retires actual runtime");
                items_enabled = true;
                if (guest) {
                    check(item::apply_state(before, 1, 1), "guest receives same-revision inventory after reset");
                } else {
                    rr64_mk64_items_step(m(), &context);
                    check(item::grant_item(canonical, item::Item::Banana), "local same-item grant after reset");
                }
                check(item::capture_state().riders[canonical].revision == before.riders[canonical].revision,
                      "cross-thread reset case deliberately reuses the held revision");
            }
        } catch (...) {
            reset_error = std::current_exception();
        }
        reset_done.count_down();
        update_worker.join();
        hud_worker.join();
        if (update_error) std::rethrow_exception(update_error);
        if (hud_error) std::rethrow_exception(hud_error);
        if (reset_error) std::rethrow_exception(reset_error);
    }
}

void native_cycle_return_input() {
    struct Frame {unsigned raw, selected; bool mk, admitted, cue;};
    const std::vector<Frame> multiple = {
        {0,2,true,false,false}, {8,2,false,false,false}, {8,2,false,false,false},
        {0,2,false,false,false}, {8,5,false,true,true}, {8,5,false,true,false},
        {0,5,false,false,false}, {8,1,true,true,true}, {8,1,true,true,false},
        {0,1,true,false,false}, {8,1,false,false,false}, {8,1,false,false,false},
        {0,1,false,false,false}, {8,2,false,true,true}};
    const std::vector<Frame> single = {
        {0,1,true,false,false}, {8,1,false,false,false}, {8,1,false,false,false},
        {0,1,false,false,false}, {8,1,true,true,true}, {8,1,true,true,false},
        {0,1,true,false,false}, {8,1,false,false,false}, {8,1,false,false,false}};
    struct Layout {unsigned views,profile;bool guest;};
    for(bool mounted:{true,false}) for(bool only_fists:{false,true})
    for(auto layout:{Layout{1,0,false},Layout{4,3,false},Layout{1,0,true}}) {
        prepare(layout.views);single_player=layout.views==1;
        physical_buttons={}; input_disabled=false;input_focused=shortcuts=true;
        popup_input::wait_for_release=false;
        w(0x800A656C,layout.views);w(0x800A6578,layout.views);w(0x800A54D8,8);
        for(unsigned slot=0;slot<layout.views;++slot) {
            w(0x800A657C+slot*4,slot);
            const auto p=item::native::pair(m(),slot);
            for(unsigned id=0;id<16;++id)h(p.bike+0x838+2*id,0);
            h(p.bike+0x83A,0xffff);
            if(!only_fists){h(p.bike+0x83C,3);h(p.bike+0x842,7);}
            w(p.rider+0x5B0,only_fists?1:2);
        }
        const unsigned canonical=layout.guest?13:layout.profile;
        if(layout.guest) {
            auto s=item::capture_state();check(item::grant(s,13,item::Item::Banana),"cycle guest13 grant");
            status.active=status.connected=status.authoritative=true;status.is_host=false;
            status.phase=netplay::Phase::Race;status.local_slot=13;status.replicated_riders=true;
            status.authority_humans=2;check(item::apply_state(s,1,1),"cycle guest13 authority");
        } else for(unsigned slot=0;slot<layout.views;++slot)
            check(item::grant_item(slot,item::Item::Banana),"cycle local held item");
        const auto before=item::capture_state();
        const auto p=item::native::pair(m(),layout.profile);
        const auto &frames=only_fists?single:multiple;
        std::array<unsigned,16> inventory{};
        for(unsigned id=0;id<inventory.size();++id)inventory[id]=hu(p.bike+0x838+id*2);
        std::barrier phase(3);
        std::uint16_t admitted=0,previous=0;
        std::exception_ptr input_error,update_error,hud_error;
        std::thread input_worker([&] {
            for(const auto &frame:frames) {
                phase.arrive_and_wait();
                try {
                    physical_buttons[layout.profile]=std::uint16_t(frame.raw|0x2000);
                    input_publications=0;published_buttons=0;float x=0,y=0;
                    check(get_input_with_trace(int(layout.profile),&admitted,&x,&y),"real main input callback connected");
                    if((admitted&8)!=(frame.admitted?8:0) || !(admitted&0x2000))
                        std::fprintf(stderr,"cycle case mounted=%u fists=%u views=%u profile=%u guest=%u frame=%zu raw=%u admitted=%u expected=%u active=%u\n",
                            mounted,only_fists,layout.views,layout.profile,layout.guest,
                            std::size_t(&frame-frames.data()),frame.raw,admitted,frame.admitted,item::input_active());
                    check((admitted&8)==(frame.admitted?8:0) && (admitted&0x2000),
                          "real main filters the whole physical cycle hold while preserving throttle");
                    check(input_publications==unsigned(layout.profile==0),"real main retains local profile publication ownership");
                    if(layout.profile==0)check(published_buttons==admitted,
                        "netplay input publication receives filtered buttons, never the consumed physical press");
                } catch(...){input_error=std::current_exception();}
                phase.arrive_and_wait();phase.arrive_and_wait();phase.arrive_and_wait();
            }
        });
        std::thread update_worker([&] {
            // Production publication happens on the native worker, not the
            // input reader or HUD fixture. No raw guest reads occur in input.
            rr64_mk64_items_before_physics(m());
            rr64_course_items_cycle_publish(m());
            for(const auto &frame:frames) {
                phase.arrive_and_wait();phase.arrive_and_wait();
                try {
                    if(!input_error) {
                        auto c=context;c.f_odd=&c.f0.u32h;
                        c.r2=guest_address(0x800A0000);c.r18=guest_address(p.bike);
                        c.r16=guest_address(p.rider);c.r19=admitted;
                        c.r22=c.r30=std::uint16_t(admitted&~previous);previous=admitted;
                        switch_cues=switch_weapon=0;
                        (mounted?item_native_cycle_mounted:item_native_cycle_rider)(m(),&c);
                        check(u(p.rider+0x5B0)==frame.selected,"real native cycle does not skip current weapon or fists after MK return");
                        check(switch_cues==unsigned(frame.cue),"only admitted fresh native cycle emits its original sound cue");
                        if(frame.cue)check(switch_weapon==frame.selected,"native sound argument matches actual wrapped/advanced selection");
                        check(item::capture_state()==before,"cycling never mutates canonical MK inventory");
                        for(unsigned id=0;id<inventory.size();++id)
                            check(hu(p.bike+0x838+id*2)==inventory[id],"cycling preserves every original native inventory slot");
                    }
                }catch(...){update_error=std::current_exception();}
                phase.arrive_and_wait();phase.arrive_and_wait();
            }
        });
        for(const auto &frame:frames) {
            phase.arrive_and_wait();phase.arrive_and_wait();phase.arrive_and_wait();
            try {
                if(!input_error&&!update_error) {
                    render_held_hud(layout.views,frame.mk?0:1u<<layout.profile);
                    bool found=false;
                    for(const auto &draw:hud_records)if(draw.view==layout.profile)found=true;
                    check(found==frame.mk,"separate HUD consumer displays MK again only at the exact native wrap");
                    check(item::capture_state().riders[canonical]==before.riders[canonical],"canonical13/local held ownership survives full round-trip");
                }
            }catch(...){hud_error=std::current_exception();}
            phase.arrive_and_wait();
        }
        input_worker.join();update_worker.join();
        if(input_error)std::rethrow_exception(input_error);
        if(update_error)std::rethrow_exception(update_error);
        if(hud_error)std::rethrow_exception(hud_error);
    }
    // The actual course-step exit must refresh an exhausted held binding
    // before another HUD frame. A fresh press must now reach native cycling.
    prepare(1);single_player=true;physical_buttons={};w(0x800A54D8,8);
    const auto p=item::native::pair(m(),0);
    w(p.rider+0x5B0,2);h(p.bike+0x83A,0xffff);h(p.bike+0x83C,3);h(p.bike+0x842,7);
    check(item::grant_item(0,item::Item::Banana),"depletion cycle grant");
    rr64_mk64_items_before_physics(m());
    rr64_course_items_cycle_publish(m());
    std::uint16_t buttons=0;float x=0,y=0;
    check(get_input_with_trace(0,&buttons,&x,&y),"release stale raw hold before depletion");
    item::request_use(0);rr64_course_items_step(m(),&context);
    check(item::capture_state().riders[0].held==item::Item::None,"real use drains final banana before next poll");
    physical_buttons[0]=8;check(get_input_with_trace(0,&buttons,&x,&y),"fresh input after depletion");
    check((buttons&8) && published_buttons==buttons,"empty inventory no longer consumes cycle even without a HUD draw");
}

void box_reward_pipeline() {
    std::array<bool,16> awarded{};
    unsigned regular = 0;
    for (unsigned seed = 1; seed <= 64; ++seed) {
        prepare(1);
        course_items::reset_runtime();
        box_definitions.push_back({0,{100,100,10},.275f,2});
        w(globals::random_state,seed);
        f(0x800D7670,.1f);
        rr64_course_items_step(m(),&context);
        check(item::capture_state().enabled && item::can_grant(0),
              "actual course hook leaves MK runtime initialized before first reward selection");
        const auto pickup = course_items::capture_state();
        check(pickup.count == 1 && pickup.roulette[0].phase == 1 && pickup.cooldown[0] > 0,
              "actual rider sphere pickup starts a bounded shared roulette");
        const auto reward = pickup.roulette[0].weapon;
        for (unsigned frame = 1; frame <= 126; ++frame) {
            f(0x800D7670,.1f+frame/60.f);
            rr64_course_items_step(m(),&context);
        }
        const auto finished = course_items::capture_state();
        check(finished.roulette[0].phase == 2,
              "actual course hook completes the same collected reward across native frames");
        if (item::is_reward(reward)) {
            const auto held = item::capture_state().riders[0].held;
            check(held == item::reward_item(reward) && native_rewards == 0,
                  "selected MK reward reaches held inventory without native weapon grant");
            awarded[unsigned(held)] = true;
            render_held_hud(); // Real box award, including phase-two blink.
            for(unsigned frame=127;frame<=162;++frame){f(0x800D7670,.1f+frame/60.f);rr64_course_items_step(m(),&context);}
            check(course_items::capture_state().roulette[0].phase==0,"completed MK reward exits the transient roulette");
            render_held_hud(); // Held inventory must remain visible after retirement.
        } else {
            ++regular;
            check(native_rewards == 1 && item::capture_state().riders[0].held == item::Item::None,
                  "regular weapon and multiplier rewards retain original native award boundary");
        }
    }
    for (unsigned item_id = 1; item_id <= 15; ++item_id)
        if (item_id != unsigned(item::Item::DoubleMushroom))
            check(awarded[item_id], "every original MK reward can be collected through the actual course hook");
    check(regular > 0, "mixed reward pipeline retains regular Road Rash rewards");
    for(unsigned views=1;views<=4;++views){
        prepare(views);w(0x800A656C,views);
        for(unsigned view=0;view<views;++view)check(item::grant_item(view,item::Item(view+1)),"held local view inventory grant");
        render_held_hud(views);
    }
    prepare(1);
    auto remote=item::capture_state();check(item::grant(remote,13,item::Item::GreenShell),"canonical guest inventory sample");
    status.active=status.connected=status.authoritative=true;status.is_host=false;
    status.phase=netplay::Phase::Race;status.local_slot=13;status.replicated_riders=14;status.authority_humans=2;
    check(item::apply_state(remote,1,1),"guest13 receives canonical held inventory");
    render_held_hud();
    for (unsigned seed = 1; seed <= 16; ++seed) {
        prepare(1);
        course_items::reset_runtime();
        items_enabled = false;
        box_definitions.push_back({0,{100,100,10},.275f,2});
        w(globals::random_state,seed);
        f(0x800D7670,.1f);
        rr64_course_items_step(m(),&context);
        const auto pickup = course_items::capture_state();
        check(pickup.roulette[0].phase == 1 && !item::is_reward(pickup.roulette[0].weapon),
              "OFF preserves real box pickup while excluding all MK rewards");
        for (unsigned frame = 1; frame <= 126; ++frame) {
            f(0x800D7670,.1f+frame/60.f);
            rr64_course_items_step(m(),&context);
        }
        check(native_rewards == 1 && !item::capture_state().enabled && !item::can_grant(0),
              "OFF retains normal native rewards without initializing imported item inventory");
        for(unsigned frame=127;frame<=162;++frame){f(0x800D7670,.1f+frame/60.f);rr64_course_items_step(m(),&context);}
        render_held_hud();
    }
    prepare();
    const auto p = item::native::pair(m(),0);
    const float original_radius = fl(p.bike+0x15C);
    auto state = item::capture_state(); state.riders[0].shrink_until=300;
    authority(state);
    check(fl(p.bike+0x15C) == original_radius*.5f, "OFF transition begins with an active native effect");
    item::ReplayState historical;
    check(item::capture_replay(historical), "capture historical enabled effects before changing option");
    auto scratch = memory;
    items_enabled = false;
    rr64_mk64_items_before_physics(m());
    check(item::capture_state() == item::Snapshot{} && !item::input_active() &&
              fl(p.bike+0x15C) == original_radius,
          "OFF immediately clears live item state and restores original native geometry");
    {
        prediction::ReplayScope scope({true,true,true,false,false,0,1});
        check(item::bind_replay(scratch.data(),historical), "historical ON state still binds after live option changes");
        rr64_mk64_items_before_physics(scratch.data());
        check(item::native::scalar(scratch.data(),p.bike+0x15C) == original_radius*.5f,
              "private replay uses recorded enabled effects rather than current OFF preference");
    }
}
void native_contacts() {
    item::reset_runtime();
    memory = original;
    unsigned mounted_count = 0;
    for (unsigned slot = 0; slot < 14; ++slot) {
        const auto p = item::native::pair(m(), slot);
        if (!item::native::mounted(m(), p)) continue;
        ++mounted_count;
        item::native::Contacts contacts;
        const auto untouched = memory;
        check(item::native::contact_spheres(m(), p, contacts),
              "actual mounted native contact geometry accepted");
        check(memory == untouched, "contact capture never updates native collision caches");
        check(contacts.count == u(p.bike + 0x134) + u(p.rider + 0x54),
              "all original bike and rider spheres retained");
        unsigned index = 0;
        for (const unsigned body : {p.bike + 0x108, p.rider + 0x28}) {
            recomp_context call{};
            call.f_odd = &call.f0.u32h;
            call.r29 = guest_address(0x807FF000);
            call.r4 = guest_address(body);
            func_8004E754(m(), &call);
            for (unsigned i = 0; i < u(body + 0x2C); ++i, ++index) {
                for (unsigned axis = 0; axis < 3; ++axis)
                    check(std::abs(contacts.spheres[index].center[axis] -
                                   fl(body + 0x190 + i * 12 + axis * 4)) < .001f,
                          "read-only world center matches full original native4E754");
                check(contacts.spheres[index].radius == fl(body + 0x54 + i * 4),
                      "contact radius uses original native geometry");
            }
        }
        item::native::Vec anchor{};
        check(item::native::vector(m(), p.bike + 0x16C, anchor), "native mounted anchor read");
        item::native::Geometry bike, rider;
        const auto shift = item::native::mounted_center_shift(m(), p, .5f);
        check(bike.scale(m(), p.bike + 0x108, .5f) &&
                  rider.scale(m(), p.rider + 0x28, .5f, shift),
              "actual mounted collision geometry can shrink");
        item::native::Contacts small;
        check(item::native::contact_spheres(m(), p, small) && small.count == contacts.count,
              "shrunken actual contacts retain every native sphere");
        for (unsigned i = 0; i < contacts.count; ++i) {
            check(small.spheres[i].radius == contacts.spheres[i].radius * .5f,
                  "shell contacts use active shrink radii");
            for (unsigned axis = 0; axis < 3; ++axis)
                check(std::abs(small.spheres[i].center[axis] -
                               (anchor[axis] + (contacts.spheres[i].center[axis] - anchor[axis]) * .5f)) < .003f,
                      "shell contacts shrink around the same mounted render anchor");
        }
        bike.restore(m()); rider.restore(m());
        const unsigned count = u(p.bike + 0x134);
        w(p.bike + 0x134, 4);
        check(!item::native::contact_spheres(m(), p, small) && small.count == 0,
              "oversized contact count rejects entire compound body");
        w(p.bike + 0x134, count);
        const unsigned radius = u(p.rider + 0x7C);
        f(p.rider + 0x7C, std::numeric_limits<float>::quiet_NaN());
        check(!item::native::contact_spheres(m(), p, small) && small.count == 0,
              "malformed later rider sphere cannot expose partial bike contacts");
        w(p.rider + 0x7C, radius);
        memory = original;
    }
    check(mounted_count == 8, "retained scene exercises eight actual mounted actors");
}
void drivetrain() {
    prepare();
    const auto p=item::native::pair(m(),0);
    for (unsigned gears=1;gears<=6;++gears) {
        for (float rpm : {8000.f,16000.f}) {
            w(p.bike+0x54,gears); f(p.bike+0xC,rpm);
            f(p.bike+0x58+gears*4,.8f);
            float speed=0;
            check(item::native::rated_speed(m(),p,speed),"bounded authored drivetrain accepted");
            auto c=context; c.r16=guest_address(p.bike); c.r2=gears;
            c.f4.fl=speed/fl(p.bike+0x3C8);
            item_native_rpm(m(),&c);
            check(std::abs(fl(p.bike+0x490)-rpm)<.003f,"rated speed inverts actual native3A098 RPM arithmetic");
            if (rpm==16000) check(speed>100,"Insanity rating is not truncated at ordinary-bike speed");
        }
    }
    w(p.bike+0x54,7); float speed=0;
    check(!item::native::rated_speed(m(),p,speed),"invalid gear count rejected");
    w(p.bike+0x54,6); f(p.bike+0x70,0);
    check(!item::native::rated_speed(m(),p,speed),"zero authored ratio rejected");
    f(p.bike+0x70,.8f); f(p.bike+0x4C,std::numeric_limits<float>::quiet_NaN());
    check(!item::native::rated_speed(m(),p,speed),"nonfinite drive ratio rejected");
}
void physics_helpers() {
    prepare();
    const auto p=item::native::pair(m(),0);
    const auto before=context;
    check(item::native::speed(m(),p,40,100),"native boost admitted");
    check(fl(p.bike+0x178)==40 && fl(p.rider+0x98)==40 && fl(p.rider+0xC0)==40,
          "boost changes all three native velocity baselines equally");
    check(fl(p.bike+0x180)==0 && !std::memcmp(&before,&context,sizeof context),
          "boost preserves gravity velocity and caller context");
    auto call=context; call.f_odd=&call.f0.u32h; call.r4=guest_address(p.rider);
    func_80036B78(m(),&call);
    check(hu(p.rider+0x57C) && hu(p.bike+0x7F8) && fl(p.bike+0x4F8)==100,
          "actual36B78 does not mistake propulsion for a collision");
    vec(p.bike+0x178,0,0,0); vec(p.rider+0xC0,40,0,0);
    call=context; call.f_odd=&call.f0.u32h; call.r4=guest_address(p.rider);
    func_80036B78(m(),&call);
    check(!hu(p.rider+0x57C) && !hu(p.bike+0x7F8) && fl(p.bike+0x4F8)==81,
          "real subsequent velocity impact still takes complete37554 path");
    check(u(p.rider+0x20)==3 && hu(p.bike+0x7F6),"native flying and recovery control states established");

    prepare();
    item::native::Geometry geometry;
    const unsigned b=p.bike+0x108;
    w(b+0x2C,3);
    for (unsigned i=0;i<9;++i) f(b+0x30+i*4,(float(i)-4)*.25f);
    for (unsigned i=0;i<3;++i) f(b+0x54+i*4,.2f+i*.1f);
    const std::vector<unsigned char> old(m()+b-kRdramBegin,m()+b-kRdramBegin+0x200);
    for (unsigned pass=0;pass<100;++pass) {
        check(geometry.scale(m(),b,.5f),"shrink sphere geometry admitted");
        check(fl(b+0x30)==-.5f && std::abs(fl(b+0x54)-.1f)<.00001f,
              "repeated geometry scaling does not accumulate");
    }
    h(b+0x188,1);
    check(geometry.scale(m(),b,.5f) && hu(b+0x188)==0,"native contact cache invalidated");
    geometry.restore(m());
    check(!std::memcmp(old.data()+0x30,m()+b-kRdramBegin+0x30,0x30),"expiry restores exact original sphere words");
    check(geometry.scale(m(),b,.5f),"reenter shrink");
    f(b+0x54,.6f);
    check(geometry.scale(m(),b,.5f) && std::abs(fl(b+0x54)-.3f)<.00001f,"new native detach geometry establishes new baseline");
    geometry.restore(m());
    check(std::abs(fl(b+0x54)-.6f)<.00001f,"native rebuilt geometry restored exactly");
}
void native_guards() {
    for (const auto effect : {item::Item::Star,item::Item::Boo}) {
        prepare();
        const auto p=item::native::pair(m(),0);
        auto state=item::capture_state();
        if (effect==item::Item::Star) state.riders[0].star_until=300;
        else state.riders[0].boo_until=300;
        authority(state);
        check(rr64_mk64_items_immune(m(),p.actor) && rr64_mk64_items_immune(m(),p.rider),"actor and rider immunity ownership");
        for (auto function : {func_80061224,func_800616BC}) {
            auto c=context; c.r4=guest_address(actor_base+0x118); c.r5=guest_address(p.actor);
            c.r6=std::bit_cast<unsigned>(100.f);
            const auto old=memory; const auto saved=c;
            function(m(),&c);
            check(memory==old && !std::memcmp(&c,&saved,sizeof c),"native damage returns before health/credit/sound effects");
        }
        vec(p.bike+0x178,0,0,0); vec(p.rider+0xC0,40,0,0);
        auto c=context; c.f_odd=&c.f0.u32h; c.r4=guest_address(p.rider);
        func_80036B78(m(),&c);
        check(hu(p.rider+0x57C) && fl(p.bike+0x4F8)==100,"immunity blocks collision-triggered native ejection");
        h(p.rider+0x57E,1);
        c=context; c.f_odd=&c.f0.u32h; c.r4=guest_address(p.rider);
        func_80036B78(m(),&c);
        check(!hu(p.rider+0x57C) && u(p.rider+0x20)==3,"immunity preserves explicit manual/fall ejection and recovery");
    }
    prepare(); auto state=item::capture_state(); state.riders[0].boo_until=300; authority(state);
    const auto p=item::native::pair(m(),0), q=item::native::pair(m(),1);
    for (unsigned reversed=0;reversed<2;++reversed) {
        unsigned i=0;
        for (auto function : {func_8005F7A4,func_8005FA18,func_80060370}) {
            auto c=context;
            c.r4=guest_address(i==2?p.rider:p.bike); c.r5=guest_address(i==0?q.bike:q.rider);
            if (reversed) std::swap(c.r4,c.r5);
            const auto old=memory; const auto saved=c; const unsigned called=native_calls;
            function(m(),&c);
            check(memory==old && !std::memcmp(&c,&saved,sizeof c) && native_calls==called,
                  "Boo bypasses complete native bike/rider pair response for either operand");
            ++i;
        }
    }
}
void replay() {
    prepare(14);
    status.active=status.connected=status.authoritative=true; status.is_host=false;
    status.phase=netplay::Phase::Race; status.local_slot=13; status.replicated_riders=true;
    auto state=item::capture_state(); state.clock=1; state.riders[13].shrink_until=31;
    state.riders[13].boost_until=20; state.riders[13].boost_speed=40;
    authority(state,1);
    item::ReplayState historical;
    check(item::capture_replay(historical) && historical.elapsed_us*30/1000000>=1,"guest capture never rewinds authoritative clock");
    const auto p=item::native::pair(m(),0), remote=item::native::pair(m(),1);
    check(std::abs(fl(p.bike+0x15C)-.225f)<.00001f,"guest mapped local sphere is actually scaled");
    check(fl(remote.bike+0x178)==10,"guest effects never propel remote host-owned actor");
    const auto live_bytes=memory; const auto live_state=item::capture_state();
    auto scratch=memory;
    {
        prediction::ReplayScope scope({true,true,true,false,true,13,1u<<13});
        check(item::bind_replay(scratch.data(),historical),"bind historical private effects");
        rr64_mk64_items_before_physics(scratch.data());
        check(!rr64_mk64_items_immune(scratch.data(),p.actor),"history ignores newer live immunity");
        item::finish_replay(10000);
        item::ReplayState output;
        check(item::replay_state(output) && output.elapsed_us==historical.elapsed_us+10000,"private elapsed duration advances once exactly");
        check(output.state.clock>=historical.state.clock,"private clock remains monotonic");
        check(memory==live_bytes && item::capture_state()==live_state,"private geometry/boost cannot mutate live bytes or snapshot");
    }
    {
        prediction::ReplayScope scope({true,true,true,false,true,13,1u<<13});
        check(!item::replay_state(historical),"next replay epoch cannot borrow an old binding");
        auto malformed=historical; malformed.actors[13].bike_geometry.count=4000;
        check(!item::bind_replay(scratch.data(),malformed),"untrusted geometry count rejected before restore");
    }
    check(item::capture_replay(historical),"recapture live history");
    auto clean=state; clean.clock=31; clean.riders={};
    check(item::correct_replay(scratch.data(),historical,clean),"authority correction restores old scaled geometry before reseeding");
    check(std::abs(item::native::scalar(scratch.data(),p.bike+0x15C)-.45f)<.00001f,
          "private expiry correction restores exact full-size collision radius");
}
void lifecycle() {
    prepare(); check(item::capture_state().enabled,"complete assets enable runtime");
    rr64_mk64_items_before_physics(m());
    check(item::input_active(),"separate item control active in native race");
    item::request_use(0); check(item::take_action(0)==2 && item::take_action(0)==0,"separate controller edge consumed exactly once");
    auto state=item::capture_state();
    h(globals::gameplay_pause_state,1); tick(.1f);
    check(!item::input_active() && item::capture_state()==state,"pause freezes item clock and clears input");
    h(globals::gameplay_pause_state,0);
    check(item::grant_item(0,item::Item::GreenShell),"grant original green shell inventory");
    item::stage_use(0,-127); tick(.1f);
    const auto fired=item::capture_state();
    bool backward=false;
    for (const auto &o:fired.objects) if(o.generation && o.kind==item::Item::GreenShell) backward=o.velocity[0]<0;
    check(backward,"raw directional use reaches actual kernel backward shell branch");
    state=item::capture_state();
    check(!item::apply_state(state,1,0),"zero authority tick rejected");
    rr64_mk64_items_mode(m(),0x2F);
    check(item::capture_state()==item::Snapshot{} && !item::input_active(),"menu transition clears all item objects/effects/input");
    for (unsigned missing=0;missing<2;++missing) {
        prepare(); item::reset_runtime();
        assets=missing!=0; audio=missing==0;
        tick(.2f);
        check(!item::capture_state().enabled && !item::input_active(),"partial imported asset set never enables invisible items");
    }
    prepare(); item::reset_runtime(); course_active=false; const auto old=memory; tick(.1f);
    check(item::capture_state()==item::Snapshot{} && !item::input_active(),"stock course has no imported item runtime");
    (void)old;
}
void hit_integration() {
    prepare();
    pair(1,102,10);
    const auto p=item::native::pair(m(),0), q=item::native::pair(m(),1);
    check(item::grant_item(0,item::Item::GreenShell),"shell inventory acquired");
    item::stage_use(0,0);
    const auto caller=context;
    const std::vector<unsigned char> old_stack(m()+0x7FE000,m()+0x7FF000);
    tick(.1f,.1f);
    check(!hu(q.bike+0x7F8) && !hu(q.rider+0x57C) && u(q.rider+0x20)==3,
          "real kernel shell hit invokes complete native detach and flying setup");
    check(u(q.bike+0xA8)==p.actor && u(q.rider+0x10)==p.actor,
          "native collision attribution retains the shell owner");
    check(fl(q.bike+0x4F8)==81 && fl(q.rider+0x310)==0,"actual native damage and health preserved");
    check(!std::memcmp(&caller,&context,sizeof context) &&
          !std::memcmp(old_stack.data(),m()+0x7FE000,old_stack.size()),
          "runtime native hit preserves caller registers and scratch stack");
    prepare(); pair(1,100.8f,10);
    check(item::grant_item(0,item::Item::Star),"Star inventory acquired");
    item::stage_use(0,0); tick(.1f,.1f);
    check(!hu(bike_base+0x4000+0x7F8) && hu(bike_base+0x7F8),
          "Star contact crashes opponent through native path while owner remains mounted");
}
void impact_motion() {
    std::array<float, 3> resulting_speed{};
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        const bool box = scenario == 0;
        const float launch_speed = box ? 0.f : scenario == 1 ? 20.f : 40.f;
        prepare();
        // Keep the smaller shell's off-center target inside this 100ms sweep.
        // Use a nearby origin so the independent centered-sphere torque oracle
        // is not dominated by subtraction of large world coordinates.
        pair(0,box ? 100.f : 8.f,box ? 0.f : launch_speed-12);
        pair(1,box ? 110.f : 12.f,box ? 20.f : 10.f);
        float elapsed = .1f;
        if (box) {
            using T = course_walls::Triangle;
            const std::vector<T> floor{
                {1, {{{80,80,9},{150,80,9},{150,140,9}}}},
                {2, {{{80,80,9},{150,140,9},{80,140,9}}}}};
            surface_mesh = course_walls::build_surface_world(floor);
            check(item::grant_item(0,item::Item::FakeBox), "motion fake box granted");
            item::stage_use(0,0);
            for (unsigned frame = 1; frame <= 6; ++frame) tick(frame*.1f,.1f);
            const auto state = item::capture_state();
            const item::Object *resting = nullptr;
            for (const auto &object : state.objects)
                if (object.generation && object.kind == item::Item::FakeBox) resting = &object;
            check(resting && resting->mode == item::ObjectMode::Resting &&
                  resting->velocity == item::Vec{}, "dropped fake box settles as a stationary solid");
            pair(1,resting->position[0]+.5f,20);
            const auto target = item::native::pair(m(),1);
            vec(target.bike+0x16C,resting->position[0]+.5f,100.5f,resting->position[2]);
            vec(target.rider+0x8C,resting->position[0]+.5f,100.5f,resting->position[2]);
            elapsed = .7f;
        } else {
            const auto owner = item::native::pair(m(),0);
            const auto target = item::native::pair(m(),1);
            f(owner.bike+0x170,8.f); f(owner.rider+0x90,8.f);
            f(target.bike+0x170,8.5f); f(target.rider+0x90,8.5f);
            check(item::grant_item(0,item::Item::GreenShell), "motion shell granted");
            item::stage_use(0,0);
        }
        const auto source = item::native::pair(m(),0), target = item::native::pair(m(),1);
        const float starting_speed = box ? 20.f : 10.f;
        // Keep the native timestep and inverse coherent for this 100ms sweep.
        timing(.1f);
        auto reference_state = item::capture_state();
        std::array<item::Racer,item::racer_capacity> poses{};
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto pair = item::native::pair(m(),slot);
            auto &pose = poses[slot];
            pose.active = pose.riding = pose.human = true;
            item::native::vector(m(),pair.bike+0x16C,pose.position);
            item::native::vector(m(),pair.bike+0x178,pose.velocity);
            pose.forward = {1,0,0};
            pose.contact_count = 1; pose.contacts[0] = {{},.45f};
        }
        std::array<item::Use,item::racer_capacity> use{};
        use[0].pressed = !box;
        const auto contact = item::step(reference_state,poses,use,unsigned(elapsed*30+.01f),.1f,{}).hits[1];
        check(contact.active, "independent swept contact exists for moving native victim");
        const auto before = memory;
        const auto caller = context;
        const std::vector<unsigned char> old_stack(m()+0x7FE000,m()+0x7FF000);
        tick(elapsed,.1f);
        std::array<float,9> actual{};
        for (unsigned axis = 0; axis < 3; ++axis) {
            actual[axis] = fl(target.bike+0x178+axis*4);
            actual[axis+3] = fl(target.rider+0x98+axis*4);
            actual[axis+6] = fl(target.bike+0x278+axis*4);
        }
        resulting_speed[scenario] = actual[0];
        check(crashes==1 && !hu(target.bike+0x7F8) && !hu(target.rider+0x57C),
              "solid item impulse followed by full native detach");
        check(!std::memcmp(&caller,&context,sizeof context) &&
              !std::memcmp(old_stack.data(),m()+0x7FE000,old_stack.size()),
              "impulse and detach preserve exact caller context and guest stack");
        for (const float value : actual) check(std::isfinite(value), "solid impact motion remains finite");
        check(box ? actual[0] < starting_speed-1 : actual[0] > starting_speed+1,
              "stationary box brakes and overtaking shell accelerates along contact normal");
        check(actual[1] > .1f, "off-center contact produces lateral motion");
        check(actual[3] == starting_speed && actual[4] == 0 && actual[5] == 0,
              "native bike-only detach preserves the rider's inertial linear velocity");
        // Independent geometry oracle: for this centered victim sphere the
        // current contact lies radius .45 behind its outward contact normal.
        // This avoids reusing the adapter's contact-time translation formula.
        memory = before;
        course_impact::Contact impact;
        impact.normal = contact.direction; impact.surface_velocity = contact.surface_velocity;
        for (unsigned axis = 0; axis < 3; ++axis)
            impact.point[axis] = poses[1].position[axis] - impact.normal[axis]*.45f;
        const auto response = course_impact::apply(m(),caller,1,course_impact::Body::Bike,impact);
        check(response.applied && response.impulse > 0, "actual native solid-contact impulse accepted");
        auto call = caller; call.f_odd = &call.f0.u32h;
        call.r4 = guest_address(source.actor); call.r5 = guest_address(target.actor);
        call.r6 = std::bit_cast<unsigned>(fl(target.rider+0x30C));
        func_800616BC(m(),&call);
        call = caller; call.f_odd = &call.f0.u32h;
        call.r4 = guest_address(target.bike); call.r5 = guest_address(target.rider);
        func_80037554(m(),&call);
        for (unsigned axis = 0; axis < 3; ++axis) {
            check(std::abs(actual[axis]-fl(target.bike+0x178+axis*4)) < .001f,
                  "runtime bike motion matches native solid-contact geometry oracle");
            check(std::abs(actual[axis+3]-fl(target.rider+0x98+axis*4)) < .001f,
                  "runtime rider trajectory matches original native detach");
            // Reconstructing the same contact by two float expressions loses
            // a few world-coordinate ULPs, amplified in native angular impulse.
            if (std::abs(actual[axis+6]-fl(target.bike+0x278+axis*4)) >= .01f)
                std::fprintf(stderr,"torque mismatch scenario=%u axis=%u actual=%g oracle=%g normal=%g,%g,%g point=%g,%g,%g\n",
                    scenario,axis,actual[axis+6],fl(target.bike+0x278+axis*4),impact.normal[0],impact.normal[1],impact.normal[2],impact.point[0],impact.point[1],impact.point[2]);
            check(std::abs(actual[axis+6]-fl(target.bike+0x278+axis*4)) < .01f,
                  "moving-victim contact preserves native torque lever");
        }
    }
    check(resulting_speed[2] > resulting_speed[1]+5 && resulting_speed[2] < 42,
          "faster shell has stronger bounded response under coherent native timing");
}
void homing_clearance() {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        prepare();
        pair(0,100,0); pair(1,130,0);
        const auto owner = item::native::pair(m(),0), target = item::native::pair(m(),1);
        const float target_height = scenario == 2 ? 20.45f : 10.45f;
        vec(owner.bike+0x16C,100,100,10.45f); vec(owner.rider+0x8C,100,100,10.45f);
        vec(target.bike+0x16C,130,106,target_height); vec(target.rider+0x8C,130,106,target_height);
        std::array<unsigned char,80> records{};
        std::array<float,5> heights{10,10,10,10,10};
        for (unsigned i = 0; i < 5; ++i) {
            for (unsigned axis = 0; axis < 2; ++axis) {
                const unsigned bits = std::bit_cast<unsigned>(axis ? 100.f : 94.f + i*8.f);
                for (unsigned byte = 0; byte < 4; ++byte)
                    records[i*16+8+axis*4+byte] = static_cast<unsigned char>(bits >> (24-byte*8));
            }
        }
        route.records_be = records.data(); route.record_count = 5; route.wrap_segment = 2;
        route.record_heights = heights.data(); route.height_count = 5;
        using T = course_walls::Triangle;
        std::vector<T> triangles{
            {1, {{{80,80,10},{150,80,10},{150,140,10}}}},
            {2, {{{80,80,10},{150,140,10},{80,140,10}}}}};
        if (scenario == 1) {
            triangles.push_back({3, {{{80,103,10},{150,103,10},{150,103,15}}}});
            triangles.push_back({4, {{{80,103,10},{150,103,15},{80,103,15}}}});
        }
        surface_mesh = course_walls::build_surface_world(triangles);
        item::reset_runtime(); rr64_mk64_items_step(m(),&context);
        check(item::grant_item(0,item::Item::RedShell), "homing shell with real surface world granted");
        item::stage_use(0,0);
        for (unsigned frame = 1; frame <= 24; ++frame) tick(frame/60.f);
        const auto state = item::capture_state();
        const item::Object *shell = nullptr;
        for (const auto &object : state.objects)
            if (object.generation && object.kind == item::Item::RedShell) shell = &object;
        check(shell != nullptr, "guided shell remains on the finite road after floor contact");
        check(std::abs(shell->position[2] - (10 + item::object_radius(item::Item::RedShell))) < .03f,
              "guidance operates after actual sphere sweep settles onto the road");
        if (scenario == 0) {
            check(shell->velocity[1] > 1,
                  "grounded homing shell pursues rider six units away from route centerline");
            for (unsigned frame = 25; frame <= 180 && hu(target.bike+0x7F8); ++frame)
                tick(frame/60.f);
            check(!hu(target.bike+0x7F8) && u(target.bike+0xA8) == owner.actor,
                  "off-center homing reaches its target and invokes the original attributed crash");
        } else
            check(std::abs(shell->velocity[1]) < .001f,
                  "blocking wall or separate deck retains native route guidance");
    }
}
void clock_parity() {
    for (bool guest : {false,true}) {
        prepare();
        auto state=item::capture_state(); state.riders[0].star_until=100;
        if (guest) authority(state);
        else { check(item::grant_item(0,item::Item::Star),"host test Star granted"); item::stage_use(0,0); tick(.016667f); }
        item::ReplayState before;
        check(item::capture_replay(before),"capture item clock before native frame");
        auto scratch=memory;
        item::ReplayState predicted;
        {
            prediction::ReplayScope scope(guest?prediction::SessionRules{true,true,true,false,false,0,1}:
                                               prediction::SessionRules{});
            check(item::bind_replay(scratch.data(),before),"bind exact phase");
            rr64_mk64_items_before_physics(scratch.data());
            item::finish_replay(16667);
            check(item::replay_state(predicted),"collect exact projected phase");
        }
        tick(.1f,1.f/60);
        item::ReplayState actual;
        check(item::capture_replay(actual),"capture same completed frame");
        check(predicted==actual,"live host/guest and private physics effects use identical integer phase");
    }
}
void lightning_runover() {
    prepare();
    const auto victim = item::native::pair(m(),1);
    const float radius = fl(victim.bike+0x15C);
    vec(victim.bike+0x178,100,0,0); vec(victim.rider+0x98,100,0,0);
    check(item::grant_item(0,item::Item::Lightning), "native Lightning grant");
    item::stage_use(0,0); tick(.02f);
    check(hu(victim.bike+0x7F8) && hu(victim.rider+0x57C) && !crashes &&
              fl(victim.rider+0x310)==100 && fl(victim.bike+0x4F8)==100,
          "Lightning keeps native rider attached with stamina/health and no crash");
    check(fl(victim.bike+0x15C)==radius*.5f && fl(victim.bike+0x178)<100 &&
              item::capture_state().riders[1].shrink_until>item::capture_state().clock,
          "Lightning applies actual half-size geometry and native speed cap");
    // Native4918C plus its original caller branch decides contact. The hook
    // further checks the scaled sphere geometry rather than the broad envelope.
    for (unsigned scenario=0;scenario<16;++scenario) {
        prepare();
        const auto large=item::native::pair(m(),0), small=item::native::pair(m(),1);
        check(item::grant_item(0,item::Item::Lightning),"runover setup Lightning grant");
        item::stage_use(0,0);tick(.02f);
        float elapsed=.02f;
        if(scenario>=3 && scenario<=6) {
            const unsigned target=scenario==6?0:1;
            const auto effect=scenario==3?item::Item::Lightning:
                              scenario==4?item::Item::Star:item::Item::Boo;
            check(item::grant_item(target,effect),"runover guard effect grant");
            item::stage_use(target,0);elapsed+=.2f;tick(elapsed,.2f);
        }
        if(scenario==2 || scenario==11) {
            const auto deadline=item::capture_state().riders[1].shrink_until;
            while(item::capture_state().clock+(scenario==11?1u:0u)<deadline) {
                elapsed+=1.f/30;tick(elapsed,1.f/30);
            }
        }
        if(scenario==7 || scenario==8) {
            status.active=status.connected=status.authoritative=true;
            status.is_host=scenario==8; status.phase=netplay::Phase::Race;
            status.local_slot=0; status.authority_humans=3;
        }
        if(scenario==9)items_enabled=false;
        const float separation=scenario==1?4.f:scenario==12?.8f:.4f;
        vec(small.bike+0x16C,100+separation,100,10);
        vec(small.rider+0x8C,100+separation,100,10);
        for(const auto p:{large,small}) {f(p.bike+0x10C,.9f);f(p.bike+0x110,1.35f);}
        rr64_mk64_items_before_physics(m());
        auto call=context;call.f_odd=&call.f0.u32h;
        call.r22=guest_address(large.bike);call.r23=guest_address(small.bike);
        call.r21=guest_address(large.bike+0x108);call.f20.fl=separation;
        vec(unsigned(call.r29)+0x10,-1,0,0);
        if(scenario==15) {
            std::swap(call.r22,call.r23);call.r21=guest_address(small.bike+0x108);
            vec(unsigned(call.r29)+0x10,1,0,0);
        }
        if(scenario==10) {
            prediction::ReplayScope scope;
            item_native_bike_contact(m(),&call);
        } else item_native_bike_contact(m(),&call);
        if(scenario==13)w(small.actor+0x1C,42); // Same addresses, different actor identity.
        if(scenario==14) {
            h(globals::gameplay_pause_state,1);
            rr64_mk64_items_before_physics(m());
            h(globals::gameplay_pause_state,0);
        }
        const unsigned before=crashes;
        elapsed+=1.f/30;f(0x800D7670,elapsed);f(globals::physics_delta,1.f/30);
        rr64_mk64_items_step(m(),&context);
        const bool expected=scenario==0 || scenario==8 || scenario==15;
        check((hu(small.bike+0x7F8)==0)==expected,
              "native contact only crashes eligible small rider on offline/host");
        if(expected) {
            check(crashes==before+1 && u(small.bike+0xA8)==large.actor,
                  "runover uses one complete attributed original crash");
            rr64_mk64_items_step(m(),&context);
            check(crashes==before+1,"runover queue consumes once");
        } else if(scenario==4) {
            check(hu(small.bike+0x7F8) && !hu(large.bike+0x7F8),
                  "Star protects its rider while retaining its original contact attack");
        } else {
            check(crashes==before,"distance/expiry/equal-size/protection/guest/private/OFF reject runover");
        }
    }
    prepare();
    const auto p=item::native::pair(m(),1);
    const float original_radius=fl(p.bike+0x15C);
    check(item::grant_item(0,item::Item::Lightning),"expiry test Lightning grant");
    item::stage_use(0,0);tick(.02f);
    for(unsigned frame=1;frame<=42;++frame)tick(.02f+frame*.25f,.25f);
    check(fl(p.bike+0x15C)==original_radius && hu(p.bike+0x7F8) && !crashes,
          "natural shrink expiry restores full geometry without a crash");
}
void shrink_alignment() {
    prepare();
    const auto p=item::native::pair(m(),0);
    vec(p.rider+0x8C,100,100,11);
    vec(p.rider+0x5DC,100,100,11); vec(p.bike+0x53C,99,100,9.5f);
    vec(p.rider+0x28+0x30,0,0,2);
    auto state=item::capture_state(); state.riders[0].shrink_until=300; authority(state);
    auto c=context; c.f_odd=&c.f0.u32h; c.r4=guest_address(p.rider+0x28);
    func_8004E754(m(),&c);
    check(std::abs(fl(p.rider+0x28+0x190+8)-11.5f)<.00001f,
          "actual4E754 mounted contact center shrinks around shared bike anchor");
    constexpr unsigned node=0x80400000,record=0x80401000,source=0x80402000,matrix=0x80403000;
    w(node,2); w(node+4,p.rider); w(node+actor_scene::current_model,record);
    for (unsigned i=0;i<3;++i) w(node+actor_scene::lod_models+i*4,record);
    w(record+0x14,source); h(source+0x12,0); f(0x8009DBAC,1);
    const auto identity=[&] {
        for (unsigned i=0;i<16;++i) f(matrix+i*4,i%5==0?1.f:0.f);
        vec(matrix+48,100,100,11);
    };
    identity(); const auto gameplay=memory;
    rr64_mk64_items_scale_matrix(m(),node,record,matrix);
    check(fl(matrix)==.5f && fl(matrix+20)==.5f && fl(matrix+40)==.5f && fl(matrix+56)==10.5f,
          "mounted rendered rider basis and anchor match scaled native collider");
    check(!std::memcmp(gameplay.data()+p.rider-kRdramBegin,m()+p.rider-kRdramBegin,rider::stride),
          "render shrink never edits simulation rider pose");
    identity(); presenting=true; recorded=state; recorded_bike={100,100,19}; recorded_rider={100,100,24}; recorded_origin={100,100,20};
    rr64_mk64_items_scale_matrix(m(),node,record,matrix);
    check(fl(matrix+56)==9,"highlight shrink uses recorded attachment offset rather than stale live anchors");
    identity(); recorded_attached=false;
    rr64_mk64_items_scale_matrix(m(),node,record,matrix);
    check(fl(matrix+56)==11 && fl(matrix)==.5f,"recorded detached rider keeps its own shrink anchor");
    presenting=false; recorded_attached=true;
    identity();
    rr64_mk64_items_scale_matrix(m(),node,record+0x100,matrix);
    check(fl(matrix)==1 && fl(matrix+56)==11,"skeletal child is not scaled a second time");
    item::ReplayState saved; check(item::capture_replay(saved),"capture translated collider baseline");
    auto private_memory=memory;
    {
        prediction::ReplayScope scope({true,true,true,false,false,0,1});
        check(item::bind_replay(private_memory.data(),saved),"valid translated sphere geometry binds privately");
    }
    rr64_mk64_items_mode(m(),0x2F);
    check(fl(p.rider+0x28+0x38)==2 && fl(p.rider+0x8C+8)==11,
          "mode reset restores original collider but preserves simulation pose");
}
void native_render_shrink() {
    constexpr unsigned node=0x80400000, bike_record=0x80401000, rider_record=0x80401100,
        bike_source=0x80402000, rider_source=0x80402100, bike_pose=0x80403000,
        rider_pose=0x80403100, camera=0x80404000, weapon_record=0x80405000,
        weapon_matrix=0x80406000;
    // Real native5D9A4 anchors, original15A90 matrices and the exact11CC0
    // production call site. The fake equal physics/render origin previously
    // used here could not reveal the bike's displaced rear render anchor.
    for(unsigned bike_bank : {1u,2u}) for(unsigned view=0;view<4;++view)
        for(bool attached : {false,true}) for(bool playback : {false,true}) {
        prepare();
        const auto p=item::native::pair(m(),0);
        const bool guest=view==3;
        const unsigned canonical=guest?13:0;
        if(guest) {
            status.active=status.connected=status.authoritative=true;
            status.phase=netplay::Phase::Race;status.local_slot=13;
            status.replicated_riders=14;status.authority_humans=2;
        }
        vec(p.bike+0x16C,100,100,10); vec(p.bike+0x53C,99,100,9.5f);
        vec(p.rider+0x8C,100,100,11); vec(p.rider+0x5DC,100,100,11);
        h(p.bike+0x7F8,attached?1:0);h(p.rider+0x57C,attached?1:0);
        auto state=item::capture_state();state.riders[canonical].shrink_until=300;
        authority(state);
        // Playback anchors deliberately differ from all live physics anchors.
        presenting=playback;recorded=state;recorded_attached=attached;
        recorded_bike={201,302,39.5f};recorded_rider={202,302,41};
        recorded_origin={202,302,40};
        const item::Vec origin=playback?recorded_origin:item::Vec{100,100,10};
        const item::Vec bike_anchor=playback?recorded_bike:item::Vec{99,100,9.5f};
        const item::Vec rider_anchor=playback?recorded_rider:item::Vec{100,100,11};
        const item::Vec eye{95+float(view)*3,93-float(view)*2,8+float(view)};
        vec(camera,eye[0],eye[1],eye[2]);
        w(globals::active_viewport,view);f(0x8009DBB0,100);f(0x8009DBB4,10);
        w(node,1);w(node+4,p.bike);w(node+actor_scene::current_model,bike_record);
        w(bike_record+0x14,bike_source);h(bike_source+0x12,bike_bank);
        w(rider_record+0x14,rider_source);h(rider_source+0x12,1);
        w(bike_record+0xC,bike_pose);w(rider_record+0xC,rider_pose);
        // Native producer writes both camera-relative translations. Playback
        // uses its recorded anchors after that live-only producer, as in game.
        auto c=context;c.f_odd=&c.f0.u32h;c.r22=guest_address(p.bike);
        c.r18=guest_address(p.rider);c.r20=guest_address(node);c.r30=guest_address(camera);
        c.r17=guest_address(bike_pose);c.r16=guest_address(rider_pose);
        item_native_render_anchors(m(),&c);
        for(unsigned axis=0;axis<3;++axis) {
            check(std::abs(fl(bike_pose+axis*4)-((axis==0?99.f:axis==1?100.f:9.5f)-eye[axis])*(bike_bank==1?100.f:10.f))<.001f,
                  "original bike anchor producer retains its authored source bank");
            check(std::abs(fl(rider_pose+axis*4)-((axis==2?11.f:100.f)-eye[axis])*100.f)<.001f,
                  "original rider anchor producer retains native100-unit camera space");
        }
        const auto gameplay=memory;
        std::array<float,16> rider_result{};
        for(unsigned type : {1u,2u}) {
            const unsigned record=type==1?bike_record:rider_record;
            const unsigned pose=type==1?bike_pose:rider_pose;
            const auto anchor=type==1?bike_anchor:rider_anchor;
            const float factor=type==1&&bike_bank==2?10.f:100.f;
            if(playback) for(unsigned axis=0;axis<3;++axis) f(pose+axis*4,(anchor[axis]-eye[axis])*factor);
            // A rotated root detects accidental component/translation scaling.
            vec(pose+12,0,0,.382683432f);f(pose+24,.923879533f);
            w(node,type);w(node+4,type==1?p.bike:p.rider);w(node+actor_scene::current_model,record);
            for(unsigned tier=0;tier<3;++tier)w(node+actor_scene::lod_models+tier*4,record);
            c=context;c.f_odd=&c.f0.u32h;c.r4=guest_address(pose+12);c.r5=guest_address(pose);
            c.r6=guest_address(weapon_matrix);func_80015A90(m(),&c);
            std::array<float,16> full{};for(unsigned i=0;i<16;++i)full[i]=fl(weapon_matrix+i*4);
            c=context;c.f_odd=&c.f0.u32h;c.r18=guest_address(record);c.r19=guest_address(node);
            item_native_render_matrix(m(),&c);
            const unsigned matrix=unsigned(c.r29)+0x10;
            for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)
                check(std::abs(fl(matrix+(row*4+col)*4)-full[row*4+col]*.5f)<.00001f,
                      "native rotated actor root scales once before packing");
            for(unsigned axis=0;axis<3;++axis) {
                const float expected=(type==1||attached)?origin[axis]+.5f*(anchor[axis]-origin[axis]):anchor[axis];
                check(std::abs(fl(matrix+48+axis*4)/factor+eye[axis]-expected)<.00002f,
                      "bike and mounted rider share actual physical pivot in camera/source units");
            }
            if(type==2)for(unsigned i=0;i<16;++i)rider_result[i]=fl(matrix+i*4);
            if(type==2 && !playback) {
                w(p.rider+0x5BC,weapon_record);
                item::scale_weapon_matrix(m(),node,weapon_record,weapon_matrix,3);
                for(unsigned i=0;i<16;++i)
                    check(std::abs(fl(weapon_matrix+i*4)-rider_result[i])<.00001f,
                          "independently submitted held weapon matches scaled rider root");
                for(unsigned i=0;i<16;++i)f(weapon_matrix+i*4,full[i]);
                item::scale_weapon_matrix(m(),node,weapon_record+0x18,weapon_matrix,3);
                for(unsigned i=0;i<16;++i)check(fl(weapon_matrix+i*4)==full[i],"weapon child rejects a second scale");
                // Prepared source2 poses use10 units, regardless of parent bank1.
                for(unsigned axis=0;axis<3;++axis)f(weapon_matrix+48+axis*4,full[12+axis]*.1f);
                item::scale_weapon_matrix(m(),node,weapon_record,weapon_matrix,2);
                for(unsigned axis=0;axis<3;++axis)
                    check(std::abs(fl(weapon_matrix+48+axis*4)-rider_result[12+axis]*.1f)<.0001f,
                          "prepared weapon source override preserves parent camera units");
            }
            // Children and private prediction never alter presentation roots.
            for(unsigned i=0;i<16;++i)f(weapon_matrix+i*4,full[i]);
            rr64_mk64_items_scale_matrix(m(),node,record+0x80,weapon_matrix);
            for(unsigned i=0;i<16;++i)check(fl(weapon_matrix+i*4)==full[i],"actor child stays authored");
            {prediction::ReplayScope scope;rr64_mk64_items_scale_matrix(m(),node,record,weapon_matrix);}
            for(unsigned i=0;i<16;++i)check(fl(weapon_matrix+i*4)==full[i],"private physics cannot rewrite render matrices");
        }
        for(unsigned offset : {0x16Cu,0x53Cu})
            check(!std::memcmp(gameplay.data()+p.bike+offset-kRdramBegin,m()+p.bike+offset-kRdramBegin,12),
                  "matrix preparation preserves original bike world anchors");
        for(unsigned offset : {0x8Cu,0x5DCu})
            check(!std::memcmp(gameplay.data()+p.rider+offset-kRdramBegin,m()+p.rider+offset-kRdramBegin,12),
                  "matrix preparation preserves original rider world anchors");
        item::Vec shown{},expected_anchor=rider_anchor;
        if(attached)for(unsigned axis=0;axis<3;++axis)
            expected_anchor[axis]=origin[axis]+.5f*(rider_anchor[axis]-origin[axis]);
        check(item::render_rider_anchor(m(),canonical,shown)&&shown==expected_anchor,
              "presentation anchor honors visible shrink, guest mapping and recorded playback");
        presenting=false;
        rr64_mk64_items_mode(m(),0x2F);
        // The next ordinary native producer fully restores unscaled output.
        c=context;c.f_odd=&c.f0.u32h;c.r18=guest_address(rider_record);c.r19=guest_address(node);
        item_native_render_matrix(m(),&c);
        check(std::abs(fl(unsigned(c.r29)+0x10)-.70710678f)<.00001f,
              "ended effect leaves subsequent native root at original full scale");
    }
}
void ai_and_disabled_clock() {
    prepare(14);
    for (unsigned slot=0;slot<14;++slot) {
        h(actor_base+slot*0x118+0x26,1);
        check(item::grant_item(slot,item::Item::Mushroom),"each native AI rider receives independent item inventory");
    }
    tick(.1f);
    const auto state=item::capture_state();
    for (unsigned slot=0;slot<14;++slot) {
        const auto p=item::native::pair(m(),slot);
        check(state.riders[slot].held==item::Item::None && state.riders[slot].boost_until>state.clock,
              "all14 native AI riders can use imported items");
        check(fl(p.bike+0x178)>100,"AI Insanity mushroom uses full authored rating");
    }
    prepare();
    authority({});
    item::ReplayState before,after;
    check(item::capture_replay(before) && before.elapsed_us==0,"disabled guest item clock is inert");
    tick(.1f);
    check(item::capture_replay(after) && before==after,"no-state guest frame cannot drift from inert private history");
}
}

int main(int argc,char **argv) {
    try {
        fixture::check(argc==2,"private native RDRAM snapshot path required");
        std::ifstream file(argv[1],std::ios::binary);
        fixture::original.assign(std::istreambuf_iterator<char>(file),{});
        fixture::check(fixture::original.size()>=rr64::engine::kRdramSize,"native snapshot size");
        fixture::original.resize(rr64::engine::kRdramSize);
        fixture::memory=fixture::original;
        fixture::native_contacts();
        fixture::box_reward_pipeline();
        fixture::weapon_switch_focus();
        fixture::weapon_switch_worker_ownership();
        fixture::native_cycle_return_input();
        fixture::drivetrain(); fixture::physics_helpers(); fixture::native_guards();
        fixture::replay(); fixture::lifecycle(); fixture::hit_integration(); fixture::impact_motion(); fixture::clock_parity();
        fixture::homing_clearance();
        fixture::native_render_shrink(); fixture::shrink_alignment();
        fixture::lightning_runover();
        fixture::ai_and_disabled_clock();
        rr64::mk64_items::reset_runtime();
        std::printf("MK64 native item fixture: %u checks passed; no game launched\n",fixture::checks);
        return 0;
    } catch(const std::exception &error) {
        std::fprintf(stderr,"FAIL after %u checks: %s\n",fixture::checks,error.what()); return 1;
    }
}

