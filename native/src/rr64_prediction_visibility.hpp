#pragma once
#include "rr64_engine_layout.hpp"
#include <array>

namespace rr64::prediction {
// 784B4 consults descriptor bit zero before AI ambient RNG decisions. Rendering
// updates this between simulation calls. Retain that input, not the renderer or
// random seed. Supply only the bit to the matching private object; never rewrite descriptors.
struct VisibilityInputs {
    struct Entry { unsigned actor=0,bike=0,object=0,descriptor=0; std::uint16_t hidden=0; std::array<unsigned,4> distances{}; };
    std::array<Entry,14> entries{};
    unsigned count=0,view_shift=0,distance_limit=0;
};
inline bool capture_visibility(unsigned char* m,VisibilityInputs& out){
    VisibilityInputs value;unsigned count=0;
    if(!m || !engine::read_u32(m,0x800a656c,count) || count>14 || !engine::read_u32(m,0x800a4f24,value.view_shift) || value.view_shift>2 ||
       !engine::read_u32(m,0x8009dc64,value.distance_limit))return false;
    for(unsigned slot=0;slot<count;++slot){
        const unsigned actor=0x800d8570+slot*0x118;std::uint16_t active=0,flags=0;
        if(!engine::read_u16(m,actor+0x24,active))return false;
        if(!active)continue;
        auto& e=value.entries[value.count];e.actor=actor;
        if(!engine::read_u32(m,actor+0xe0,e.bike) || !engine::valid_guest_range(e.bike,engine::bike::stride) ||
           !engine::read_u32(m,e.bike+8,e.object) || !engine::valid_guest_range(e.object,0x2c) ||
           !engine::read_u32(m,e.object+0x28,e.descriptor) || !engine::valid_guest_range(e.descriptor,0xc) ||
           !engine::read_u16(m,e.descriptor+0xa,flags))return false;
        for(unsigned view=0;view<4;++view)if(!engine::read_u32(m,e.object+8+view*4,e.distances[view]))return false;
        e.hidden=flags&1;++value.count;
    }
    out=value;return true;
}
inline bool validate_visibility(unsigned char* m,const VisibilityInputs& value){
    if(!m || value.count>14 || value.view_shift>2)return false;
    for(unsigned i=0;i<value.count;++i){
        const auto& e=value.entries[i];unsigned bike=0,object=0;
        if(e.actor<0x800d8570 || e.actor>=0x800d8570+14*0x118 ||
           (e.actor-0x800d8570)%0x118 || e.hidden>1 ||
           !engine::read_u32(m,e.actor+0xe0,bike) || bike!=e.bike ||
           !engine::read_u32(m,bike+8,object) || object!=e.object)return false;
        for(unsigned j=0;j<i;++j)if(value.entries[j].object==object)return false;
    }
    return true;
}
inline thread_local const VisibilityInputs* historical_visibility=nullptr;
struct VisibilityScope {
    const VisibilityInputs* previous;
    explicit VisibilityScope(const VisibilityInputs& value):previous(historical_visibility){historical_visibility=&value;}
    ~VisibilityScope(){historical_visibility=previous;}
};
// Called only at the private 784B4 visibility predicate. Descriptor banks can
// change between updates while the owning bike/object remains the same.
inline unsigned historical_visibility_bit(unsigned object,unsigned original){
    if(historical_visibility)
        for(unsigned i=0;i<historical_visibility->count;++i)
            if(historical_visibility->entries[i].object==object)return historical_visibility->entries[i].hidden;
    return original&1;
}
inline unsigned historical_visibility_distance(unsigned object,unsigned address,unsigned original){
    if(historical_visibility && address>=object+8 && address<object+24 && !((address-object-8)%4))
        for(unsigned i=0;i<historical_visibility->count;++i)
            if(historical_visibility->entries[i].object==object)
                return historical_visibility->entries[i].distances[(address-object-8)/4];
    return original;
}
inline unsigned historical_visibility_parameter(bool view,unsigned original){
    if(!historical_visibility)return original;
    return view?historical_visibility->view_shift:historical_visibility->distance_limit;
}
}
