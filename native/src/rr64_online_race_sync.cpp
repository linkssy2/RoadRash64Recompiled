#include "rr64_authoritative_dynamics_native.hpp"
#include "rr64_authoritative_loading.hpp"
#include "rr64_prediction_correction.hpp"
#include "rr64_prediction_reconcile.hpp"
#include "rr64_prediction_presentation.hpp"
#include <mutex>
#include "rr64_remote_presentation.hpp"
#include <chrono>
#include "rr64_online_race_sync.hpp"
#include "rr64_local_players.hpp"
#include "rr64_highlights.hpp"
#include "rr64_highlight_camera.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

#include "recomp.h"

#include "rr64_native.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_sync_log.hpp"
#include "rr64_traffic_sync_capture.hpp"
#include "rr64_attack_visual_memory.hpp"
#include "rr64_authoritative_capture.hpp"
#include "rr64_prediction_cop_state.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_course_items.hpp"
#include "rr64_course_hazards.hpp"
#include "rr64_mk64_items.hpp"
#endif

namespace rr64::online_race_sync {
namespace {

// A frame owns one offset per remote pair. Render hooks consume only this
// immutable result, never a newly arrived packet halfway through drawing.
struct PresentationOffset { unsigned bike=0,rider=0; std::array<float,3> delta{}; };
struct AttackPresentation { unsigned rider=0; AttackVisual state{}; };
std::array<AttackPresentation,netplay::kMaximumPlayers> published_attacks{};
thread_local std::array<AttackPresentation,netplay::kMaximumPlayers> frame_attacks{};
thread_local std::array<attack_visual::Saved,netplay::kMaximumPlayers> saved_attacks{};
thread_local unsigned attack_scope_depth=0;
thread_local unsigned char *attack_scope_mapping=nullptr;
thread_local std::array<RemotePresentation,netplay::kMaximumPlayers> presentation_history{};
thread_local std::array<PresentationOffset,netplay::kMaximumPlayers> presentation_offsets{};
std::array<std::atomic_uint,netplay::kMaximumPlayers> presentation_matrix_counts{};
std::mutex presentation_mutex;
std::array<PresentationOffset,netplay::kMaximumPlayers> published_offsets{};
unsigned char *published_mapping=nullptr;
thread_local std::array<PresentationOffset,netplay::kMaximumPlayers> draw_offsets{};
thread_local bool presentation_draw=false;
thread_local bool presentation_authority=false;
thread_local unsigned presentation_round=0;
thread_local unsigned char *presentation_mapping=nullptr;
std::uint32_t g_local_tick = 0;
thread_local ViewportRenderPlan g_viewport_render_plan{};
std::atomic_bool g_render_race{false};
std::atomic_bool g_render_results{false};
std::atomic_bool g_host_pause_open{false};

float read_float(unsigned char *rdram, std::uint32_t address) {
    float value = 0.0f;
    engine::read_float(rdram, address, value);
    return value;
}

void write_float(unsigned char *rdram, std::uint32_t address, float value) {
    engine::write_float(rdram, address, value);
}

bool read_vector(unsigned char *rdram, std::uint32_t address, float &x, float &y, float &z) {
    if (!engine::valid_guest_range(address, 12)) {
        return false;
    }
    x = read_float(rdram, address + 0);
    y = read_float(rdram, address + 4);
    z = read_float(rdram, address + 8);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

void write_vector(unsigned char *rdram, std::uint32_t address, float x, float y, float z) {
    if (!engine::valid_guest_range(address, 12) || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z)) {
        return;
    }
    write_float(rdram, address + 0, x);
    write_float(rdram, address + 4, y);
    write_float(rdram, address + 8, z);
}

std::uint32_t bike_pool(unsigned char *rdram) {
    if (rdram == nullptr) {
        return 0;
    }
    std::uint32_t pool = 0;
    engine::read_u32(rdram, engine::globals::bike_pool_pointer, pool);
    return pool;
}

std::uint32_t active_racers(unsigned char *rdram) {
    if (rdram == nullptr) {
        return 0;
    }
    std::uint32_t count_bits = 0;
    // EF5C is the four-controller menu count. 6574 is the populated
    // bike/rider count, including network riders represented by AI slots.
    engine::read_u32(rdram, 0x800A6574u, count_bits);
    const std::int32_t count = static_cast<std::int32_t>(count_bits);
    return count > 0 ? std::min<std::uint32_t>(count, netplay::kMaximumPlayers) : 0;
}

// Racer slots own allocated bikes through +E0. The allocator is a free list
// (4621C), so pool order is not a stable player identity after reuse.
std::uint32_t bike_for_guest(unsigned char *m,unsigned guest) {
    if (guest>=active_racers(m)) return 0;
    unsigned bike=0,rider=0,owner=0,actor_rider=0;
    const unsigned actor=0x800D8570u+guest*0x118u;
    if (!engine::read_u32(m,actor+0xe0u,bike) || !engine::valid_guest_range(bike,engine::bike::stride) ||
        !engine::read_u32(m,actor+0xe4u,actor_rider) ||
        !engine::read_u32(m,bike+engine::bike::rider_pointer,rider) || rider!=actor_rider ||
        !engine::valid_guest_range(rider,engine::rider::stride) ||
        !engine::read_u32(m,rider+engine::rider::bike_pointer,owner) || owner!=bike) return 0;
    return bike;
}

std::uint8_t guest_index_for_slot(std::uint8_t network_slot, std::uint8_t local_slot) {
    return static_cast<std::uint8_t>(online_flow::mapped_slot(network_slot,local_slot,true));
}

std::uint8_t network_slot_for_guest_index(std::uint8_t guest_index, std::uint8_t local_slot,
                                         bool replicated_riders) {
    return static_cast<std::uint8_t>(online_flow::mapped_slot(guest_index,local_slot,replicated_riders));
}

std::string safe_display_name(const netplay::Status &status, std::uint8_t slot) {
    std::string result;
    if (slot < netplay::kMaximumPlayers) {
        result = status.players[slot].name;
    }
    if (result.empty()) {
        result = "Rider " + std::to_string(static_cast<unsigned>(slot) + 1u);
    }
    for (char &character : result) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value < 0x20u || value > 0x7Eu) {
            character = '_';
        }
    }
    result.resize(std::min<std::size_t>(result.size(),
                                        engine::globals::multiplayer_display_name_stride - 1u));
    return result;
}

