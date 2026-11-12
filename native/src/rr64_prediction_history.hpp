#pragma once
#include "rr64_prediction_frame.hpp"
#include "rr64_prediction_journal.hpp"
#include "rr64_prediction_entered_edges.hpp"
#include <algorithm>
#include <span>
#include <vector>
#include <type_traits>

namespace rr64::prediction {
struct HistoricalStreaming {
    StreamingInput input;
    ResourceInventory schedule;
};
struct HistoricalReplay {
    std::uint64_t epoch=0;
    std::uint64_t baseline_elapsed_us=0;
    std::uint32_t round=0,acknowledged=0,last=0;
    std::vector<unsigned char> memory;
    FrameOutput baseline;
    std::vector<FrameInput> steps;
    // Operations between the previous completed update and each step's entry.
    std::vector<std::vector<HistoricalStreaming>> streaming;
    // Empty means an unmodified historical verification. Production fills one
    // duration per retained command, including zero for an elapsed interval.
    std::vector<unsigned> remaining_us;
    // Local native protection after each originally completed command. These
    // values are evidence for consumed edges, never blanket future state.
    std::vector<ManualEjectProtection> completed_eject;
};
inline bool align_replay_time(HistoricalReplay &plan,std::uint64_t simulated_us){
    if(simulated_us<plan.baseline_elapsed_us)return false;
    std::vector<unsigned> remaining;remaining.reserve(plan.steps.size());
    auto elapsed=plan.baseline_elapsed_us;
    for(const auto &step:plan.steps){
        const unsigned duration=step.command.duration_us;
        if(!duration || duration>250000 || elapsed>UINT64_MAX-duration)return false;
        elapsed+=duration;
        remaining.push_back(simulated_us>=elapsed?0:static_cast<unsigned>(std::min<std::uint64_t>(duration,elapsed-simulated_us)));
    }
    plan.remaining_us=std::move(remaining);return true;
}
inline bool seed_entered_eject(HistoricalReplay &plan,unsigned actor,const authority::Outcome &outcome){
    if(plan.remaining_us.size()!=plan.steps.size() || plan.completed_eject.size()!=plan.steps.size() ||
       actor<0x800d8570u || (actor-0x800d8570u)%0x118u || (actor-0x800d8570u)/0x118u>=14)return false;
    const unsigned slot=(actor-0x800d8570u)/0x118u;
    // Even a fully acknowledged baseline can contain protection from a locally
    // predicted eject the host rejected. Validate before any replay can heal
    // authoritative damage, including when no pending interval has entered.
    auto candidate=entered_eject_protection(plan.memory.data(),actor,outcome,plan.baseline.eject[slot]);
    for(std::size_t i=0;i<plan.steps.size();++i){
        const auto &input=plan.steps[i];
        if(input.actor!=actor || plan.remaining_us[i]>input.command.duration_us)return false;
        if(plan.remaining_us[i]==input.command.duration_us)break;
        if(input.command.actions&authority::action_eject)
            candidate=entered_eject_protection(plan.memory.data(),actor,outcome,plan.completed_eject[i]);
    }
    plan.baseline.eject[slot]=candidate;
    return true;
}
struct ReplayedHistory {
    GuestJournal guest;
    std::vector<FrameOutput> native;
    // Inspect each completed private image while it is already available.
    // Commit needs this inventory, not an 8 MiB restore of every saved frame.
    std::vector<ResourceInventory> resources;
    std::vector<unsigned char> final_memory;
};
// Obtain one coherent historical baseline and all later input metadata without
// changing either journal or network acknowledgements. The caller must apply
// validated host state to this private image before executing the steps.
enum class ReplayAdmission { Invalid, Ready, ResourcePending };
bool prepare_history_replay(std::uint32_t round,std::uint32_t acknowledged,
    std::span<const authority::Command> commands,unsigned actor,HistoricalReplay&,
    ReplayAdmission *admission=nullptr);
// Host correction must already be applied to plan.memory. Retain each corrected
// state, so the next acknowledgement starts from corrected history too. Failure
// discards the private work and leaves the original plan/journal untouched.
template<class Prepare,class Execute,class Stream=std::nullptr_t>
bool evaluate_history_replay(const HistoricalReplay &plan,std::span<const unsigned char> rom,
    ReplayedHistory &out,Prepare prepare,Execute execute,Stream stream=nullptr){
    if(!plan.round || plan.memory.size()!=engine::kRdramSize || plan.last<plan.acknowledged ||
       plan.steps.size()!=std::size_t(plan.last-plan.acknowledged) ||
       (!plan.streaming.empty() && plan.streaming.size()!=plan.steps.size()) ||
       (!plan.remaining_us.empty() && plan.remaining_us.size()!=plan.steps.size()))return false;
    try{
        Resources resources(plan.memory,rom);
        ReplayedHistory candidate;
        candidate.guest.reset(plan.round,plan.acknowledged);
        if(!candidate.guest.capture(plan.round,plan.acknowledged,resources.memory(),engine::kRdramSize))return false;
        candidate.native.reserve(plan.steps.size()+1);candidate.native.push_back(plan.baseline);
        candidate.resources.reserve(plan.steps.size()+1);
        candidate.resources.push_back(ResourceInventory::inspect(resources.memory()));
        auto native=plan.baseline;
        for(std::size_t i=0;i<plan.steps.size();++i){
            auto input=plan.steps[i];
            if(input.command.round!=plan.round || input.command.sequence!=std::uint64_t(plan.acknowledged)+1+i)return false;
            input.rules=native.rules;input.posts=native.posts;input.eject=native.eject;input.items=native.items;
            if(!plan.streaming.empty())for(const auto &event:plan.streaming[i]){
                if constexpr(std::is_same_v<Stream,std::nullptr_t>)return false;
                else if(!stream(resources,event.input,event.schedule))return false;
            }
            const unsigned remaining=plan.remaining_us.empty()?input.command.duration_us:plan.remaining_us[i];
            if(remaining>input.command.duration_us)return false;
            FrameOutput completed=native;
            if(remaining){
                input.replay_duration_us=remaining;
                if(remaining<input.command.duration_us){
                    if(!slice_timing(input.timing,input.command.duration_us,remaining))return false;
                    // This interval has already entered on the host. A manual
                    // eject or button press must not fire again in its tail.
                    input.command.actions=0;input.previous_buttons=input.command.buttons;
                }
                // Sequence zero is captured BEFORE the first update. Every other
                // baseline is an update completion: rebuild derived race order and
                // neighbors before the next update, just as the verified private
                // continuation does. Never copy the live roster into history.
                if((plan.acknowledged!=0 || i!=0) && !prepare(resources,input))return false;
                if(!execute(resources,input,completed))return false;
            }
            // Keep journals/streaming aligned even when the host has already
            // simulated a whole interval. Skipping physics is not skipping I/O.
            if(!candidate.guest.capture(plan.round,input.command.sequence,resources.memory(),engine::kRdramSize))return false;
            candidate.native.push_back(completed);native=completed;
            candidate.resources.push_back(ResourceInventory::inspect(resources.memory()));
        }
        candidate.final_memory=std::move(resources).release_image();
        out.guest.swap(candidate.guest);out.native.swap(candidate.native);
        out.resources.swap(candidate.resources);out.final_memory.swap(candidate.final_memory);
        return true;
    }catch(const std::bad_alloc&){return false;}
     catch(const Resources::Invalid&){return false;}
     catch(const Resources::Blocked&){return false;}
}
// Validate live-state ownership first. Approval consumes the network ticket
// only AFTER all history allocation/validation succeeds. After approval, the
// journal swap cannot fail. Approval must not reenter or mutate game history.
// This commits historical storage only; it never writes to live RDRAM.
using CommitApproval=bool(*)(void*) noexcept;
bool commit_history_replay(const HistoricalReplay&,ReplayedHistory&,CommitApproval,void*);
}
