#include "rr64_prediction_replay.hpp"
#include "rr64_engine_contract.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>

#include "rr64_actor_pose.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_engine_snapshot.hpp"
#include "hle/rt64_rr64_pipeline_diagnostics.h"

#include "rr64_msvc_crt_compat.hpp"

namespace rr64::engine {
namespace {

std::mutex g_contract_mutex;
ContractSnapshot g_snapshot{};
std::uint32_t g_last_logged_mode = 0xFFFFFFFFu;
std::uint32_t g_last_logged_stage = 0xFFFFFFFFu;
bool g_logged_invalid_racer_count = false;
bool g_logged_invalid_bike_pool = false;
bool g_logged_invalid_physics_delta = false;
bool g_logged_invalid_mode = false;
std::uint32_t g_consecutive_live_frames = 0;
std::uint64_t g_parity_live_frame = 0;
bool g_parity_was_live = false;
std::uint32_t g_last_captured_mode = 0xFFFFFFFFu;
std::uint64_t g_actor_capture = 0;
std::uint32_t g_random_before_update = 0;
bool g_random_before_update_valid = false;

struct DispatchTraceSample {
    std::uint64_t frame = 0;
    std::uint32_t mode = 0;
    std::uint32_t boundary_mask = 0;
    std::array<std::uint32_t, 3> targets{};
    std::array<std::uint32_t, 4> random_states{};
    std::array<std::uint64_t, 4> pose_hashes{};
};

DispatchTraceSample g_dispatch_trace{};
std::uint32_t g_race_trace_mode = 0;
std::uint32_t g_race_trace_random_before = 0;
std::uint32_t g_race_trace_random_mid = 0;
std::uint64_t g_race_trace_pose_before = 0;
std::uint64_t g_race_trace_pose_mid = 0;
std::uint32_t g_race_trace_boundary_mask = 0;
bool g_race_trace_before_valid = false;

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kReferenceFrameMagic = 0x334D5246u; // "FRM3"
constexpr std::uint32_t kReferenceHeaderSize = 32u;
constexpr std::uint32_t kReferenceFrameHeaderSize = 80u;
constexpr std::uint32_t kReferenceActorHeaderSize = 16u;
constexpr std::uint32_t kReferenceBikeValid = 1u << 0u;
constexpr std::uint32_t kReferenceRiderValid = 1u << 1u;
constexpr std::uint32_t kDynamicsEventMagic = 0x314E5944u; // "DYN1" LE
constexpr std::uint32_t kDynamicsHeaderSize = 32u;
constexpr std::uint32_t kDynamicsEventHeaderSize = 64u;
thread_local std::array<std::uint32_t, 28> g_dynamics_actors{};

void write_le16(FILE *file, std::uint16_t value) {
    const std::uint8_t bytes[] = {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8u),
    };
    std::fwrite(bytes, 1u, sizeof(bytes), file);
}

void write_le32(FILE *file, std::uint32_t value) {
    const std::uint8_t bytes[] = {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8u),
        static_cast<std::uint8_t>(value >> 16u),
        static_cast<std::uint8_t>(value >> 24u),
    };
    std::fwrite(bytes, 1u, sizeof(bytes), file);
}

void write_le64(FILE *file, std::uint64_t value) {
    write_le32(file, static_cast<std::uint32_t>(value));
    write_le32(file, static_cast<std::uint32_t>(value >> 32u));
}

bool read_guest_bytes(unsigned char *rdram, std::uint32_t address, std::uint8_t *destination,
                      std::uint32_t size) {
    if (!valid_guest_range(address, size)) {
        return false;
    }
    for (std::uint32_t offset = 0; offset < size; ++offset) {
        if (!read_u8(rdram, address + offset, destination[offset])) {
            return false;
        }
    }
    return true;
}

struct RawReferenceActor {
    std::uint32_t racer_index = 0;
    std::uint32_t bike_address = 0;
    std::uint32_t rider_address = 0;
    std::uint32_t flags = 0;
    std::array<std::uint8_t, bike::stride> bike_bytes{};
    std::array<std::uint8_t, rider::stride> rider_bytes{};
};

void hash_u32(std::uint64_t &hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= kFnvPrime;
    }
}

FILE *parity_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_PARITY_LOG") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "w");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-CONTRACT] Could not open RR64_PARITY_LOG.\n");
            return nullptr;
        }

        std::fprintf(
            opened,
            "schema,frame,race_frame,mode,multiplayer_stage,active_racers,input,stick_x,stick_y,bike_pool,bike_samples,control_hash,local_bike_hash,world_bike_hash,local_front_x,local_front_y,local_front_z,local_body_x,local_body_y,local_body_z,local_rear_x,local_rear_y,local_rear_z\n");
        std::fflush(opened);
        return opened;
    }();
    return file;
}

FILE *dispatch_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_DISPATCH_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "w");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-CONTRACT] Could not open RR64_DISPATCH_TRACE.\n");
            return nullptr;
        }

        std::fprintf(
            opened,
            "kind,frame,mode,mask,target0,target1,target2,rng0,rng1,rng2,rng3,pose0,pose1,pose2,pose3\n");
        std::fflush(opened);
        return opened;
    }();
    return file;
}

std::uint64_t capture_pose_hash(unsigned char *rdram) {
    FrameSnapshot frame{};
    if (!capture_frame_snapshot(rdram, frame)) {
        return 0u;
    }

    std::uint64_t hash = kFnvOffset;
    hash_u32(hash, frame.mode);
    hash_u32(hash, frame.active_racers);
    for (std::uint32_t racer = 0; racer < frame.active_racers && racer < kMaximumRacers; ++racer) {
        const BikePoseSnapshot &pose = frame.bikes[racer];
        hash_u32(hash, racer);
        hash_u32(hash, pose.valid ? 1u : 0u);
        if (!pose.valid) {
            continue;
        }
        const Vec3Snapshot positions[] = {
            pose.front_wheel,
            pose.body,
            pose.rear_wheel,
        };
        for (const Vec3Snapshot &position : positions) {
            const float axes[] = {position.x, position.y, position.z};
            for (const float axis : axes) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &axis, sizeof(bits));
                hash_u32(hash, bits);
            }
        }
    }
    return hash;
}