void apply_online_display_names(unsigned char *rdram) {
    const netplay::Status status = netplay::get_status();
    if (!status.active && rdram && local_players::active.load(std::memory_order_acquire)) {
        static_assert(engine::globals::multiplayer_display_name_stride == 12);
        const auto names = local_players::snapshot();
        for (unsigned slot = 0; slot < names.size(); ++slot) {
            const auto address = engine::globals::multiplayer_display_names +
                                 slot * engine::globals::multiplayer_display_name_stride;
            for (unsigned i = 0; i < engine::globals::multiplayer_display_name_stride; ++i) {
                engine::write_s8(rdram, address + i, i < names[slot].size() ? names[slot][i] : 0);
            }
        }
        return;
    }
    if (!status.active || !status.connected || status.phase < netplay::Phase::CharacterSelect ||
        status.local_slot >= netplay::kMaximumPlayers) {
        return;
    }

    for (std::uint8_t guest = 0; guest < engine::globals::multiplayer_display_name_count; ++guest) {
        const std::uint8_t slot =
            network_slot_for_guest_index(guest, status.local_slot, status.replicated_riders);
        const std::string name = safe_display_name(status, slot);
        const std::uint32_t address =
            engine::globals::multiplayer_display_names +
            static_cast<std::uint32_t>(guest) * engine::globals::multiplayer_display_name_stride;
        for (std::uint32_t index = 0; index < engine::globals::multiplayer_display_name_stride;
             ++index) {
            const char character = index < name.size() ? name[index] : '\0';
            engine::write_s8(rdram, address + index, static_cast<std::int8_t>(character));
        }
    }
}

bool capture_bike(unsigned char *rdram, std::uint32_t bike, netplay::RiderState &state) {
    if (!engine::valid_guest_range(bike, engine::bike::stride)) {
        return false;
    }
    std::uint32_t rider=0, owner=0;
    if (engine::read_u32(rdram,bike+engine::bike::rider_pointer,rider) &&
        engine::valid_guest_range(rider,engine::rider::stride) &&
        engine::read_u32(rdram,rider+engine::rider::bike_pointer,owner) && owner==bike) {
        // Root sources verified at 5EA6C (bike), 5EC3C (rider),
        // 5E8C0/5E954 (mounted height), 5E94C (detached height).
        bool ok=true;
        // 37EEC/37F04 builds wheel position from +16C. 37F0C/37F20
        // builds wheel velocity from +194. 3800C consumes +178; 36B00
        // compares rider +B4 against bike +194. Synchronize these sources
        // together so the next physical update does not restart elsewhere.
        for (unsigned i=0;i<3;++i) {
            ok &= engine::read_float(rdram,bike+0x16cu+i*4,state.root.bike_origin[i]) && std::isfinite(state.root.bike_origin[i]);
            ok &= engine::read_float(rdram,bike+0x194u+i*4,state.root.bike_velocity[i]) && std::isfinite(state.root.bike_velocity[i]);
            ok &= engine::read_float(rdram,bike+0x178u+i*4,state.root.bike_motion[i]) && std::isfinite(state.root.bike_motion[i]);
            ok &= engine::read_float(rdram,rider+0xb4u+i*4,state.root.rider_velocity[i]) && std::isfinite(state.root.rider_velocity[i]);
            // 5E6D8 consumes +5DC for the final detailed rider root; +8C alone
            // does not replace this simulation-produced anchor after correction.
            ok &= engine::read_float(rdram,rider+0x5dcu+i*4,state.root.rider_anchor[i]) && std::isfinite(state.root.rider_anchor[i]);
        }
        for(unsigned i=0;i<4;++i) {
            ok &= engine::read_float(rdram,bike+0x244u+i*4,state.root.bike_rotation[i]) && std::isfinite(state.root.bike_rotation[i]);
            ok &= engine::read_float(rdram,rider+0x164u+i*4,state.root.rider_rotation[i]) && std::isfinite(state.root.rider_rotation[i]);
        }
        constexpr unsigned display_offsets[]={0x21c,0x4ac,0x4b8};
        for(unsigned i=0;i<3;++i)
            ok &= engine::read_float(rdram,bike+display_offsets[i],state.root.bike_display_angles[i]) && std::isfinite(state.root.bike_display_angles[i]);
        ok &= engine::read_u16(rdram,bike+engine::bike::drive_control_lockout,state.root.drive_lockout);
        ok &= engine::read_u16(rdram,bike+0x818u,state.root.vault_latch) && state.root.vault_latch<=3;
        ok &= engine::read_float(rdram,bike+0x550u,state.root.bike_height) && std::isfinite(state.root.bike_height);
        ok &= engine::read_float(rdram,rider+0x238u,state.root.rider_height) && std::isfinite(state.root.rider_height);
        ok &= engine::read_u16(rdram,bike+engine::bike::rider_attached,state.root.bike_attached);
        ok &= engine::read_u16(rdram,rider+engine::rider::bike_attached,state.root.rider_attached);
        ok &= engine::read_u16(rdram,rider+engine::rider::ejected,state.root.ejected);
        ok &= engine::read_float(rdram,rider+0x310,state.root.rider_impact_reserve) && std::isfinite(state.root.rider_impact_reserve);
        ok &= engine::read_float(rdram,bike+engine::bike::durability_current,state.root.durability) && std::isfinite(state.root.durability);
        ok &= engine::read_float(rdram,bike+engine::bike::durability_capacity,state.root.durability_capacity) && std::isfinite(state.root.durability_capacity);
        unsigned weapon=0;
        if(engine::read_u32(rdram,rider+engine::rider::selected_weapon,weapon) && weapon>=1 && weapon<=14) {
            state.weapon=static_cast<std::uint8_t>(weapon); state.root.equipment_valid=1;
            for(unsigned i=0;i<state.root.inventory.size();++i)
                ok &= engine::read_u16(rdram,bike+0x838u+i*2u,state.root.inventory[i]);
        }
        state.root.valid=ok ? 1u : 0u;
        state.root.attack=attack_visual::capture(rdram,rider);
        // 5E948/19FE8 consume the separate rider world position at +8C.
        state.rider_position_valid=read_vector(rdram,rider+0x8cu,
            state.rider_x,state.rider_y,state.rider_z) ? 1u : 0u;
    }
    return read_vector(rdram, bike + engine::bike::body_position, state.position_x,
                       state.position_y, state.position_z) &&
           read_vector(rdram, bike + engine::bike::front_wheel_position, state.front_wheel_x,
                       state.front_wheel_y, state.front_wheel_z) &&
           read_vector(rdram, bike + engine::bike::rear_wheel_position, state.rear_wheel_x,
                       state.rear_wheel_y, state.rear_wheel_z);
}

