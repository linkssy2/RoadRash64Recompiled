#include "rr64_actor_held_pose.hpp"
#include "rr64_actor_render_snapshot.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "recomp.h"

extern "C" void func_80015834(unsigned char *, recomp_context *);

namespace rr64::lod {
namespace {
using namespace engine;

bool overlaps(std::uint32_t a, std::uint32_t bytes, std::uint32_t b, std::uint32_t other_bytes) {
    return std::uint64_t(a) < std::uint64_t(b) + other_bytes &&
           std::uint64_t(b) < std::uint64_t(a) + bytes;
}

bool finite_read(unsigned char *memory, std::uint32_t address, float &value) {
    return read_float(memory, address, value) && std::isfinite(value);
}

bool next_record(unsigned char *memory, std::uint32_t record, std::uint32_t &next) {
    std::uint16_t delta = 0;
    if (!read_u16(memory, record + 2u, delta) || delta == 0u) {
        return false;
    }
    const auto address = std::int64_t(record) + std::int16_t(delta) * 8;
    if (address < kRdramBegin || address > std::int64_t(kRdramBegin) + kRdramSize - 0x18) {
        return false;
    }
    next = static_cast<std::uint32_t>(address);
    return true;
}
} // namespace

bool prepare_held_bike_pose(unsigned char *scratch, std::uint32_t node,
                            const void *context) noexcept {
    if (!scratch || !context || !supported_scene(scratch)) {
        return false;
    }
    constexpr std::array<std::array<std::uint32_t, 2>, 9> constants{{{0x80005da0u, 0x42c80000u},
                                                                     {0x80005da4u, 0x3f400000u},
                                                                     {0x80005da8u, 0xc1200000u},
                                                                     {0x80005dacu, 0x40800000u},
                                                                     {0x80005db0u, 0x41200000u},
                                                                     {0x80005db8u, 0xc0a00000u},
                                                                     {0x80005dc0u, 0x40000000u},
                                                                     {0x80005dc4u, 0x40000000u},
                                                                     {0x80005dc8u, 0x3eb4b4afu}}};
    for (const auto &constant : constants) {
        std::uint32_t bits = 0;
        if (!read_u32(scratch, constant[0], bits) || bits != constant[1]) {
            return false;
        }
    }
    std::uint32_t type = 0, entity = 0, owner = 0, rider_node = 0, model_state = 0, id = 0;
    std::uint32_t graph = 0, detailed = 0, view = 0, views = 0;
    std::uint16_t pause = 0, enabled = 0, special = 0, active = 0, selected = 0, flags = 0;
    float speed = 0.0f, limit = 0.0f, effect = 0.0f;
    if (!valid_guest_range(node, 0x148u) || !read_u32(scratch, node, type) || type != 1u ||
        !read_u32(scratch, node + 4u, entity) || !valid_guest_range(entity, bike::stride) ||
        !read_u32(scratch, entity + bike::model_state_pointer, model_state) ||
        !valid_guest_range(model_state, actor_scene::model_state_pose_owner + 4u) ||
        !read_u32(scratch, model_state, id) || id >= kMaximumRacers ||
        !read_u32(scratch, model_state + actor_scene::model_state_pose_owner, owner) ||
        !read_u32(scratch, owner + actor_scene::pose_owner_rider_node, rider_node) ||
        !visual_pair(scratch, node, rider_node) || !read_u32(scratch, 0x8009DB88u, views) ||
        !read_u32(scratch, globals::active_viewport, view) || view >= views ||
        !read_u16(scratch, globals::gameplay_pause_state, pause) || pause != 0u ||
        !read_u16(scratch, 0x800A65BCu, enabled) || enabled == 0u ||
        !read_u16(scratch, 0x800D8570u + id * 0x118u + 0x24u, active) || active == 0u ||
        !finite_read(scratch, entity + 0x184u, speed) ||
        !finite_read(scratch, 0x80005DB0u, limit) || limit != 10.0f ||
        !read_u16(scratch, entity + 0x818u, special) || special != 0u ||
        !finite_read(scratch, entity + 0x4D4u, effect) ||
        !read_u16(scratch, node + actor_scene::selected_lod, selected) || selected != 0u ||
        !read_u32(scratch, node + actor_scene::current_model, graph) ||
        !read_u32(scratch, node + actor_scene::lod_models, detailed) || graph != detailed ||
        !read_u16(scratch, graph + 0xau, flags) || (flags & 1u) != 0u) {
        return false;
    }

    if (effect > 0.0f) {
        // B948 also holds wheels throughout the airborne transition. Build
        // those held children from current phase/suspension, just as at low
        // speed, without advancing the live jump timer or wheel phase.
        std::uint32_t state = 0;
        std::uint16_t attached = 0, ejected = 0;
        if (!read_u32(scratch, entity + 0x100u, state) ||
            !read_u16(scratch, entity + bike::rider_attached, attached) ||
            !read_u16(scratch, owner + rider::ejected, ejected))
            return false;
        // The original BA44..BA54 hold does not depend on attachment or
        // drive state. Ejection preserves visual ownership, but changes the
        // state independently of this timer. The caller still requires fresh
        // independent bike/rider roots and a completed rider crash animation.
        const bool airborne_mounted = mounted_pair(scratch, node, rider_node);
        const bool airborne_ejected = attached == 0u && ejected == 1u;
        if (!airborne_mounted && !airborne_ejected)
            return false;
    } else if (!(speed < limit))
        return false;

    // The bike constructor 1AB20 links +2 only through type12 records whose
    // source+16 is one (1AC10..1AC18). The renderer's +8 chain also includes
    // static type12 records. B948 updates the two linked wheels; 1B020
    // initializes the static record from source+38 XYZ and source+28 quaternion.
    ModelGraphTopologySnapshot topology{};
    if (!capture_model_graph_topology(scratch, graph, topology) || topology.record_count == 0u ||
        topology.records[0].type != actor_scene::model_record_triple_transform) {
        return false;
    }
    // Keep the two wheel records first, then every independently certified
    // static record. Captured bike families have either one or two static
    // children; the constructor contract applies within the graph's bound.
    std::array<std::uint32_t, actor_scene::maximum_model_records> records{}, poses{}, sources{};
    if (!next_record(scratch, graph, records[0]) || !next_record(scratch, records[0], records[1]) ||
        records[0] == records[1]) {
        return false;
    }
    std::uint16_t last_link = 0;
    if (!read_u16(scratch, records[1] + 2u, last_link) || last_link != 0u) {
        return false;
    }
    std::size_t children = 0, part_count = 2;
    std::array<bool, 2> wheel_found{};
    for (std::size_t i = 1; i < topology.record_count; ++i) {
        const auto &record = topology.records[i];
        if (record.type == actor_scene::model_record_triple_transform) {
            return false;
        }
        if (record.type != actor_scene::model_record_single_transform) {
            continue;
        }
        if (++children > records.size()) {
            return false;
        }
        std::uint32_t source = 0, source_type = 0;
        std::uint16_t animated = 0;
        if (!read_u32(scratch, record.address + 0x14u, source) ||
            !valid_guest_range(source, 0x44u) || !read_u32(scratch, source, source_type) ||
            source_type != 0x12u || !read_u16(scratch, source + 0x16u, animated)) {
            return false;
        }
        if (record.address == records[0] || record.address == records[1]) {
            const auto wheel = record.address == records[0] ? 0u : 1u;
            if (animated != 1u || wheel_found[wheel]) {
                return false;
            }
            wheel_found[wheel] = true;
        } else {
            if (animated != 0u || part_count >= records.size()) {
                return false;
            }
            records[part_count++] = record.address;
        }
    }
    if (children != part_count || !wheel_found[0] || !wheel_found[1]) {
        return false;
    }

    const auto &original_context = *static_cast<const recomp_context *>(context);
    const auto stack = static_cast<std::uint32_t>(original_context.r29);
    if (stack < 0x300u || !valid_guest_range(stack - 0x300u, 0x300u)) {
        return false;
    }
    if (overlaps(stack - 0x300u, 0x300u, node, 0x148u) ||
        overlaps(stack - 0x300u, 0x300u, entity, bike::stride) ||
        overlaps(stack - 0x300u, 0x300u, owner, rider::stride) ||
        overlaps(stack - 0x300u, 0x300u, model_state, actor_scene::model_state_pose_owner + 4u)) {
        return false;
    }
    std::uint32_t root_pose = 0;
    if (!read_u32(scratch, graph + 0xcu, root_pose) || !valid_guest_range(root_pose, 32u)) {
        return false;
    }
    if (overlaps(stack - 0x300u, 0x300u, root_pose, 32u)) {
        return false;
    }
    for (std::size_t i = 0; i < topology.record_count; ++i) {
        if (overlaps(stack - 0x300u, 0x300u, topology.records[i].address, 0x18u)) {
            return false;
        }
    }
    std::array<std::array<float, 7>, actor_scene::maximum_model_records> values{};
    std::array<float, 2> phases{}, suspension{};
    for (std::size_t part = 0; part < part_count; ++part) {
        if (!read_u32(scratch, records[part] + 0xcu, poses[part]) ||
            !read_u32(scratch, records[part] + 0x14u, sources[part]) || (poses[part] & 3u) != 0u ||
            !valid_guest_range(poses[part], 32u) || !valid_guest_range(sources[part], 0x44u) ||
            overlaps(stack - 0x300u, 0x300u, sources[part], 0x44u)) {
            return false;
        }
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (!finite_read(scratch, sources[part] + 0x38u + axis * 4u, values[part][axis])) {
                return false;
            }
        }
        if (overlaps(poses[part], 32u, root_pose, 32u) ||
            overlaps(poses[part], 32u, node, 0x148u) ||
            overlaps(poses[part], 32u, entity, bike::stride) ||
            overlaps(poses[part], 32u, owner, rider::stride) ||
            overlaps(poses[part], 32u, model_state, actor_scene::model_state_pose_owner + 4u) ||
            overlaps(poses[part], 32u, stack - 0x300u, 0x300u)) {
            return false;
        }
        for (std::size_t i = 0; i < topology.record_count; ++i) {
            if (overlaps(poses[part], 32u, topology.records[i].address, 0x18u)) {
                return false;
            }
        }
        for (std::size_t previous = 0; previous < part; ++previous) {
            if (overlaps(poses[part], 32u, poses[previous], 32u)) {
                return false;
            }
        }
    }
    for (std::size_t part = 0; part < part_count; ++part) {
        for (std::size_t source = 0; source < part_count; ++source) {
            if (overlaps(poses[part], 32u, sources[source], 0x44u)) {
                return false;
            }
        }
    }
    for (std::size_t wheel = 0; wheel < 2; ++wheel) {
        if (!finite_read(scratch, entity + (wheel == 0u ? 0x3F0u : 0x328u), phases[wheel]) ||
            !finite_read(scratch, entity + (wheel == 0u ? 0x560u : 0x55Cu), suspension[wheel])) {
            return false;
        }
    }
    for (std::size_t part = 2; part < part_count; ++part) {
        float static_norm = 0.0f;
        for (std::uint32_t axis = 0; axis < 4u; ++axis) {
            if (!finite_read(scratch, sources[part] + 0x28u + axis * 4u, values[part][axis + 3u])) {
                return false;
            }
            static_norm += values[part][axis + 3u] * values[part][axis + 3u];
        }
        if (!(static_norm > 0.0f && static_norm <= 16.0f)) {
            return false;
        }
    }

