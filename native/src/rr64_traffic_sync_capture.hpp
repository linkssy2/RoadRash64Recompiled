#include "rr64_traffic_roster.hpp"
#pragma once
#include "rr64_engine_layout.hpp"
#include "rr64_world_sync.hpp"
namespace rr64::world_sync {
// 47668 stores the result of 7B220 in scene+40, not its input model ID.
// Resolve through the same descriptor/group-count tables. Never feed a local
// resource index to the remote model constructor. Exact descriptor aliases
// use the lowest ID. Native constructor dimensions disambiguate different
// definitions sharing graphics; remaining ambiguity still rejects.
inline bool traffic_model_from_resource(unsigned char *m,unsigned resource,unsigned &model,
                                       const char **failure=nullptr,unsigned entity=0) {
    using namespace engine;
    const auto reject=[&](const char *reason){if(failure)*failure=reason;return false;};
    unsigned found=0,found_descriptor=0;
    for(unsigned candidate=0xd8;candidate<=0x126;++candidate){
        // 47730/47734 normalizes F8 to F7 before resource lookup.
        if(candidate==0xf8 || (candidate>0x101 && candidate<0x125))continue;
        unsigned descriptor=0;
        if(!read_u32(m,0x800df210+candidate*4,descriptor))return reject("traffic-model-table");
        if(!descriptor)continue;
        std::uint8_t group=0,item=0;
        if(!read_u8(m,descriptor+4,group) || !read_u8(m,descriptor+3,item))return reject("traffic-model-descriptor");
        std::uint64_t index=item;
        for(unsigned i=0;i<group;++i){
            unsigned count=0;if(!read_u32(m,0x800a7734+i*4,count))return reject("traffic-model-group");
            index+=count;
        }
        if(index!=resource)continue;
        if(entity){
            // 47790..477DC copies descriptor signed halfwords +12/+10/+0E,
            // multiplied by the original scale, into entity +30/+34/+38.
            // Graphics identity alone loses this constructor information.
            float scale=0;
            if(!read_float(m,0x80004b70,scale))return reject("traffic-model-scale");
            bool dimensions_match=true;
            for(unsigned axis=0;axis<3;++axis){
                std::uint16_t raw=0;float actual=0;
                if(!read_u16(m,descriptor+0x12-axis*2,raw) ||
                   !read_float(m,entity+0x30+axis*4,actual))return reject("traffic-model-dimensions");
                const float expected=static_cast<float>(static_cast<std::int16_t>(raw))*scale;
                if(actual!=expected)dimensions_match=false;
            }
            if(!dimensions_match)continue;
        }
        if(found){
            // 47668 reads dimensions and graphics from this descriptor, then
            // discards the original model ID at477FC. Replication supplies
            // the captured class separately. Equal descriptor identity thus
            // preserves both native definition and captured vehicle class.
            if(descriptor!=found_descriptor)return reject("traffic-model-ambiguous");
            continue;
        }
        found=candidate;found_descriptor=descriptor;
    }
    if(!found)return reject("traffic-model-unmapped");
    model=found;if(failure)*failure=nullptr;return true;
}
// Read one complete native roster at the simulation boundary. Reject a torn
// or malformed list rather than publishing an accidental removal/partial car.
template<std::size_t N> inline bool capture_traffic(unsigned char *m,unsigned round,std::uint64_t tick,TrafficSnapshot<N> &out,
                            const char **failure=nullptr) {
    using namespace engine;
    const auto stage=[&](const char *reason){if(failure)*failure=reason;};
    stage("traffic-roster");
    TrafficSnapshot<N> s{};s.round=round;s.tick=tick;
    unsigned count=0;
    if(!read_u32(m,0x800A6528,count) || count>N) return false;
    std::array<unsigned,N> entities{};
    for(unsigned i=0;i<count;++i) {
        stage("traffic-entity");
        unsigned entity=0;std::uint16_t active=0;
        if(!rr64::traffic::read_entity(m,i,entity) || !valid_guest_range(entity,0x360) ||
            !read_u16(m,entity+0x334,active) || active>1) return false;
        entities[i]=entity;
        for(unsigned j=0;j<i;++j) if(entities[j]==entity) return false;
        if(!active) continue;
        auto &v=s.traffic[i];v.active=1;
        stage("traffic-motion");
        if(!read_u32(m,entity+4,v.id) || !read_u32(m,entity,v.kind)) return false;
        if(!read_float(m,entity+0x24,v.road_distance)) return false;
        for(unsigned f=0;f<v.motion.size();++f)
            if(!read_float(m,entity+0xa8+f*4,v.motion[f])) return false;
        for(unsigned f=0;f<v.directions.size();++f)
            if(!read_float(m,entity+0x310+f*4,v.directions[f])) return false;
        v.motion_valid=1;
        for(unsigned axis=0;axis<3;++axis) {
            if(!read_float(m,entity+0xA8+axis*4,v.position[axis]) ||
                !read_float(m,entity+0xB4+axis*4,v.velocity[axis]) ||
                !read_float(m,entity+0xF8+axis*4,v.angles[axis])) return false;
        }
    }
    stage("traffic-scene");
    unsigned node=0;if(!read_u32(m,globals::traffic_scene_head,node))return false;
    // Native scene may contain inactive nodes as well as the live cars.
    unsigned visited=0;
    while(node && visited++<128) {
        unsigned kind=0,entity=0,next=0,model=0;
        if(!valid_guest_range(node,0x44) || !read_u32(m,node,kind) ||
            !read_u32(m,node+4,entity) || !read_u32(m,node+0x3C,next) ||
            !read_u32(m,node+0x40,model)) return false;
        if(kind==4) for(unsigned i=0;i<count;++i)
            if(s.traffic[i].active && entities[i]==entity) {
                if(s.traffic[i].model) {stage("traffic-duplicate-scene");return false;}
                stage("traffic-model-resolution");
                if(!traffic_model_from_resource(m,model,s.traffic[i].model,failure,entity))return false;
            }
        node=next;
    }
    if(node) {stage("traffic-scene-cycle");return false;}
    for(const auto &v:s.traffic)if(v.active && !v.model){stage("traffic-missing-model");return false;}
    stage("traffic-invalid-state");
    if(!valid(s))return false;
    out=s;stage(nullptr);return true;
}
}
