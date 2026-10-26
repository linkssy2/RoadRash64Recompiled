#include "rr64_traffic_roster.hpp"
#pragma once
#include "rr64_traffic_sync_capture.hpp"
#include "rr64_traffic_reconcile.hpp"
namespace rr64::prediction {
enum class TrafficAdmission { Invalid, Ready, TopologyPending };
// Disposable historical image only. Match by race identity/model, never host
// pointers or compacted indices. New/retired allocations require the live
// native cleanup path; defer rather than fabricate resources in old history.
inline TrafficAdmission correct_traffic_baseline(unsigned char *m,const world_sync::Snapshot &host){
    world_sync::Snapshot local;
    if(!world_sync::valid(host) || !world_sync::capture_traffic(m,host.round,host.tick,local))
        return TrafficAdmission::Invalid;
    for(const auto &car:host.traffic)if(car.active && !car.motion_valid)return TrafficAdmission::Invalid;
    world_sync::TrafficPlan plan;
    if(!world_sync::plan_traffic(local,host,plan))return TrafficAdmission::Invalid;
    if(plan.removes || plan.creates)return TrafficAdmission::TopologyPending;
    std::array<unsigned,world_sync::capacity> entities{};
    for(unsigned i=0;i<plan.updates;++i)
        if(!rr64::traffic::read_entity(m,plan.update[i].local,entities[i]) ||
           !engine::valid_guest_range(entities[i],0x360))return TrafficAdmission::Invalid;
    for(unsigned i=0;i<plan.updates;++i){
        const auto entity=entities[i];const auto &car=host.traffic[plan.update[i].remote];
        for(unsigned j=0;j<car.motion.size();++j)engine::write_float(m,entity+0xa8+4*j,car.motion[j]);
        for(unsigned j=0;j<car.directions.size();++j)engine::write_float(m,entity+0x310+4*j,car.directions[j]);
        engine::write_float(m,entity+0x24,car.road_distance);
    }
    return TrafficAdmission::Ready;
}
}
