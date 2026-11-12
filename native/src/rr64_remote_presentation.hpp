#pragma once
#include "rr64_netplay.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rr64::online_race_sync {
// Game-thread position history, sampled once at the pose boundary. The returned
// state is a calculation result only: callers extract its translation offset.
// It must NEVER be passed to apply_bike or written into guest actor records.
class RemotePresentation {
    struct Sample { netplay::RiderState state{}; std::int64_t us=0; };
    std::array<Sample, 16> samples_{};
    unsigned count_=0, next_=0;
    std::int64_t last_observed_us_=-1;
    std::int64_t last_received_us_=0;
    double playback_us_=0;
    bool playback_started_=false;
    const Sample &at(unsigned i) const { return samples_[(next_+16-count_+i)%16]; }
    static bool discontinuity(const netplay::RiderState &a,const netplay::RiderState &b) {
        const float dx=b.position_x-a.position_x,dy=b.position_y-a.position_y,dz=b.position_z-a.position_z;
        return a.character!=b.character || a.bike!=b.bike ||
            a.root.bike_attached!=b.root.bike_attached || a.root.rider_attached!=b.root.rider_attached ||
            a.root.ejected!=b.root.ejected || a.root.drive_lockout!=b.root.drive_lockout ||
            a.root.vault_latch!=b.root.vault_latch || dx*dx+dy*dy+dz*dz>512.f*512.f;
    }
public:
    void reset() { count_=next_=0; last_observed_us_=-1; playback_started_=false; playback_us_=0; }
    netplay::RiderState sample(const netplay::RiderState &latest,std::int64_t now) {
        const auto elapsed=last_observed_us_<0 ? 0 : std::max<std::int64_t>(0,now-last_observed_us_);
        if(last_observed_us_>now) reset();
        last_observed_us_=now;
        if (!latest.active || !latest.root.valid || !latest.rider_position_valid) { reset();return latest; }
        if (count_) {
            const auto &last=at(count_-1);
            if (latest.tick<last.state.tick || now-last_received_us_>500000 || discontinuity(last.state,latest)) reset();
        }
        if (!count_ || latest.tick!=at(count_-1).state.tick) {
            // Preserve sender spacing instead of timing updates by the frame
            // on which the receiver happens to observe them. Anchor the first
            // sample locally; only elapsed owner time crosses clock domains.
            auto sample_us=now;
            if(count_ && latest.sample_time_us && at(count_-1).state.sample_time_us) {
                const auto &last=at(count_-1);
                if(latest.sample_time_us<=last.state.sample_time_us ||
                    latest.sample_time_us-last.state.sample_time_us>500000) reset();
                else sample_us=last.us+static_cast<std::int64_t>(latest.sample_time_us-last.state.sample_time_us);
            }
            samples_[next_]={latest,sample_us}; next_=(next_+1)%16;count_=std::min(count_+1,16u);
            last_received_us_=now;
        }
        last_observed_us_=now;
        if (count_<2) return latest;
        // Advance a presentation clock, not a target that jumps forward after
        // a network outage. Never run beyond received state or extrapolate an
        // unobserved crash. Extra buffered time drains gradually on recovery.
        // A wide dead band absorbs the normal 25 ms packet cadence; small rate
        // corrections also prevent sender/receiver clock drift accumulating.
        if(!playback_started_) {
            playback_us_=now-75000;
            playback_started_=true;
        } else {
            const double buffered=at(count_-1).us-playback_us_;
            std::int64_t cadence=500000;
            for(unsigned i=1;i<count_;++i) cadence=std::min(cadence,at(i).us-at(i-1).us);
            const double low_water=std::max<std::int64_t>(15000,75000-cadence);
            const double rate=buffered>160000 ? 1.10 : buffered>125000 ? 1.02 : buffered<low_water ? 0.98 : 1.0;
            playback_us_+=elapsed*rate;
        }
        playback_us_=std::min(playback_us_,double(at(count_-1).us));
        if(count_==samples_.size()) playback_us_=std::max(playback_us_,double(at(0).us));
        const auto target=static_cast<std::int64_t>(playback_us_);
        auto position=at(0).state;
        for(unsigned i=1;i<count_;++i) {
            const auto &a=at(i-1),&b=at(i);
            if(target>=b.us) { position=b.state;continue; }
            const float t=std::clamp(float(target-a.us)/float(std::max<std::int64_t>(1,b.us-a.us)),0.f,1.f);
            position.position_x=a.state.position_x+(b.state.position_x-a.state.position_x)*t;
            position.position_y=a.state.position_y+(b.state.position_y-a.state.position_y)*t;
            position.position_z=a.state.position_z+(b.state.position_z-a.state.position_z)*t;
            position.root.attack=a.state.root.attack;
            const auto &aa=a.state.root.attack, &ab=b.state.root.attack;
            // Never blend an onset, cancellation or restarted swing into the
            // preceding attack. Continuous phases share the movement timeline.
            if(aa.valid && ab.valid && aa.descriptor==ab.descriptor &&
               aa.equipment==ab.equipment && aa.weapon_visible==ab.weapon_visible &&
               aa.clocks[0]>0 && ab.clocks[0]>=aa.clocks[0]) {
                for(unsigned c=0;c<aa.clocks.size();++c)
                    position.root.attack.clocks[c]=aa.clocks[c]+(ab.clocks[c]-aa.clocks[c])*t;
            }
            break;
        }
        // Keep native crash/eject/vault motion exact. Only mounted rider/bike pairs
        // receive a common world-space translation; no invented trajectory.
        if (!latest.root.bike_attached || !latest.root.rider_attached || latest.root.ejected || latest.root.drive_lockout || latest.root.vault_latch) return latest;
        auto result=latest;
        result.root.attack=position.root.attack;
        const float dx=position.position_x-latest.position_x,dy=position.position_y-latest.position_y,dz=position.position_z-latest.position_z;
        result.position_x+=dx;result.position_y+=dy;result.position_z+=dz;
        result.front_wheel_x+=dx;result.front_wheel_y+=dy;result.front_wheel_z+=dz;
        result.rear_wheel_x+=dx;result.rear_wheel_y+=dy;result.rear_wheel_z+=dz;
        result.rider_x+=dx;result.rider_y+=dy;result.rider_z+=dz;
        result.root.bike_origin[0]+=dx;result.root.bike_origin[1]+=dy;result.root.bike_origin[2]+=dz;
        result.root.bike_height+=dz;result.root.rider_height+=dz;
        return result;
    }
};
}
