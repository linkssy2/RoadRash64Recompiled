#include "rr64_prediction_journal.hpp"
#include "rr64_prediction_context.hpp"
#include "rr64_prediction_cop_state.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_prediction_resources.hpp"
#include "rr64_prediction_rules.hpp"
#include "rr64_prediction_timing.hpp"
#include "rr64_prediction_history.hpp"
#include "ultramodern/rr64_snapshot_gate.hpp"
#include <memory>
#include <mutex>

namespace {
// Rendering supplies loading requests on another native thread. Keep only
// bounded request inputs and ownership evidence, never live allocation pointers.
struct StreamingMailbox {
    std::mutex mutex;
    bool recording=false,in_update=false,complete=true;
    std::vector<rr64::prediction::HistoricalStreaming> events;
} streaming_mailbox;
struct NativeFrame {
    std::uint64_t elapsed_us=0;
    std::vector<rr64::prediction::HistoricalStreaming> streaming;
    bool streaming_complete=true;
    rr64::authority::NativeTiming timing{};
    rr64::prediction::UpdateCounters counters{};
    rr64::prediction::VisibilityInputs visibility{};
    rr64::prediction::TerrainAvailability terrain{};
    rr64::prediction::SessionRules session{};
    rr64::authority::Command command{};
    rr64::prediction::CpuContext entry{};
    rr64::prediction::CopRulesState rules{};
    rr64::prediction::CopPostsState posts{};
    rr64::prediction::ManualEjectState eject{};
    rr64::mk64_items::ReplayState items{};
    rr64::prediction::ResourceInventory resources{};
};
struct History {
    std::vector<rr64::prediction::HistoricalStreaming> pending_streaming;
    bool pending_streaming_complete=true;
    std::uint64_t epoch=0;
    unsigned round=0,last=0;
    rr64::authority::Command pending{};
    rr64::prediction::CpuContext pending_entry{};
    rr64::prediction::SessionRules pending_session{};
    rr64::authority::NativeTiming pending_timing{};
    rr64::prediction::UpdateCounters pending_counters{};
    rr64::prediction::VisibilityInputs pending_visibility{};
    rr64::prediction::TerrainAvailability pending_terrain{};
    rr64::prediction::GuestJournal guest;
    std::vector<unsigned char> scratch=std::vector<unsigned char>(rr64::engine::kRdramSize);
    std::deque<NativeFrame> native;
};
thread_local std::unique_ptr<History> history;
thread_local std::uint64_t next_epoch=0;
bool capture(unsigned char *memory,rr64::authority::Command command) {
    NativeFrame frame{};frame.command=command;frame.entry=history->pending_entry;
    frame.elapsed_us=history->native.empty()?0:history->native.back().elapsed_us+command.duration_us;
    frame.session=history->pending_session;
    frame.timing=history->pending_timing;
    frame.counters=history->pending_counters;
    frame.visibility=history->pending_visibility;frame.terrain=history->pending_terrain;
    frame.streaming=history->pending_streaming;
    frame.streaming_complete=history->pending_streaming_complete;
    if(!ultramodern::rr64::copy_guest_snapshot(memory,history->scratch.data(),history->scratch.size()) ||
       !rr64::prediction::capture_cop_rules(frame.rules) || !rr64::prediction::capture_cop_posts(frame.posts) || !rr64::prediction::capture_manual_eject(frame.eject))return false;
    if(!rr64::prediction::capture_item_state(frame.items))return false;
    // Do not infer a quiescent resource worker from a successful memory copy.
    // Retain the evidence with this exact frame for the replay admission gate.
    frame.resources=rr64::prediction::ResourceInventory::inspect(history->scratch.data());
    // Reserve the native frame first, so allocation failure cannot leave a
    // guest-only history entry. The temporary image is owned by this thread.
    history->native.push_back(frame);
    if(!history->guest.capture(command.round,command.sequence,history->scratch.data(),history->scratch.size())){
        history->native.pop_back();return false;
    }
    history->last=command.sequence;return true;
}
}
namespace rr64::prediction {
bool prepare_history_replay(std::uint32_t round,std::uint32_t acknowledged,
    std::span<const authority::Command> commands,unsigned actor,HistoricalReplay &out,ReplayAdmission *admission){
    if(admission)*admission=ReplayAdmission::Invalid;
    if(active() || !history || history->pending.sequence || history->round!=round ||
       acknowledged>history->last || commands.size()!=std::size_t(history->last-acknowledged) ||
       actor<0x800d8570u || (actor-0x800d8570u)%0x118u || (actor-0x800d8570u)/0x118u>=14)return false;
    try {
        const NativeFrame *baseline=nullptr;
        for(const auto &frame:history->native)
            if(frame.command.sequence==acknowledged){baseline=&frame;break;}
        if(!baseline || !baseline->resources.valid)return false;
        HistoricalReplay candidate{};candidate.round=round;candidate.epoch=history->epoch;
        candidate.acknowledged=acknowledged;candidate.last=history->last;
        candidate.memory.resize(engine::kRdramSize);
        if(!history->guest.restore(round,acknowledged,candidate.memory.data(),candidate.memory.size()))return false;
        candidate.baseline={baseline->rules,baseline->posts,baseline->eject,baseline->items};
        candidate.baseline_elapsed_us=baseline->elapsed_us;
        candidate.steps.reserve(commands.size());
        candidate.completed_eject.reserve(commands.size());
        auto previous=baseline->command.buttons;
        for(const auto &frame:history->native){
            if(frame.command.sequence<=acknowledged)continue;
            const auto i=candidate.steps.size();
            if(i>=commands.size() || frame.command!=commands[i] ||
               frame.command.sequence!=std::uint64_t(acknowledged)+1+i)return false;
            if(!frame.streaming_complete)return false;
            FrameInput input{};input.command=frame.command;input.entry=frame.entry;
            input.session=frame.session;input.timing=frame.timing;input.passes=frame.timing.substeps;
            input.counters=frame.counters;
            input.visibility=frame.visibility;input.terrain=frame.terrain;
            input.actor=actor;input.previous_buttons=previous;
            // Native post-state is carried from the corrected baseline and then
            // each completed replay, not taken from the old uncorrected future.
            candidate.steps.push_back(input);previous=frame.command.buttons;
            candidate.completed_eject.push_back(frame.eject[(actor-0x800d8570u)/0x118u]);
            candidate.streaming.push_back(frame.streaming);
        }
        if(candidate.steps.size()!=commands.size())return false;
        // A held worker task is not a corrupt journal. Its CPU continuation is
        // absent from this snapshot, so wait for a newer host acknowledgement
        // instead of restarting it or discarding outstanding commands. Validate
        // all command metadata first so malformed history cannot be deferred.
        // With no commands to replay, correction needs no resource worker.
        if(!commands.empty() && !baseline->resources.can_start_worker()){
            if(admission)*admission=ReplayAdmission::ResourcePending;
            return false;
        }
        out=std::move(candidate);
        if(admission)*admission=ReplayAdmission::Ready;
        return true;
    }catch(const std::bad_alloc&){return false;}
}
bool commit_history_replay(const HistoricalReplay &plan,ReplayedHistory &result,CommitApproval approve,void *user){
    if(!approve || active() || !history || next_epoch==UINT64_MAX || history->pending.sequence || history->epoch!=plan.epoch ||
       history->round!=plan.round || history->last!=plan.last ||
       result.native.size()!=plan.steps.size()+1 || result.final_memory.size()!=engine::kRdramSize ||
       result.guest.frames()!=result.native.size() || result.resources.size()!=result.native.size())return false;
    try{
        std::deque<NativeFrame> replacement;
        for(const auto &old:history->native){
            if(old.command.sequence<plan.acknowledged)continue;
            const auto index=replacement.size();
            if(index>=result.native.size())return false;
            auto frame=old;const auto &native=result.native[index];
            frame.rules=native.rules;frame.posts=native.posts;frame.eject=native.eject;frame.items=native.items;
            if(!result.guest.contains(plan.round,frame.command.sequence))return false;
            frame.resources=result.resources[index];
            replacement.push_back(frame);
        }
        if(replacement.size()!=result.native.size())return false;
        if(!approve(user))return false;
        history->native.swap(replacement);history->guest.swap(result.guest);
        history->epoch=++next_epoch;
        return true;
    }catch(const std::bad_alloc&){return false;}
}
}
extern "C" void rr64_prediction_capture_reset(){
    std::lock_guard lock(streaming_mailbox.mutex);
    streaming_mailbox.recording=false;streaming_mailbox.in_update=false;
    streaming_mailbox.complete=true;streaming_mailbox.events.clear();history.reset();
}
extern "C" void rr64_prediction_capture_streaming(unsigned char *memory,void *raw){
    if(rr64::prediction::active())return;
    std::lock_guard lock(streaming_mailbox.mutex);
    if(!streaming_mailbox.recording)return;
    // An overlapping update has no established replay order; reject that
    // interval rather than quietly omitting or reordering a loading request.
    if(streaming_mailbox.in_update || !memory || !raw || streaming_mailbox.events.size()>=64){
        streaming_mailbox.complete=false;return;
    }
    try{
        rr64::prediction::HistoricalStreaming event;
        if(!event.input.capture(memory,*static_cast<recomp_context*>(raw))){
            streaming_mailbox.complete=false;return;
        }
        event.schedule=rr64::prediction::ResourceInventory::inspect(memory);
        if(!event.schedule.valid){streaming_mailbox.complete=false;return;}
        streaming_mailbox.events.push_back(std::move(event));
    }catch(const std::bad_alloc&){streaming_mailbox.complete=false;}
}
extern "C" int rr64_prediction_capture_before(unsigned char *memory,const void *input,const void *context){
    if(!input || !context || rr64::prediction::active())return 0;
    rr64::prediction::CpuContext entry;
    if(!entry.capture(*static_cast<const recomp_context*>(context)))return 0;
    rr64::authority::NativeTiming timing{};
    rr64::prediction::UpdateCounters counters{};
    rr64::prediction::VisibilityInputs visibility{};
    rr64::prediction::TerrainAvailability terrain{};
    if(!rr64::prediction::capture_timing(memory,timing) ||
       !rr64::prediction::capture_update_counters(memory,counters) ||
       !rr64::prediction::capture_visibility(memory,visibility) || !rr64::prediction::capture_terrain_availability(memory,terrain))return 0;
    const auto command=*static_cast<const rr64::authority::Command*>(input);
    const auto session=rr64::prediction::capture_session_rules(rr64::netplay::get_status());
    if(!command.round || !command.sequence)return 0;
    try {
        if(!history || history->round!=command.round){
            if(command.sequence!=1 || next_epoch==UINT64_MAX)return 0;
            history=std::make_unique<History>();history->round=command.round;
            history->epoch=++next_epoch;
            history->guest.reset(command.round);
            {
                std::lock_guard lock(streaming_mailbox.mutex);
                streaming_mailbox.events.clear();streaming_mailbox.complete=true;
                streaming_mailbox.recording=true;streaming_mailbox.in_update=false;
            }
            history->pending_entry=entry;
            history->pending_session=session;
            history->pending_timing=timing;
            history->pending_counters=counters;
            history->pending_visibility=visibility;history->pending_terrain=terrain;
            auto baseline=command;baseline.sequence=0;
            // Baseline0 precedes the first command. Reconstruct the preceding
            // button state from the native changed mask instead of treating
            // the first press as already held during replay.
            const unsigned controller=session.replicated_riders?0:session.local_slot;
            std::uint16_t buttons=0,changed=0;
            if(controller>=4 || !rr64::engine::read_u16(memory,rr64::engine::globals::controller_buttons+controller*2,buttons) ||
               !rr64::engine::read_u16(memory,rr64::engine::globals::controller_changed_buttons+controller*2,changed)){
                history.reset();return 0;
            }
            baseline.buttons=buttons^changed;baseline.actions=0;
            if(!capture(memory,baseline)){history.reset();return 0;}
        }
        if(history->pending.sequence || command.sequence!=history->last+1)return 0;
        {
            std::lock_guard lock(streaming_mailbox.mutex);
            history->pending_streaming.clear();
            history->pending_streaming.swap(streaming_mailbox.events);
            history->pending_streaming_complete=streaming_mailbox.complete;
            streaming_mailbox.complete=true;streaming_mailbox.in_update=true;
        }
        history->pending=command;history->pending_entry=entry;history->pending_session=session;
        history->pending_timing=timing;history->pending_counters=counters;
        history->pending_visibility=visibility;history->pending_terrain=terrain;return 1;
    } catch(const std::bad_alloc&){history.reset();return 0;}
}
extern "C" int rr64_prediction_capture_after(unsigned char *memory){
    if(!history || !history->pending.sequence || rr64::prediction::active())return 0;
    try {
        // Snapshot admission may wait for the renderer. Never hold its mailbox
        // lock while taking that snapshot, or the two workers can deadlock.
        if(!capture(memory,history->pending))return 0;
        {
            std::lock_guard lock(streaming_mailbox.mutex);
            history->native.back().streaming_complete &= streaming_mailbox.complete;
            streaming_mailbox.in_update=false;streaming_mailbox.complete=true;
        }
        history->pending={};return 1;
    } catch(const std::bad_alloc&){return 0;}
}
