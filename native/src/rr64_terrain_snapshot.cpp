#include "rr64_terrain_snapshot.hpp"
#include "rr64_terrain_residency.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

#include "rr64_msvc_crt_compat.hpp"

namespace rr64::engine {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_u32(std::uint64_t &hash, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= kFnvPrime;
    }
}

bool cell_index_for_address(std::uint32_t cell_grid, std::uint32_t cell_count,
                            std::uint32_t address, std::uint32_t &index) noexcept {
    if (address < cell_grid) {
        return false;
    }
    const std::uint32_t offset = address - cell_grid;
    if ((offset % terrain::cell_stride) != 0u) {
        return false;
    }
    index = offset / terrain::cell_stride;
    return index < cell_count;
}

} // namespace

bool capture_terrain_scene_snapshot(unsigned char *rdram, TerrainSceneSnapshot &snapshot) noexcept {
    snapshot = {};
    std::uint32_t map_width = 0;
    if (rdram == nullptr || !read_u32(rdram, globals::main_mode, snapshot.mode) ||
        !read_u32(rdram, globals::terrain_current_record, snapshot.current_record) ||
        !read_u32(rdram, globals::terrain_cell_grid, snapshot.cell_grid) ||
        !read_u32(rdram, globals::terrain_map_width, map_width) || map_width == 0u ||
        map_width > terrain::maximum_map_width ||
        !read_float(rdram, globals::terrain_camera_position, snapshot.camera_x) ||
        !read_float(rdram, globals::terrain_camera_position + sizeof(float), snapshot.camera_z)) {
        return false;
    }

    const std::uint64_t cell_count_wide = static_cast<std::uint64_t>(map_width) * map_width;
    const std::uint64_t grid_size_wide = cell_count_wide * terrain::cell_stride;
    if (cell_count_wide > std::numeric_limits<std::uint32_t>::max() ||
        grid_size_wide > std::numeric_limits<std::uint32_t>::max() ||
        !valid_guest_range(snapshot.cell_grid, static_cast<std::uint32_t>(grid_size_wide)) ||
        !valid_guest_range(snapshot.current_record, terrain::candidate_record_size)) {
        return false;
    }
    snapshot.map_width = map_width;
    snapshot.cell_count = static_cast<std::uint32_t>(cell_count_wide);

    if (!read_u32(rdram, snapshot.current_record + terrain::candidate_count,
                  snapshot.candidate_count) ||
        snapshot.candidate_count > terrain::candidate_pointer_capacity) {
        return false;
    }

    std::array<std::uint32_t, terrain::candidate_pointer_capacity> candidates{};
    std::uint32_t minimum_candidate_row = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t minimum_candidate_column = std::numeric_limits<std::uint32_t>::max();
    snapshot.candidate_hash = kFnvOffset;
    for (std::uint32_t candidate = 0; candidate < snapshot.candidate_count; ++candidate) {
        std::uint32_t cell = 0;
        if (!read_u32(rdram, snapshot.current_record + candidate * sizeof(std::uint32_t), cell)) {
            continue;
        }
        candidates[candidate] = cell;
        hash_u32(snapshot.candidate_hash, cell);
        std::uint32_t cell_index = 0;
        if (!cell_index_for_address(snapshot.cell_grid, snapshot.cell_count, cell, cell_index)) {
            continue;
        }
        ++snapshot.candidate_valid;
        const std::uint32_t row = cell_index / snapshot.map_width;
        const std::uint32_t column = cell_index % snapshot.map_width;
        minimum_candidate_row = std::min(minimum_candidate_row, row);
        snapshot.candidate_max_row = std::max(snapshot.candidate_max_row, row);
        minimum_candidate_column = std::min(minimum_candidate_column, column);
        snapshot.candidate_max_column = std::max(snapshot.candidate_max_column, column);
        bool duplicate = false;
        for (std::uint32_t previous = 0; previous < candidate; ++previous) {
            if (candidates[previous] == cell) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            ++snapshot.candidate_unique;
        }
        std::uint8_t state = 0;
        if (read_u8(rdram, cell + terrain::cell_state, state) && state == terrain::ready_state) {
            ++snapshot.candidate_ready;
        }
    }
    if (snapshot.candidate_valid > 0u) {
        snapshot.candidate_min_row = minimum_candidate_row;
        snapshot.candidate_min_column = minimum_candidate_column;
    }

    std::array<std::uint32_t, terrain::display_list_slot_count> display_lists{};
    for (std::uint32_t slot = 0; slot < terrain::display_list_slot_count; ++slot) {
        if (read_u32(rdram, globals::terrain_display_list_slots + slot * sizeof(std::uint32_t),
                     display_lists[slot]) &&
            valid_guest_range(display_lists[slot], 8u)) {
            ++snapshot.display_list_slots_valid;
        }
    }

    std::array<std::uint16_t, terrain::display_list_slot_count> slot_owners{};
    snapshot.lifecycle_hash = kFnvOffset;
    snapshot.resource_hash = kFnvOffset;
    float minimum_outside = std::numeric_limits<float>::infinity();
    for (std::uint32_t index = 0; index < snapshot.cell_count; ++index) {
        const std::uint32_t cell = snapshot.cell_grid + index * terrain::cell_stride;
        std::uint8_t state = 0;
        std::uint32_t request_epoch = 0;
        std::uint32_t payload = 0;
        std::int8_t slot = -1;
        if (!read_u8(rdram, cell + terrain::cell_state, state) ||
            !read_u32(rdram, cell + terrain::cell_request_epoch, request_epoch) ||
            !read_u32(rdram, cell + terrain::cell_payload, payload) ||
            !read_s8(rdram, cell + terrain::cell_display_list_slot, slot)) {
            return false;
        }
        hash_u32(snapshot.lifecycle_hash, state);
        hash_u32(snapshot.lifecycle_hash, request_epoch);
        snapshot.cell_states[index] = state;
        if (state < snapshot.state_counts.size()) {
            ++snapshot.state_counts[state];
        } else {
            ++snapshot.other_state_count;
        }
        if (state != terrain::ready_state) {
            continue;
        }

        ++snapshot.ready_cells;
        bool in_candidate = false;
        for (std::uint32_t candidate = 0; candidate < snapshot.candidate_count; ++candidate) {
            if (candidates[candidate] == cell) {
                in_candidate = true;
                break;
            }
        }
        if (in_candidate) {
            ++snapshot.ready_in_candidate;
        } else {
            ++snapshot.ready_outside_candidate;
        }

        const bool valid_payload = valid_guest_range(payload, terrain::payload_minimum_size);
        const bool valid_slot =
            slot >= 0 && static_cast<std::uint32_t>(slot) < terrain::display_list_slot_count;
        const bool valid_display_list =
            valid_slot && valid_guest_range(display_lists[static_cast<std::uint32_t>(slot)], 8u);
        if (valid_payload && valid_display_list) {
            ++snapshot.ready_resources;
            ++slot_owners[static_cast<std::uint32_t>(slot)];
            hash_u32(snapshot.resource_hash, payload);
            hash_u32(snapshot.resource_hash, static_cast<std::uint8_t>(slot));
            hash_u32(snapshot.resource_hash, display_lists[static_cast<std::uint32_t>(slot)]);
        }

        float world_x = 0.0f;
        float world_z = 0.0f;
        if (!valid_payload || !read_float(rdram, payload + terrain::payload_world_x, world_x) ||
            !read_float(rdram, payload + terrain::payload_world_z, world_z) ||
            !std::isfinite(world_x) || !std::isfinite(world_z)) {
            continue;
        }
        const float distance = std::hypot(world_x - snapshot.camera_x, world_z - snapshot.camera_z);
        snapshot.maximum_ready_distance = std::max(snapshot.maximum_ready_distance, distance);
        if (in_candidate) {
            snapshot.maximum_candidate_distance =
                std::max(snapshot.maximum_candidate_distance, distance);
        } else {
            minimum_outside = std::min(minimum_outside, distance);
            snapshot.maximum_outside_candidate_distance =
                std::max(snapshot.maximum_outside_candidate_distance, distance);
        }
    }

    for (std::uint16_t owners : slot_owners) {
        if (owners > 1u) {
            snapshot.duplicate_slot_owners += owners - 1u;
        }
    }
    snapshot.minimum_outside_candidate_distance =
        std::isfinite(minimum_outside) ? minimum_outside : 0.0f;
    snapshot.valid = snapshot.candidate_valid == snapshot.candidate_count &&
                     snapshot.candidate_unique == snapshot.candidate_count &&
                     snapshot.ready_resources == snapshot.ready_cells &&
                     snapshot.duplicate_slot_owners == 0u;
    return true;
}

} // namespace rr64::engine

