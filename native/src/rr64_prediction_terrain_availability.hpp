#pragma once
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_resources.hpp"
#include <array>
namespace rr64::prediction {
// Query availability is an external streaming input. Do not resurrect unloaded
// payloads or copy live allocator pointers into historical memory.
struct TerrainAvailability {
 unsigned table=0,width=0;
 std::array<std::uint64_t,256> loaded{};
};
inline bool capture_terrain_availability(unsigned char* m,TerrainAvailability& out){
 TerrainAvailability value;
 if(!engine::read_u32(m,0x800dea8c,value.width) || value.width>128)return false;
 if(!value.width){out=value;return true;}
 if(!engine::read_u32(m,0x800ddea4,value.table) ||
    !engine::valid_guest_range(value.table,value.width*value.width*16))return false;
 for(unsigned i=0;i<value.width*value.width;++i){
  std::uint8_t state=0;if(!engine::read_u8(m,value.table+i*16+12,state))return false;
  if(state==5)value.loaded[i/64]|=std::uint64_t{1}<<(i%64);
 }
 out=value;return true;
}
inline thread_local const TerrainAvailability* historical_terrain=nullptr;
struct TerrainAvailabilityScope {
 const TerrainAvailability* previous=historical_terrain;
 explicit TerrainAvailabilityScope(const TerrainAvailability& value){historical_terrain=&value;}
 ~TerrainAvailabilityScope(){historical_terrain=previous;}
};
inline unsigned historical_terrain_state(unsigned address,unsigned original){
 const auto* t=historical_terrain;if(!t || !t->width)return original;
 if(t->width>128 || address<t->table || (address-t->table)%16 ||
    (address-t->table)/16>=t->width*t->width)throw Resources::Invalid("historical terrain table identity changed");
 unsigned i=(address-t->table)/16;
 if(!(t->loaded[i/64]&(std::uint64_t{1}<<(i%64))))return 0;
 if(original!=5)throw Resources::Invalid("historical terrain load not replayed");
 return original;
}
}