FILE *reference_state_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_NATIVE_REFERENCE_LOG") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "wb");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-CONTRACT] Could not open RR64_NATIVE_REFERENCE_LOG.\n");
            return nullptr;
        }

        const char magic[8] = {'R', 'R', '6', '4', 'R', 'E', 'F', '3'};
        std::fwrite(magic, 1u, sizeof(magic), opened);
        write_le32(opened, kReferenceHeaderSize);
        write_le32(opened, bike::stride);
        write_le32(opened, rider::stride);
        write_le32(opened, kMaximumRacers);
        write_le32(opened, 0u);
        write_le32(opened, 0u);
        std::fflush(opened);
        return opened;
    }();
    return file;
}

FILE *dynamics_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_NATIVE_DYNAMICS_LOG") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "wb");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-CONTRACT] Could not open RR64_NATIVE_DYNAMICS_LOG.\n");
            return nullptr;
        }

        const char magic[8] = {'R', 'R', '6', '4', 'D', 'Y', 'N', '1'};
        std::fwrite(magic, 1u, sizeof(magic), opened);
        write_le32(opened, kDynamicsHeaderSize);
        write_le32(opened, bike::stride);
        write_le32(opened, rider::stride);
        write_le32(opened, kDynamicsEventHeaderSize);
        write_le32(opened, 0u);
        write_le32(opened, 0u);
        std::fflush(opened);
        return opened;
    }();
    return file;
}

void write_reference_state_frame(FILE *file, unsigned char *rdram, const FrameSnapshot &frame,
                                 std::uint64_t source_frame, std::uint64_t race_frame) {
    std::uint32_t pending_mode = 0;
    std::uint32_t physics_delta_bits = 0;
    std::uint32_t update_ticks = 0;
    std::uint32_t wait_ticks = 0;
    std::uint32_t total_ticks = 0;
    std::uint32_t random_state = 0;
    std::uint16_t changed_buttons = 0;
    std::uint16_t pressed_buttons = 0;
    read_u32(rdram, globals::pending_mode, pending_mode);
    read_u32(rdram, globals::physics_delta, physics_delta_bits);
    read_u32(rdram, globals::update_ticks, update_ticks);
    read_u32(rdram, globals::wait_ticks, wait_ticks);
    read_u32(rdram, globals::total_ticks, total_ticks);
    read_u32(rdram, globals::random_state, random_state);
    read_u16(rdram, globals::controller_changed_buttons, changed_buttons);
    read_u16(rdram, globals::controller_pressed_buttons, pressed_buttons);

    const std::uint32_t actor_count = std::min(frame.active_racers, kMaximumRacers);
    std::array<RawReferenceActor, kMaximumRacers> actors{};
    std::uint32_t record_size = kReferenceFrameHeaderSize;
    for (std::uint32_t racer = 0; racer < actor_count; ++racer) {
        RawReferenceActor &actor = actors[racer];
        actor.racer_index = racer;
        actor.bike_address = frame.bike_pool + racer * bike::stride;
        if (read_guest_bytes(rdram, actor.bike_address, actor.bike_bytes.data(), bike::stride)) {
            actor.flags |= kReferenceBikeValid;
            read_u32(rdram, actor.bike_address + bike::rider_pointer, actor.rider_address);
            if (read_guest_bytes(rdram, actor.rider_address, actor.rider_bytes.data(),
                                 rider::stride)) {
                actor.flags |= kReferenceRiderValid;
            }
        }
        record_size += kReferenceActorHeaderSize;
        if ((actor.flags & kReferenceBikeValid) != 0u) {
            record_size += bike::stride;
        }
        if ((actor.flags & kReferenceRiderValid) != 0u) {
            record_size += rider::stride;
        }
    }

    write_le32(file, kReferenceFrameMagic);
    write_le32(file, record_size);
    write_le64(file, source_frame);
    write_le64(file, race_frame);
    write_le32(file, frame.mode);
    write_le32(file, pending_mode);
    write_le32(file, frame.multiplayer_stage);
    write_le32(file, frame.active_racers);
    write_le32(file, frame.bike_pool);
    write_le32(file, physics_delta_bits);
    write_le32(file, update_ticks);
    write_le32(file, wait_ticks);
    write_le32(file, total_ticks);
    write_le32(file, g_random_before_update_valid ? g_random_before_update : random_state);
    write_le32(file, random_state);
    write_le16(file, frame.buttons);
    write_le16(file, changed_buttons);
    write_le16(file, pressed_buttons);
    std::fputc(static_cast<std::uint8_t>(frame.stick_x), file);
    std::fputc(static_cast<std::uint8_t>(frame.stick_y), file);
    write_le32(file, actor_count);

    for (std::uint32_t racer = 0; racer < actor_count; ++racer) {
        const RawReferenceActor &actor = actors[racer];
        write_le32(file, actor.racer_index);
        write_le32(file, actor.bike_address);
        write_le32(file, actor.rider_address);
        write_le32(file, actor.flags);
        if ((actor.flags & kReferenceBikeValid) != 0u) {
            std::fwrite(actor.bike_bytes.data(), 1u, actor.bike_bytes.size(), file);
        }
        if ((actor.flags & kReferenceRiderValid) != 0u) {
            std::fwrite(actor.rider_bytes.data(), 1u, actor.rider_bytes.size(), file);
        }
    }
}

FILE *actor_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_ACTOR_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "w");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-ENGINE] Could not open RR64_ACTOR_TRACE.\n");
            return nullptr;
        }

        std::fprintf(
            opened,
            "schema,capture,viewport,mode,active_racers,matched_actors,valid_actors,graph_complete,racer,bike_node,rider_node,bike_entity,rider_entity,bike_lod,bike_previous,rider_lod,rider_previous,bike_depth,rider_depth,bike_current_model,bike_model,bike_segment_table,rider_current_model,rider_model,rider_segment_table,proxy_metadata_valid,proxy_direct_safe,proxy_requires_pose_rebuild,proxy_transform_remap_safe,bike_transform_remap_safe,rider_transform_remap_safe,bike_stock_segment_count,bike_high_segment_count,rider_stock_segment_count,rider_high_segment_count,bike_proxy_model,bike_proxy_display_list,bike_proxy_segment_count,rider_proxy_model,rider_proxy_display_list,rider_proxy_segment_count,bike_selected_segments_valid,bike_selected_segment_hash,rider_selected_segments_valid,rider_selected_segment_hash,bike_proxy_segments_valid,bike_proxy_segment_hash,rider_proxy_segments_valid,rider_proxy_segment_hash,bike_tier0_transforms,bike_tier1_transforms,bike_tier2_transforms,rider_tier0_transforms,rider_tier1_transforms,rider_tier2_transforms,bike_tier0_segment_count,bike_tier1_segment_count,bike_tier2_segment_count,rider_tier0_segment_count,rider_tier1_segment_count,rider_tier2_segment_count,bike_tier0_graph_hash,bike_tier1_graph_hash,bike_tier2_graph_hash,rider_tier0_graph_hash,rider_tier1_graph_hash,rider_tier2_graph_hash,bike_tier0_allocation_hash,bike_tier1_allocation_hash,bike_tier2_allocation_hash,rider_tier0_allocation_hash,rider_tier1_allocation_hash,rider_tier2_allocation_hash,bike_stock_transform_count,bike_high_transform_count,bike_transform_buffer_valid,bike_transform_buffer_hash,rider_stock_transform_count,rider_high_transform_count,rider_transform_buffer_valid,rider_transform_buffer_hash,front_x,front_y,front_z,body_x,body_y,body_z,rear_x,rear_y,rear_z\n");
        std::fflush(opened);
        return opened;
    }();
    return file;
}