void apply_bike(unsigned char *rdram, std::uint32_t bike, const netplay::RiderState &state) {
    std::uint32_t rider=0,owner=0;
    // Resolve the complete local pair before any writes. Never copy pointers
    // from another process, and never install half of an attachment snapshot.
    if (!state.root.valid || !state.rider_position_valid || state.root.vault_latch>3 ||
        !engine::valid_guest_range(bike,engine::bike::stride) ||
        !engine::read_u32(rdram,bike+engine::bike::rider_pointer,rider) ||
        !engine::valid_guest_range(rider,engine::rider::stride) ||
        !engine::read_u32(rdram,rider+engine::rider::bike_pointer,owner) || owner!=bike) return;
    for (unsigned i=0;i<3;++i) {
        write_float(rdram,bike+0x16cu+i*4,state.root.bike_origin[i]);
        write_float(rdram,bike+0x194u+i*4,state.root.bike_velocity[i]);
        write_float(rdram,bike+0x178u+i*4,state.root.bike_motion[i]);
        write_float(rdram,rider+0xb4u+i*4,state.root.rider_velocity[i]);
        write_float(rdram,rider+0x5dcu+i*4,state.root.rider_anchor[i]);
    }
    write_vector(rdram,bike+engine::bike::body_position,state.position_x,state.position_y,state.position_z);
    write_vector(rdram,bike+engine::bike::front_wheel_position,state.front_wheel_x,state.front_wheel_y,state.front_wheel_z);
    write_vector(rdram,bike+engine::bike::rear_wheel_position,state.rear_wheel_x,state.rear_wheel_y,state.rear_wheel_z);
    write_vector(rdram,rider+0x8cu,state.rider_x,state.rider_y,state.rider_z);
    for(unsigned i=0;i<4;++i) {
        write_float(rdram,bike+0x244u+i*4,state.root.bike_rotation[i]);
        write_float(rdram,rider+0x164u+i*4,state.root.rider_rotation[i]);
    }
    constexpr unsigned display_offsets[]={0x21c,0x4ac,0x4b8};
    for(unsigned i=0;i<3;++i) write_float(rdram,bike+display_offsets[i],state.root.bike_display_angles[i]);
    write_float(rdram,bike+0x550u,state.root.bike_height);
    write_float(rdram,rider+0x238u,state.root.rider_height);
    write_float(rdram,rider+0x310,state.root.rider_impact_reserve);
    write_float(rdram,bike+engine::bike::durability_current,state.root.durability);
    write_float(rdram,bike+engine::bike::durability_capacity,state.root.durability_capacity);
    // 40EAC cycles +5B0 against the fifteen halfword inventory entries.
    // Copy values only; weapon model/graph pointers remain local.
    if(state.root.equipment_valid && state.weapon>=1 && state.weapon<=14) {
        for(unsigned i=0;i<state.root.inventory.size();++i)
            engine::write_u16(rdram,bike+0x838u+i*2u,state.root.inventory[i]);
        engine::write_u32(rdram,rider+engine::rider::selected_weapon,state.weapon);
    }
    // 5AFD0 chooses crash versus riding pose from this bike flag.
    engine::write_u16(rdram,bike+engine::bike::drive_control_lockout,state.root.drive_lockout);
    // Restore the host's finite native vault phase, including its zero reset.
    // Wheelie input/state remain owned by the native local simulation.
    engine::write_u16(rdram,bike+0x818u,state.root.vault_latch);
    engine::write_u16(rdram,bike+engine::bike::rider_attached,state.root.bike_attached);
    engine::write_u16(rdram,rider+engine::rider::bike_attached,state.root.rider_attached);
    engine::write_u16(rdram,rider+engine::rider::ejected,state.root.ejected);
}

