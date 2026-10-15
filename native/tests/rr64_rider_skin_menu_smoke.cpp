#include "rr64_rider_skin_fixture.hpp"
#include "rr64_rider_skin_menu.hpp"
#include "rr64_rider_skin_preferences.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>

namespace skin=rr64::rider_skins;
namespace prefs=rr64::rider_skin_preferences;
namespace engine=rr64::engine;
namespace {
unsigned checks=0,labels=0;bool online=false,cop=false,host=true;
unsigned humans=0x3fff,local=0;
std::vector<unsigned char> memory(engine::kRdramSize);
auto* m=memory.data();
constexpr unsigned selected=0x8009f670,actors=0x800d8570,pool=0x80280000;
void check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void put(unsigned p,unsigned v){engine::write_u32(m,p,v);}
void half(unsigned p,unsigned v){engine::write_u16(m,p,std::uint16_t(v));}
unsigned word(unsigned p){unsigned v=0;engine::read_u32(m,p,v);return v;}
void mode(unsigned value,unsigned update=0x80072704,unsigned draw=0x8007273c){
    put(engine::globals::main_mode,value);put(engine::globals::pending_mode,value);
    if(value<engine::kModeRecordCount){const unsigned r=engine::globals::mode_records+value*engine::kModeRecordSize;put(r+8,update);put(r+12,draw);}
}
unsigned preview(unsigned slot){
    const unsigned node=0x80480000+slot*0x100;put(0x800d13c8,pool);put(node,2);
    put(node+4,pool+slot*engine::rider::stride);put(node+0x40,slot);put(node+0x28,0x80500000+slot*0x100);
    put(node+0x3c,0);put(0x800a1454,node);return node;
}
unsigned racer(unsigned canonical,unsigned slot,unsigned donor){
    const unsigned a=actors+canonical*0x118,r=0x80200000+canonical*0x1000,b=0x80300000+canonical*0x1000,n=0x80400000+canonical*0x100;
    put(0x800a656c,14);put(a+8,slot);put(a+0x1c,donor);half(a+0x24,1);half(a+0x26,0);
    put(a+0xe0,b);put(a+0xe4,r);put(b+4,a);put(r+4,a);put(b+engine::bike::rider_pointer,r);put(r+engine::rider::bike_pointer,b);
    put(n,2);put(n+4,r);return n;
}
}
rr64::netplay::PhysicsRules rr64::netplay::get_physics_rules(){PhysicsRules r;r.active=online;r.connected=true;r.authoritative=true;r.is_host=host;r.local_slot=local;r.authority_humans=humans;r.replicated_riders=true;return r;}
extern "C" int rr64_custom_cop_enabled(){return cop;}
extern "C" unsigned rr64_custom_cop_bike_entry(unsigned char*,unsigned,unsigned){return cop?0x800a684c:0;}
extern "C" int rr64_custom_cop_can_start(unsigned char*){return !cop;}
extern "C" void func_800796F8(unsigned char*,recomp_context*){++labels;}
int main(int argc, char** argv){
    if(argc>1) {
        std::ifstream in(argv[1],std::ios::binary);
        std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)),{});
        check(skin::load_stock(rom),"all forty native rider textures load from supported ROM");
        check(!skin::load_stock({}),"invalid ROM rejected");
    }
    const auto directory=std::filesystem::temp_directory_path()/("rr64-skin-menu-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    prefs::initialize(directory);
    std::vector<skin::Appearance> catalog;
    for(unsigned i=0;i<skin::maximum_characters;++i)catalog.push_back(skin_fixture("rider-"+std::to_string(i),"Rider "+std::to_string(i),i==3?10:0));
    auto bad=catalog;bad[3].donor=45;check(!skin::install_catalog(bad)&&!skin::enabled(),"invalid native donor rejected atomically");
    bad=catalog;bad[3].textures[0].bytes.back()=0;check(!skin::install_catalog(bad),"transparent palette rejected");
    bad=catalog;bad[4].id=bad[3].id;check(!skin::install_catalog(bad),"duplicate stable identity rejected");
    check(skin::install_catalog(catalog)&&skin::count()==skin::maximum_characters,"full bounded catalog installs");
    put(0x800d6a58,17);
    for(unsigned unlocked=0;unlocked<2;++unlocked){
        half(0x800a77d8,unlocked);const int maximum=unlocked?44:39,ring=maximum+1+skin::count();
        for(unsigned slot=0;slot<4;++slot)for(int direction:{-1,1}){
            skin::clear_menu();put(selected+slot*4,0);
            for(int step=0;step<ring;++step){
                int next=rr64_rider_skin_cycle(m,slot,int(word(selected+slot*4))+direction);
                if(next<0)next=maximum;if(next>maximum)next=0;put(selected+slot*4,unsigned(next));
                const unsigned a=skin::menu_selection(slot),logical=a?maximum+a:next;
                check(logical==unsigned(direction>0?step+1:ring-step-1)%ring,"both-direction ring visits every native and custom exactly once");
                check(!a || unsigned(next)==skin::native_donor(a),"female skin uses native female donor throughout cycling");
            }
        }
    }
    check(word(0x800d6a58)==17,"cycling cannot change campaign record");
    // Native05 capture: selectors have shared 72704/7273C callbacks. The draw
    // dispatcher at 2F184 selects 256C0/27A90/2DC30/2CD60 by main_mode - 1.
    for(unsigned native_mode:{33u,35u,45u,46u}){
        mode(native_mode);put(engine::local_race::menu_humans,4);
        const unsigned slots=native_mode==35?4:1;
        for(unsigned slot=0;slot<slots;++slot){
            skin::set_menu_selection(slot,4);put(selected+slot*4,10);const unsigned n=preview(slot),g=word(n+0x28);
            check(rr64_rider_skin_actor_selection(m,n)==4 && rr64_rider_skin_preview_selection(m,g)==4,"Cortana identity reaches every selector and local preview");
            // Bike/posture changes do not alter the native donor or appearance.
            put(0x800a6690,5);check(rr64_rider_skin_actor_selection(m,n)==4,"bike class change preserves female skin identity");
            put(n,1);check(!rr64_rider_skin_preview_selection(m,g),"bike graph cannot receive rider skin");put(n,2);
            put(n+4,pool+4);check(!rr64_rider_skin_actor_selection(m,n),"stale preview pointer rejected");
        }
        const unsigned outside=preview(slots);
        check(!rr64_rider_skin_actor_selection(m,outside),"preview slot outside this selector is rejected");
    }
    skin::set_menu_selection(0,4);put(selected,10);const unsigned n=preview(0),g=word(n+0x28);
    for(unsigned value=0;value<engine::kModeRecordCount;++value){
        if(value==33||value==35||value==45||value==46)continue;
        mode(value);check(!rr64_rider_skin_actor_selection(m,n)&&!rr64_rider_skin_preview_selection(m,g),"shared callbacks cannot authorize an unrelated mode");
    }
    for(unsigned value:{engine::kModeRecordCount,0xffffffffu}){
        mode(value);check(!rr64_rider_skin_preview_selection(m,g),"out of range mode is rejected");
    }
    for(unsigned old_handler:{0x800256c0u,0x80027a90u,0x8002cd60u,0x8002dc30u}){
        mode(2,old_handler);check(!rr64_rider_skin_preview_selection(m,g),"old synthetic direct-selector callback cannot authorize preview");
        mode(35,old_handler);check(!rr64_rider_skin_preview_selection(m,g),"selector mode requires native generic update callback");
    }
    mode(35,0x80072704,0x80072778);check(!rr64_rider_skin_preview_selection(m,g),"selector requires native draw callback at record plus twelve");
    mode(35);put(engine::globals::pending_mode,34);
    check(rr64_rider_skin_preview_selection(m,g)==4,"current selector remains drawable until pending mode is committed");
    for(unsigned count:{0u,1u,2u,3u,4u,5u,0xffffffffu}){
        mode(35);put(engine::local_race::menu_humans,count);
        for(unsigned slot=0;slot<4;++slot){
            skin::set_menu_selection(slot,4);put(selected+slot*4,10);const unsigned node=preview(slot);
            check(rr64_rider_skin_actor_selection(m,node)==((count>=1&&count<=4&&slot<count)?4u:0u),"multiplayer previews require bounded native human count and local slot");
        }
    }
    mode(35);put(engine::local_race::menu_humans,1);preview(0);
    check(!rr64_rider_skin_preview_selection(m,g+0x20),"unowned graph cannot receive rider skin");
    put(selected,0);check(!rr64_rider_skin_preview_selection(m,g),"wrong native donor cannot borrow selected skin");put(selected,10);
    online=true;check(!rr64_rider_skin_preview_selection(m,g),"online selector preview remains native");online=false;
    rr64_rider_skin_race_begin(m);mode(0x12);
    for(unsigned slot=0;slot<4;++slot){
        const unsigned a=slot+1,donor=skin::native_donor(a),canonical=10-slot;
        skin::set_menu_selection(slot,a);put(selected+slot*4,donor);const unsigned n=racer(canonical,slot,donor);
        rr64_rider_skin_spawn(m,canonical);check(rr64_rider_skin_actor_selection(m,n)==a,"four local humans own separate canonical appearances");
        half(actors+canonical*0x118+0x26,1);check(!rr64_rider_skin_actor_selection(m,n),"AI cannot borrow local appearance");
        half(actors+canonical*0x118+0x26,0);online=true;check(!rr64_rider_skin_actor_selection(m,n),"online stays native");online=false;
        mode(0x14);check(rr64_rider_skin_actor_selection(m,n)==a,"results and highlights preserve appearance");mode(0x12);
    }
    // Exercise the real owner gate and verify cosmetics write no native state.
    skin::set_random_cpu(true);
    for(unsigned i=0;i<40;++i) skin::install_stock(i, skin_fixture("stock", "Stock", i%2?0:10));
    for(unsigned pool=0;pool<3;++pool) for(unsigned percent : {0u,50u,100u}) {
        skin::set_cpu_pool(pool); skin::set_cpu_percent(percent); skin::clear_race();
        unsigned chosen=0;
        for(unsigned i=0;i<10;++i) {
            const auto pick=skin::random_cpu_selection(i,i%2?0:10,1023);
            chosen+=pick!=0;
            check(!pick || pool!=1 || pick>32,"default pool contains only original riders");
            check(!pick || pool!=2 || pick<=32,"mod pool contains only added riders");
        }
        check(chosen==percent/10,"exact CPU percentage across complete roster");
    }
    skin::set_cpu_pool(0); skin::set_cpu_percent(100);
    unsigned customs=0,stock=0;
    for(unsigned race=0;race<64;++race){
        rr64_rider_skin_race_begin(m);mode(0x12);
        for(unsigned canonical=0;canonical<14;++canonical){
            const unsigned native=canonical%2?7:17, donor=canonical%2?0:10;
            const unsigned node=racer(canonical,0,native);
            half(actors+canonical*0x118+0x26,1);
            half(0x800a1574+native*8,donor==0?1:0);
            const auto before=memory;
            const unsigned choice=rr64_rider_skin_actor_selection(m,node);
            check(memory==before,"CPU appearance never alters native physics or RNG");
            check(!choice || skin::native_donor(choice)==donor,"CPU skin fits original gender geometry");
            customs+=choice>0&&choice<=32;stock+=choice>32;
            skin::set_random_cpu(false);
            check(rr64_rider_skin_actor_selection(m,node)==choice,"mid-race toggle changes wait for next race");
            skin::set_random_cpu(true);
            check(rr64_rider_skin_actor_selection(m,node)==choice,"CPU skin stays stable between draws");
            mode(0x14);check(rr64_rider_skin_actor_selection(m,node)==choice,"CPU skin stays stable in highlights/results");mode(0x12);
            online=true;humans=1u<<canonical;
            check(!rr64_rider_skin_actor_selection(m,node),"online human protected even with native AI flag");
            humans=1u<<((canonical+1)%14);
            check(rr64_rider_skin_actor_selection(m,node)==choice,"online CPU uses local cosmetic choice");
            host=false;local=(canonical+1)%14;humans=1u<<local;
            const unsigned mapped=rr64::online_flow::mapped_slot(canonical,local,true);
            if(mapped==canonical)check(rr64_rider_skin_actor_selection(m,node)==choice,"guest CPU appearance remains local");
            humans=0x3fff;check(!rr64_rider_skin_actor_selection(m,node),"guest mapping protects every remote human");
            online=false;host=true;local=0;
            half(actors+canonical*0x118+0x26,0);
            check(!rr64_rider_skin_actor_selection(m,node),"offline human never borrows CPU selection");
            half(actors+canonical*0x118+0x26,1);put(actors+canonical*0x118+0x1c,40);
            check(!rr64_rider_skin_actor_selection(m,node),"police keep identifiable native uniforms");
        }
    }
    check(customs>0&&stock>0,"random roster contains stock and custom looks");
    skin::set_random_cpu(false);rr64_rider_skin_race_begin(m);
    for(unsigned i=0;i<14;++i)check(!skin::random_cpu_selection(i,0),"off restores original next race");
    skin::set_menu_selection(0,4);put(selected,10);rr64_rider_skin_remember(m,0);prefs::flush();
    prefs::initialize(directory);skin::clear_menu();half(0x8009eae8,0);put(selected,0);
    rr64_rider_skin_restore(m,0,0);check(skin::menu_selection(0)==4 && word(selected)==10,"restart restores stable appearance and female donor");
    put(selected,17);half(0x8009eae8,1);rr64_rider_skin_restore(m,0,0);check(word(selected)==17,"loaded/manual native choice is preserved");
    rr64_rider_skin_campaign_load(m,0x800d6a40);check(!skin::menu_selection(0),"unrelated campaign donor stays original");
    put(0x800d6a58,10);rr64_rider_skin_campaign_load(m,0x800d6a40);check(skin::menu_selection(0)==4,"matching loaded campaign donor restores cosmetic identity");
    put(selected,7);rr64_rider_skin_remember(m,0);check(prefs::last(0).empty(),"choosing stock clears only cosmetic preference");
    skin::set_menu_selection(0,4);put(selected,10);cop=true;check(rr64_rider_skin_cycle(m,0,11)==11&&!skin::menu_selection(0),"cop selector restrictions remain native");cop=false;
    mode(33);recomp_context c{};c.r29=engine::guest_address(0x807f0000);const auto saved=c;
    rr64_rider_skin_selection_hint(m,&c);check(labels==1&&!std::memcmp(&c,&saved,sizeof(c)),"footer draw preserves caller registers");
    const auto* pointer=skin::appearance(4);skin::unavailable("disabled");
    check(!skin::enabled()&&!skin::menu_selection(0)&&rr64_rider_skin_cycle(m,0,11)==11,"disabled mod restores untouched native selector");
    check(pointer->donor==10&&skin::install_catalog(catalog)&&skin::appearance(4)==pointer,"identical rescan preserves published texture lifetime");
    skin::begin_session();skin::unavailable("attempted hot disable");check(skin::enabled()&&!skin::can_change(),"session blocks destructive catalog change");
    std::printf("Rider skin menu: %u checks passed.\n",checks);
}