FILE *actor_matrix_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_ACTOR_MATRIX_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "w");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-ENGINE] Could not open RR64_ACTOR_MATRIX_TRACE.\n");
            return nullptr;
        }

        std::fprintf(opened,
                     "schema,capture,viewport,buffer_slot,racer,part,lod,node,model,graph_hash,"
                     "transform_buffer,transform_index,record_index,record_type,"
                     "record_first_transform,record_transform_count,"
                     "m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23,m30,m31,m32,m33,"
                     "front_x,front_y,front_z,body_x,body_y,body_z,rear_x,rear_y,rear_z\n");
        std::fflush(opened);
        return opened;
    }();
    return file;
}

FILE *actor_segment_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t path_length = 0;
        if (_dupenv_s(&path, &path_length, "RR64_ACTOR_SEGMENT_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }

        FILE *opened = nullptr;
        const errno_t open_result = fopen_s(&opened, path, "w");
        std::free(path);
        if (open_result != 0 || opened == nullptr) {
            std::fprintf(stderr, "[RR64-ENGINE] Could not open RR64_ACTOR_SEGMENT_TRACE.\n");
            return nullptr;
        }

        std::fprintf(opened,
                     "schema,capture,viewport,buffer_slot,racer,part,lod,selected,node,"
                     "segment_table,segment_count,segment_index,segment_record,payload_hash,"
                     "front_x,front_y,front_z,body_x,body_y,body_z,rear_x,rear_y,rear_z\n");
        std::fflush(opened);
        return opened;
    }();
    return file;
}

void mark_warning() {
    g_snapshot.health = ContractHealth::Warning;
}

bool engine_observation_enabled() {
    static const bool enabled = [] {
        const auto environment_value_present = [](const char *name, bool accept_zero) {
            char *value = nullptr;
            std::size_t length = 0;
            const errno_t result = _dupenv_s(&value, &length, name);
            const bool present = result == 0 && value != nullptr && value[0] != '\0' &&
                                 (accept_zero || value[0] != '0');
            std::free(value);
            return present;
        };
        return environment_value_present("RR64_ENGINE_TRACE", false) ||
               environment_value_present("RR64_PARITY_LOG", true) ||
               environment_value_present("RR64_NATIVE_REFERENCE_LOG", true) ||
               environment_value_present("RR64_DISPATCH_TRACE", true) ||
               environment_value_present("RR64_NATIVE_DYNAMICS_LOG", true);
    }();
    return enabled;
}

} // namespace

void observe_frame(unsigned char *rdram) noexcept {
    if (rdram == nullptr || !engine_observation_enabled()) {
        return;
    }

    std::lock_guard lock{g_contract_mutex};
    if (g_snapshot.health == ContractHealth::Unobserved) {
        g_snapshot.health = ContractHealth::Healthy;
        std::fprintf(stderr, "[RR64-ENGINE] Contract schema 1 active for USA v1.0.\n");
    }

    ++g_snapshot.observed_frames;
    read_u32(rdram, globals::main_mode, g_snapshot.main_mode);
    read_u32(rdram, globals::multiplayer_stage, g_snapshot.multiplayer_stage);
    read_u32(rdram, globals::active_racer_count, g_snapshot.active_racers);
    read_u32(rdram, globals::bike_pool_pointer, g_snapshot.bike_pool);
    read_float(rdram, globals::physics_delta, g_snapshot.physics_delta);
    const bool live_race = is_live_race_mode(g_snapshot.main_mode);
    g_consecutive_live_frames = live_race ? g_consecutive_live_frames + 1u : 0u;

    if (g_snapshot.main_mode != g_last_logged_mode) {
        std::fprintf(stderr, "[RR64-ENGINE] frame=%llu mode=0x%02X live_race=%d\n",
                     static_cast<unsigned long long>(g_snapshot.observed_frames),
                     g_snapshot.main_mode, live_race ? 1 : 0);
        g_last_logged_mode = g_snapshot.main_mode;
    }

    if (g_snapshot.multiplayer_stage != g_last_logged_stage) {
        std::fprintf(stderr, "[RR64-ENGINE] multiplayer_stage=%u\n", g_snapshot.multiplayer_stage);
        g_last_logged_stage = g_snapshot.multiplayer_stage;
    }

    if (g_snapshot.observed_frames > 1u && !is_valid_mode(g_snapshot.main_mode)) {
        mark_warning();
        if (!g_logged_invalid_mode) {
            std::fprintf(stderr,
                         "[RR64-CONTRACT] Mode 0x%08X is outside the decoded 0x00-0x39 table.\n",
                         g_snapshot.main_mode);
            g_logged_invalid_mode = true;
        }
    }

    if (live_race && g_consecutive_live_frames > 2u && g_snapshot.active_racers > kMaximumRacers) {
        mark_warning();
        if (!g_logged_invalid_racer_count) {
            std::fprintf(
                stderr,
                "[RR64-CONTRACT] Active racer count %u exceeds the verified pool capacity %u.\n",
                g_snapshot.active_racers, kMaximumRacers);
            g_logged_invalid_racer_count = true;
        }
    }

    if (live_race && g_consecutive_live_frames > 2u && g_snapshot.active_racers > 0 &&
        !valid_guest_range(g_snapshot.bike_pool, g_snapshot.active_racers * bike::stride)) {
        mark_warning();
        if (!g_logged_invalid_bike_pool) {
            std::fprintf(stderr,
                         "[RR64-CONTRACT] Bike pool 0x%08X is invalid for %u active racers.\n",
                         g_snapshot.bike_pool, g_snapshot.active_racers);
            g_logged_invalid_bike_pool = true;
        }
    }

    if (live_race && g_consecutive_live_frames > 8u &&
        (!std::isfinite(g_snapshot.physics_delta) || std::abs(g_snapshot.physics_delta) > 1.0f)) {
        mark_warning();
        if (!g_logged_invalid_physics_delta) {
            std::fprintf(stderr,
                         "[RR64-CONTRACT] Physics delta is outside its safe observed domain.\n");
            g_logged_invalid_physics_delta = true;
        }
    }

    std::fflush(stderr);
}