void apply_remote_riders(unsigned char *rdram, bool prepare_presentation=false) {
    if (prepare_presentation) { presentation_offsets={}; frame_attacks={}; }
    const netplay::Status status = netplay::get_status();
    if(presentation_authority!=status.authoritative || presentation_round!=status.game_setup.revision) {
        presentation_history={};presentation_offsets={};frame_attacks={};
        prediction::local_correction_presentation.reset();
        presentation_authority=status.authoritative;presentation_round=status.game_setup.revision;
    }
    if (!status.active || !status.connected || status.host_disconnected ||
        (status.authoritative && status.is_host) ||
        status.phase != netplay::Phase::Race || status.local_slot >= netplay::kMaximumPlayers) {
        presentation_history={}; presentation_offsets={};
        prediction::local_correction_presentation.reset();
        { std::lock_guard lock(presentation_mutex); published_offsets={}; published_mapping=nullptr; }
        return;
    }

    const std::uint32_t pool = bike_pool(rdram);
    const std::uint32_t count = active_racers(rdram);
    if (!engine::valid_guest_range(pool, count * engine::bike::stride)) {
        return;
    }

    // Fetch once: do not combine roots from a newer tick with damping state
    // copied from the preceding host frame during a concurrent network update.
    netplay::AuthorityFrame authority_frame{};
    if(status.authoritative && !netplay::authority_get_frame(authority_frame))return;
    if(status.authoritative) {
        auto remote_frame=authority_frame;
        // The local rider belongs to prediction/reconciliation, not the remote
        // presentation path. Apply every other pair from this one tick together.
        remote_frame.riders[status.local_slot].active=false;
        // Outcome-only records are independently applicable. Excluding only
        // the pose still overwrites local crash/progress state before its
        // reconciliation ticket has been accepted.
        remote_frame.outcomes[status.local_slot].valid=0;
        prediction::MovementCorrection correction;
        if(!correction.prepare(rdram,remote_frame,status.local_slot,status.replicated_riders,true) ||
           !correction.commit([](void*) noexcept {return true;},nullptr))return;
#ifdef RR64_EXPERIMENTAL_COURSE
        // The same pinned host tick owns item presentation and rider equipment.
        // Client prediction never consumes boxes or rolls another reward.
        if(!course_items::apply_state(authority_frame.course_items,authority_frame.stamp.round,
                                     authority_frame.stamp.tick) ||
           !course_hazards::apply_state(authority_frame.course_hazards,authority_frame.stamp.round,
                                       authority_frame.stamp.tick) ||
           !mk64_items::apply_state(authority_frame.mk64_items,authority_frame.stamp.round,
                                   authority_frame.stamp.tick)) {
            netplay::authority_fail("course item state could not be applied");return;
        }
#endif
    }
    for (std::uint8_t slot = 0; slot < netplay::kMaximumPlayers; ++slot) {
        if (slot == status.local_slot) {
            presentation_history[slot].reset();
            if(prepare_presentation && status.authoritative) {
                const auto guest=online_flow::mapped_slot(slot,status.local_slot,status.replicated_riders);
                const auto pose=prediction::mounted_pose(rdram,authority_frame.stamp.round,guest);
                const auto delta=prediction::local_correction_presentation.sample(pose,prediction::presentation_time_us());
                if(pose.valid)presentation_offsets[slot]={pose.bike,pose.rider,delta};
            }
            continue;
        }
        if (status.is_host && !status.players[slot].connected) {
            presentation_history[slot].reset();
            continue;
        }
        const std::uint8_t guest_index = status.replicated_riders
            ? guest_index_for_slot(slot, status.local_slot) : slot;
        if (guest_index >= count) {
            continue;
        }
        netplay::RiderState state{};
        const bool available=status.authoritative
            ? (state=authority_frame.riders[slot],state.active)
            : netplay::get_rider_state(slot,state);
        if (available) {
            const unsigned bike=bike_for_guest(rdram,guest_index);
            if (state.host_ai) {
                unsigned kind=0,model=0;
                const unsigned a=0x800D8570u+guest_index*0x118u;
                engine::read_u32(rdram,a+0x18,model); engine::read_u32(rdram,a+0x1C,kind);
                if(kind!=state.character || model!=state.bike) continue;
            }
            if(!status.authoritative)apply_bike(rdram, bike, state);
            if (prepare_presentation && bike) {
                unsigned rider=0;
                if (!engine::read_u32(rdram,bike+engine::bike::rider_pointer,rider)) continue;
                const auto now=std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const auto shown=presentation_history[slot].sample(state,now);
                frame_attacks[slot]={rider,shown.root.attack};
                presentation_offsets[slot]={bike,rider,{shown.position_x-state.position_x,
                    shown.position_y-state.position_y,shown.position_z-state.position_z}};
            }
        }
    }
    if (prepare_presentation) {
        std::lock_guard lock(presentation_mutex);
        published_offsets=presentation_offsets;
        published_attacks=frame_attacks;
        published_mapping=rdram;
    }
}

void capture_local_rider(unsigned char *rdram) {
    const netplay::Status status = netplay::get_status();
    if (!status.active || !status.connected || status.phase != netplay::Phase::Race ||
        status.local_slot >= netplay::kMaximumPlayers) {
        return;
    }
    const std::uint32_t pool = bike_pool(rdram);
    const std::uint32_t count = active_racers(rdram);
    const std::uint8_t guest_index = status.replicated_riders ? 0 : status.local_slot;
    if (guest_index >= count || !engine::valid_guest_range(pool, count * engine::bike::stride)) {
        return;
    }
    netplay::RiderState state{};
    if (!capture_bike(rdram, bike_for_guest(rdram,guest_index),
                      state)) {
        return;
    }
    if (!state.root.valid || !state.rider_position_valid) return;
    state.active = true;
    state.tick = ++g_local_tick;
    state.sample_time_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    state.character = status.players[status.local_slot].character;
    netplay::set_local_rider_state(state);
    // Publish existing AI roster actors from the host. Identity mismatch on a
    // client is rejected; a local allocator pointer is never sent over UDP.
    if(status.is_host) for(unsigned slot=status.connected_players;slot<count;++slot) {
        netplay::RiderState ai{}; unsigned character=0,model=0;
        const unsigned a=0x800D8570u+slot*0x118u;
        engine::read_u32(rdram,a+0x18,model); engine::read_u32(rdram,a+0x1C,character);
        if(character>44 || model>31 || !capture_bike(rdram,bike_for_guest(rdram,slot),ai) ||
            !ai.root.valid || !ai.rider_position_valid) continue;
        ai.active=true; ai.tick=state.tick; ai.sample_time_us=state.sample_time_us; ai.character=character; ai.bike=model;
        netplay::set_host_ai_rider_state(slot,ai);
    }
}

