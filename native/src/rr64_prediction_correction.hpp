#pragma once
#include "rr64_netplay.hpp"
#include "rr64_engine_layout.hpp"
#include <algorithm>
#include <bit>

namespace rr64::prediction {
// A prepared set of local value writes. Preparation validates the entire frame;
// commit checks that local ownership/state has not changed before consuming a
// reconciliation ticket. No allocation or failure is possible after approval.
// This covers the movement schema, not callbacks, collision contacts or traffic.
class MovementCorrection {
    struct Word { unsigned address,before,after,width; };
    std::array<Word,4096> words_{};
    unsigned count_=0;
    unsigned char *mapping_=nullptr;
    bool ready_=false;
    bool add(unsigned address,unsigned value,unsigned width=4) {
        if(count_==words_.size() || (address&(width-1)) || !engine::valid_guest_range(address,width))return false;
        unsigned old=0;
        if(width==2){std::uint16_t half=0;if(!engine::read_u16(mapping_,address,half))return false;old=half;}
        else if(!engine::read_u32(mapping_,address,old))return false;
        words_[count_++]={address,old,value,width};return true;
    }
    bool number(unsigned address,float value) {
        return std::isfinite(value) && add(address,std::bit_cast<unsigned>(value));
    }
    template<std::size_t N> bool vector(unsigned address,const std::array<float,N> &v){
        for(unsigned i=0;i<N;++i)if(!number(address+i*4,v[i]))return false;
        return true;
    }
    bool integrator(unsigned body,const IntegratorState &s){
        return vector(body+0x64,s.translation) && vector(body+0xf4,s.force) &&
               vector(body+0x10c,s.rotation) && add(body+0x60,s.flags,2);
    }
    bool pair(unsigned actor,const netplay::RiderState &s,const authority::RiderDynamics &d){
        if(!s.root.valid || !s.rider_position_valid || s.root.vault_latch>3 || !authority::valid_dynamics(d))return false;
        unsigned bike=0,rider=0,owner=0,model=0,character=0;
        std::uint16_t active=0;
        if(!engine::read_u16(mapping_,actor+0x24,active) || !active ||
           !engine::read_u32(mapping_,actor+0xe0,bike) || !engine::read_u32(mapping_,actor+0xe4,rider) ||
           (bike&3) || (rider&3) || !engine::valid_guest_range(bike,engine::bike::stride) ||
           !engine::valid_guest_range(rider,engine::rider::stride) ||
           !engine::read_u32(mapping_,bike+engine::bike::rider_pointer,owner) || owner!=rider ||
           !engine::read_u32(mapping_,rider+engine::rider::bike_pointer,owner) || owner!=bike ||
           !engine::read_u32(mapping_,actor+0x18,model) || model!=s.bike ||
           !engine::read_u32(mapping_,actor+0x1c,character) || character!=s.character)return false;
        // Guards are retained even if no value at that address changes.
        if(!add(actor+0x24,active,2) || !add(actor+0xe0,bike) || !add(actor+0xe4,rider) ||
           !add(actor+0x18,model) || !add(actor+0x1c,character) ||
           !add(bike+engine::bike::rider_pointer,rider) || !add(rider+engine::rider::bike_pointer,bike))return false;
        const auto &r=s.root;
        if(!vector(bike+0x16c,r.bike_origin) || !vector(bike+0x194,r.bike_velocity) ||
           !vector(bike+0x178,r.bike_motion) || !vector(rider+0xb4,r.rider_velocity) ||
           !vector(rider+0x5dc,r.rider_anchor) ||
           !vector(bike+engine::bike::body_position,std::array{s.position_x,s.position_y,s.position_z}) ||
           !vector(bike+engine::bike::front_wheel_position,std::array{s.front_wheel_x,s.front_wheel_y,s.front_wheel_z}) ||
           !vector(bike+engine::bike::rear_wheel_position,std::array{s.rear_wheel_x,s.rear_wheel_y,s.rear_wheel_z}) ||
           !vector(rider+0x8c,std::array{s.rider_x,s.rider_y,s.rider_z}) ||
           !vector(bike+0x244,r.bike_rotation) || !vector(rider+0x164,r.rider_rotation) ||
           !number(bike+0x21c,r.bike_display_angles[0]) || !number(bike+0x4ac,r.bike_display_angles[1]) ||
           !number(bike+0x4b8,r.bike_display_angles[2]) || !number(bike+0x550,r.bike_height) ||
           !number(rider+0x238,r.rider_height) || !number(rider+0x310,r.rider_impact_reserve) ||
           !number(bike+engine::bike::durability_current,r.durability) ||
           !number(bike+engine::bike::durability_capacity,r.durability_capacity) ||
           !add(bike+engine::bike::drive_control_lockout,r.drive_lockout,2) ||
           !add(bike+0x818,r.vault_latch,2) ||
           !add(bike+engine::bike::rider_attached,r.bike_attached,2) ||
           !add(rider+engine::rider::bike_attached,r.rider_attached,2) ||
           !add(rider+engine::rider::ejected,r.ejected,2))return false;
        if(r.equipment_valid){
            if(s.weapon<1 || s.weapon>14 || !add(rider+engine::rider::selected_weapon,s.weapon))return false;
            for(unsigned i=0;i<r.inventory.size();++i)if(!add(bike+0x838+i*2,r.inventory[i],2))return false;
        }
        //36D3C/36D6C advance hit-reaction clocks;6B234 tests+5C4 before
        // selecting404BC. They affect simulation as well as the displayed pose.
        // Restore those values without installing the visual descriptor/model
        // pointers, which belong to the local attack/resource controller.
        if(r.attack.valid) {
            if(!valid_attack_visual(r.attack))return false;
            for(unsigned i=0;i<3;++i)
                if(!number(rider+0x5c4+i*4,r.attack.clocks[3+i]))return false;
        }
        if(!integrator(bike+0x108,d.bike_physics) || !integrator(rider+0x28,d.rider_physics))return false;
        for(unsigned v=0;v<authority::rider_dynamics_offsets.size();++v)
            for(unsigned axis=0;axis<3;++axis)
                if(!number(rider+authority::rider_dynamics_offsets[v]+axis*4,d.values[v*3+axis]))return false;
        return add(rider+0x20,d.damping_mode) && add(rider+0x5d4,d.effect) &&
               number(rider+0x5d8,d.effect_remaining) && number(bike+0x4cc,d.recovery_age);
    }
public:
    // A new prepare invalidates the old transaction even on rejection.
    bool prepare(unsigned char *memory,const netplay::AuthorityFrame &frame,unsigned local_slot,bool mapped,bool include_outcomes=false){
        ready_=false;count_=0;mapping_=memory;
        if(!memory || local_slot>=14 || !frame.stamp.round || !frame.stamp.tick)return false;
        unsigned count=0;
        if(!engine::read_u32(memory,0x800a656c,count) || !count || count>14 || !add(0x800a656c,count))return false;
        std::array<unsigned,14> bikes{},riders{},routes{};
        auto overlaps=[](unsigned a,unsigned as,unsigned b,unsigned bs){
            return std::uint64_t(a)<std::uint64_t(b)+bs && std::uint64_t(b)<std::uint64_t(a)+as;
        };
        for(unsigned slot=0;slot<14;++slot)if(frame.riders[slot].active){
            const unsigned guest=online_flow::mapped_slot(slot,local_slot,mapped);
            if(guest>=count)return false;
            const auto actor=0x800d8570u+guest*0x118u;
            engine::read_u32(memory,actor+0xe0,bikes[slot]);engine::read_u32(memory,actor+0xe4,riders[slot]);
            if(overlaps(bikes[slot],engine::bike::stride,riders[slot],engine::rider::stride) ||
               overlaps(bikes[slot],engine::bike::stride,0x800d8570,14*0x118) ||
               overlaps(riders[slot],engine::rider::stride,0x800d8570,14*0x118))return false;
            for(unsigned prior=0;prior<slot;++prior)if(frame.riders[prior].active &&
               (overlaps(bikes[prior],engine::bike::stride,bikes[slot],engine::bike::stride) ||
                overlaps(riders[prior],engine::rider::stride,riders[slot],engine::rider::stride) ||
                overlaps(bikes[prior],engine::bike::stride,riders[slot],engine::rider::stride) ||
                overlaps(riders[prior],engine::rider::stride,bikes[slot],engine::bike::stride)))return false;
            if(!pair(actor,frame.riders[slot],frame.dynamics[slot]))return false;
        }
        if(include_outcomes)for(unsigned slot=0;slot<14;++slot)
            if(frame.riders[slot].active || frame.outcomes[slot].valid) {
                const unsigned guest=online_flow::mapped_slot(slot,local_slot,mapped);
                if(guest>=count)return false;
                const unsigned actor=0x800d8570+guest*0x118;
                const auto &outcome=frame.outcomes[slot];unsigned route=0;
                if(!authority::valid_outcome(outcome) || !engine::read_u32(memory,actor+0xe8,route) ||
                   (route&3) || !engine::valid_guest_range(route,0x64) ||
                   overlaps(route,0x64,0x800d8570,14*0x118))return false;
                routes[slot]=route;
                // Stats are resolved through this process's actor. Keep their
                // owning pointer as a commit guard; never copy a host pointer.
                if(!add(actor+0xe8,route) || !add(actor+0x20,outcome.role) ||
                   !add(route+0x48,outcome.eligible,2) || !add(route+0x4c,outcome.busted,2) ||
                   !add(route+0x58,outcome.busts) || !add(route+0x40,outcome.recovery_count) ||
                   !add(route+0x50,outcome.recovery_flag,2) || !add(route+0x52,outcome.finished,2) ||
                   !add(route+0x4e,outcome.progress_gate,2) ||
                   !number(route+8,outcome.progress[0]) || !number(route+0xc,outcome.progress[1]) ||
                   !number(route+0x20,outcome.progress[2]))return false;
            }
        for(unsigned slot=0;slot<14;++slot)if(routes[slot])
            for(unsigned other=0;other<14;++other)if(
                (frame.riders[other].active && (overlaps(routes[slot],0x64,bikes[other],engine::bike::stride) ||
                 overlaps(routes[slot],0x64,riders[other],engine::rider::stride))) ||
                 (other!=slot && routes[other] && overlaps(routes[slot],0x64,routes[other],0x64)))return false;
        std::sort(words_.begin(),words_.begin()+count_,[](const Word&a,const Word&b){return a.address<b.address;});
        unsigned kept=0;
        for(unsigned i=0;i<count_;++i){
            if(kept && words_[i].address<words_[kept-1].address+words_[kept-1].width){
                const auto &old=words_[kept-1];const auto &next=words_[i];
                // Overlapping roots/physics must agree bit-for-bit; a frame
                // with contradictory copies is not a coherent correction.
                if(old.address!=next.address || old.width!=next.width || old.after!=next.after)return false;
            }else words_[kept++]=words_[i];
        }
        count_=kept;ready_=true;return true;
    }
    using Approval=bool(*)(void*) noexcept;
    bool commit(Approval approve,void *user) noexcept {
        if(!ready_ || !approve)return false;
        ready_=false;
        for(unsigned i=0;i<count_;++i){
            const auto &w=words_[i];unsigned now=0;
            if(w.width==2){std::uint16_t v=0;engine::read_u16(mapping_,w.address,v);now=v;}
            else engine::read_u32(mapping_,w.address,now);
            if(now!=w.before)return false;
        }
        if(!approve(user))return false;
        for(unsigned i=0;i<count_;++i){
            const auto &w=words_[i];if(w.before==w.after)continue;
            if(w.width==2)engine::write_u16(mapping_,w.address,static_cast<std::uint16_t>(w.after));
            else engine::write_u32(mapping_,w.address,w.after);
        }
        return true;
    }
};
}