void capture_pre_update(unsigned char *rdram) noexcept {
    if (rdram == nullptr) {
        return;
    }
    std::uint32_t random_state = 0;
    if (!read_u32(rdram, globals::random_state, random_state)) {
        return;
    }
    std::lock_guard lock{g_contract_mutex};
    g_random_before_update = random_state;
    g_random_before_update_valid = true;
}

void capture_dispatch_boundary(unsigned char *rdram, std::uint32_t boundary,
                               std::uint32_t target) noexcept {
    FILE *file = dispatch_trace_file();
    if (rdram == nullptr || file == nullptr || boundary > 3u) {
        return;
    }

    std::uint32_t random_state = 0;
    std::uint32_t mode = 0;
    if (!read_u32(rdram, globals::random_state, random_state) ||
        !read_u32(rdram, globals::main_mode, mode)) {
        return;
    }
    const std::uint64_t pose_hash = capture_pose_hash(rdram);

    std::lock_guard lock{g_contract_mutex};
    if (boundary == 0u) {
        g_dispatch_trace = {};
        g_dispatch_trace.frame = g_snapshot.observed_frames;
        g_dispatch_trace.mode = mode;
    }
    g_dispatch_trace.boundary_mask |= 1u << boundary;
    g_dispatch_trace.random_states[boundary] = random_state;
    g_dispatch_trace.pose_hashes[boundary] = pose_hash;
    if (boundary < g_dispatch_trace.targets.size()) {
        g_dispatch_trace.targets[boundary] = target;
    }

    if (boundary == 3u) {
        std::fprintf(
            file,
            "dispatch,%llu,0x%02X,0x%X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%016llX,0x%016llX,0x%016llX,0x%016llX\n",
            static_cast<unsigned long long>(g_dispatch_trace.frame), g_dispatch_trace.mode,
            g_dispatch_trace.boundary_mask, g_dispatch_trace.targets[0],
            g_dispatch_trace.targets[1], g_dispatch_trace.targets[2],
            g_dispatch_trace.random_states[0], g_dispatch_trace.random_states[1],
            g_dispatch_trace.random_states[2], g_dispatch_trace.random_states[3],
            static_cast<unsigned long long>(g_dispatch_trace.pose_hashes[0]),
            static_cast<unsigned long long>(g_dispatch_trace.pose_hashes[1]),
            static_cast<unsigned long long>(g_dispatch_trace.pose_hashes[2]),
            static_cast<unsigned long long>(g_dispatch_trace.pose_hashes[3]));
        std::fflush(file);
    }
}

void capture_race_update_boundary(unsigned char *rdram, std::uint32_t boundary) noexcept {
    FILE *file = dispatch_trace_file();
    if (rdram == nullptr || file == nullptr || boundary > 2u) {
        return;
    }

    std::uint32_t random_state = 0;
    std::uint32_t mode = 0;
    if (!read_u32(rdram, globals::random_state, random_state) ||
        !read_u32(rdram, globals::main_mode, mode)) {
        return;
    }
    const std::uint64_t pose_hash = capture_pose_hash(rdram);

    std::lock_guard lock{g_contract_mutex};
    if (boundary == 0u) {
        g_race_trace_mode = mode;
        g_race_trace_random_before = random_state;
        g_race_trace_random_mid = 0u;
        g_race_trace_pose_before = pose_hash;
        g_race_trace_pose_mid = 0u;
        g_race_trace_boundary_mask = 1u;
        g_race_trace_before_valid = true;
        return;
    }
    if (!g_race_trace_before_valid) {
        return;
    }
    if (boundary == 1u) {
        g_race_trace_random_mid = random_state;
        g_race_trace_pose_mid = pose_hash;
        g_race_trace_boundary_mask |= 1u << 1u;
        return;
    }

    g_race_trace_boundary_mask |= 1u << 2u;

    std::fprintf(
        file,
        "func_8006AFFC,%llu,0x%02X,0x%X,0x8006AFFC,0x00000000,0x00000000,0x%08X,0x%08X,0x%08X,0x00000000,0x%016llX,0x%016llX,0x%016llX,0x0000000000000000\n",
        static_cast<unsigned long long>(g_snapshot.observed_frames), g_race_trace_mode,
        g_race_trace_boundary_mask, g_race_trace_random_before, g_race_trace_random_mid,
        random_state, static_cast<unsigned long long>(g_race_trace_pose_before),
        static_cast<unsigned long long>(g_race_trace_pose_mid),
        static_cast<unsigned long long>(pose_hash));
    std::fflush(file);
    g_race_trace_before_valid = false;
}

