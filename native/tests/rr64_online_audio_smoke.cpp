#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_replay.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
#define DECL(n) void func_##n(unsigned char*,recomp_context*); void stock_func_##n(unsigned char*,recomp_context*);
DECL(80059324) DECL(800593F0) DECL(800594BC) DECL(80059588) DECL(80059648)
DECL(800571DC) DECL(800597A0) DECL(8005656C) DECL(80056208)
DECL(80056000) DECL(80058600)
#undef DECL
void audio_switch_from_bike(unsigned char*,recomp_context*);
void audio_switch_from_rider(unsigned char*,recomp_context*);
void stock_audio_switch_from_bike(unsigned char*,recomp_context*);
void stock_audio_switch_from_rider(unsigned char*,recomp_context*);
void rr64_online_audio_owner(unsigned char*,void*);
void rr64_online_audio_listener(unsigned char*,void*);
void rr64_online_audio_weapon_source(unsigned char*,void*,unsigned);
void rr64_online_audio_weapon_gain(unsigned char*,void*);
}
namespace {
rr64::netplay::PhysicsRules rules{};
constexpr unsigned actors=0x800D8570, stack=0x807FF000, out=0x807E0000;
unsigned checks=0, starts=0, updates=0, effect=0, last_handle=0, pan=0;
float volume=0;
void check(bool v,const char *name){++checks;if(!v){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
unsigned actor(unsigned s){return actors+s*0x118;}
unsigned body(unsigned s){return 0x80300000+s*rr64::engine::rider::stride;}
unsigned bike(unsigned s){return 0x80100000+s*rr64::engine::bike::stride;}
void put(unsigned char*m,unsigned a,unsigned v){rr64::engine::write_u32(m,a,v);}
unsigned get(unsigned char*m,unsigned a){unsigned v=0;rr64::engine::read_u32(m,a,v);return v;}
void scalar(unsigned char*m,unsigned a,float v){put(m,a,std::bit_cast<unsigned>(v));}
float value(unsigned char*m,unsigned a){return std::bit_cast<float>(get(m,a));}
void position(unsigned char*m,unsigned slot,float x){
    for(unsigned object:{body(slot),bike(slot)})for(unsigned offset:{0x8Cu,0x16Cu}){
        scalar(m,object+offset,x);scalar(m,object+offset+4,0);scalar(m,object+offset+8,0);
    }

}
recomp_context context(unsigned arg){recomp_context c{};c.r29=rr64::engine::guest_address(stack);c.r4=rr64::engine::guest_address(arg);return c;}
using Fn=void(*)(unsigned char*,recomp_context*);
void call(Fn fn,unsigned char*m,unsigned arg){volume=0;effect=0;auto c=context(arg);fn(m,&c);check(c.r29==rr64::engine::guest_address(stack),"native stack restored");}
void switch_weapon(Fn fn,unsigned char*m,unsigned slot,unsigned weapon){
    put(m,body(slot)+0x5B0,weapon);volume=0;effect=0;pan=0;
    auto c=context(body(slot));c.r16=c.r4;fn(m,&c);
    check(c.r29==rr64::engine::guest_address(stack),"weapon caller and producer restore stack");
    check(get(m,body(slot)+0x5B0)==weapon,"cue does not change selected weapon");
}
float gain(Fn fn,unsigned char*m,unsigned first,unsigned second,bool pair){
    auto c=context(bike(first));
    if(pair){c.r5=rr64::engine::guest_address(bike(second));c.r6=rr64::engine::guest_address(out);c.r7=rr64::engine::guest_address(out+4);put(m,stack+16,out+8);}
    else{c.r5=rr64::engine::guest_address(out);c.r6=rr64::engine::guest_address(out+4);c.r7=rr64::engine::guest_address(out+8);}
    fn(m,&c);check(c.r29==rr64::engine::guest_address(stack),"native gain restores stack");return value(m,out);
}
}
namespace rr64::netplay { PhysicsRules get_physics_rules(){return rules;} }
extern "C" void switch_error(const char*,std::uint32_t,std::uint32_t){check(false,"unexpected native switch target");}
extern "C" void _nsqrtf(unsigned char*,recomp_context*c){c->f0.fl=std::sqrt(c->f12.fl);}
extern "C" void func_80080450(unsigned char*,recomp_context*c){++starts;effect=unsigned(c->r4);volume=float(unsigned(c->r5));pan=unsigned(c->r6);c->r2=last_handle=1000+starts;}
extern "C" void func_80080768(unsigned char*,recomp_context*c){c->r2=1;}
extern "C" void func_800806B4(unsigned char*,recomp_context*){}
extern "C" void func_800807C0(unsigned char*,recomp_context*c){++updates;volume=float(unsigned(c->r5));last_handle=unsigned(c->r4);}
extern "C" void func_80080890(unsigned char*,recomp_context*){}
extern "C" void func_800591BC(unsigned char*,recomp_context*){check(false,"unexpected random attack selector");}
extern "C" void func_800591EC(unsigned char*,recomp_context*){check(false,"unexpected random attack selector");}
extern "C" void func_8005921C(unsigned char*,recomp_context*){check(false,"unexpected random attack selector");}

int main(int argc,char**argv){
    check(argc==2,"private ROM argument");
    std::ifstream stream(argv[1],std::ios::binary);
    std::vector<unsigned char> rom((std::istreambuf_iterator<char>(stream)),{});
    check(rom.size()>=0xD0C00 && rom[0]==0x80 && rom[1]==0x37,"supported big endian ROM input");
    std::vector<unsigned char> memory(8*1024*1024);auto*m=memory.data();
    for(unsigned i=0x400;i<0xD0000;++i)m[i^3]=rom[i+0xC00];
    for(unsigned g=0;g<14;++g){
        put(m,actor(g),g);put(m,actor(g)+8,g);put(m,actor(g)+0xE0,bike(g));put(m,actor(g)+0xE4,body(g));
        put(m,body(g)+4,actor(g));put(m,bike(g)+4,actor(g));position(m,g,0);
    }
    const std::array<Fn,5> patched={func_80059324,func_800593F0,func_800594BC,func_80059588,func_80059648};
    const std::array<Fn,5> original={stock_func_80059324,stock_func_800593F0,stock_func_800594BC,stock_func_80059588,stock_func_80059648};
    unsigned peer_cases=0;
    for(unsigned count:{2u,4u,14u})for(unsigned local=0;local<count;++local){
        ++peer_cases;rules={};rules.active=rules.connected=true;rules.phase=rr64::netplay::Phase::Race;
        rules.local_slot=local;rules.replicated_riders=count>4;rules.authoritative=true;rules.is_host=local==0;
        const unsigned own=count>4?0:local,remote=own==0?1:0;
        position(m,own,10000);
        for(unsigned i=0;i<patched.size();++i){
            float previous=1000;
            for(float distance:{0.f,30.f,100.f,1000.f}){
                position(m,remote,10000+distance);
                const auto loud=gain(original[i],m,remote,remote,i<3);
                const auto actual=gain(patched[i],m,remote,remote,i<3);
                check(loud>100,"negative control gives every native human full gain at any distance");
                check(actual<=previous && actual<loud,"remote native falloff declines with distance");
                check(distance<130?actual>0:actual==0,"nearby remote audible; distant remote silent");previous=actual;
                check(gain(patched[i],m,own,own,i<3)==gain(original[i],m,own,own,i<3),"local own sounds unchanged");
                if(i<3)check(gain(patched[i],m,remote,own,true)==loud,"contact involving listener retains local gain");
            }
            // Offline/local split screen, disconnected sessions, menus and
            // private replay must execute the exact original gain path.
            for(unsigned mode=0;mode<4;++mode){
                auto saved=rules;
                if(mode==0)rules.active=false;
                if(mode==1)rules.connected=false;
                if(mode==2)rules.phase=rr64::netplay::Phase::Lobby;
                if(mode==3){rr64::prediction::ReplayScope replay;check(gain(patched[i],m,remote,remote,i<3)==gain(original[i],m,remote,remote,i<3),"private replay bypasses live audio ownership");}
                else check(gain(patched[i],m,remote,remote,i<3)==gain(original[i],m,remote,remote,i<3),"offline/menu/disconnected sound gain unchanged");
                rules=saved;
            }
        }
        // Verify already-playing native engines update their existing handle
        // when moving from near to far and back. Only the external mixer is modeled.
        const auto source=bike(remote);put(m,source,0);put(m,actor(remote)+8,remote);
        put(m,0x800A1830,100);scalar(m,0x800A1820,10);scalar(m,0x800A1818,100);
        scalar(m,source+0xC,600);scalar(m,source+0x490,0);scalar(m,source+0x494,0);scalar(m,source+0x498,10);
        call(func_800597A0,m,source);position(m,remote,10000);call(func_800571DC,m,source);
        const unsigned handle=last_handle,allocated=starts;check(volume>0,"near engine starts audible");
        position(m,remote,11000);call(func_800571DC,m,source);
        check(starts==allocated && last_handle==handle && volume==0,"distant engine reuses handle at zero gain");
        position(m,remote,10000);call(func_800571DC,m,source);
        check(starts==allocated && last_handle==handle && volume>0,"returning engine restores native gain on same handle");
        // Fixed attack swing path uses the real native weapon producer.
        put(m,body(remote)+0x568,0x803F0000);put(m,0x803F0000,7);put(m,body(remote)+0x564,2);
        const auto attacks=starts;
        position(m,remote,11000);call(func_80056208,m,body(remote));check(starts==attacks && volume==0,"remote distant attack swing uses zero native gain and allocates no voice");
        position(m,remote,10000);call(func_80056208,m,body(remote));check(effect==0x135 && volume>0,"nearby remote attack swing remains audible");
        // Both actual native call sites, all fourteen weapon IDs, real cue
        // dispatch and zero-volume gate. Only the external mixer is modeled.
        const std::array<Fn,2> switchers={audio_switch_from_bike,audio_switch_from_rider};
        const std::array<Fn,2> stock_switchers={stock_audio_switch_from_bike,stock_audio_switch_from_rider};
        for(unsigned path=0;path<2;++path)for(unsigned weapon=1;weapon<=14;++weapon){
            float previous=1000;
            for(float distance:{0.f,30.f,100.f,130.f,1000.f}){
                position(m,remote,10000+distance);
                switch_weapon(stock_switchers[path],m,remote,weapon);
                const float full=volume;const auto native_effect=effect,native_pan=pan;
                check(full>100,"negative control weapon cue is globally loud");
                const float native_falloff=gain(func_80059588,m,remote,remote,false);
                const auto allocated_before=starts;
                switch_weapon(switchers[path],m,remote,weapon);
                check(volume<=previous && volume<full,"remote weapon cue follows distance");previous=volume;
                check(volume==std::trunc(native_falloff),"weapon cue matches actual native distance gain");
                if(distance<130)check(starts==allocated_before+1 && effect==native_effect && pan==native_pan,"near weapon cue keeps native effect and pan");
                else check(starts==allocated_before,"distant weapon cue allocates no mixer voice");
                switch_weapon(switchers[path],m,own,weapon);
                check(volume==full && effect==native_effect && pan==native_pan,"local weapon cue unchanged");
            }
            for(unsigned mode=0;mode<4;++mode){
                auto saved=rules;
                if(mode==0)rules.active=false;
                if(mode==1)rules.connected=false;
                if(mode==2)rules.phase=rr64::netplay::Phase::Lobby;
                if(mode==3){rr64::prediction::ReplayScope replay;switch_weapon(switchers[path],m,remote,weapon);}
                else switch_weapon(switchers[path],m,remote,weapon);
                check(volume>100,"offline/menu/disconnected/private replay weapon cue unchanged");rules=saved;
            }
        }
        // Hooks themselves must be register-only, with no actor/cache writes.
        const auto before=memory;auto c=context(0);c.r2=rr64::engine::guest_address(actor(remote));c.r3=remote;
        rr64_online_audio_owner(m,&c);rr64_online_audio_listener(m,&c);
        check(memory==before,"audio hooks never mutate gameplay or cached voices");
        check(c.r3==static_cast<gpr>(-1) && c.r4==rr64::engine::guest_address(body(own)),"remote ownership and local body selected");
    }
    // A cue binding is consumed once, and cannot leak to another context,
    // stack, guest mapping, effect, private replay or later unscoped sound.
    rules={};rules.active=rules.connected=true;rules.phase=rr64::netplay::Phase::Race;
    rules.local_slot=0;position(m,0,0);position(m,1,1000);
    const auto native_cue_gain=value(m,0x80005978);
    for(unsigned invalid=0;invalid<9;++invalid){
        auto c=context(0);c.r4=0xEB;c.r5=std::bit_cast<unsigned>(native_cue_gain);c.r6=c.r5;c.r7=0;
        unsigned source=body(1);
        if(invalid==1)source=0;
        if(invalid==2)source+=1;
        if(invalid==3)source=body(0); // Local switch must retain full gain.
        rr64_online_audio_weapon_source(m,&c,source);
        c.r29=ADD32(c.r29,-0x20);
        if(invalid==4)c.r29=ADD32(c.r29,-8);
        if(invalid==5)c.r4=0x135;
        const auto before_memory=memory;
        auto expected=c;
        if(invalid==0)expected.r5=0;
        if(invalid==6){auto other=c;rr64_online_audio_weapon_gain(m,&other);check(std::memcmp(&other,&c,sizeof(c))==0,"cue cannot affect another CPU context");}
        if(invalid==8){auto other_memory=memory;rr64_online_audio_weapon_gain(other_memory.data(),&c);}
        if(invalid==7){rr64::prediction::ReplayScope replay;rr64_online_audio_weapon_gain(m,&c);}
        else rr64_online_audio_weapon_gain(m,&c);
        check(std::memcmp(&c,&expected,sizeof(c))==0,"weapon cue only changes valid immediate gain register");
        check(memory==before_memory,"weapon cue never changes guest memory");
        c.r5=std::bit_cast<unsigned>(native_cue_gain);expected=c;
        rr64_online_audio_weapon_gain(m,&c);
        check(std::memcmp(&c,&expected,sizeof(c))==0,"weapon cue binding consumed once");
        call(func_80056000,m,14);
        check(volume==std::trunc(native_cue_gain),"later unscoped cue preserves original native gain");
    }
    // Compare diagonal horizontal separation directly against native 59588;
    // vertical displacement alone must not introduce a different sound curve.
    for(const auto p:std::array<std::array<float,3>,3>{{{3,4,1000},{-30,40,-1000},{0,0,1000}}}){
        for(unsigned object:{body(1),bike(1)})for(unsigned axis=0;axis<3;++axis)scalar(m,object+0x8C+axis*4,p[axis]);
        const auto expected=std::trunc(gain(func_80059588,m,1,1,false));
        switch_weapon(audio_switch_from_rider,m,1,6);
        check(volume==expected,"switch cue matches native horizontal distance on both axes");
    }
    // Missing identity must neither dereference a foreign pointer nor modify
    // the branch/call registers. Original local/transition behavior remains.
    for(unsigned invalid=0;invalid<5;++invalid){
        rules.active=rules.connected=true;rules.phase=rr64::netplay::Phase::Race;
        rules.replicated_riders=false;rules.local_slot=0;
        const auto saved=get(m,actor(0)+0xE4);
        if(invalid==0)rules.local_slot=255;
        if(invalid==1)put(m,actor(0)+0xE4,0);
        if(invalid==2)put(m,actor(0)+0xE4,0x807FFFFC);
        if(invalid==3)put(m,actor(0)+0xE4,body(1));
        if(invalid==4)put(m,actor(0)+0xE4,body(0)+1);
        auto c=context(0x80100000);c.r2=rr64::engine::guest_address(actor(1));c.r3=7;
        const auto saved4=c.r4;rr64_online_audio_owner(m,&c);rr64_online_audio_listener(m,&c);
        check(c.r3==7 && c.r4==saved4,"invalid listener identity leaves original call registers intact");
        put(m,actor(0)+0xE4,saved);
    }
    rr64_online_audio_owner(nullptr,nullptr);rr64_online_audio_listener(nullptr,nullptr);
    rr64_online_audio_weapon_source(nullptr,nullptr,0);rr64_online_audio_weapon_gain(nullptr,nullptr);
    std::printf("{\"passed\":true,\"checks\":%u,\"peer_cases\":%u,\"native_gain_routines\":5,\"weapon_switch_caller_paths\":2,\"weapon_ids\":14,\"engine_volume_updates\":%u,\"game_launched\":false}\n",checks,peer_cases,updates);
}