void capture_sync_sample(unsigned char *rdram, unsigned stage) {
    if (!sync_log::writer) return;
    static unsigned race = 0, frame = 0;
    static bool racing = false;
    const auto status = netplay::get_status();
    const bool campaign = sync_log::campaign() && !status.active;
    const bool live = (campaign || (status.active && status.connected)) && g_render_race.load();
    if (!live) { racing = false; return; }
    if (!racing) { ++race; frame = 0; racing = true; }
    // First pre-update captures initial placement; every post-update captures
    // the local result. Local frame numbers are NOT a shared simulation clock.
    if (stage == 0 && frame != 0) return;
    // Campaign observation only: sample at 10 Hz without replaying or modifying
    // native state. Keep real update counts even between sampled frames.
    static auto last_campaign = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (campaign && stage==1 && frame && now-last_campaign<std::chrono::milliseconds(100)) { ++frame; return; }
    if (campaign) last_campaign=now;
    sync_log::Sample s{};
    s.reconciliation=prediction::last_reconcile;
    s.host=status.is_host; s.local=status.local_slot; s.race=race;
    s.frame=frame; s.phase=unsigned(status.phase); s.stage=stage;
    s.options=status.game_setup.race_options;
    s.us=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    engine::read_u32(rdram, engine::globals::random_state, s.rng);
    s.setup=2166136261u;
    for (const auto address : engine::globals::multiplayer_game_setup_words) {
        unsigned value=0;
        engine::read_u32(rdram,address,value);
        s.setup=(s.setup^value)*16777619u;
    }
    const auto root_hash=[](const netplay::RiderState &state) {
        const auto &root=state.root;
        unsigned hash=2166136261u;
        const auto word=[&](unsigned x){hash=(hash^x)*16777619u;};
        const auto scalar=[&](float x){unsigned bits;std::memcpy(&bits,&x,4);word(bits);};
        word(state.weapon);word(root.valid);for(float x:root.bike_display_angles)scalar(x);for(float x:root.bike_rotation)scalar(x);for(float x:root.rider_rotation)scalar(x);
        for(const auto &v:{root.bike_origin,root.bike_velocity,root.bike_motion,root.rider_velocity,root.rider_anchor})
            for(float x:v)scalar(x);
        scalar(root.bike_height);scalar(root.rider_height);scalar(root.rider_impact_reserve);scalar(root.durability);scalar(root.durability_capacity);word(root.equipment_valid);for(auto v:root.inventory)word(v);
        word(root.bike_attached);word(root.rider_attached);word(root.ejected);word(root.drive_lockout);word(root.vault_latch);return hash;
    };
    const auto count=active_racers(rdram);
    if(campaign) {
        world_sync::Snapshot traffic{};
        s.traffic_valid=world_sync::capture_traffic(rdram,race,static_cast<std::uint64_t>(frame)+1,traffic);
        if(s.traffic_valid) {
            s.traffic_hash=2166136261u;
            for(const auto& car:traffic.traffic) if(car.active) {
                ++s.traffic_count;
                s.traffic_hash=(s.traffic_hash^car.id)*16777619u;
                for(float v:car.position) { unsigned bits;std::memcpy(&bits,&v,4);s.traffic_hash=(s.traffic_hash^bits)*16777619u; }
            }
        }
    }
    for (unsigned slot=0; slot<netplay::kMaximumPlayers; ++slot) {
        netplay::RiderState wire{};
        const bool have_wire=netplay::get_rider_state(static_cast<std::uint8_t>(slot),wire);
        if (campaign ? slot>=count : (!status.players[slot].connected && (!have_wire || !wire.host_ai))) continue;
        auto &r=s.riders[slot];
        r.host_ai=wire.host_ai;
        const unsigned guest=status.replicated_riders
            ? guest_index_for_slot(static_cast<std::uint8_t>(slot),status.local_slot) : slot;
        r.actor_bike=bike_for_guest(rdram,guest);
        netplay::RiderState local{}, received{};
        if (guest<count && capture_bike(rdram,bike_for_guest(rdram,guest),local)) {
            r.weapon=local.weapon;r.durability=local.root.durability;
            r.bike_position=local.root.bike_origin;r.body_position=local.root.rider_anchor;
            r.body_velocity=local.root.rider_velocity;
            r.bike_attached=local.root.bike_attached;r.body_attached=local.root.rider_attached;r.ejected=local.root.ejected;
            r.root_hash=root_hash(local); r.valid=1; r.x=local.position_x; r.y=local.position_y; r.z=local.position_z;
        }
        std::uint16_t buttons=0;
        r.input_valid=netplay::get_player_input(static_cast<std::uint8_t>(slot),buttons,r.stick_x,r.stick_y);
        r.buttons=buttons;
        r.presentation_matrices=presentation_matrix_counts[slot].load(std::memory_order_relaxed);
        r.presentation_delta=presentation_offsets[slot].delta;
        if (netplay::get_rider_state(static_cast<std::uint8_t>(slot),received) && received.active) {
            r.received_weapon=received.weapon;r.received_durability=received.root.durability;
            r.received_root_hash=root_hash(received); r.received=1; r.tick=received.tick;
            r.received_x=received.position_x; r.received_y=received.position_y;
            r.received_z=received.position_z;
        }
    }
    sync_log::writer->submit(s);
    if (stage==1) ++frame;
}

} // namespace

std::uint32_t requested_racer_count(std::uint32_t original_count) {
    const netplay::Status status = netplay::get_status();
    if (!status.active || !status.connected || status.phase < netplay::Phase::GameSetup) {
        return original_count;
    }
    // This hook is in the controller-count chooser, not the racer pool.
    return status.replicated_riders ? 1u : std::clamp<unsigned>(status.connected_players, 1u, 4u);
}

std::uint32_t prepare_render_layout(std::uint32_t stock_layout) {
    // A cinematic owns camera/matrix bank 0 regardless of this peer's rider
    // slot. Do not restore a different logical bank after its physical viewport.
    if (highlight_camera::active()) {
        g_viewport_render_plan = {};
        return 0;
    }
    const netplay::Status status = netplay::get_status();
    g_viewport_render_plan =
        make_viewport_render_plan(stock_layout, status.active, status.connected,
                                  status.phase == netplay::Phase::Race &&
                                      (g_render_race.load() || g_render_results.load()),
                                  status.replicated_riders, status.local_slot);
    return g_viewport_render_plan.layout;
}

