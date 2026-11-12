#include "rr64_authoritative_native.hpp"
#include "rr64_prediction_controls.hpp"
#include "rr64_authoritative_controller.hpp"
#include "rr64_engine_layout.hpp"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" void func_80040664(unsigned char*,recomp_context*);
extern "C" int rr64_authority_translate(unsigned char *m,void *ctx){return rr64::prediction::Controls::translate(m,ctx);}
void check(bool b){if(!b){std::fputs("generated control comparison failed\n",stderr);std::exit(1);}}
gpr guest(unsigned x){return static_cast<std::int64_t>(static_cast<std::int32_t>(x));}
int main(){
 using namespace rr64;
 unsigned cases=0;
 for(unsigned slot=0;slot<14;++slot)for(unsigned locked:{0u,1u})for(int axis:{-100,0,100})for(unsigned burst:{0u,0x20u}){
  std::vector<unsigned char> baseline(16*1024*1024);
  auto *m=baseline.data();unsigned actor=0x800d8570+slot*0x118,bike=0x80100000;
  engine::write_u32(m,actor+4,0);engine::write_u32(m,actor+0xe0,bike);
  engine::write_u16(m,bike+0x7f6,locked);
  // Synthetic finite coefficients exercise the original conversion code;
  // equality is to native slot0 behavior, not a claim about tuned ROM physics.
  for(unsigned a=0x80004914;a<=0x80004944;a+=4)engine::write_float(m,a,0.5f);
  engine::write_float(m,bike+0x4a4,1.f);engine::write_float(m,bike+0x4a8,1.f);
  recomp_context original{};original.r29=guest(0x807f0000);original.r4=guest(actor);
  original.r5=guest(0x807e0000);original.r6=guest(0x807e0002);original.r7=guest(0x807e0004);
  engine::write_u32(m,0x807f0010,0x807e0008);engine::write_u32(m,0x807f0014,0x807e0010);
  auto adapted=baseline;
  authority::Command command{7,1,0x8020,static_cast<std::int8_t>(axis),static_cast<std::int8_t>(-axis)};
  engine::write_u16(m,engine::globals::controller_buttons,command.buttons);
  engine::write_u16(m,engine::globals::controller_changed_buttons,(command.buttons^0x20)|burst);
  engine::write_u16(m,engine::globals::controller_pressed_buttons,(command.buttons&~0x20)|burst);
  engine::write_u32(m,engine::globals::controller_stick_x,std::uint32_t(std::uint8_t(command.x))<<24);
  engine::write_u32(m,engine::globals::controller_stick_y,std::uint32_t(std::uint8_t(command.y))<<24);
  auto entry=original;
  auto bridged=original;
  func_80040664(m,&original);
  engine::write_u32(adapted.data(),actor+4,slot);
  check(authority::native_controls(adapted.data(),bridged,command,0x20,7,burst));
  for(unsigned offset=0;offset<28;offset+=4){unsigned a=0,b=0;engine::read_u32(m,0x807e0000+offset,a);engine::read_u32(adapted.data(),0x807e0000+offset,b);check(a==b);}
  check(original.r29==bridged.r29 && original.r2==bridged.r2);
  unsigned index=0;engine::read_u32(adapted.data(),actor+4,index);check(index==slot);
  std::uint16_t restored=1;engine::read_u16(adapted.data(),engine::globals::controller_buttons,restored);check(restored==0);
  // Replay invokes the original hook. Two passes must match two ordinary
  // translations, with a press only on the first pass and scratch restored.
  auto replay_memory=adapted,reference_memory=adapted;
  auto replay_ctx=entry,reference_ctx=entry;
  std::array<std::vector<unsigned char>,2> expected;
  for(unsigned pass=0;pass<2;++pass){
   reference_ctx=entry;
   check(authority::native_controls(reference_memory.data(),reference_ctx,command,pass?command.buttons:0x20,7));
   expected[pass]=reference_memory;
  }
  {
   prediction::ReplayScope replay;
   prediction::Controls controls(actor,command,0x20);check(controls.valid());
   prediction::Controls nested(actor,command,0x20);check(!nested.valid());
   for(unsigned pass=0;pass<2;++pass){
    replay_ctx=entry;
    func_80040664(replay_memory.data(),&replay_ctx);
    check(replay_memory==expected[pass]);
   }
   check(controls.completed(2) && !controls.completed(1));
  }
  ++cases;
 }
 std::printf("%u actual generated control comparisons passed\n",cases);
}
