#include "rr64_prediction_correction.hpp"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <limits>
namespace {
void check(bool b){if(!b)std::abort();}
bool accept(void *p) noexcept {++*static_cast<unsigned*>(p);return true;}
bool reject(void *p) noexcept {++*static_cast<unsigned*>(p);return false;}
}
int main(){
 using namespace rr64;
 for(unsigned count:{2u,4u,14u})for(unsigned local=0;local<count;++local){
   std::vector<unsigned char> memory(engine::kRdramSize);auto *m=memory.data();
   engine::write_u32(m,0x800a656c,count);
   netplay::AuthorityFrame frame{};frame.stamp.round=7;frame.stamp.tick=23;
   auto expected=memory;
   for(unsigned slot=0;slot<count;++slot){
     const auto guest=online_flow::mapped_slot(slot,local,true);
     const auto a=0x800d8570u+guest*0x118u,b=0x80100000u+(13-guest)*engine::bike::stride,
                r=0x80300000u+guest*engine::rider::stride;
     engine::write_u16(m,a+0x24,1);engine::write_u32(m,a+0x18,slot);engine::write_u32(m,a+0x1c,slot+1);
     engine::write_u32(m,a+0xe0,b);engine::write_u32(m,a+0xe4,r);
     engine::write_u32(m,b+engine::bike::rider_pointer,r);engine::write_u32(m,r+engine::rider::bike_pointer,b);
     auto &s=frame.riders[slot];s.active=true;s.bike=slot;s.character=slot+1;
     s.root.valid=s.rider_position_valid=1;s.position_x=100.f+slot;
     s.root.bike_height=20.f+slot;s.root.durability=50.f;
     s.root.attack.valid=1;s.root.attack.clocks[3]=0.25f+slot;
     engine::write_u32(m,r+0x568,0x800a5584);
     frame.dynamics[slot].bike_physics.force[0]=0.001f*slot;
   }
   expected=memory;
   for(unsigned slot=0;slot<count;++slot){
     const auto guest=online_flow::mapped_slot(slot,local,true),b=0x80100000u+(13-guest)*engine::bike::stride;
     engine::write_float(expected.data(),b+engine::bike::body_position,100.f+slot);
     engine::write_float(expected.data(),b+0x550,20.f+slot);
     engine::write_float(expected.data(),b+engine::bike::durability_current,50.f);
     engine::write_float(expected.data(),b+0x108+0xf4,0.001f*slot);
     const auto r=0x80300000u+guest*engine::rider::stride;
     engine::write_float(expected.data(),r+0x5c4,0.25f+slot);
   }
   prediction::MovementCorrection transaction;
   const auto initial=memory;unsigned approvals=0;
   check(transaction.prepare(m,frame,local,true));
   check(!transaction.commit(reject,&approvals) && approvals==1 && memory==initial);
   check(!transaction.commit(accept,&approvals) && approvals==1);
   check(transaction.prepare(m,frame,local,true));
   // A late allocation change on the last actor rejects the whole write set.
   const auto last=0x800d8570u+(count-1)*0x118u;
   engine::write_u32(m,last+0xe4,0);const auto changed=memory;
   check(!transaction.commit(accept,&approvals) && approvals==1 && memory==changed);
   std::copy(initial.begin(),initial.end(),memory.begin());
   auto contradiction=frame;contradiction.riders[count-1].root.bike_origin[0]=1;
   check(!transaction.prepare(m,contradiction,local,true) && memory==initial);
   auto malformed=frame;malformed.dynamics[count-1].rider_physics.rotation[27]=std::numeric_limits<float>::infinity();
   check(!transaction.prepare(m,malformed,local,true) && memory==initial);
   check(transaction.prepare(m,frame,local,true));
   check(transaction.commit(accept,&approvals) && approvals==2 && memory==expected);
   check(!transaction.commit(accept,&approvals) && approvals==2);
   for(unsigned slot=0;slot<count;++slot){
     const auto guest=online_flow::mapped_slot(slot,local,true),a=0x800d8570+guest*0x118;
     engine::write_u32(m,a+0xe8,0x80500000+guest*0x64);
     frame.outcomes[slot].role=7;frame.outcomes[slot].busts=slot+1;frame.outcomes[slot].busted=1;
     frame.outcomes[slot].recovery_count=3;frame.outcomes[slot].recovery_flag=1;
     frame.outcomes[slot].finished=slot%2;
     frame.outcomes[slot].progress={0.25f,120.f,950.f+slot};frame.outcomes[slot].progress_gate=2;
   }
   check(transaction.prepare(m,frame,local,true,true));
   const auto before_outcomes=memory;
   check(!transaction.commit(reject,&approvals) && memory==before_outcomes);
   check(transaction.prepare(m,frame,local,true,true));check(transaction.commit(accept,&approvals));
   for(unsigned slot=0;slot<count;++slot){
     unsigned role=0,busts=0;std::uint16_t busted=0;
     const auto guest=online_flow::mapped_slot(slot,local,true),a=0x800d8570+guest*0x118,route=0x80500000+guest*0x64;
     engine::read_u32(m,a+0x20,role);engine::read_u32(m,route+0x58,busts);engine::read_u16(m,route+0x4c,busted);
     check(role==7 && busts==slot+1 && busted==1);
     engine::read_u32(m,route+0x4c,busts);check(busts==0x10002);
     float part=0,length=0,base=0;
     engine::read_float(m,route+8,part);engine::read_float(m,route+0xc,length);engine::read_float(m,route+0x20,base);
     check(part==0.25f && length==120.f && base==950.f+slot);
     engine::read_u32(m,route+0x40,busts);engine::read_u16(m,route+0x50,busted);check(busts==3 && busted==1);
     engine::read_u32(m,route+0x50,busts);check(busts==(0x10000u|slot%2));
   }
   auto retired=frame;retired.riders[count-1].active=false;
   retired.outcomes[count-1].valid=1;retired.outcomes[count-1].busts=29;
   check(transaction.prepare(m,retired,local,true,true));check(transaction.commit(accept,&approvals));
   unsigned final_busts=0;
   const unsigned retired_guest=online_flow::mapped_slot(count-1,local,true);
   engine::read_u32(m,0x80500000+retired_guest*0x64+0x58,final_busts);check(final_busts==29);
   engine::write_u32(m,0x800d8570+0xe8,0x800d8570);
   const auto bad_route=memory;
   check(!transaction.prepare(m,frame,local,true,true) && memory==bad_route);
 }
 std::puts("20 local-slot mappings: whole-frame movement correction, stale ownership, conflicting fields and rejected tickets checked; no live replay activation");
}
