#include "rr64_prediction_journal.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_integrator.hpp"
#include <vector>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
extern "C" void func_80034594(unsigned char*,recomp_context*);
extern "C" void func_800348D8(unsigned char*,recomp_context*);
extern "C" void _nsqrtf(unsigned char*,recomp_context *c){c->f0.fl=std::sqrt(c->f12.fl);}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 std::ifstream rom(argv[1],std::ios::binary);std::vector<unsigned char> data(0x100000);rom.read(reinterpret_cast<char*>(data.data()),data.size());
 if(!rom || data[0]!=0x80 || data[1]!=0x37 || data[2]!=0x12 || data[3]!=0x40)return 2;
 std::vector<unsigned char> memory(0x800000);
 // Boot-loaded program constants only, supplied privately at test time.
 for(unsigned i=0;i<0xc0000;++i)memory[(0x400+i)^3]=data[0x1000+i];
 using namespace rr64::engine;const unsigned body=0x80300000;
 write_float(memory.data(),body,1);write_float(memory.data(),body+0x18,1);write_float(memory.data(),body+0x1c,1);write_float(memory.data(),body+0x20,1);
 write_float(memory.data(),body+0x148,1);write_float(memory.data(),body+0x64,500);write_float(memory.data(),body+0x68,100);write_float(memory.data(),body+0x6c,500);
 write_float(memory.data(),body+0x70,2);write_float(memory.data(),body+0x74,1);write_float(memory.data(),body+0x78,3);
 write_float(memory.data(),0x8009cba8,.01f);write_float(memory.data(),0x8009cbac,.0001f);write_float(memory.data(),0x8009cbb0,.01f);write_float(memory.data(),0x8009cbb4,100);
 const auto initial=memory;
 auto step=[&](std::vector<unsigned char> &m){recomp_context c{};c.f_odd=&c.f0.u32h;c.r29=guest_address(0x807f0000);c.r4=guest_address(body);func_80034594(m.data(),&c);c.r4=guest_address(body);func_800348D8(m.data(),&c);};
 rr64::prediction::GuestJournal journal;journal.reset(1);
 if(!journal.capture(1,0,memory.data(),memory.size()))return 4;
 auto replay=memory;
 for(unsigned n=0;n<120;++n){step(memory);step(replay);if(memory!=replay)std::abort();if(!journal.capture(1,n+1,memory.data(),memory.size()))return 4;}
 if(!journal.restore(1,60,replay.data(),replay.size()))return 4;
 for(unsigned n=60;n<120;++n)step(replay);
 if(memory!=replay)std::abort();
 // Correct a divergent body's integrator values into a different local image,
 // then use the original routines to verify its next movement. This is stronger
 // than copying an entire saved RDRAM image back to the same allocation.
 using rr64::prediction::IntegratorState;
 for(unsigned scenario=0;scenario<4;++scenario){
   auto host=initial,client=initial;
   write_u16(host.data(),body+0x60,scenario);
   write_float(host.data(),body+0xf4,4.0f);
   write_float(host.data(),body+0x100,0.01f);
   write_float(host.data(),body+0x164,0.02f);
   write_float(host.data(),body+0x170,0.001f);
   // The caller supplies a fresh impulse each update;34594 scales it in
   // place. Reusing its output as the next input grows it exponentially.
   for(unsigned n=0;n<35;++n){write_float(host.data(),body+0x100,0.01f);step(host);}
   IntegratorState correction;
   if(!rr64::prediction::capture_integrator(host.data(),body,correction)){std::fprintf(stderr,"invalid correction scenario%u\n",scenario);std::abort();}
   auto invalid=correction;invalid.rotation.back()=std::numeric_limits<float>::quiet_NaN();
   auto untouched=client;
   if(rr64::prediction::restore_integrator(client.data(),body,invalid) || client!=untouched)std::abort();
   if(rr64::prediction::restore_integrator(client.data(),body+1,correction) || client!=untouched)std::abort();
   if(!rr64::prediction::restore_integrator(client.data(),body,correction))std::abort();
   // Setup data and the collision/ownership area outside the explicit values
   // remain the client's own data. No adjacent halfword is overwritten.
   for(unsigned offset=0;offset<0x64;++offset)if(offset!=0x60 && offset!=0x61){
      std::uint8_t a=0,b=0;read_u8(client.data(),body+offset,a);read_u8(untouched.data(),body+offset,b);
      if(a!=b)std::abort();
   }
   for(unsigned n=0;n<40;++n){
      write_float(host.data(),body+0x100,0.01f);write_float(client.data(),body+0x100,0.01f);
      step(host);step(client);
      IntegratorState expected,actual;
      if(!rr64::prediction::capture_integrator(host.data(),body,expected) ||
         !rr64::prediction::capture_integrator(client.data(),body,actual) || expected!=actual){std::fprintf(stderr,"correction mismatch scenario%u step%u\n",scenario,n);std::abort();}
   }
 }
 float x=0;read_float(memory.data(),body+0x64,x);if(!std::isfinite(x)||memory==initial)return 3;
 std::printf("120 native steps plus four divergent-state corrections with40 native steps each passed; final x %.6f (not full collision/race replay)\n",x);
}
