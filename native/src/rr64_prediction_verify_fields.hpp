#pragma once
#include "rr64_netplay.hpp"
#include <cstdio>

namespace rr64::prediction {
// Diagnostic-only named differences. Never serialize struct padding or pointers.
inline void report_replay_fields(const char *boundary,unsigned slot,
    const netplay::AuthorityFrame &live,const netplay::AuthorityFrame &replay){
    unsigned remaining=8;
    const auto value=[&](const char *name,unsigned index,auto a,auto b){
        if(a!=b && remaining){
            --remaining;
            std::fprintf(stderr,"[RR64-REPLAY-FIELD] boundary=%s slot=%u field=%s index=%u live=%.17g replay=%.17g\n",
                boundary,slot,name,index,double(a),double(b));
        }
    };
    const auto array=[&](const char *name,const auto &a,const auto &b){
        for(unsigned i=0;i<a.size();++i)value(name,i,a[i],b[i]);
    };
    const auto integrator=[&](const char *translation,const char *force,const char *rotation,const char *flags,
                              const auto &a,const auto &b){
        array(translation,a.translation,b.translation);array(force,a.force,b.force);
        array(rotation,a.rotation,b.rotation);value(flags,0,a.flags,b.flags);
    };
    const auto &a=live.outcomes[slot],&b=replay.outcomes[slot];
    value("outcome.valid",0,a.valid,b.valid);value("outcome.role",0,a.role,b.role);
    value("outcome.busts",0,a.busts,b.busts);value("outcome.eligible",0,a.eligible,b.eligible);
    value("outcome.busted",0,a.busted,b.busted);value("outcome.stats40",0,a.recovery_count,b.recovery_count);
    value("outcome.stats50",0,a.recovery_flag,b.recovery_flag);
    value("outcome.finished",0,a.finished,b.finished);
    array("outcome.progress",a.progress,b.progress);
    value("outcome.progress_gate",0,a.progress_gate,b.progress_gate);
    value("outcome.siren",0,a.siren,b.siren);value("outcome.cue",0,a.cue_age,b.cue_age);
    const auto &d=live.dynamics[slot],&e=replay.dynamics[slot];
    integrator("bike.translation","bike.force","bike.rotation","bike.flags",d.bike_physics,e.bike_physics);
    integrator("rider.translation","rider.force","rider.rotation","rider.flags",d.rider_physics,e.rider_physics);
    array("rider.damping_vectors",d.values,e.values);
    value("rider.damping_mode",0,d.damping_mode,e.damping_mode);
    value("rider.effect",0,d.effect,e.effect);value("rider.effect_remaining",0,d.effect_remaining,e.effect_remaining);
    value("bike.recovery_age",0,d.recovery_age,e.recovery_age);
}
}