void capture_dynamics_boundary_impl(unsigned char *rdram, std::uint32_t function_id,
                                    std::uint32_t boundary, std::uint32_t actor_address,
                                    const std::uint32_t *auxiliary_words,
                                    std::uint32_t auxiliary_word_count) noexcept {
    // Replay reads historical memory; it must not consume live diagnostic
    // budgets or change entry/exit pairing for the next real physics call.
    if(rr64::prediction::active())return;
    const std::uint32_t boundary_value = actor_address;
    FILE *file = dynamics_trace_file();
    if (rdram == nullptr || file == nullptr || function_id == 0u ||
        function_id >= g_dynamics_actors.size() || boundary > 1u || auxiliary_word_count > 32u ||
        (auxiliary_word_count != 0u && auxiliary_words == nullptr)) {
        return;
    }

    if (boundary == 0u) {
        g_dynamics_actors[function_id] = actor_address;
    } else {
        actor_address = g_dynamics_actors[function_id];
        g_dynamics_actors[function_id] = 0u;
    }
    if (actor_address == 0u) {
        return;
    }

    std::uint32_t bike_address = 0u;
    std::uint32_t rider_address = 0u;
    const bool rider_function = function_id <= 3u || function_id >= 26u;
    if (!rider_function) {
        bike_address = actor_address;
        if (!read_u32(rdram, bike_address + bike::rider_pointer, rider_address)) {
            return;
        }
    } else {
        rider_address = actor_address;
        if (!read_u32(rdram, rider_address + rider::bike_pointer, bike_address)) {
            return;
        }
    }

    std::uint32_t local_bike = 0u;
    std::uint32_t mode = 0u;
    std::uint32_t physics_delta_bits = 0u;
    std::uint32_t random_state = 0u;
    std::uint32_t update_ticks = 0u;
    if (!read_u32(rdram, globals::bike_pool_pointer, local_bike) ||
        !read_u32(rdram, globals::main_mode, mode) ||
        !read_u32(rdram, globals::physics_delta, physics_delta_bits) ||
        !read_u32(rdram, globals::random_state, random_state) ||
        !read_u32(rdram, globals::update_ticks, update_ticks) || bike_address != local_bike ||
        !is_live_race_mode(mode)) {
        return;
    }

    std::array<std::uint8_t, bike::stride> bike_bytes{};
    std::array<std::uint8_t, rider::stride> rider_bytes{};
    if (!read_guest_bytes(rdram, bike_address, bike_bytes.data(), bike::stride) ||
        !read_guest_bytes(rdram, rider_address, rider_bytes.data(), rider::stride)) {
        return;
    }

    std::uint32_t flags = kReferenceBikeValid | kReferenceRiderValid;
    const bool terrain_lookup = function_id == 7u || function_id == 9u || function_id == 10u ||
                                function_id == 11u || function_id == 12u;
    if (terrain_lookup && boundary == 1u) {
        flags |= 1u << 2u;
        if (boundary_value != 0u) {
            flags |= 1u << 3u;
        }
    }
    const std::uint32_t event_size = kDynamicsEventHeaderSize + bike::stride + rider::stride +
                                     auxiliary_word_count * sizeof(std::uint32_t);
    std::lock_guard lock{g_contract_mutex};
    write_le32(file, kDynamicsEventMagic);
    write_le32(file, event_size);
    write_le64(file, g_snapshot.observed_frames);
    write_le64(file, g_parity_live_frame);
    write_le32(file, function_id);
    write_le32(file, boundary);
    write_le32(file, actor_address);
    write_le32(file, bike_address);
    write_le32(file, rider_address);
    write_le32(file, mode);
    write_le32(file, flags);
    write_le32(file, physics_delta_bits);
    write_le32(file, random_state);
    write_le32(file, update_ticks);
    std::fwrite(bike_bytes.data(), 1u, bike_bytes.size(), file);
    std::fwrite(rider_bytes.data(), 1u, rider_bytes.size(), file);
    for (std::uint32_t index = 0u; index < auxiliary_word_count; ++index) {
        write_le32(file, auxiliary_words[index]);
    }
    if ((function_id == 4u || function_id >= 13u) && boundary == 1u) {
        std::fflush(file);
    }
}

void capture_dynamics_boundary(unsigned char *rdram, std::uint32_t function_id,
                               std::uint32_t boundary, std::uint32_t actor_address) noexcept {
    capture_dynamics_boundary_impl(rdram, function_id, boundary, actor_address, nullptr, 0u);
}

void capture_dynamics_auxiliary_boundary(unsigned char *rdram, std::uint32_t function_id,
                                         std::uint32_t boundary, std::uint32_t actor_address,
                                         std::uint32_t auxiliary_0, std::uint32_t auxiliary_1,
                                         std::uint32_t auxiliary_2, std::uint32_t auxiliary_3,
                                         std::uint32_t auxiliary_4) noexcept {
    const std::array auxiliary_words{
        auxiliary_0, auxiliary_1, auxiliary_2, auxiliary_3, auxiliary_4,
    };
    capture_dynamics_boundary_impl(rdram, function_id, boundary, actor_address,
                                   auxiliary_words.data(),
                                   static_cast<std::uint32_t>(auxiliary_words.size()));
}

void capture_dynamics_auxiliary8_boundary(unsigned char *rdram, std::uint32_t function_id,
                                          std::uint32_t boundary, std::uint32_t actor_address,
                                          std::uint32_t auxiliary_0, std::uint32_t auxiliary_1,
                                          std::uint32_t auxiliary_2, std::uint32_t auxiliary_3,
                                          std::uint32_t auxiliary_4, std::uint32_t auxiliary_5,
                                          std::uint32_t auxiliary_6,
                                          std::uint32_t auxiliary_7) noexcept {
    const std::array auxiliary_words{
        auxiliary_0, auxiliary_1, auxiliary_2, auxiliary_3,
        auxiliary_4, auxiliary_5, auxiliary_6, auxiliary_7,
    };
    capture_dynamics_boundary_impl(rdram, function_id, boundary, actor_address,
                                   auxiliary_words.data(),
                                   static_cast<std::uint32_t>(auxiliary_words.size()));
}