std::uint32_t first_render_viewport(std::uint32_t stock_viewport) {
    return g_viewport_render_plan.peer_fullscreen ? g_viewport_render_plan.first_viewport
                                                  : stock_viewport;
}

std::uint32_t geometry_render_viewport(std::uint32_t stock_viewport) {
    return g_viewport_render_plan.peer_fullscreen ? g_viewport_render_plan.geometry_viewport
                                                  : stock_viewport;
}

void restore_active_render_viewport(unsigned char *rdram, std::uint32_t stock_viewport) {
    if (!g_viewport_render_plan.peer_fullscreen) {
        return;
    }
    const std::uint32_t local_viewport = g_viewport_render_plan.first_viewport;
    if (stock_viewport == local_viewport) {
        engine::write_u32(rdram, engine::globals::active_viewport, local_viewport);
    }
}

void before_guest_update(unsigned char *rdram, std::uint32_t mode) {
    const auto online=netplay::get_status();
    if(!online.active || !online.connected || online.phase!=netplay::Phase::Race) {
        std::lock_guard lock(presentation_mutex);
        published_attacks={};
    }
    std::uint16_t pause=0;
    engine::read_u16(rdram,engine::globals::pause_menu_state,pause);
    g_host_pause_open.store(pause!=0);
    std::uint32_t pending = 0;
    engine::read_u32(rdram, engine::globals::pending_mode, pending);
    g_render_race.store(engine::is_live_race_transition(mode, pending));
    g_render_results.store(engine::is_race_results_mode(mode));
    // The optional queue replaces synchronous transition logging on this thread.
    capture_sync_sample(rdram, 0);
    if (rr64_is_live_race_mode(mode) != 0) {
        apply_online_display_names(rdram);
        apply_remote_riders(rdram);
    }
}

void after_guest_update(unsigned char *rdram, std::uint32_t mode) {
    std::uint32_t pending = 0;
    engine::read_u32(rdram, engine::globals::pending_mode, pending);
    g_render_race.store(engine::is_live_race_transition(mode, pending));
    g_render_results.store(engine::is_race_results_mode(mode));
    if (rr64_is_live_race_mode(mode) == 0) {
        return;
    }
    // Positions are committed before pose preparation, not after it: otherwise
    // the memory trace and the already-prepared visible geometry disagree.
    capture_sync_sample(rdram, 1);
}

} // namespace rr64::online_race_sync

bool rr64::prediction::capture_prediction_movement(unsigned char *m,const authority::Stamp &stamp,
        unsigned local,unsigned humans,bool mapped,netplay::AuthorityFrame &out,const char **failure,bool diagnostic_ai_roster){
    if(failure)*failure="local-slot";
    if(local>=14)return false;
    unsigned guest_humans=0;
    for(unsigned slot=0;slot<14;++slot)if(humans&(1u<<slot))
        guest_humans|=1u<<online_flow::mapped_slot(slot,local,mapped);
    netplay::AuthorityFrame guest;
    if(!authority::capture_frame(m,stamp,guest_humans,0,guest,online_race_sync::capture_bike,failure,diagnostic_ai_roster))return false;
    auto canonical=guest;
    for(unsigned slot=0;slot<14;++slot){
        const auto index=online_flow::mapped_slot(slot,local,mapped);
        canonical.riders[slot]=guest.riders[index];canonical.dynamics[slot]=guest.dynamics[index];
        canonical.outcomes[slot]=guest.outcomes[index];
    }
    out=canonical;return true;
}

// Scoped around stock pose builders, including their nested calls. Physics and
// combat resume with byte-identical controller fields when the builder exits.
extern "C" void rr64_online_attack_pose_begin(unsigned char *rdram) {
    using namespace rr64::online_race_sync;
    if(attack_scope_depth++) return;
    attack_scope_mapping=rdram;
    std::array<AttackPresentation,rr64::netplay::kMaximumPlayers> attacks{};
    {
        std::lock_guard lock(presentation_mutex);
        if(g_render_race.load() && published_mapping==rdram) attacks=published_attacks;
    }
    for(unsigned i=0;i<attacks.size();++i)
        saved_attacks[i].apply(rdram,attacks[i].rider,attacks[i].state);
}
extern "C" void rr64_online_attack_pose_end(unsigned char *rdram) {
    using namespace rr64::online_race_sync;
    if(!attack_scope_depth || --attack_scope_depth) return;
    for(auto &saved:saved_attacks) saved.restore(attack_scope_mapping);
    attack_scope_mapping=nullptr;
}

extern "C" void rr64_online_sync_before_pose(unsigned char *rdram) {
    rr64::online_race_sync::capture_local_rider(rdram);
    const auto status=rr64::netplay::get_status();
    if(status.is_host && !status.authoritative && status.phase==rr64::netplay::Phase::Race) {
        rr64::world_sync::Snapshot traffic{};
        const auto tick=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(rr64::world_sync::capture_traffic(rdram,status.game_setup.revision,tick,traffic))
            rr64::netplay::set_host_world_state(traffic);
    }
    rr64::online_race_sync::apply_remote_riders(rdram, true);
}

extern "C" int rr64_online_authority_capture(unsigned char *rdram,const void *completed) {
    const auto status=rr64::netplay::get_status();
    if(!completed || !status.authoritative || !status.is_host)return 0;
    const auto &stamp=*static_cast<const rr64::authority::Stamp*>(completed);
    rr64::netplay::AuthorityFrame frame{};
    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(!rr64::authority::capture_frame(rdram,stamp,status.authority_humans,us,frame,
                                      rr64::online_race_sync::capture_bike))return 0;
    rr64::prediction::CopRulesState rules;
    rr64::prediction::CopPostsState posts;
    if(!rr64::prediction::capture_cop_rules(rules) || !rr64::prediction::capture_cop_posts(posts))return 0;
    frame.cop_mode=rules.race_enabled?1:0;
    const float now=std::bit_cast<float>(frame.timing.bits[5]);
    if(frame.cop_mode && rules.win_started>=0 && now>=rules.win_started)
        frame.cop_win_age=now-rules.win_started;
    for(unsigned s=0;s<posts.size();++s)if(frame.cop_mode && frame.riders[s].active && frame.outcomes[s].role==7){
        frame.outcomes[s].siren=posts[s].muted?0:1;
        const float age=now-posts[s].cue;
        if(posts[s].pursuit && age>=0 && age<4)frame.outcomes[s].cue_age=age;
    }
#ifdef RR64_EXPERIMENTAL_COURSE
    frame.course_items=rr64::course_items::capture_state();
    frame.course_hazards=rr64::course_hazards::capture_state();
    frame.mk64_items=rr64::mk64_items::capture_state();
#endif
    return rr64::netplay::authority_publish_frame(frame)?1:0;
}