namespace {
std::atomic_uint64_t g_terrain_trace_calls{0};
std::atomic_uint64_t g_terrain_trace_samples{0};
std::mutex g_terrain_trace_mutex;

struct TerrainCellTraceState {
    bool initialized = false;
    std::uint32_t grid = 0;
    std::uint32_t width = 0;
    std::array<std::uint8_t,
               rr64::engine::terrain::maximum_map_width * rr64::engine::terrain::maximum_map_width>
        states{};
    std::array<std::uint32_t,
               rr64::engine::terrain::maximum_map_width * rr64::engine::terrain::maximum_map_width>
        payloads{};
    std::array<std::int8_t,
               rr64::engine::terrain::maximum_map_width * rr64::engine::terrain::maximum_map_width>
        slots{};
    std::array<float,
               rr64::engine::terrain::maximum_map_width * rr64::engine::terrain::maximum_map_width>
        world_x{};
    std::array<float,
               rr64::engine::terrain::maximum_map_width * rr64::engine::terrain::maximum_map_width>
        world_z{};
    std::array<bool,
               rr64::engine::terrain::maximum_map_width * rr64::engine::terrain::maximum_map_width>
        world_position_valid{};
};

TerrainCellTraceState g_terrain_cell_trace_state{};

FILE *terrain_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_TERRAIN_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(
                opened,
                "schema,sample,hook_call,mode,valid,current_record,cell_grid,map_width,cell_count,"
                "camera_x,camera_z,candidate_count,candidate_valid,candidate_unique,candidate_ready,"
                "candidate_min_row,candidate_max_row,candidate_min_column,candidate_max_column,"
                "display_slots_valid,state0,state1,state2,state3,state4,state5,state6,state7,state_other,"
                "ready_cells,ready_resources,ready_in_candidate,ready_outside_candidate,duplicate_slot_owners,"
                "maximum_ready_distance,maximum_candidate_distance,minimum_outside_candidate_distance,"
                "maximum_outside_candidate_distance,lifecycle_hash,resource_hash,candidate_hash\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

FILE *terrain_cell_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_TERRAIN_CELL_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(
                opened,
                "schema,hook_call,mode,cell_index,row,column,old_state,new_state,"
                "old_payload,new_payload,old_slot,new_slot,request_epoch,in_candidate,distance\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

FILE *terrain_prefetch_trace_file() {
    static FILE *file = []() -> FILE * {
        char *path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "RR64_TERRAIN_PREFETCH_TRACE") != 0 || path == nullptr ||
            path[0] == '\0') {
            std::free(path);
            return nullptr;
        }
        FILE *opened = _fsopen(path, "w", _SH_DENYNO);
        std::free(path);
        if (opened != nullptr) {
            std::fprintf(
                opened,
                "schema,sample,hook_call,mode,valid,candidate_count,candidate_min_row,"
                "candidate_max_row,candidate_min_column,candidate_max_column,ready_resources,"
                "inflight_resources,reserved_slots,available_budget,ring_cells_considered,"
                "eligible_cells,planned_cells,complete,ring_state0,ring_state1,ring_state2,"
                "ring_state3,ring_state4,ring_state5,ring_state6,ring_state7,ring_state_other\n");
            std::fflush(opened);
        }
        return opened;
    }();
    return file;
}

void trace_terrain_cell_transitions(FILE *file, unsigned char *rdram, std::uint64_t hook_call) {
    if (file == nullptr || rdram == nullptr) {
        return;
    }
    std::uint32_t mode = 0;
    std::uint32_t grid = 0;
    std::uint32_t width = 0;
    std::uint32_t record = 0;
    float camera_x = 0.0f;
    float camera_z = 0.0f;
    if (!rr64::engine::read_u32(rdram, rr64::engine::globals::main_mode, mode) ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::terrain_cell_grid, grid) ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::terrain_map_width, width) ||
        !rr64::engine::read_u32(rdram, rr64::engine::globals::terrain_current_record, record) ||
        !rr64::engine::read_float(rdram, rr64::engine::globals::terrain_camera_position,
                                  camera_x) ||
        !rr64::engine::read_float(
            rdram, rr64::engine::globals::terrain_camera_position + sizeof(float), camera_z) ||
        width == 0u || width > rr64::engine::terrain::maximum_map_width ||
        !rr64::engine::valid_guest_range(record, rr64::engine::terrain::candidate_record_size)) {
        return;
    }
    const std::uint32_t cell_count = width * width;
    if (!rr64::engine::valid_guest_range(grid, cell_count * rr64::engine::terrain::cell_stride)) {
        return;
    }

    std::array<std::uint32_t, rr64::engine::terrain::candidate_pointer_capacity> candidates{};
    std::uint32_t candidate_count = 0;
    if (!rr64::engine::read_u32(rdram, record + rr64::engine::terrain::candidate_count,
                                candidate_count) ||
        candidate_count > candidates.size()) {
        return;
    }
    for (std::uint32_t candidate = 0; candidate < candidate_count; ++candidate) {
        rr64::engine::read_u32(rdram, record + candidate * sizeof(std::uint32_t),
                               candidates[candidate]);
    }

    const bool reset = !g_terrain_cell_trace_state.initialized ||
                       g_terrain_cell_trace_state.grid != grid ||
                       g_terrain_cell_trace_state.width != width;
    if (reset) {
        g_terrain_cell_trace_state = {};
    }
    std::uint32_t transitions = 0;
    for (std::uint32_t index = 0; index < cell_count; ++index) {
        const std::uint32_t cell = grid + index * rr64::engine::terrain::cell_stride;
        std::uint8_t state = 0;
        std::uint32_t payload = 0;
        std::int8_t slot = -1;
        if (!rr64::engine::read_u8(rdram, cell + rr64::engine::terrain::cell_state, state) ||
            !rr64::engine::read_u32(rdram, cell + rr64::engine::terrain::cell_payload, payload) ||
            !rr64::engine::read_s8(rdram, cell + rr64::engine::terrain::cell_display_list_slot,
                                   slot)) {
            return;
        }
        const std::uint8_t old_state = g_terrain_cell_trace_state.states[index];
        const std::uint32_t old_payload = g_terrain_cell_trace_state.payloads[index];
        const std::int8_t old_slot = g_terrain_cell_trace_state.slots[index];
        float payload_world_x = 0.0f;
        float payload_world_z = 0.0f;
        if (rr64::engine::valid_guest_range(payload, rr64::engine::terrain::payload_minimum_size) &&
            rr64::engine::read_float(rdram, payload + rr64::engine::terrain::payload_world_x,
                                     payload_world_x) &&
            rr64::engine::read_float(rdram, payload + rr64::engine::terrain::payload_world_z,
                                     payload_world_z) &&
            std::isfinite(payload_world_x) && std::isfinite(payload_world_z)) {
            g_terrain_cell_trace_state.world_x[index] = payload_world_x;
            g_terrain_cell_trace_state.world_z[index] = payload_world_z;
            g_terrain_cell_trace_state.world_position_valid[index] = true;
        }
        g_terrain_cell_trace_state.states[index] = state;
        g_terrain_cell_trace_state.payloads[index] = payload;
        g_terrain_cell_trace_state.slots[index] = slot;
        if (reset || (state == old_state && payload == old_payload && slot == old_slot)) {
            continue;
        }

        std::uint32_t request_epoch = 0;
        rr64::engine::read_u32(rdram, cell + rr64::engine::terrain::cell_request_epoch,
                               request_epoch);
        bool in_candidate = false;
        for (std::uint32_t candidate = 0; candidate < candidate_count; ++candidate) {
            if (candidates[candidate] == cell) {
                in_candidate = true;
                break;
            }
        }
        float distance = 0.0f;
        if (g_terrain_cell_trace_state.world_position_valid[index]) {
            distance = std::hypot(g_terrain_cell_trace_state.world_x[index] - camera_x,
                                  g_terrain_cell_trace_state.world_z[index] - camera_z);
        }
        std::fprintf(file, "1,%llu,0x%02X,%u,%u,%u,%u,%u,0x%08X,0x%08X,%d,%d,%u,%u,%.9g\n",
                     static_cast<unsigned long long>(hook_call), mode, index, index / width,
                     index % width, static_cast<unsigned>(old_state), static_cast<unsigned>(state),
                     old_payload, payload, static_cast<int>(old_slot), static_cast<int>(slot),
                     request_epoch, in_candidate ? 1u : 0u, distance);
        ++transitions;
    }
    g_terrain_cell_trace_state.initialized = true;
    g_terrain_cell_trace_state.grid = grid;
    g_terrain_cell_trace_state.width = width;
    if (transitions > 0u) {
        std::fflush(file);
    }
}
} // namespace