    // Exact BC24..BC54 and BE24..BE94 arithmetic, with the ROM float factor
    // preserved. No wheel phase integration or trail/effect generation runs.
    const float front_scaled = suspension[0] * 100.0f;
    const float rear_scaled = suspension[1] * 100.0f - 2.0f;
    if (!std::isfinite(front_scaled) || !std::isfinite(rear_scaled)) {
        return false;
    }
    const float front = std::clamp(front_scaled, -5.0f, 8.0f) * 0.75f;
    const float rear = std::clamp(rear_scaled, -10.0f, 4.0f) * 0.75f;
    values[0][2] += front;
    values[1][2] += rear;
    values[1][0] -= rear * std::bit_cast<float>(0x3eb4b4afu);
    for (std::size_t wheel = 0; wheel < 2; ++wheel) {
        auto local_context = original_context;
        local_context.f_odd =
            local_context.mips3_float_mode ? &local_context.f1.u32l : &local_context.f0.u32h;
        local_context.r29 = static_cast<std::int32_t>(stack - 0x100u);
        const auto angles = stack - 0x80u;
        const auto quaternion = stack - 0x70u;
        write_float(scratch, angles, 0.0f);
        write_float(scratch, angles + 4u, phases[wheel]);
        write_float(scratch, angles + 8u, 0.0f);
        local_context.r4 = static_cast<std::int32_t>(angles);
        local_context.r5 = static_cast<std::int32_t>(quaternion);
        func_80015834(scratch, &local_context);
        for (std::uint32_t axis = 0; axis < 4; ++axis) {
            if (!finite_read(scratch, quaternion + axis * 4u, values[wheel][axis + 3u])) {
                return false;
            }
        }
        for (const float value : values[wheel]) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
    }
    for (std::size_t part = 0; part < part_count; ++part) {
        for (std::uint32_t word = 0; word < 7; ++word) {
            write_float(scratch, poses[part] + word * 4u, values[part][word]);
        }
    }
    return true;
}
} // namespace rr64::lod