extern "C" int rr64_online_wait_for_race(unsigned char *rdram, unsigned mode) {
    if(rr64_online_postrace_update(rdram,mode)) return 1;
    static bool disconnect_transition_requested=false;
    static unsigned finish_round=0,requested_finish=0;
    auto s=rr64::netplay::get_status();
    if(!s.authoritative || finish_round!=s.game_setup.revision) {
        finish_round=s.game_setup.revision;requested_finish=0;
    }
    if (!s.host_disconnected) disconnect_transition_requested=false;
    if (s.host_disconnected) {
        if (mode == 0x20u) {
            // The stock dispatcher has completed race teardown and entered
            // the main menu. Only now release the retained online camera.
            rr64::netplay::shutdown();
            disconnect_transition_requested=false;
            return 0;
        }
        if (s.host_disconnect_age_ms >= 3000u && !disconnect_transition_requested) {
            disconnect_transition_requested=true;
            // Equivalent to stock request-mode helper 48544, at the update
            // boundary. Its dispatcher owns cleanup; never quit the process
            // or run a menu initializer from inside a render callback.
            rr64::engine::write_u32(rdram, rr64::engine::globals::pending_mode, 0x20u);
            rr64::engine::write_u32(rdram, 0x8009CC50u, 0u);
            rr64::engine::write_u16(rdram, rr64::engine::globals::pause_menu_state, 0u);
            rr64::engine::write_u16(rdram, rr64::engine::globals::gameplay_pause_state, 0u);
        }
        // Mode 39 is the stock transition handler. Freezing it prevents the
        // requested menu from loading; resetting the request also restarts it.
        return !disconnect_transition_requested || rr64::engine::is_live_race_mode(mode);
    }
    if(s.active && s.connected && s.authoritative) {
        if(s.is_host) {
            unsigned pending=0;
            if(rr64::engine::read_u32(rdram,rr64::engine::globals::pending_mode,pending))
                rr64::netplay::authority_set_finish_mode(pending);
        }
        const unsigned finish=rr64::netplay::authority_finish_mode();
        if(finish && !s.is_host && !requested_finish) {
            requested_finish=finish;
            rr64::engine::write_u32(rdram,rr64::engine::globals::pending_mode,finish);
            rr64::engine::write_u32(rdram,0x8009CC50u,0);
            rr64::engine::write_u16(rdram,rr64::engine::globals::pause_menu_state,0);
            rr64::engine::write_u16(rdram,rr64::engine::globals::gameplay_pause_state,0);
        }
        // Never hold/reissue mode39: stock teardown must run to completion.
        if(finish)return 0;
    }
    if (!s.active || !s.connected || rr64_is_live_race_mode(mode)==0) return 0;
    if (s.phase==rr64::netplay::Phase::TrackSelect && s.local_slot<rr64::netplay::kMaximumPlayers) {
        auto choice=s.players[s.local_slot].selection;
        choice.loaded=1;
        rr64::netplay::set_selection(choice);
        rr64::netplay::host_release_selection();
        s=rr64::netplay::get_status();
    }
    if(s.phase!=rr64::netplay::Phase::Race)return 1;
    // Start authority only after stock track initialization. Guests wait for
    // the host's round announcement; neither peer runs legacy simulation in
    // the gap between menu release and the native loading barrier.
    if(!s.authoritative){
        if(!s.is_host || !rr64::netplay::authority_start())return 1;
        s=rr64::netplay::get_status();
    }
    if(s.authoritative) {
        const bool loaded=rr64::authority::native_roster_loaded(rdram,s.authority_humans,
            s.local_slot,s.replicated_riders);
        return !rr64::netplay::authority_race_gate(loaded);
    }
    return 1;
}
extern "C" int rr64_online_host_pause_active() {
    const auto s=rr64::netplay::get_status();
    return s.active && s.connected && rr64::online_race_sync::g_host_pause_open.load();
}

extern "C" unsigned int rr64_online_requested_racer_count(unsigned int original_count) {
    return rr64::online_race_sync::requested_racer_count(original_count);
}

extern "C" unsigned int rr64_online_prepare_render_layout(unsigned int stock_layout) {
    return rr64::online_race_sync::prepare_render_layout(stock_layout);
}

extern "C" unsigned int rr64_online_render_first_viewport(unsigned int stock_viewport) {
    return rr64::online_race_sync::first_render_viewport(stock_viewport);
}

