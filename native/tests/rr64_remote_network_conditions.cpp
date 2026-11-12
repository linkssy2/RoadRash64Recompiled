#include "rr64_remote_presentation.hpp"
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <limits>
struct Packet { int arrival; unsigned tick; float x; };
int main() {
    using rr64::netplay::RiderState;
    for(int scenario=0;scenario<6;++scenario) {
        const int duration=scenario>=4 ? 600000 : 10000;
        const double clock_rate=scenario==4 ? 1.001 : scenario==5 ? .999 : 1.0;
        std::vector<Packet> packets;
        unsigned random=12345;
        for(int sent=0;sent<=duration;sent+=25) {
            random=random*1664525u+1013904223u;
            if(scenario>=2 && random%20==0) continue; // reproducible 5% loss
            if(scenario==3 && sent>=5000 && sent<5200) continue;
            const int jitter=scenario ? int((random>>8)%41)-20 : 0;
            packets.push_back({sent+50+jitter,unsigned(sent/25+1),sent*.1f});
        }
        std::stable_sort(packets.begin(),packets.end(),[](auto a,auto b){return a.arrival<b.arrival;});
        rr64::online_race_sync::RemotePresentation history;
        RiderState latest{};
        unsigned at=0,steps=0,stalls=0,stale=0;double squared=0,max_step=0;float previous=0;
        for(int ms=0;ms<=duration;ms+=10) {
            while(at<packets.size() && packets[at].arrival<=ms) {
                const auto p=packets[at++];
                if(p.tick<=latest.tick) {++stale;continue;}
                latest={};latest.tick=p.tick;latest.sample_time_us=1000000ULL+std::uint64_t((p.tick-1)*25000ULL*clock_rate);latest.active=true;latest.root.valid=latest.rider_position_valid=1;
                latest.root.bike_attached=latest.root.rider_attached=1;
                latest.position_x=p.x;latest.front_wheel_x=p.x+2;latest.rear_wheel_x=p.x-2;latest.rider_x=p.x+1;
            }
            const auto shown=history.sample(latest,ms*1000LL);
            if(!std::isfinite(shown.position_x)) std::abort();
            const float tolerance=std::max(.001f,2*std::numeric_limits<float>::epsilon()*std::abs(shown.position_x));
            if(latest.active && (std::abs(shown.rider_x-shown.position_x-1)>tolerance ||
                std::abs(shown.front_wheel_x-shown.position_x-2)>tolerance)) std::abort();
            if(ms>=1000) {
                const double step=shown.position_x-previous;
                squared+=(step-1)*(step-1);max_step=std::max(max_step,std::abs(step));
                stalls+=std::abs(step)<.0001;++steps;
            }
            previous=shown.position_x;
        }
        std::printf("scenario=%d nominal_step=1 rms_step_error=%.6f max_step=%.6f stalls=%u reordered_drops=%u samples=%u\n",
            scenario,std::sqrt(squared/steps),max_step,stalls,stale,steps);
        std::fflush(stdout);
        if(scenario==0 && (max_step>1.001 || squared/steps>0.000001 || stalls)) std::abort();
        if((scenario==1 || scenario==2) && (max_step>1.021 || squared/steps>.0001 || stalls)) std::abort();
        // Loss can require a hold, but recovery must not jump many frames.
        if(scenario==3 && (max_step>1.101 || squared/steps>.03)) std::abort();
        // Ten minutes at +/-1000 ppm includes jitter/loss. Check that clock
        // drift does not cause recurring stalls or half-second history resets.
        if(scenario>=4 && (max_step>1.12 || squared/steps>.002 || stalls)) std::abort();
    }
}