void capture_post_update(unsigned char *rdram) noexcept {
    if (rdram == nullptr) {
        return;
    }

    FILE *file = parity_file();
    FILE *reference_file = reference_state_file();
    FILE *dynamics_file = dynamics_trace_file();
    if (file == nullptr && reference_file == nullptr && dynamics_file == nullptr) {
        return;
    }

    std::lock_guard lock{g_contract_mutex};

    FrameSnapshot frame{};
    if (!capture_frame_snapshot(rdram, frame)) {
        return;
    }

    const bool live_race = is_live_race_mode(frame.mode);
    if (live_race && !g_parity_was_live) {
        g_parity_live_frame = 0;
    }
    if (live_race) {
        ++g_parity_live_frame;
    } else {
        g_parity_live_frame = 0;
    }
    g_parity_was_live = live_race;

    if (reference_file != nullptr) {
        write_reference_state_frame(reference_file, rdram, frame, g_snapshot.observed_frames,
                                    g_parity_live_frame);
    }

    if (file != nullptr) {
        std::uint64_t control_hash = kFnvOffset;
        hash_u32(control_hash, frame.mode);
        hash_u32(control_hash, frame.multiplayer_stage);
        hash_u32(control_hash, frame.active_racers);
        hash_u32(control_hash, frame.buttons);
        hash_u32(control_hash, static_cast<std::uint8_t>(frame.stick_x));
        hash_u32(control_hash, static_cast<std::uint8_t>(frame.stick_y));
        std::uint64_t local_bike_hash = control_hash;
        std::uint64_t world_bike_hash = control_hash;

        if (live_race) {
            for (std::uint32_t racer = 0; racer < frame.active_racers && racer < kMaximumRacers;
                 ++racer) {
                const BikePoseSnapshot &pose = frame.bikes[racer];
                if (!pose.valid) {
                    continue;
                }
                const Vec3Snapshot positions[] = {
                    pose.front_wheel,
                    pose.body,
                    pose.rear_wheel,
                };
                for (const Vec3Snapshot &position : positions) {
                    const float axes[] = {position.x, position.y, position.z};
                    for (float axis : axes) {
                        std::uint32_t position_bits = 0;
                        std::memcpy(&position_bits, &axis, sizeof(position_bits));
                        hash_u32(world_bike_hash, position_bits);
                        if (racer == 0u) {
                            hash_u32(local_bike_hash, position_bits);
                        }
                    }
                }
            }
        }

        const BikePoseSnapshot local_pose = frame.bikes[0];
        std::fprintf(
            file,
            "3,%llu,%llu,0x%02X,%u,%u,0x%04X,%d,%d,0x%08X,%u,0x%016llX,0x%016llX,0x%016llX,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
            static_cast<unsigned long long>(g_snapshot.observed_frames),
            static_cast<unsigned long long>(g_parity_live_frame), frame.mode,
            frame.multiplayer_stage, frame.active_racers, static_cast<unsigned>(frame.buttons),
            static_cast<int>(frame.stick_x), static_cast<int>(frame.stick_y), frame.bike_pool,
            frame.valid_bikes, static_cast<unsigned long long>(control_hash),
            static_cast<unsigned long long>(local_bike_hash),
            static_cast<unsigned long long>(world_bike_hash), local_pose.front_wheel.x,
            local_pose.front_wheel.y, local_pose.front_wheel.z, local_pose.body.x,
            local_pose.body.y, local_pose.body.z, local_pose.rear_wheel.x, local_pose.rear_wheel.y,
            local_pose.rear_wheel.z);
    }

    if (frame.mode != g_last_captured_mode || (g_snapshot.observed_frames % 60u) == 0u) {
        if (file != nullptr) {
            std::fflush(file);
        }
        if (reference_file != nullptr) {
            std::fflush(reference_file);
        }
        g_last_captured_mode = frame.mode;
    }
}