extern "C" void rr64_trace_terrain_scene(unsigned char *rdram) {
    FILE *file = terrain_trace_file();
    FILE *cell_file = terrain_cell_trace_file();
    FILE *prefetch_file = terrain_prefetch_trace_file();
    if (file == nullptr && cell_file == nullptr && prefetch_file == nullptr) {
        return;
    }
    const std::uint64_t hook_call = ++g_terrain_trace_calls;
    trace_terrain_cell_transitions(cell_file, rdram, hook_call);
    if ((file == nullptr && prefetch_file == nullptr) ||
        (hook_call != 1u && (hook_call % 15u) != 0u)) {
        return;
    }
    rr64::engine::TerrainSceneSnapshot snapshot{};
    const bool captured = rr64::engine::capture_terrain_scene_snapshot(rdram, snapshot);
    const std::uint64_t sample = ++g_terrain_trace_samples;
    std::lock_guard lock{g_terrain_trace_mutex};
    if (file != nullptr) {
        std::fprintf(
            file,
            "1,%llu,%llu,0x%02X,%u,0x%08X,0x%08X,%u,%u,%.9g,%.9g,"
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
            "%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,"
            "0x%016llX,0x%016llX,0x%016llX\n",
            static_cast<unsigned long long>(sample), static_cast<unsigned long long>(hook_call),
            snapshot.mode, captured && snapshot.valid ? 1u : 0u, snapshot.current_record,
            snapshot.cell_grid, snapshot.map_width, snapshot.cell_count, snapshot.camera_x,
            snapshot.camera_z, snapshot.candidate_count, snapshot.candidate_valid,
            snapshot.candidate_unique, snapshot.candidate_ready, snapshot.candidate_min_row,
            snapshot.candidate_max_row, snapshot.candidate_min_column,
            snapshot.candidate_max_column, snapshot.display_list_slots_valid,
            snapshot.state_counts[0], snapshot.state_counts[1], snapshot.state_counts[2],
            snapshot.state_counts[3], snapshot.state_counts[4], snapshot.state_counts[5],
            snapshot.state_counts[6], snapshot.state_counts[7], snapshot.other_state_count,
            snapshot.ready_cells, snapshot.ready_resources, snapshot.ready_in_candidate,
            snapshot.ready_outside_candidate, snapshot.duplicate_slot_owners,
            snapshot.maximum_ready_distance, snapshot.maximum_candidate_distance,
            snapshot.minimum_outside_candidate_distance,
            snapshot.maximum_outside_candidate_distance,
            static_cast<unsigned long long>(snapshot.lifecycle_hash),
            static_cast<unsigned long long>(snapshot.resource_hash),
            static_cast<unsigned long long>(snapshot.candidate_hash));
        std::fflush(file);
    }

    if (prefetch_file != nullptr) {
        constexpr std::uint32_t kReservedResourceSlots = 16u;
        const bool states_valid = captured && snapshot.valid && snapshot.candidate_count > 0u;
        rr64::engine::TerrainPrefetchPlan plan{};
        const std::uint32_t inflight_resources = snapshot.state_counts[4];
        const bool planned =
            states_valid &&
            rr64::engine::build_terrain_prefetch_plan(
                snapshot.map_width, snapshot.candidate_min_row, snapshot.candidate_max_row,
                snapshot.candidate_min_column, snapshot.candidate_max_column,
                std::span<const std::uint8_t>{snapshot.cell_states.data(), snapshot.cell_count},
                snapshot.ready_resources + inflight_resources, kReservedResourceSlots, plan);
        std::fprintf(
            prefetch_file,
            "1,%llu,%llu,0x%02X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
            "%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
            static_cast<unsigned long long>(sample), static_cast<unsigned long long>(hook_call),
            snapshot.mode, planned && plan.valid ? 1u : 0u, snapshot.candidate_count,
            snapshot.candidate_min_row, snapshot.candidate_max_row, snapshot.candidate_min_column,
            snapshot.candidate_max_column, snapshot.ready_resources, inflight_resources,
            kReservedResourceSlots, plan.available_resource_budget, plan.ring_cells_considered,
            plan.eligible_cells, plan.planned_cells, plan.complete ? 1u : 0u,
            plan.ring_state_counts[0], plan.ring_state_counts[1], plan.ring_state_counts[2],
            plan.ring_state_counts[3], plan.ring_state_counts[4], plan.ring_state_counts[5],
            plan.ring_state_counts[6], plan.ring_state_counts[7], plan.ring_other_state_count);
        std::fflush(prefetch_file);
    }
}