// Only active inside the world draw. Camera/LOD indices stay in canonical
// player slots; every hardware viewport/scissor request targets full-screen 0.
extern "C" unsigned rr64_online_graphics_layout(unsigned original) {
    return rr64::online_race_sync::g_viewport_render_plan.peer_fullscreen ? 0u : original;
}
extern "C" void rr64_online_render_begin(unsigned char *m) {
    rr64::online_race_sync::presentation_draw=false;
    rr64::online_race_sync::presentation_mapping=m;
    {
        using namespace rr64::online_race_sync;
        std::lock_guard lock(presentation_mutex);
        draw_offsets=published_mapping==m ? published_offsets : decltype(draw_offsets){};
    }
    unsigned layout=0;
    rr64::engine::read_u32(m,0x800A4F24,layout);
    rr64::online_race_sync::prepare_render_layout(layout);
    rr64_online_logical_viewports(m);
    rr64::online_race_sync::presentation_draw=rr64::online_race_sync::g_viewport_render_plan.peer_fullscreen;
}
extern "C" void rr64_online_logical_viewports(unsigned char *m) {
    const auto &p=rr64::online_race_sync::g_viewport_render_plan;
    if (!p.peer_fullscreen) return;
    const auto s=rr64::netplay::get_status();
    // Pose caches and culling index all simulated cameras, even though only
    // one camera is drawn. DB88 must not shrink below the guest camera slot.
    rr64::engine::write_u32(m,0x8009DB88,s.replicated_riders ? 1u :
        std::clamp<unsigned>(s.connected_players,1u,4u));
}
extern "C" unsigned rr64_online_graphics_viewport(unsigned char *m, unsigned original) {
    if (!rr64::online_race_sync::g_viewport_render_plan.peer_fullscreen) return original;
    // DB84 is also the stock hardware-viewport cache. Invalidate before issuing
    // viewport 0 so the logical camera index cannot incorrectly skip its setup.
    rr64::engine::write_u32(m,rr64::engine::globals::active_viewport,~0u);
    return 0;
}
extern "C" void rr64_online_graphics_viewport_end(unsigned char *m) {
    const auto &p=rr64::online_race_sync::g_viewport_render_plan;
    if (p.peer_fullscreen)
        rr64::engine::write_u32(m,rr64::engine::globals::active_viewport,p.first_viewport);
}
extern "C" unsigned rr64_online_render_loop_continue(unsigned original) {
    return rr64::online_race_sync::g_viewport_render_plan.peer_fullscreen ? 0u : original;
}
extern "C" void rr64_online_render_end(unsigned char *m) {
    rr64::online_race_sync::presentation_draw=false;
    if (rr64::online_race_sync::g_viewport_render_plan.peer_fullscreen) {
        // Simulation prepares poses between draws. Keep its logical camera
        // count; a physical full-screen viewport is not a one-player race.
        rr64_online_logical_viewports(m);
        rr64::engine::write_u32(m,rr64::engine::globals::active_viewport,~0u);
    }
    rr64::online_race_sync::g_viewport_render_plan={};
}

extern "C" unsigned int rr64_online_render_geometry_viewport(unsigned int stock_viewport) {
    return rr64::online_race_sync::geometry_render_viewport(stock_viewport);
}

extern "C" void rr64_online_restore_active_viewport(unsigned char *rdram,
                                                    unsigned int stock_viewport) {
    rr64::online_race_sync::restore_active_render_viewport(rdram, stock_viewport);
}

extern "C" void rr64_online_apply_display_names(unsigned char *rdram) {
    rr64::online_race_sync::apply_online_display_names(rdram);
}

extern "C" void rr64_online_race_sync_before_update(unsigned char *rdram, unsigned int mode) {
    rr64::online_race_sync::before_guest_update(rdram, mode);
}

extern "C" void rr64_online_race_sync_after_update(unsigned char *rdram, unsigned int mode) {
    rr64::online_race_sync::after_guest_update(rdram, mode);
}

// Called only on the temporary float matrix built by 15A90, before fixed-point
// packing and before Max LOD's source-1 -> source-2 normalization. Translation
// is already camera-relative and source-scaled; add the matching world delta.
extern "C" void rr64_online_presentation_matrix(unsigned char *m,unsigned node,
                                                unsigned record,unsigned matrix,
                                                unsigned weapon_source) {
    using namespace rr64::engine;
    using namespace rr64::online_race_sync;
    if (rr64_highlights_presenting()) return;
    if (!record || !presentation_draw || m!=presentation_mapping || !valid_guest_range(matrix,64) || (matrix&3)) return;
    unsigned type=0,entity=0,root=0;
    if (!read_u32(m,node,type) || (type!=1 && type!=2) ||
        !read_u32(m,node+actor_scene::entity,entity) ||
        !read_u32(m,node+actor_scene::current_model,root)) return;
    // A weapon uses its owner's separately submitted root graph. Never shift
    // child matrices: the translated parent already moves their entire graph.
    bool actor_root=record==root;
    for (unsigned lod=0;!actor_root && lod<3;++lod) {
        unsigned model=0;
        actor_root=read_u32(m,node+actor_scene::lod_models+lod*4,model) && model==record;
    }
    if (actor_root) root=record; // Max LOD can select a graph without changing current_model.
    else {
        unsigned weapon=0;
        if (type!=2 || !read_u32(m,entity+0x5BC,weapon) || record!=weapon) return;
    }
    const PresentationOffset *offset=nullptr;
    for(const auto &entry:draw_offsets)
        if (entity && ((type==1 && entity==entry.bike)||(type==2 && entity==entry.rider))) { offset=&entry;break; }
    if (!offset) return;
    unsigned linked_rider=0,linked_bike=0;
    if (!read_u32(m,offset->bike+bike::rider_pointer,linked_rider) || linked_rider!=offset->rider ||
        !read_u32(m,offset->rider+rider::bike_pointer,linked_bike) || linked_bike!=offset->bike) return;
    unsigned source_record=0;std::uint16_t source=0;
    if (!read_u32(m,root+0x14,source_record) || !read_u16(m,source_record+0x12,source)) return;
    if (weapon_source<=2) source=static_cast<std::uint16_t>(weapon_source);
    if (source>2) return;
    float scale=0;
    if (!read_float(m,0x8009DBAC+source*4,scale) || !std::isfinite(scale) || scale<=0 || scale>1000) return;
    std::array<float,3> translated{};
    for(unsigned axis=0;axis<3;++axis) {
        if (!read_float(m,matrix+48+axis*4,translated[axis])) return;
        translated[axis]+=offset->delta[axis]*scale;
        if (!std::isfinite(translated[axis])) return;
    }
    for(unsigned axis=0;axis<3;++axis) rr64::engine::write_float(m,matrix+48+axis*4,translated[axis]);
    ++presentation_matrix_counts[static_cast<std::size_t>(offset-draw_offsets.data())];
}