void capture_actor_presentation(unsigned char *rdram, std::uint32_t viewport) noexcept {
    if (rdram == nullptr) {
        return;
    }

    FILE *file = actor_trace_file();
    FILE *matrix_file = actor_matrix_trace_file();
    FILE *segment_file = actor_segment_trace_file();
    if (file == nullptr && matrix_file == nullptr && segment_file == nullptr) {
        return;
    }

    ActorSceneSnapshot scene{};
    if (!capture_actor_scene_snapshot(rdram, viewport, scene) || !is_live_race_mode(scene.mode)) {
        return;
    }

    std::lock_guard lock{g_contract_mutex};
    ++g_actor_capture;
    auto write_actor = [&](const ActorPresentationSnapshot &actor, int racer_index) {
        const std::uint32_t bike_lod =
            actor.bike.selected_lod < actor.bike.models.size() ? actor.bike.selected_lod : 0u;
        const std::uint32_t rider_lod =
            actor.rider.selected_lod < actor.rider.models.size() ? actor.rider.selected_lod : 0u;
        std::fprintf(
            file,
            "12,%llu,%u,0x%02X,%u,%u,%u,%u,%d,"
            "0x%08X,0x%08X,0x%08X,0x%08X,%u,%u,%u,%u,%.9g,%.9g,"
            "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
            "0x%08X,0x%08X,%u,0x%08X,0x%08X,%u,"
            "%u,0x%016llX,%u,0x%016llX,%u,0x%016llX,%u,0x%016llX,"
            "%u,%u,%u,%u,%u,%u,"
            "%u,%u,%u,%u,%u,%u,"
            "0x%016llX,0x%016llX,0x%016llX,0x%016llX,0x%016llX,0x%016llX,"
            "0x%016llX,0x%016llX,0x%016llX,0x%016llX,0x%016llX,0x%016llX,"
            "%u,%u,%u,0x%016llX,%u,%u,%u,0x%016llX,"
            "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
            static_cast<unsigned long long>(g_actor_capture), scene.viewport, scene.mode,
            scene.active_racers, scene.matched_actors, scene.valid_actors,
            scene.graph_complete ? 1u : 0u, racer_index, actor.bike.node, actor.rider.node,
            actor.bike.entity, actor.rider.entity, static_cast<unsigned>(actor.bike.selected_lod),
            static_cast<unsigned>(actor.bike.previous_lod),
            static_cast<unsigned>(actor.rider.selected_lod),
            static_cast<unsigned>(actor.rider.previous_lod), actor.bike.viewport_depth,
            actor.rider.viewport_depth, actor.bike.current_model, actor.bike.models[bike_lod],
            actor.bike.primary_segment_tables[bike_lod], actor.rider.current_model,
            actor.rider.models[rider_lod], actor.rider.primary_segment_tables[rider_lod],
            actor.render_proxy.metadata_valid ? 1u : 0u,
            actor.render_proxy.directly_renderable ? 1u : 0u,
            actor.render_proxy.requires_pose_rebuild ? 1u : 0u,
            actor.render_proxy.renderable_with_selected_transforms ? 1u : 0u,
            actor.render_proxy.bike.selected_transform_remap_safe ? 1u : 0u,
            actor.render_proxy.rider.selected_transform_remap_safe ? 1u : 0u,
            static_cast<unsigned>(actor.render_proxy.bike.stock_segment_count),
            static_cast<unsigned>(actor.render_proxy.bike.desired_segment_count),
            static_cast<unsigned>(actor.render_proxy.rider.stock_segment_count),
            static_cast<unsigned>(actor.render_proxy.rider.desired_segment_count),
            actor.render_proxy.bike.desired_model, actor.render_proxy.bike.desired_display_list,
            static_cast<unsigned>(actor.render_proxy.bike.desired_segment_count),
            actor.render_proxy.rider.desired_model, actor.render_proxy.rider.desired_display_list,
            static_cast<unsigned>(actor.render_proxy.rider.desired_segment_count),
            actor.bike.segment_payloads_valid[bike_lod] ? 1u : 0u,
            static_cast<unsigned long long>(actor.bike.segment_payload_hashes[bike_lod]),
            actor.rider.segment_payloads_valid[rider_lod] ? 1u : 0u,
            static_cast<unsigned long long>(actor.rider.segment_payload_hashes[rider_lod]),
            actor.render_proxy.bike.desired_segments_valid ? 1u : 0u,
            static_cast<unsigned long long>(actor.render_proxy.bike.desired_segment_payload_hash),
            actor.render_proxy.rider.desired_segments_valid ? 1u : 0u,
            static_cast<unsigned long long>(actor.render_proxy.rider.desired_segment_payload_hash),
            static_cast<unsigned>(actor.bike.transform_counts[0]),
            static_cast<unsigned>(actor.bike.transform_counts[1]),
            static_cast<unsigned>(actor.bike.transform_counts[2]),
            static_cast<unsigned>(actor.rider.transform_counts[0]),
            static_cast<unsigned>(actor.rider.transform_counts[1]),
            static_cast<unsigned>(actor.rider.transform_counts[2]),
            static_cast<unsigned>(actor.bike.segment_counts[0]),
            static_cast<unsigned>(actor.bike.segment_counts[1]),
            static_cast<unsigned>(actor.bike.segment_counts[2]),
            static_cast<unsigned>(actor.rider.segment_counts[0]),
            static_cast<unsigned>(actor.rider.segment_counts[1]),
            static_cast<unsigned>(actor.rider.segment_counts[2]),
            static_cast<unsigned long long>(actor.bike.model_graph_hashes[0]),
            static_cast<unsigned long long>(actor.bike.model_graph_hashes[1]),
            static_cast<unsigned long long>(actor.bike.model_graph_hashes[2]),
            static_cast<unsigned long long>(actor.rider.model_graph_hashes[0]),
            static_cast<unsigned long long>(actor.rider.model_graph_hashes[1]),
            static_cast<unsigned long long>(actor.rider.model_graph_hashes[2]),
            static_cast<unsigned long long>(actor.bike.model_allocation_hashes[0]),
            static_cast<unsigned long long>(actor.bike.model_allocation_hashes[1]),
            static_cast<unsigned long long>(actor.bike.model_allocation_hashes[2]),
            static_cast<unsigned long long>(actor.rider.model_allocation_hashes[0]),
            static_cast<unsigned long long>(actor.rider.model_allocation_hashes[1]),
            static_cast<unsigned long long>(actor.rider.model_allocation_hashes[2]),
            static_cast<unsigned>(actor.render_proxy.bike.stock_transform_count),
            static_cast<unsigned>(actor.render_proxy.bike.desired_transform_count),
            actor.render_proxy.bike.selected_transform_buffer_valid ? 1u : 0u,
            static_cast<unsigned long long>(actor.render_proxy.bike.selected_transform_hash),
            static_cast<unsigned>(actor.render_proxy.rider.stock_transform_count),
            static_cast<unsigned>(actor.render_proxy.rider.desired_transform_count),
            actor.render_proxy.rider.selected_transform_buffer_valid ? 1u : 0u,
            static_cast<unsigned long long>(actor.render_proxy.rider.selected_transform_hash),
            actor.physics.front_wheel.x, actor.physics.front_wheel.y, actor.physics.front_wheel.z,
            actor.physics.body.x, actor.physics.body.y, actor.physics.body.z,
            actor.physics.rear_wheel.x, actor.physics.rear_wheel.y, actor.physics.rear_wheel.z);
    };

    if (file != nullptr) {
        bool wrote_actor = false;
        for (const ActorPresentationSnapshot &actor : scene.actors) {
            if (actor.bike.node == 0u) {
                continue;
            }
            write_actor(actor, static_cast<int>(actor.racer_index));
            wrote_actor = true;
        }

        if (!wrote_actor) {
            write_actor(ActorPresentationSnapshot{}, -1);
        }
        std::fflush(file);
    }

    // This trace is intentionally sampled: it is a reverse-engineering aid,
    // not a gameplay data path. It decodes the dynamic matrices selected by
    // the stock renderer without mutating or retaining guest memory.
    if (matrix_file != nullptr && (g_actor_capture % 30u) == 1u) {
        auto write_transform_set = [&](const ActorPresentationSnapshot &actor,
                                       const ModelLodSnapshot &model, const char *part) {
            if (!model.valid || model.selected_lod >= actor_scene::lod_record_count) {
                return;
            }
            RenderTransformSetSnapshot transforms{};
            if (!capture_render_transform_set(rdram, model.selected_transform_buffer,
                                              model.transform_counts[model.selected_lod],
                                              transforms)) {
                return;
            }
            ModelGraphTopologySnapshot topology{};
            if (!capture_model_graph_topology(rdram, model.models[model.selected_lod], topology) ||
                topology.transform_count != transforms.transform_count) {
                return;
            }

            for (std::uint32_t index = 0; index < transforms.transform_count; ++index) {
                const Matrix4x4Snapshot &matrix = transforms.transforms[index];
                std::uint32_t record_index = 0xFFFFFFFFu;
                ModelRecordTopologySnapshot record{};
                for (std::uint32_t candidate = 0; candidate < topology.record_count; ++candidate) {
                    const ModelRecordTopologySnapshot &current = topology.records[candidate];
                    if (index >= current.first_transform &&
                        index < current.first_transform + current.transform_count) {
                        record_index = candidate;
                        record = current;
                        break;
                    }
                }
                std::fprintf(
                    matrix_file,
                    "2,%llu,%u,%u,%u,%s,%u,0x%08X,0x%08X,0x%016llX,0x%08X,"
                    "%u,%u,0x%04X,%u,%u,"
                    "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                    static_cast<unsigned long long>(g_actor_capture), scene.viewport,
                    scene.render_buffer_slot, actor.racer_index, part,
                    static_cast<unsigned>(model.selected_lod), model.node,
                    model.models[model.selected_lod],
                    static_cast<unsigned long long>(model.model_graph_hashes[model.selected_lod]),
                    model.selected_transform_buffer, index, record_index,
                    static_cast<unsigned>(record.type),
                    static_cast<unsigned>(record.first_transform),
                    static_cast<unsigned>(record.transform_count), matrix.values[0],
                    matrix.values[1], matrix.values[2], matrix.values[3], matrix.values[4],
                    matrix.values[5], matrix.values[6], matrix.values[7], matrix.values[8],
                    matrix.values[9], matrix.values[10], matrix.values[11], matrix.values[12],
                    matrix.values[13], matrix.values[14], matrix.values[15],
                    actor.physics.front_wheel.x, actor.physics.front_wheel.y,
                    actor.physics.front_wheel.z, actor.physics.body.x, actor.physics.body.y,
                    actor.physics.body.z, actor.physics.rear_wheel.x, actor.physics.rear_wheel.y,
                    actor.physics.rear_wheel.z);
            }
        };

        for (const ActorPresentationSnapshot &actor : scene.actors) {
            if (!actor.valid) {
                continue;
            }
            write_transform_set(actor, actor.bike, "bike");
            write_transform_set(actor, actor.rider, "rider");
        }
        std::fflush(matrix_file);
    }

    if (segment_file != nullptr && (g_actor_capture % 30u) == 1u) {
        auto write_segment_tables = [&](const ActorPresentationSnapshot &actor,
                                        const ModelLodSnapshot &model, const char *part) {
            for (std::uint32_t lod = 0; lod < actor_scene::lod_record_count; ++lod) {
                SegmentTableSnapshot segments{};
                if (!capture_segment_table(rdram, model.node, lod, model.segment_counts[lod],
                                           segments)) {
                    continue;
                }
                for (std::uint32_t index = 0; index < segments.segment_count; ++index) {
                    std::fprintf(segment_file,
                                 "1,%llu,%u,%u,%u,%s,%u,%u,0x%08X,0x%08X,%u,%u,0x%08X,"
                                 "0x%016llX,"
                                 "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                                 static_cast<unsigned long long>(g_actor_capture), scene.viewport,
                                 scene.render_buffer_slot, actor.racer_index, part, lod,
                                 lod == model.selected_lod ? 1u : 0u, model.node, segments.table,
                                 static_cast<unsigned>(segments.segment_count), index,
                                 segments.records[index],
                                 static_cast<unsigned long long>(segments.payload_hashes[index]),
                                 actor.physics.front_wheel.x, actor.physics.front_wheel.y,
                                 actor.physics.front_wheel.z, actor.physics.body.x,
                                 actor.physics.body.y, actor.physics.body.z,
                                 actor.physics.rear_wheel.x, actor.physics.rear_wheel.y,
                                 actor.physics.rear_wheel.z);
                }
            }
        };

        for (const ActorPresentationSnapshot &actor : scene.actors) {
            if (!actor.valid) {
                continue;
            }
            write_segment_tables(actor, actor.bike, "bike");
            write_segment_tables(actor, actor.rider, "rider");
        }
        std::fflush(segment_file);
    }
}

