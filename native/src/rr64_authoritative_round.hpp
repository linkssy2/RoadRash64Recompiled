#pragma once
#include "rr64_authoritative_input.hpp"

namespace rr64::authority {
constexpr unsigned maximum_players=14, commands_per_packet=16;
struct InputBatch {
    std::uint32_t round=0,count=0;
    std::array<Command,commands_per_packet> commands{};
};
static_assert(sizeof(InputBatch)<1200);
struct Step {
    std::uint32_t round=0;
    std::uint64_t tick=0;
    std::array<Command,maximum_players> inputs{};
    std::array<std::uint16_t,maximum_players> presses{};
};
struct Stamp {
    std::uint32_t round=0;
    std::uint64_t tick=0;
    std::array<std::uint32_t,maximum_players> acknowledged{};
    // Elapsed host simulation since this slot's first contiguous input. This
    // advances during partial/held updates even while acknowledged is unchanged.
    std::array<std::uint64_t,maximum_players> simulated_us{};
};

// Game-thread coordinator. The network hands validated batches to receive;
// native simulation consumes begin's immutable Step and calls finish afterward.
class HostRound {
    std::uint32_t round_=0,humans_=0;
    std::array<HostInput,maximum_players> queues_{};
    std::array<std::uint64_t,maximum_players> missing_us_{};
    std::array<std::uint32_t,maximum_players> entered_{},staged_ack_{};
    std::array<std::uint16_t,maximum_players> entered_buttons_{};
    std::array<bool,maximum_players> started_{};
    std::array<std::uint64_t,maximum_players> staged_us_{};
    std::array<Command,maximum_players> held_{};
    Step staged_{};
    Stamp completed_{};
    bool running_=false;
    std::uint32_t disconnected_=0;
public:
    void disconnect(unsigned slot){
        if(slot==0 || slot>=maximum_players)return;
        disconnected_|=1u<<slot;
        // A currently executing step may finish normally. Its queue commit
        // must remain intact until then, so disconnect is applied at begin.
    }
    bool reset(std::uint32_t round,std::uint32_t humans) {
        if(!round || !humans || humans>>maximum_players)return false;
        *this={};round_=round;humans_=humans;completed_.round=round;
        for(auto &q:queues_)q.reset(round);
        return true;
    }
    bool receive(unsigned authenticated_slot,const InputBatch &batch) {
        if(authenticated_slot>=maximum_players || (disconnected_&(1u<<authenticated_slot)) || !(humans_&(1u<<authenticated_slot)) ||
           batch.round!=round_ || !batch.count || batch.count>commands_per_packet)return false;
        // Reject the whole batch before mutating the queue. Redundant already
        // processed entries are harmless, but future conflicting entries aren't.
        auto candidate=queues_[authenticated_slot];
        for(unsigned i=0;i<batch.count;++i) {
            auto c=batch.commands[i];
            if(!valid(c,round_))return false;
            if(authenticated_slot)c.buttons&=~std::uint16_t(0x1000); // host-only pause
            if(c.sequence<=candidate.processed())continue;
            if(!candidate.receive(c))return false;
        }
        queues_[authenticated_slot]=candidate;return true;
    }
    bool begin(Step &out,std::uint32_t duration_us=16667) {
        if(!round_ || running_ || !duration_us || duration_us>250000)return false;
        staged_={};staged_.round=round_;staged_.tick=completed_.tick+1;
        staged_ack_={};staged_us_=completed_.simulated_us;
        for(unsigned s=0;s<maximum_players;++s)if(humans_&(1u<<s)) {
            if(disconnected_&(1u<<s)){queues_[s].discard_pending();held_[s]={};continue;}
            if(!started_[s] && queues_[s].has_next())started_[s]=true;
            if(!started_[s])continue;
            staged_us_[s]+=duration_us;
            Command next{};bool fresh=false;
            if(queues_[s].stage_timed(staged_us_[s],entered_[s],entered_buttons_[s],next,
                                      staged_.presses[s],staged_ack_[s],fresh)) {
                held_[s]=next;held_[s].actions=0;staged_.inputs[s]=next;
                // A tap released inside the burst still occupies this native
                // update. The retained held state remains the newest sample,
                // so it releases on the following update instead of sticking.
                staged_.inputs[s].buttons|=staged_.presses[s];
                if(!fresh)staged_.inputs[s].sequence=0;
            }
            else {
                staged_.inputs[s]=held_[s];staged_.inputs[s].sequence=0;staged_.inputs[s].actions=0;
            }
            // A network scheduling gap must not release throttle after merely
            // two host frames. Bound stale controls in elapsed time instead.
            missing_us_[s]=fresh?0:missing_us_[s]+duration_us;
            if(missing_us_[s]>250000){staged_.inputs[s].buttons=0;staged_.inputs[s].x=staged_.inputs[s].y=0;}
        }
        running_=true;out=staged_;return true;
    }
    bool finish(std::uint64_t tick) {
        if(!running_ || tick!=staged_.tick)return false;
        for(unsigned s=0;s<maximum_players;++s) {
            const auto sequence=staged_ack_[s];
            if(sequence && !queues_[s].commit(sequence))return false;
            completed_.acknowledged[s]=queues_[s].processed();
            completed_.simulated_us[s]=staged_us_[s];
        }
        completed_.tick=tick;running_=false;return true;
    }
    Stamp stamp() const{return completed_;}
};
}
