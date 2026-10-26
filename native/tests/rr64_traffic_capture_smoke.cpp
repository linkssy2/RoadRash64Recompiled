#include "rr64_traffic_sync_capture.hpp"
#include "rr64_prediction_traffic.hpp"
#include "rr64_traffic_comparison.hpp"
#include <vector>
#include <cstdlib>
#include <string_view>
void check(bool v){if(!v)std::abort();}
int main(){
 using namespace rr64::engine;
 std::vector<unsigned char> memory(8*1024*1024);auto m=memory.data();
 const unsigned e=0x80100000,n=0x80200000;
 write_u32(m,0x800A6528,1);write_u32(m,0x800D76E0,e);
 write_u32(m,e,1);write_u32(m,e+4,77);write_u16(m,e+0x334,1);
 write_float(m,e+0xA8,123);write_float(m,e+0xB4,7);write_float(m,e+0xF8,0.4f);
 // The native constructor stores a flattened resource index, NOT model D8.
 const unsigned descriptor=0x80300000;
 write_u32(m,0x800df210+0xd8*4,descriptor);
 write_u32(m,descriptor,3);write_u32(m,descriptor+4,0x02000000);
 write_u32(m,0x800a7734,10);write_u32(m,0x800a7738,20);
 write_u32(m,globals::traffic_scene_head,n);write_u32(m,n,4);write_u32(m,n+4,e);write_u32(m,n+0x40,33);
 rr64::world_sync::Snapshot s{};
 const auto before=memory;
 check(rr64::world_sync::capture_traffic(m,1,1,s));check(memory==before);
 check(s.traffic[0].id==77 && s.traffic[0].model==0xd8 && s.traffic[0].position[0]==123 && s.traffic[0].velocity[0]==7);
 unsigned resolved=999;
 const char *model_failure=nullptr;
 check(!rr64::world_sync::traffic_model_from_resource(m,34,resolved,&model_failure) && resolved==999);
 check(std::string_view(model_failure)=="traffic-model-unmapped");
 write_u32(m,0x800df210+0xd9*4,descriptor);
 check(rr64::world_sync::traffic_model_from_resource(m,33,resolved,&model_failure) && resolved==0xd8 && !model_failure);
 // Same graphic with another native definition must not be canonicalized.
 const unsigned other_descriptor=descriptor+0x100;
 write_u32(m,other_descriptor,3);write_u32(m,other_descriptor+4,0x02000000);
 write_u32(m,0x800df210+0xd9*4,other_descriptor);resolved=999;
 check(!rr64::world_sync::traffic_model_from_resource(m,33,resolved,&model_failure) && resolved==999);
 check(std::string_view(model_failure)=="traffic-model-ambiguous");
 // Two native definitions can share graphics but have different constructor
 // dimensions (observed DD/DE and FE/126 pairs in a private live case).
 write_float(m,0x80004b70,0.5f);
 const unsigned offsets[3]={0x12,0x10,0xe};
 for(unsigned axis=0;axis<3;++axis){
     write_u16(m,descriptor+offsets[axis],static_cast<std::uint16_t>(100+axis));
     write_u16(m,other_descriptor+offsets[axis],static_cast<std::uint16_t>(200+axis));
     write_float(m,e+0x30+axis*4,static_cast<float>(200+axis)*0.5f);
 }
 check(rr64::world_sync::capture_traffic(m,1,2,s,&model_failure) && s.traffic[0].model==0xd9);
 // No approximate dimension match, guessed model or partial output on failure.
 write_float(m,e+0x34,100.50001f);
 check(!rr64::world_sync::capture_traffic(m,1,3,s,&model_failure) && s.tick==2 && s.traffic[0].model==0xd9);
 for(unsigned axis=0;axis<3;++axis)
     write_float(m,e+0x30+axis*4,static_cast<float>(100+axis)*0.5f);
 check(rr64::world_sync::capture_traffic(m,1,1,s,&model_failure) && s.traffic[0].model==0xd8);
 write_u32(m,0x800df210+0xd9*4,0);
 check(rr64::world_sync::traffic_model_from_resource(m,33,resolved,&model_failure) && !model_failure && resolved==0xd8);
 // Rejections identify unavailable scene ownership without publishing an
 // empty replacement roster. A later successful capture clears the reason.
 const char *failure=nullptr;
 write_u32(m,globals::traffic_scene_head,0);
 check(!rr64::world_sync::capture_traffic(m,1,2,s,&failure));
 check(std::string_view(failure)=="traffic-missing-model" && s.tick==1 && s.traffic[0].id==77);
 write_u32(m,globals::traffic_scene_head,n);
 check(rr64::world_sync::capture_traffic(m,1,1,s,&failure) && failure==nullptr);
 // Reordered network slots retain local allocation/scene ownership.
 auto host=s;host.traffic[19]=host.traffic[0];host.traffic[0]={};
 auto comparison=rr64::world_sync::compare_traffic(s.traffic,host.traffic);
 check(comparison.valid && comparison.count==0);
 auto changed=host;changed.traffic[19].motion[0]+=1;
 comparison=rr64::world_sync::compare_traffic(s.traffic,changed.traffic);
 check(comparison.valid && comparison.count==1 && comparison.differences[0].live==0 && comparison.differences[0].replay==19);
 changed.traffic[19].id=78;
 comparison=rr64::world_sync::compare_traffic(s.traffic,changed.traffic);
 check(comparison.valid && comparison.count==2);
 changed.traffic[0]=changed.traffic[19];
 check(!rr64::world_sync::compare_traffic(s.traffic,changed.traffic).valid);
 host.traffic[19].motion[0]=456;host.traffic[19].directions[3]=0.75f;
 const auto original=memory;
 check(rr64::prediction::correct_traffic_baseline(m,host)==rr64::prediction::TrafficAdmission::Ready);
 float corrected=0;read_float(m,e+0xa8,corrected);check(corrected==456);
 read_float(m,e+0x31c,corrected);check(corrected==0.75f);
 unsigned owner=0;read_u32(m,n+4,owner);check(owner==e);
 memory=original;
 host.traffic[19].id=78;
 check(rr64::prediction::correct_traffic_baseline(m,host)==rr64::prediction::TrafficAdmission::TopologyPending && memory==original);
 host.traffic[19].id=77;host.traffic[19].motion_valid=0;
 check(rr64::prediction::correct_traffic_baseline(m,host)==rr64::prediction::TrafficAdmission::Invalid && memory==original);
 write_u32(m,n+0x3C,n);check(!rr64::world_sync::capture_traffic(m,1,2,s));check(s.tick==1);
 write_u32(m,n+0x3C,0);write_u32(m,0x800A6528,0);
 check(rr64::world_sync::capture_traffic(m,1,3,s));check(!s.traffic[0].active && s.tick==3);
 write_u32(m,0x800A6528,21);check(!rr64::world_sync::capture_traffic(m,1,4,s));check(s.tick==3);
 memory=before;
 write_u32(m,0x800A6528,20);
 for(unsigned i=0;i<20;++i){
  const unsigned entity=e+i*0x400,node=n+i*0x80;
  write_u32(m,0x800D76E0u+i*4,entity);
  write_u32(m,entity,1);write_u32(m,entity+4,100+i);write_u16(m,entity+0x334,1);
  write_u32(m,node,4);write_u32(m,node+4,entity);write_u32(m,node+0x40,33);
  write_u32(m,node+0x3c,i==19?0:node+0x80);
 }
 rr64::world_sync::TrafficSnapshot<rr64::traffic::capacity> expanded{};
 check(rr64::world_sync::capture_traffic(m,1,5,expanded));
 check(expanded.traffic[19].active && expanded.traffic[19].id==119);
 check(rr64::world_sync::capture_traffic(m,1,5,s));
 write_u32(m,0x800A6528,21);check(!rr64::world_sync::capture_traffic(m,1,6,expanded));
 check(expanded.tick==5);
}