ContractSnapshot contract_snapshot() noexcept {
    std::lock_guard lock{g_contract_mutex};
    return g_snapshot;
}

} // namespace rr64::engine

extern "C" void rr64_engine_observe_frame(unsigned char *rdram) {
    rr64::engine::observe_frame(rdram);
}

extern "C" void rr64_engine_capture_pre_update(unsigned char *rdram) {
    rr64::engine::capture_pre_update(rdram);
}

extern "C" void rr64_engine_capture_post_update(unsigned char *rdram) {
    rr64::engine::capture_post_update(rdram);
}

extern "C" void rr64_engine_capture_dispatch_boundary(unsigned char *rdram, unsigned int boundary,
                                                      unsigned int target) {
    rr64::engine::capture_dispatch_boundary(rdram, boundary, target);
}

extern "C" void rr64_engine_capture_race_update_boundary(unsigned char *rdram,
                                                         unsigned int boundary) {
    // Observe the existing update boundaries without changing guest state.
    using Clock = std::chrono::steady_clock;
    static thread_local Clock::time_point updateStart;
    static thread_local bool updateActive = false;
    if (boundary == 0u) {
        updateStart = Clock::now();
        updateActive = true;
    } else if ((boundary == 2u) && updateActive) {
        updateActive = false;
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - updateStart)
                .count();
        if (ns >= 0) {
            rr64_record_pipeline_stage(
                static_cast<unsigned int>(RT64::RR64PipelineDiagnostics::Stage::GuestUpdate),
                static_cast<unsigned long long>(ns));
        }
    }
    rr64::engine::capture_race_update_boundary(rdram, boundary);
}

extern "C" void rr64_engine_capture_dynamics_boundary(unsigned char *rdram,
                                                      unsigned int function_id,
                                                      unsigned int boundary,
                                                      unsigned int actor_address) {
    rr64::engine::capture_dynamics_boundary(rdram, function_id, boundary, actor_address);
}

extern "C" void rr64_engine_capture_dynamics_auxiliary_boundary(
    unsigned char *rdram, unsigned int function_id, unsigned int boundary,
    unsigned int actor_address, unsigned int auxiliary_0, unsigned int auxiliary_1,
    unsigned int auxiliary_2, unsigned int auxiliary_3, unsigned int auxiliary_4) {
    rr64::engine::capture_dynamics_auxiliary_boundary(rdram, function_id, boundary, actor_address,
                                                      auxiliary_0, auxiliary_1, auxiliary_2,
                                                      auxiliary_3, auxiliary_4);
}

extern "C" void rr64_engine_capture_dynamics_auxiliary8_boundary(
    unsigned char *rdram, unsigned int function_id, unsigned int boundary,
    unsigned int actor_address, unsigned int auxiliary_0, unsigned int auxiliary_1,
    unsigned int auxiliary_2, unsigned int auxiliary_3, unsigned int auxiliary_4,
    unsigned int auxiliary_5, unsigned int auxiliary_6, unsigned int auxiliary_7) {
    rr64::engine::capture_dynamics_auxiliary8_boundary(
        rdram, function_id, boundary, actor_address, auxiliary_0, auxiliary_1, auxiliary_2,
        auxiliary_3, auxiliary_4, auxiliary_5, auxiliary_6, auxiliary_7);
}

extern "C" void rr64_engine_capture_actor_presentation(unsigned char *rdram,
                                                       unsigned int viewport) {
    rr64::engine::capture_actor_presentation(rdram, viewport);
}

extern "C" int rr64_engine_contract_has_warning() {
    return rr64::engine::contract_snapshot().health == rr64::engine::ContractHealth::Warning ? 1
                                                                                             : 0;
}
