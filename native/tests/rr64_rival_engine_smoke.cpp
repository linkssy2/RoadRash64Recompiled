#include "rr64_rival_engine.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_replay.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <vector>

extern "C" {
#define DECL(n) void func_##n(unsigned char*,recomp_context*);
DECL(800571DC) DECL(800597A0) DECL(8005980C) DECL(80080450) DECL(80082798)
DECL(800806B4) DECL(80080768) DECL(800807C0) DECL(80080820) DECL(80080890)
DECL(80083210) DECL(80083408) DECL(800817C0)
DECL(80081914)
DECL(80082180)
DECL(800824E8)
#undef DECL
void stock_func_800571DC(unsigned char*,recomp_context*);
void stock_func_8005980C(unsigned char*,recomp_context*);
void rival_native_pan_coefficients(unsigned char*,recomp_context*);
void rival_host_stereo(std::int16_t*,std::size_t,std::int16_t*);
void rr64_rival_engine_audio_reset();
// A native divide trap is a test failure, never an ignored arithmetic error.
void do_break(std::uint32_t vram) {
    std::fprintf(stderr, "Unexpected native break at %08x\n", unsigned(vram));
    std::abort();
}
}
namespace {
std::atomic<unsigned long long> allocations{0};
unsigned checks=0, native_pan=0, native_volume=0;float native_ratio=0;int highlights=0;
std::array<unsigned,3> scans{};
std::array<unsigned,7> native_calls{};
unsigned rules_queries=0;
rr64::netplay::PhysicsRules rules{};
constexpr unsigned actors=0x800D8570,stack=0x807FF000,pool_base=0x80600000;
constexpr unsigned bank=0x800C1510,voice_stride=0x13C,sample_bank=0x80700000;
std::vector<unsigned char> initial,memory;
unsigned char *m=nullptr;
void check(bool v,const char*name){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
unsigned word(unsigned a){unsigned v=0;rr64::engine::read_u32(m,a,v);return v;}
unsigned half(unsigned a){std::uint16_t v=0;rr64::engine::read_u16(m,a,v);return v;}
void put(unsigned a,unsigned v){rr64::engine::write_u32(m,a,v);}
void shortword(unsigned a,unsigned v){rr64::engine::write_u16(m,a,std::uint16_t(v));}
void scalar(unsigned a,float v){put(a,std::bit_cast<unsigned>(v));}
float value(unsigned a){return std::bit_cast<float>(word(a));}
unsigned actor(unsigned s){return actors+s*0x118;}
unsigned bike(unsigned s){return 0x80100000+s*rr64::engine::bike::stride;}
unsigned body(unsigned s){return 0x80300000+s*rr64::engine::rider::stride;}
unsigned cache(unsigned s){return rr64::engine::racer_audio::cache+s*rr64::engine::racer_audio::row_stride;}
unsigned row(unsigned i){return pool_base+i*voice_stride;}
unsigned row_for(unsigned handle){for(unsigned i=4;i<word(0x800DF6E4);++i)if(word(row(i)+4)&&word(row(i)+0x44)==handle)return row(i);return 0;}
unsigned active(){unsigned n=0;for(unsigned i=4;i<word(0x800DF6E4);++i)n+=word(row(i)+4)!=0;return n;}
unsigned managed(){unsigned n=0;for(unsigned i=0;i<14;++i)n+=row_for(word(cache(i)))!=0;return n;}
void vec(unsigned a,float x,float y,float z=0){scalar(a,x);scalar(a+4,y);scalar(a+8,z);}
void position(unsigned slot,float x,float y,float z=0){
    vec(bike(slot)+rr64::engine::bike::body_position,x,y,z);vec(bike(slot)+0x16C,x,y,z);
    vec(body(slot)+0x8C,x,y,z);vec(bike(slot)+0x8C,x,y,z);
    vec(bike(slot)+rr64::engine::bike::front_wheel_position,x,y+1,z);
    vec(bike(slot)+rr64::engine::bike::rear_wheel_position,x,y-1,z);
}
recomp_context context(unsigned arg=0){recomp_context c{};c.r29=rr64::engine::guest_address(stack);c.r4=rr64::engine::guest_address(arg);return c;}
using Fn=void(*)(unsigned char*,recomp_context*);
void call(Fn f,unsigned arg){auto c=context(arg);f(m,&c);check(c.r29==rr64::engine::guest_address(stack),"native stack restored");}
void settle_releases(){
    // Models the eventual audio worker release only; allocation/update/stop
    // requests themselves execute the actual native code above.
    for(unsigned i=4;i<word(0x800DF6E4);++i)
        if(word(row(i)+4)&&word(row(i)+0x10)!=~0u){put(row(i)+4,0);put(row(i)+0x44,0);}
}
void frame(float seconds=1.f/60.f,bool releases=true,bool dispatch=false){
    scalar(0x800A1820,value(0x800A1820)+seconds);put(0x800A1830,word(0x800A1830)+1);
    // Production runs each rider's native audio/stale-cache sweep before the
    // manager. Opt in so isolated disabled-path checks and timing stay scoped.
    if(dispatch)for(unsigned s=0;s<14;++s)
        if(half(actor(s)+0x24)&&static_cast<int>(word(actor(s)+8))<0)
            call(func_8005980C,bike(s));
    auto c=context();auto before=c;rr64_rival_engine_frame(m,&c);
    check(std::memcmp(&c,&before,sizeof(c))==0,"manager preserves caller registers");
    if(releases)settle_releases();
}
void frames(unsigned n=20){for(unsigned i=0;i<n;++i)frame();}
void reset(unsigned effect_count=16,unsigned humans=1){
    if(m){auto c=context();rr64_rival_engine_mode(m,&c,0);}
    rr64_rival_engine_audio_reset();rules={};highlights=0;
    memory=initial;m=memory.data();
    put(0x800DF6E4,effect_count+4+14);put(0x800DF6EC,pool_base);put(0x800DF6F0,pool_base+4*voice_stride);
    put(0x800DF6F4,60);put(0x800DF700,1);put(0x800DF718,bank);put(0x800DF71C,0);
    put(rr64::engine::globals::main_mode,9);put(rr64::engine::globals::pending_mode,9);
    put(rr64::engine::local_race::humans,humans);scalar(0x800A1820,10);scalar(0x800A1818,100);put(0x800A1830,100);
    for(unsigned i=0;i<4;++i){put(0x800A657C+i*4,i);vec(0x800B7418+i*0x24,0,0);vec(0x800B7424+i*0x24,0,1);}
    for(unsigned s=0;s<14;++s){
        put(actor(s),s);put(actor(s)+8,s<humans?s:~0u);shortword(actor(s)+0x24,1);
        put(actor(s)+0xE0,bike(s));put(actor(s)+0xE4,body(s));
        put(bike(s)+4,actor(s));put(body(s)+4,actor(s));
        put(bike(s)+rr64::engine::bike::rider_pointer,body(s));put(body(s)+rr64::engine::rider::bike_pointer,bike(s));
        shortword(bike(s)+rr64::engine::bike::rider_attached,1);shortword(body(s)+rr64::engine::rider::bike_attached,1);
        put(bike(s),0);scalar(bike(s)+0xC,600);scalar(bike(s)+0x490,0);scalar(bike(s)+0x494,0);scalar(bike(s)+0x498,10);
        position(s,s?1000.f+float(s):0,0);call(func_800597A0,bike(s));
    }
    rr64::rival_engine::set_volume_percent(35);
    rr64::rival_engine::set_enabled(true);
}
unsigned start_native(unsigned effect=0xEF,unsigned priority=100,bool unique=false){auto c=context(effect);c.r5=100;c.r6=128;c.r7=unique;put(stack+16,priority);func_80080450(m,&c);return unsigned(c.r2);}
void child_voice(unsigned parent){auto c=context(parent);c.r5=rr64::engine::guest_address(0x807E0300);m[0x7E0300^3]=0x80;m[0x7E0301^3]=0xEF;func_80083408(m,&c);check(c.r29==rr64::engine::guest_address(stack),"native child allocator restores stack");}
std::array<unsigned,2> coefficients(unsigned effective_pan){
    constexpr unsigned env=0x807E0000;auto c=context();c.r16=rr64::engine::guest_address(env);c.r21=rr64::engine::guest_address(0x800A8360);c.r8=127;
    shortword(env+0x58,effective_pan);shortword(env+0x5A,32767);rival_native_pan_coefficients(m,&c);
    return {half(env+0x5C),half(env+0x5E)};
}
unsigned effective_pan(unsigned voice){
    // Actual authored engine pan command, then actual mixer update logic.
    auto c=context(voice);c.r5=rr64::engine::guest_address(0x807E0100);m[0x7E0100^3]=127;func_80083210(m,&c);
    c=context(voice);c.r5=4;func_800817C0(m,&c);return native_pan;
}
void worker_pitch_and_gain(unsigned voice) {
    // 80F0C passes this producer pitch into the real worker pitch conversion,
    // then updates the physical gain. A live handle alone cannot prove sound.
    auto c=context(voice);c.r5=(voice-pool_base)/voice_stride;c.r6=word(voice+0x30);
    func_80081914(m,&c);
    c=context(voice);c.r5=(voice-pool_base)/voice_stride;
    func_800817C0(m,&c);
}
void initialize_engine_note(unsigned voice) {
    const unsigned program=word(voice+4);
    auto byte=[](unsigned a){return unsigned(m[(a&0x7fffff)^3]);};
    // Validate the ROM's common engine program before using its authored
    // note, instrument and full envelope. No sound samples or device run.
    check(byte(program)==0x81&&byte(program+1)==0x80&&byte(program+3)==0x84&&
              byte(program+4)==1&&byte(program+5)==127&&byte(program+6)==1&&
              byte(program+7)==127&&byte(program+8)==1&&byte(program+9)==127&&
              byte(program+10)==16&&byte(program+11)==0x9c&&byte(program+12)==127&&
              byte(program+13)==0xa6&&byte(program+14)==127&&byte(program+15)==48,
          "worker fixture validates the original engine note program");
    const unsigned instrument=byte(program+2);
    const unsigned sample=half(word(bank+0x14)+instrument*2);
    check(sample<word(sample_bank+0x20),"engine note uses a valid ROM sample");
    scalar(voice+0x2c,float(byte(program+15))+value(word(sample_bank+0x28)+sample*4));
    m[((voice+0xbb)&0x7fffff)^3]=127;
    m[((voice+0xc4)&0x7fffff)^3]=127;
    shortword(0x800df6fc,32767);
}
}
void* operator new(std::size_t n){allocations.fetch_add(1,std::memory_order_relaxed);if(auto*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p) noexcept {std::free(p);}
void operator delete(void*p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void*p) noexcept {std::free(p);}
namespace rr64::netplay {PhysicsRules get_physics_rules(){++rules_queries;return rules;}}
extern "C" void rr64_rival_engine_test_scan(unsigned kind){++scans.at(kind);}
extern "C" void rr64_rival_engine_fixture_call(unsigned address){
    constexpr std::array<unsigned,7> functions{0x800571dc,0x80080450,0x80082798,0x800806b4,0x800807c0,0x80080820,0x80080890};
    for(unsigned i=0;i<functions.size();++i)if(functions[i]==address){++native_calls[i];return;}
    check(false,"unexpected native fixture trace entry");
}
extern "C" int rr64_highlights_presenting(){return highlights;}
extern "C" void _nsqrtf(unsigned char*,recomp_context*c){c->f0.fl=std::sqrt(c->f12.fl);}
extern "C" void func_80087070(unsigned char*,recomp_context*c){native_volume=unsigned(c->r5);}
extern "C" void func_80086F50(unsigned char*,recomp_context*c){native_pan=unsigned(c->r5);}
extern "C" void func_80086FE0(unsigned char*,recomp_context*c){native_ratio=std::bit_cast<float>(unsigned(c->r5));}
extern "C" void osWritebackDCacheAll_recomp(unsigned char*,recomp_context*){}
// Other sustained racers' producers are outside this fixture's engine scope.
extern "C" void func_80056F48(unsigned char*,recomp_context*){}
extern "C" void func_80057578(unsigned char*,recomp_context*){}
extern "C" void func_800576D4(unsigned char*,recomp_context*){}
extern "C" void func_80057860(unsigned char*,recomp_context*){}

#include "rr64_rival_doppler_cases.hpp"
#include "rr64_rival_engine_init_cases.hpp"

int main(int argc,char**argv){
    check(argc==2,"private ROM argument");std::ifstream input(argv[1],std::ios::binary);
    std::vector<unsigned char> rom((std::istreambuf_iterator<char>(input)),{});
    check(rom.size()>0x1E18380&&rom[0]==0x80&&rom[1]==0x37,"supported big endian ROM");
    initial.resize(8*1024*1024);m=initial.data();
    for(unsigned i=0x400;i<0xD0000;++i)m[i^3]=rom[i+0xC00];
    for(unsigned i=0;i<0x12220;++i)m[((bank&0x7FFFFF)+i)^3]=rom[0x1E06160+i];
    for(unsigned effect=0;effect<word(bank);++effect)put(bank+0x18+effect*8,bank+word(bank+0x18+effect*8));
    put(bank+0x14,bank+word(bank+0x14));
    // Relocate the real ROM tuning metadata with the original initializer.
    // The scratch address keeps it separate from actors and the fixture heap.
    for(unsigned i=0;i<0xd7f0;++i)m[((sample_bank&0x7fffff)+i)^3]=rom[0x1305500+i];
    auto sample_init=context(sample_bank);sample_init.r5=0xB1312CF0u;
    func_80082180(m,&sample_init);
    // Keep the bank initialized without opening its sample bank or a device.
    put(bank+0x10,0);m=nullptr;
    test_rival_engine_init_cases();reset();
    check(rr64::rival_engine::get_volume_percent()==35,"default rival volume");
    check(rr64::rival_engine::get_enabled(),"rival engines default enabled");
    rr64::rival_engine::set_enabled(false);position(1,8,0);
    const auto first_scans=scans;
    const auto first_calls=native_calls;
    const auto first_rules=rules_queries;
    frames();
    check(active()==0 && scans==first_scans && native_calls==first_calls && rules_queries==first_rules,
          "disabled first race frame never prepares a rival or requests a voice");
    rr64::rival_engine::set_enabled(true);
    for(double bad:{-5.,200.,double(NAN)}){rr64::rival_engine::set_volume_percent(bad);check(rr64::rival_engine::get_volume_percent()>=0&&rr64::rival_engine::get_volume_percent()<=100,"bounded volume setting");}
    // Negative control proves the native dispatcher excludes AI engines.
    reset();position(1,8,0);call(stock_func_8005980C,bike(1));check(active()==0,"stock AI gate has no engine");
    frames();
    check(active()==1&&row_for(word(cache(1))),"managed AI uses real native engine producer");
    check(word(row_for(word(cache(1)))+0x48)==0,"managed engine lower priority than native effects");
    check(word(cache(0))==~0u,"local human is never duplicated");
    // A playing handle receives continuous native pan/volume updates.
    const auto handle=word(cache(1));const auto right=coefficients(effective_pan(row_for(handle)));
    check(right[1]>right[0],"right source reaches physical right coefficient");
    shortword(0x807E0200,right[0]);shortword(0x807E0202,right[1]);std::array<std::int16_t,2> host{};
    rival_host_stereo(reinterpret_cast<std::int16_t*>(m+0x7E0200),2,host.data());
    check(host[0]==right[0]&&host[1]==right[1]&&host[1]>host[0],"production host endian correction preserves logical left/right");
    position(1,-8,0);frames();check(word(cache(1))==handle,"crossing listener retains engine handle");
    const auto left=coefficients(effective_pan(row_for(handle)));check(left[0]>left[1],"left source reaches physical left coefficient");
    const auto near_gain=half(row_for(handle)+0x9E);position(1,-60,0);frames();check(half(row_for(handle)+0x9E)<near_gain,"distance reduces native engine gain");
    position(1,-400,0);frames();check(active()==0,"out-of-range engine fades and releases");
    // Clock-based fades are monotonic and do not allocate a second voice.
    reset();position(1,0,15);unsigned old_gain=0;for(unsigned i=0;i<5;++i){frame(.01f);auto h=word(cache(1));check(row_for(h)&&half(row_for(h)+0x9E)>=old_gain,"fade in monotonic");old_gain=half(row_for(h)+0x9E);check(active()==1,"fade keeps one engine voice");}
    rr64::rival_engine::set_volume_percent(0);frames(60);check(active()==0,"zero slider fades to disabled");
    // Feature-off is stronger than zero volume: one native stop request, then
    // no listeners/profiles/rules, producers, mixer updates or allocations.
    reset();position(1,8,0);call(func_8005980C,bike(0));const auto own_engine=word(cache(0));
    frames();const auto owned_handle=word(cache(1)),owned_parent=row_for(owned_handle);
    child_voice(owned_parent);check(active()==3,"off fixture owns parent and child beside local engine");
    rr64::rival_engine::set_enabled(false);
    const auto before_child=active();child_voice(owned_parent);
    check(active()==before_child,"disabled queued owned child cannot allocate before cleanup");
    frame(1.f/60.f,false);
    check(row_for(owned_handle) && word(row_for(owned_handle)+0x10)!=~0u,
          "feature off requests immediate native owned-loop stop without waiting for volume fade");
    settle_releases();check(active()==1 && row_for(own_engine),"off cleanup removes owned parent/children and preserves own engine");
    const auto quiet_scans=scans;
    const auto quiet_native=native_calls;
    const auto quiet_rules=rules_queries;
    const auto quiet_allocations=allocations.load();
    frames(120);
    check(scans==quiet_scans && rules_queries==quiet_rules,"disabled frames skip listener, bike, profile and online-rules processing");
    check(native_calls==quiet_native && allocations.load()==quiet_allocations,"disabled frames make no native voice start/update/allocation or host allocation calls");
    const auto unrelated=start_native();check(row_for(unrelated) && row_for(own_engine),"disabled setting preserves shared player samples and unrelated effects");
    rr64::rival_engine::set_enabled(true);frames();
    check(active()==3 && row_for(word(cache(1))) && word(cache(0))==own_engine && row_for(unrelated),"reenable restores one rival without duplicating own engine or unrelated sounds");
    // Start cues take a different native early-return path than steady loops.
    reset();put(bike(1),12);position(1,8,0);scalar(0x800A1818,0);scalar(0x800A1820,0);frame();
    check(row_for(word(cache(1)))!=0,"native start cue can begin quietly");
    frames(4);auto cue=word(cache(1));auto cue_gain=half(row_for(cue)+0x9E);rr64::rival_engine::set_volume_percent(5);frame(.01f);
    check(word(cache(1))==cue&&half(row_for(cue)+0x9E)<cue_gain,"native start cue follows fade while retaining handle");
    // Every nearby opponent receives a dedicated row in both quality presets.
    for(unsigned effects:{8u,16u}){reset(effects);for(unsigned s=1;s<14;++s)position(s,float(s),10);frames();check(managed()==13&&active()==13,"all thirteen nearby rivals have engines");
        for(unsigned s=1;s<14;++s)position(s,1000+float(s),0);position(13,0,5);frames(30);check(row_for(word(cache(13)))&&active()==1,"later AI can become audible after selection changes");}
    // Ordinary SFX and music can exhaust/steal their original pool without
    // touching an engine. Test the separate unique-effect reuse path as well.
    for(unsigned effects:{8u,16u})for(unsigned riders:{1u,2u,3u,13u}){
        reset(effects);for(unsigned s=1;s<=riders;++s)position(s,float(s),10);frames();
        std::array<unsigned,13> handles{};for(unsigned s=1;s<=riders;++s)handles[s-1]=word(cache(s));
        for(unsigned i=0;i<effects;++i)check(start_native()!=0,"original SFX capacity retained beside rivals");
        for(unsigned i=0;i<effects*4;++i){
            start_native(0xEF,101+i);
            start_native(half(row_for(handles[0])+0xA6),200,true);
            auto music=context(0x807E0800);music.r5=i;func_800824E8(m,&music);
            check(unsigned(music.r2)<effects+4,"music allocator cannot borrow dedicated engines");
            frame(1.f/60.f,true,true);
            for(unsigned s=1;s<=riders;++s)
                check(word(cache(s))==handles[s-1]&&row_for(handles[s-1]),"SFX pressure cannot evict or restart nearby engines");
        }
        check(active()==effects+riders,"native effects and every nearby engine coexist");
    }
    reset();position(1,5,0);frames();
    // Stop is a request, not a free row: a queued release must count against admission.
    auto pending=word(cache(1));auto c=context(pending);c.r5=0;func_800806B4(m,&c);
    check(row_for(pending)&&active()==1,"actual native stop retains voice until audio worker");frame(1.f/60.f,false);check(active()==1,"queued releases are not treated as free");
    // Native script fanout shares a handle; budget counts rows, not handles.
    reset();position(1,8,0);frames();auto parent=row_for(word(cache(1)));auto parent_handle=word(cache(1));
    for(unsigned i=0;i<12;++i)child_voice(parent);check(active()==13,"owned child voices consume row budget");
    for(unsigned i=20;i<33;++i)check(word(row(i)+0x44)==parent_handle&&word(row(i)+0x48)==0,"child adopts owned parent handle at low priority");
    child_voice(parent);check(active()==13,"script children cannot occupy the RPM handoff row");
    put(cache(1),~0u);auto orphan_context=context();rr64_rival_engine_mode(m,&orphan_context,0);settle_releases();check(active()==0,"cleanup releases owned orphan and all child rows");
    // Existing local split-screen player engines stay native and AI is shared.
    reset(16,4);for(unsigned s=0;s<4;++s){position(s,float(s)*100,0);call(func_8005980C,bike(s));}
    const std::array<unsigned,4> human_handles={word(cache(0)),word(cache(1)),word(cache(2)),word(cache(3))};
    position(13,301,5);frames();check(active()==5&&row_for(word(cache(13))),"AI uses closest of four listeners once");
    for(unsigned s=0;s<4;++s)check(word(cache(s))==human_handles[s],"local multiplayer native engine unchanged");
    rr64::rival_engine::set_enabled(false);frame();
    check(active()==4,"local multiplayer off removes managed AI only");
    for(unsigned s=0;s<4;++s){call(func_8005980C,bike(s));check(word(cache(s))==human_handles[s],"off keeps each split-screen player's normal own engine");}
    // Online human opponents are positional and their original dispatcher is gated.
    for(bool replicated:{false,true})for(unsigned local=0;local<(replicated?14u:4u);++local){
        reset();rules.active=rules.connected=true;rules.phase=rr64::netplay::Phase::Race;rules.local_slot=local;rules.replicated_riders=replicated;
        const unsigned own=replicated?0:local,remote=own?0:1;put(actor(own)+8,0);put(actor(remote)+8,1);
        position(own,0,0);position(remote,8,0);put(0x800A657C+(replicated?0:local)*4,own);
        call(func_8005980C,bike(remote));check(active()==0,"online remote original producer gated");
        call(func_8005980C,bike(own));const auto own_handle=word(cache(own));frames();
        check(active()==2&&word(cache(own))==own_handle,"online local native plus one remote managed engine");
        rr64::rival_engine::set_enabled(false);frame();
        call(func_8005980C,bike(remote));call(func_8005980C,bike(own));frames();
        check(active()==1 && word(cache(own))==own_handle && !row_for(word(cache(remote))),
              "off keeps online remote native dispatcher suppressed for every ownership mapping");
        rr64::rival_engine::set_enabled(true);frames();
        check(active()==2 && word(cache(own))==own_handle,"online reenable restores one managed remote and no duplicate own engine");
        auto private_memory=memory;auto private_ctx=context();const auto before=memory;
        {rr64::prediction::ReplayScope replay;rr64_rival_engine_frame(private_memory.data(),&private_ctx);rr64_rival_engine_mode(private_memory.data(),&private_ctx,0);rr64_rival_engine_gate(private_memory.data(),&private_ctx);}
        check(private_memory==before&&memory==before,"private replay cannot produce, stop or alter live engine state");
    }
    // Bike source and listening rider must separate after an eject.
    reset();for(unsigned s=2;s<14;++s)position(s,3000+float(s),0);position(1,8,0);frames();auto near=half(row_for(word(cache(1)))+0x9E);
    shortword(body(0)+rr64::engine::rider::bike_attached,0);vec(body(0)+0x8C,1000,0);frames(60);check(active()==0,"fallen listener follows body away from parked own bike");
    position(1,1008,0);frames();check(row_for(word(cache(1)))&&half(row_for(word(cache(1)))+0x9E)==near,"engine source stays on rival bike near fallen listener");
    vec(0x800B7418,1000,0);vec(0x800B7424,1000,-1);frames();const auto turned=coefficients(effective_pan(row_for(word(cache(1)))));check(turned[0]>turned[1],"camera turn reverses stereo while fallen");
    // Lifecycle cases must release all owned voices and leave unrelated effects.
    for(unsigned mode=0;mode<9;++mode){reset();position(1,8,0);frames();const auto sound=start_native();
        if(mode==0)shortword(actor(1)+0x24,0);
        if(mode==1)shortword(bike(1)+rr64::engine::bike::rider_attached,0);
        if(mode==2)shortword(rr64::engine::globals::gameplay_pause_state,1);
        if(mode==3)highlights=1;
        if(mode==4)put(rr64::engine::globals::main_mode,0);
        if(mode==5){rules.active=true;rules.connected=false;}
        if(mode==6){auto ctx=context();rr64_rival_engine_recovery(m,&ctx,actor(1));position(1,1000,0);}
        if(mode==7){auto ctx=context();rr64_rival_engine_mode(m,&ctx,0);put(rr64::engine::globals::main_mode,0);}
        if(mode==8){auto ctx=context();rr64_rival_engine_mode(m,&ctx,0);put(rr64::engine::globals::pending_mode,0);}
        frames();check(active()==1&&row_for(sound),"disabled/dead/paused/highlight/menu/recovery cleanup preserves unrelated effect");
    }
    // The original table has 32 model entries, including later choppers,
    // Scooter, both Insanity bikes and cop variants. Reserved model 22 points
    // at unrelated packed data and must never enter the native producer.
    for(unsigned type=0;type<32;++type){
        reset();put(bike(0),type);put(bike(1),25);position(1,8,0);frames();
        check(row_for(word(cache(1)))!=0,"every local model remains a listener, including reserved model");
    }
    for(unsigned type:{22u,32u,0xFFFFFFFFu}){
        reset();put(bike(1),type);position(1,8,0);put(bike(2),26);position(2,10,0);frames();
        check(!row_for(word(cache(1)))&&row_for(word(cache(2)))&&active()==1,"invalid source profile cannot silence a valid rival");
    }
    for(unsigned bad:{0u,0x800A4981u,0x90000000u}){
        reset();put(0x800A4B48,bad);position(1,8,0);put(bike(2),26);position(2,10,0);frames();
        check(!row_for(word(cache(1)))&&row_for(word(cache(2))),"malformed source table entry leaves local listener intact");
    }
    for(unsigned offset=0;offset<16;offset+=4){
        reset();put(word(0x800A4B48)+offset,word(bank));position(1,8,0);frames();
        check(active()==0,"each authored effect index is checked before native playback");
    }

    // Use actual RPM branches: values below 1000 only exercise idle. At 100%
    // and close distance the native per-model gain must equal a local engine.
    // Still positions make the optional Doppler contribution exactly zero.
    for(unsigned type=0;type<32;++type)for(unsigned phase=0;phase<4;++phase){
        if(type==22)continue;
        reset();rr64::rival_engine::set_volume_percent(100);position(1,8,0);
        for(unsigned slot:{1u,2u}){
            put(bike(slot),type);scalar(bike(slot)+0xC,6000);
            scalar(bike(slot)+0x490,phase==1?3500.f:phase==2?4000.f:500.f);
            scalar(bike(slot)+0x494,phase==1?4000.f:0.f);
            scalar(bike(slot)+0x498,phase==1?3000.f:phase==2?3500.f:1000.f);
        }
        put(actor(2)+8,2);
        if(phase==3){scalar(0x800A1818,0);scalar(0x800A1820,0);}
        frames(60);const auto managed_row=row_for(word(cache(1)));
        check(managed_row!=0,"all valid bike models play through idle acceleration coast and start");
        const auto effect=half(managed_row+0xA6),gain=half(managed_row+0x9E);
        const auto pitch=value(managed_row+0x30);
        const auto profile=word(0x800A4B48+type*4);
        const unsigned expected_offset=phase==3?0:phase==1?8:phase==2?12:value(profile+0x24)>0?4:12;
        check(effect==word(profile+expected_offset),"engine branch selects its authored model sample");
        call(stock_func_800571DC,bike(2));const auto native_row=row_for(word(cache(2)));
        check(native_row&&half(native_row+0xA6)==effect&&value(native_row+0x30)==pitch,"managed engine retains exact native model sample and stationary pitch");
        check(half(native_row+0x9E)==gain,"maximum close rival gain equals same model local engine");
        if(phase==3){
            // The fixture models worker completion, not sample rendering: end
            // each start cue and verify the real producer enters its loop.
            for(unsigned slot:{1u,2u}){auto end=context(word(cache(slot)));end.r5=0;func_800806B4(m,&end);}
            settle_releases();scalar(0x800A1818,10);frames();call(stock_func_800571DC,bike(2));
            const auto loop=row_for(word(cache(1))),original=row_for(word(cache(2)));
            const unsigned idle_offset=value(profile+0x24)>0?4:12;
            check(loop&&original&&half(loop+0xA6)==word(profile+idle_offset)&&half(original+0xA6)==half(loop+0xA6),"every completed start cue transitions to the correct model loop");
        }
    }
    // Exercise the actual dispatcher/manager ordering, including the native
    // one-frame cache grace period, with small packs and every steady RPM phase.
    for(unsigned count=1;count<=3;++count)for(unsigned type:{0u,12u,25u,26u}){
        reset();
        for(unsigned s=1;s<=count;++s){position(s,float(s),5);put(bike(s),type);scalar(bike(s)+0xC,6000);}
        for(unsigned phase:{0u,1u,2u,0u}){
            for(unsigned s=1;s<=count;++s){
                scalar(bike(s)+0x490,phase==1?3500.f:phase==2?4000.f:500.f);
                scalar(bike(s)+0x494,phase==1?4000.f:0.f);
                scalar(bike(s)+0x498,phase==1?3000.f:phase==2?3500.f:1000.f);
            }
            std::array<unsigned,3> handles{};
            for(unsigned tick=0;tick<8;++tick){
                frame(1.f/60.f,true,true);
                for(unsigned s=1;s<=count;++s){
                    const auto handle=word(cache(s));
                    check(row_for(handle)!=0,"native AI dispatcher retains every nearby managed engine");
                    check(word(cache(s)+0xC)==word(0x800A1830),"manager refreshes native cache epoch after dispatcher");
                    if(tick>=4)check(handle==handles[s-1],"settled RPM loop retains handle across native stale sweeps");
                    handles[s-1]=handle;
                }
            }
        }
    }
    // A genuinely stale, unmanaged loop must still be stopped by the same
    // dispatcher; retaining managed engines must not disable native cleanup.
    const auto stale=start_native(),unrelated_loop=start_native();
    put(cache(4),stale);put(cache(4)+4,0xEF);put(cache(4)+0xC,word(0x800A1830)-2);
    call(func_8005980C,bike(4));settle_releases();
    check(word(cache(4))==~0u&&!row_for(stale),"native dispatcher releases a genuinely stale unmanaged loop");
    check(row_for(unrelated_loop)&&managed()==3,"stale cleanup preserves current managed engines and unrelated sounds");

    // Retain nearby detail and extend useful range without increasing voices.
    unsigned preceding_gain=0;
    for(float distance:{0.f,8.f,12.f,60.f,130.f,200.f,300.f,320.f,400.f}){
        reset();rr64::rival_engine::set_volume_percent(100);position(1,distance,0);frames(60);
        const auto voice=row_for(word(cache(1))),gain=voice?half(voice+0x9E):0;
        if(distance<=12)check(gain>0&&(!preceding_gain||gain==preceding_gain),"close range retains full local-equivalent level");
        else check(gain<=preceding_gain,"extended distance curve fades monotonically");
        if(distance==130||distance==200)check(gain>0,"rival stays audible beyond the previous range");
        if(distance>=320)check(!voice,"extended range ends in released silence");
        preceding_gain=gain;
    }
    // An approaching fourth bike cannot fade an existing bike out of the pack.
    reset();position(1,100,0);position(2,150,0);position(3,240,0);frames(60);
    const auto outgoing=word(cache(3));
    check(row_for(outgoing)&&managed()==3,"quiet handoff begins with three native engines");
    unsigned fading_gain=half(row_for(outgoing)+0x9E);
    check(fading_gain>1,"quiet outgoing engine has measurable native gain");
    position(4,200,0);
    for(unsigned i=0;i<60;++i){
        frame();const auto voice=row_for(outgoing),gain=voice?half(voice+0x9E):0;
        check(voice&&gain==fading_gain,"fourth approaching engine does not silence a distant bike");
        check(active()==4&&managed()==4,"four approaching engines play together");
    }

    // Move continuously rather than taking the >80-unit teleport cleanup path.
    // Read native mixer gain; the test does not reproduce the attenuation curve.
    reset();position(1,340,0);frames(60);
    unsigned moving_handle=0,moving_gain=0;
    for(int distance=340;distance>=8;distance-=4){
        position(1,float(distance),0);frame();
        const auto voice=row_for(word(cache(1))),gain=voice?half(voice+0x9E):0;
        check(gain>=moving_gain,"approaching bike gains volume without a silent dip");
        if(voice&&!moving_handle)moving_handle=word(cache(1));
        if(moving_handle)check(voice&&word(cache(1))==moving_handle,"approach retains one continuous engine handle");
        moving_gain=gain;
    }
    frames(60);check(row_for(moving_handle)!=0,"approaching rival reaches listener audibly");
    moving_gain=half(row_for(moving_handle)+0x9E);
    unsigned receding_steps=0;
    for(int distance=12;distance<=340;distance+=4){
        position(1,float(distance),0);frame();
        const auto voice=row_for(word(cache(1))),gain=voice?half(voice+0x9E):0;
        check(gain<=moving_gain,"receding bike fades without restarting louder");
        if(voice)check(word(cache(1))==moving_handle,"receding bike keeps its handle until silence");
        if(gain<moving_gain)++receding_steps;
        moving_gain=gain;
    }
    frames(60);check(!row_for(moving_handle)&&active()==0&&receding_steps>3,
                     "gradual recession has intermediate gains before releasing silence");

    // Real model0 metadata makes idle audible at280 but its coast clip falls
    // below the native start threshold. Preserve the old loop at integer silence
    // instead of stopping it before a replacement which cannot start.
    reset();position(1,280,0);scalar(bike(1)+0xC,6000);frames(60);
    const auto quiet_handle=word(cache(1));
    check(row_for(quiet_handle)&&half(row_for(quiet_handle)+0x9E)>0,"quiet RPM fixture starts an audible idle loop");
    const auto before_quiet_transition=native_calls;
    scalar(bike(1)+0x490,4000);scalar(bike(1)+0x494,0);scalar(bike(1)+0x498,3500);
    frames(20);
    check(word(cache(1))==quiet_handle&&row_for(quiet_handle)&&half(row_for(quiet_handle)+0x9E)<=1,
          "inaudible RPM replacement retains its old handle and lowers native gain");
    check(native_calls[1]==before_quiet_transition[1]&&native_calls[3]==before_quiet_transition[3],
          "below-threshold RPM changes do not stop or reallocate native voices");
    position(1,260,0);frames(60);
    check(row_for(word(cache(1)))&&word(cache(1))!=quiet_handle&&
              half(row_for(word(cache(1)))+0xA6)==word(word(0x800A4B48)+12),
          "closer rival transitions to its correct authored coast sample");
    // Native engine changes stop their old loop before requesting the next.
    // A packed field must retain sound while those stops await the audio worker.
    for(unsigned type:{0u,12u,25u,26u}){
        reset();
        for(unsigned s=1;s<=13;++s){position(s,float(s),5);put(bike(s),type);scalar(bike(s)+0xC,6000);}
        frames();check(managed()==13,"transition fixture begins with all thirteen audible rivals");
        for(unsigned phase:{1u,2u,0u,1u}){
            for(unsigned s=1;s<=13;++s){
                scalar(bike(s)+0x490,phase==1?3500.f:phase==2?4000.f:500.f);
                scalar(bike(s)+0x494,phase==1?4000.f:0.f);
                scalar(bike(s)+0x498,phase==1?3000.f:phase==2?3500.f:1000.f);
            }
            for(unsigned i=0;i<16;++i){
                frame();check(managed()==13,"packed idle to acceleration transition has no missing engine");
                check(active()==13,"completed sound transition retains thirteen native rows");
            }
            const auto profile=word(0x800A4B48+type*4);
            const unsigned offset=phase==1?8:phase==2?12:value(profile+0x24)>0?4:12;
            for(unsigned s=1;s<=13;++s)
                check(half(row_for(word(cache(s)))+0xA6)==word(profile+offset),"packed transitions eventually reach every native RPM sample");
        }
    }
    // An occupied transition row is not free, even across several game frames.
    reset();for(unsigned s=1;s<=13;++s){position(s,float(s),5);scalar(bike(s)+0xC,6000);}
    frames();for(unsigned s=1;s<=13;++s){scalar(bike(s)+0x490,3500);scalar(bike(s)+0x494,4000);scalar(bike(s)+0x498,3000);}
    for(unsigned i=0;i<10;++i){frame(1.f/60.f,false);check(active()==14&&managed()==13,"worker delay allows only one retiring transition row");}
    child_voice(row_for(word(cache(1))));check(active()==14,"transition allowance cannot admit a script child");
    settle_releases();frames();check(active()==13&&managed()==13,"worker release completes remaining transitions");
    // Every passby remains audible beside the fallen listener.
    reset();shortword(body(0)+rr64::engine::rider::bike_attached,0);
    for(unsigned s=1;s<=3;++s)position(s,18.f+2.f*s,0);
    frames();position(4,1,0);frames(30);
    check(row_for(word(cache(4)))&&managed()==4,"new passby joins the other engines beside fallen listener");
    test_rival_doppler_cases();
    // Alive handles alone miss worker-side silence. Consume actual authored
    // note tuning, final exponential pitch and mixer gain for every bike.
    for(unsigned type=0;type<32;++type)for(unsigned phase=0;phase<3;++phase){
        if(type==22)continue;
        reset();rr64::rival_engine::set_volume_percent(100);
        put(bike(1),type);position(1,8,0);scalar(bike(1)+0xC,6000);
        scalar(bike(1)+0x490,phase==0?500.f:phase==1?5900.f:6000.f);
        scalar(bike(1)+0x494,phase==1?6000.f:0.f);
        scalar(bike(1)+0x498,phase==0?1000.f:6000.f);
        frames(60);const auto voice=row_for(word(cache(1))),handle=word(cache(1));
        check(voice!=0,"every model reaches its native worker note");
        initialize_engine_note(voice);
        check(value(voice+0x2c)==-12.f,"real engine note retains ROM sample tuning");
        for(float speed:{0.f,-150.f,150.f,-1000.f,1000.f,0.f}){
            vec(bike(1)+0x178,speed,0);frames(90);worker_pitch_and_gain(voice);
            check(word(cache(1))==handle&&m[((voice+0xbb)&0x7fffff)^3]!=0&&
                      half(voice+0xa0)>0&&native_volume==half(voice+0xa0)&&
                      std::isfinite(native_ratio)&&native_ratio>0&&native_ratio<=2,
                  "every RPM and approaching/receding note stays audible in native worker");
        }
    }
    reset();shortword(body(0)+rr64::engine::rider::bike_attached,0);
    for(unsigned s=1;s<14;++s){
        put(bike(s),s%3==0?25:s%3==1?0:12);
        position(s,float(s)*2,5);vec(bike(s)+0x178,0,100);
        scalar(bike(s)+0xC,6000);scalar(bike(s)+0x490,5900);
        scalar(bike(s)+0x494,6000);scalar(bike(s)+0x498,6000);
    }
    frames(90);
    for(unsigned s=1;s<14;++s){
        const auto voice=row_for(word(cache(s)));
        check(voice!=0,"fallen listener retains all thirteen passing engines");
        initialize_engine_note(voice);worker_pitch_and_gain(voice);
        check(half(voice+0xa0)>0&&native_volume==half(voice+0xa0),
              "every mixed-family passby reaches nonzero native physical gain");
    }
    // Control: deliberately invalid pitch still follows the stock silent-note
    // behavior. This proves the check can see silence despite a valid handle.
    reset();const auto clipped_handle=start_native(),clipped_voice=row_for(clipped_handle);
    initialize_engine_note(clipped_voice);scalar(clipped_voice+0x30,50);
    worker_pitch_and_gain(clipped_voice);
    check(row_for(clipped_handle)&&half(clipped_voice+0xa0)==0&&native_volume==0,
          "native over-range pitch control exposes silence behind a live handle");
    scalar(clipped_voice+0x30,0);worker_pitch_and_gain(clipped_voice);
    check(row_for(clipped_handle)&&half(clipped_voice+0xa0)==0,
          "native over-range note remains silent until its next note starts");
    // Bounded producer benchmark: real fourteen-racer/four-view scan and
    // actual native memory operations; no sound device, IO or allocator calls.
    reset(16,4);for(unsigned s=0;s<14;++s)position(s,float(s),5);frames();
    const auto before_alloc=allocations.load();const auto start=std::chrono::steady_clock::now();
    constexpr unsigned iterations=20000;for(unsigned i=0;i<iterations;++i)frame();
    const auto micros=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/iterations;
    check(allocations.load()==before_alloc,"hot path has zero host heap allocations");check(active()==10,"long four-player run sustains every AI engine");
    std::printf("{\"passed\":true,\"checks\":%u,\"benchmark_frames\":%u,\"microseconds_per_frame\":%.3f,\"benchmark_heap_allocations\":0,\"native_allocator\":true,\"native_worker_pitch_gain\":true,\"game_launched\":false}\n",checks,iterations,micros);
}
