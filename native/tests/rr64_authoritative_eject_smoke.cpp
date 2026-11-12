#include "rr64_authoritative_eject.hpp"
#include <vector>
#include <cstdio>
#include <cstdlib>
void check(bool x){if(!x)std::abort();}
int main(){
 using namespace rr64;
 std::vector<unsigned char> data(engine::kRdramSize);auto *m=data.data();
 prediction::ManualEjectState state{};
 for(unsigned slot=0;slot<14;++slot){
  const unsigned a=0x800d8570+slot*0x118,b=0x80200000+slot*0x1000,r=0x80300000+slot*0x1000;
  engine::write_u32(m,a+0xe0,b);engine::write_u32(m,a+0xe4,r);
  engine::write_u32(m,b+engine::bike::rider_pointer,r);engine::write_u32(m,r+engine::rider::bike_pointer,b);
  engine::write_u16(m,b+engine::bike::rider_attached,1);
  engine::write_float(m,b+engine::bike::durability_current,75);engine::write_float(m,b+engine::bike::durability_capacity,100);
  unsigned calls=0;
  auto transition=[&](unsigned bike){check(bike==b);++calls;engine::write_u16(m,b+engine::bike::drive_control_lockout,1);engine::write_u16(m,r+engine::rider::ejected,1);return true;};
  check(authority::eject_step(m,slot,true,state,transition) && calls==1 && state[slot].active);
  engine::write_float(m,b+engine::bike::durability_current,20);
  check(authority::eject_step(m,slot,false,state,transition));float health=0;engine::read_float(m,b+engine::bike::durability_current,health);check(health==75);
  check(authority::eject_step(m,slot,true,state,transition) && calls==1);
  engine::write_u16(m,b+engine::bike::drive_control_lockout,0);
  check(authority::eject_step(m,slot,false,state,transition) && !state[slot].active);
  engine::write_float(m,b+engine::bike::durability_current,30);
  check(authority::eject_step(m,slot,false,state,transition));engine::read_float(m,b+engine::bike::durability_current,health);check(health==30);
  engine::write_u32(m,r+engine::rider::bike_pointer,0);
  check(!authority::eject_step(m,slot,true,state,transition) && calls==1);
 }
 check(!authority::eject_step(m,14,true,state,[](unsigned){return true;}));
 std::puts("14-slot eject ownership, falling-edge rejection, health protection and release passed (native transition substituted)");
}
