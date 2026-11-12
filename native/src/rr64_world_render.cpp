#include "rr64_presentation_options.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_world_render.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_actor_render_snapshot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

extern "C" void func_80018668(unsigned char *, recomp_context *);

namespace {
using namespace rr64::engine;
using rr64::lod::ActorSnapshot;
constexpr unsigned capacity = 128u;
constexpr unsigned node_bytes = 0x148u;
constexpr unsigned animation_bones = 11u;
constexpr unsigned animation_table = 0x800b74b0u;
constexpr unsigned animation_style = 0x8009dc2cu;
constexpr unsigned camera_position = 0x800d69f8u;
constexpr unsigned camera_sector = 0x800a4fdcu;
constexpr float maximum_root_component = 30000.0f;
std::array<std::atomic<unsigned long long>, 5> counters{};

bool overlap(unsigned a, unsigned an, unsigned b, unsigned bn) {
    return std::uint64_t(a) < std::uint64_t(b) + bn && std::uint64_t(b) < std::uint64_t(a) + an;
}
void mix(std::uint64_t &hash, unsigned word) {
    hash = (hash ^ word) * 1099511628211ull;
}
unsigned head_address(unsigned type) {
    return 0x800a1458u + (type - 3u) * 4u;
}
unsigned entity_size(unsigned type) {
    return type == 3u ? 0x350u : type == 4u ? 0x35cu : 0x28u;
}
unsigned position_offset(unsigned type) {
    return type == 3u ? 0x7cu : type == 4u ? 0xa8u : 0x18u;
}
unsigned rotation_offset(unsigned type) {
    return type == 3u ? 0x154u : type == 4u ? 0x180u : 8u;
}

struct Allocation {
    unsigned node = 0, entity = 0, type = 0;
    std::array<unsigned, 3> models{};
    std::array<unsigned, 8> buffers{}, bytes{};
};
struct RootStamp {
    unsigned node = 0, entity = 0, type = 0;
    std::uint64_t generation = 0, resource = 0, inputs = 0;
    std::array<unsigned, 8> root{};
};
struct State {
    unsigned char *mapping = nullptr;
    std::uint64_t generation = 1, scratch_generation = 0;
    std::array<Allocation, capacity> allocations{};
    std::array<RootStamp, capacity> roots{};
    std::vector<unsigned char> scratch;
    std::mutex mutex;
};
State &state() {
    static auto instance = std::make_unique<State>();
    return *instance;
}
thread_local bool draw_active = false;
thread_local rr64::lod::PoseBinding binding;
thread_local unsigned char *bound_mapping = nullptr;
thread_local unsigned bound_node = 0, bound_root = 0, bound_stock = 0;
thread_local bool normalized = false;

void map(State &s, unsigned char *m) {
    if (s.mapping == m)
        return;
    s.mapping = m;
    s.allocations = {};
    s.roots = {};
    ++s.generation;
    s.scratch_generation = 0;
}
void expire(State &s) {
    ++s.generation;
    s.roots = {};
    s.scratch_generation = 0;
}

bool identity(unsigned char *m, unsigned node, unsigned &type, unsigned &entity) {
    return valid_guest_range(node, node_bytes) && read_u32(m, node, type) && type >= 3u &&
           type <= 5u && read_u32(m, node + 4u, entity) &&
           valid_guest_range(entity, entity_size(type));
}
bool active(unsigned char *m, unsigned type, unsigned entity) {
    std::uint16_t enabled = 0;
    if (type == 5u)
        return true;
    return read_u16(m, entity + (type == 3u ? 8u : 0x334u), enabled) && enabled != 0u;
}
bool member(unsigned char *m, unsigned node, unsigned type) {
    unsigned cursor = 0;
    if (!read_u32(m, head_address(type), cursor))
        return false;
    std::array<unsigned, capacity> seen{};
    bool found = false;
    for (unsigned count = 0; cursor; ++count) {
        if (count == seen.size() || !valid_guest_range(cursor, node_bytes) ||
            std::find(seen.begin(), seen.begin() + count, cursor) != seen.begin() + count)
            return false;
        seen[count] = cursor;
        found |= cursor == node;
        if (!read_u32(m, cursor + actor_scene::next, cursor))
            return false;
    }
    return found;
}
bool inputs(unsigned char *m, unsigned type, unsigned entity, std::uint64_t &hash) {
    hash = 14695981039346656037ull;
    if (!active(m, type, entity))
        return false;
    for (auto offset : {position_offset(type), rotation_offset(type)}) {
        const auto count = offset == position_offset(type) ? 3u : 4u;
        for (unsigned i = 0; i < count; ++i) {
            unsigned bits = 0;
            if (!read_u32(m, entity + offset + i * 4u, bits) ||
                !std::isfinite(std::bit_cast<float>(bits)))
                return false;
            mix(hash, bits);
        }
    }
    if (type == 3u) {
        for (unsigned offset : {0xcu, 0x304u, 0x310u}) {
            unsigned word = 0;
            if (!read_u32(m, entity + offset, word))
                return false;
            mix(hash, word);
        }
    }
    // Both terms are needed after the original camera sector rebase.
    for (unsigned address : {camera_position, camera_sector}) {
        for (unsigned i = 0; i < 3u; ++i) {
            unsigned bits = 0;
            if (!read_u32(m, address + i * 4u, bits) || !std::isfinite(std::bit_cast<float>(bits)))
                return false;
            mix(hash, bits);
        }
    }
    return true;
}
bool finite_pose(const rr64::lod::Pose &pose) {
    float norm = 0;
    for (unsigned i = 0; i < 7u; ++i) {
        const float value = std::bit_cast<float>(pose.words[i]);
        if (!std::isfinite(value))
            return false;
        if (i >= 3u)
            norm += value * value;
    }
    return norm > 0.0f && norm <= 16.0f;
}
const Allocation *allocation(State &s, unsigned char *m, unsigned node) {
    for (const auto &a : s.allocations) {
        if (a.node != node)
            continue;
        unsigned type = 0, entity = 0;
        if (!identity(m, node, type, entity) || type != a.type || entity != a.entity)
            return nullptr;
        for (unsigned i = 0; i < 3u; ++i) {
            unsigned model = 0;
            if (!read_u32(m, node + 0x2cu + i * 4u, model) || model != a.models[i])
                return nullptr;
        }
        return &a;
    }
    return nullptr;
}

bool bank_ready(unsigned char *m, unsigned slot) {
    // This is the same bank consumed by source-2 stock world geometry. The
    // actor helper validates that bank through its source-1 conversion plan.
    rr64::lod::RootRenderPlan bank{};
    bank.original_source = 1u;
    bank.render_source = 2u;
    bank.normalized = true;
    return rr64_world_camera_ready(m, 2u, slot) &&
           rr64::lod::root_render_bank_ready(m, bank, 0u, slot);
}
bool capture(State &s, unsigned char *m, unsigned node, unsigned slot, ActorSnapshot &out,
             bool require_bank = true) {
    out = {};
    const auto *a = allocation(s, m, node);
    if (!a || slot >= 2u || !active(m, a->type, a->entity) || !member(m, node, a->type))
        return false;
    out.node = node;
    out.type = a->type;
    out.entity = a->entity;
    out.model = a->models[0];
    if (!read_u16(m, node + 0x44u, out.stock_lod) || out.stock_lod > (a->type == 3u ? 1u : 0u) ||
        !read_u32(m, node + 0x28u, out.stock_model) ||
        out.stock_model != a->models[out.stock_lod] || !read_u32(m, node + 0x50u, out.display_list))
        return false;
    ModelGraphTopologySnapshot topology;
    if (!capture_model_graph_topology(m, out.model, topology) || topology.record_count == 0u ||
        topology.records[0].type != 0x13u)
        return false;
    for (unsigned i = 0; i < 2u; ++i) {
        unsigned buffer = 0;
        if (!read_u32(m, node + 0x5cu + i * 4u, buffer) || buffer != a->buffers[i] ||
            topology.transform_count * 64u > a->bytes[i] || !valid_guest_range(buffer, a->bytes[i]))
            return false;
        out.certified_transform_buffers[i] = buffer;
        out.certified_allocation_bytes[i] = a->bytes[i];
    }
    if (overlap(a->buffers[0], a->bytes[0], a->buffers[1], a->bytes[1]))
        return false;
    out.transform_buffer = a->buffers[slot];
    out.allocation_bytes = a->bytes[slot];
    auto &hash = out.resource_signature;
    hash = 14695981039346656037ull;
    mix(hash, out.model);
    mix(hash, out.stock_model);
    mix(hash, out.stock_lod);
    unsigned tail = 0;
    if (!read_u32(m, node + 0x13cu, tail) || tail < out.display_list || (out.display_list & 7u) ||
        (tail & 7u) || tail - out.display_list > 0x100000u)
        return false;
    out.display_list_bytes = tail - out.display_list + 8u;
    if (!valid_guest_range(out.display_list, out.display_list_bytes))
        return false;
    for (unsigned offset = 0; offset < out.display_list_bytes; offset += 4u) {
        unsigned word = 0;
        if (!read_u32(m, out.display_list + offset, word))
            return false;
        mix(hash, word);
    }
    if (!read_u16(m, node + actor_scene::segment_counts, out.segment_count) || !out.segment_count ||
        out.segment_count > 8u)
        return false;
    for (unsigned i = 0; i < out.segment_count; ++i) {
        auto &segment = out.segment_records[i];
        if (!read_u32(m, node + 0x7cu + i * 4u, segment) || !valid_guest_range(segment, 128u))
            return false;
        mix(hash, segment);
        for (unsigned j = 0; j < 128u; j += 4u) {
            unsigned word = 0;
            if (!read_u32(m, segment + j, word))
                return false;
            mix(hash, word);
        }
    }
    for (unsigned i = 0; i < topology.record_count; ++i) {
        const auto &record = topology.records[i];
        mix(hash, record.address);
        mix(hash, record.type);
        mix(hash, unsigned(record.next_delta));
        std::uint16_t animation_next = 0;
        if (!read_u16(m, record.address + 2u, animation_next))
            return false;
        mix(hash, animation_next);
        if (!record.transform_count)
            continue;
        if ((i != 0u && record.type != 0x12u) || out.pose_count >= out.poses.size())
            return false;
        auto &pose = out.poses[out.pose_count++];
        pose.record = record.address;
        pose.type = record.type;
        if (!read_u32(m, record.address + 0xcu, pose.address) ||
            !valid_guest_range(pose.address, 32u) || (pose.address & 3u) ||
            !read_u32(m, record.address + 0x14u, pose.source_record) ||
            !valid_guest_range(pose.source_record, record.type == 0x13u ? 0x14u : 0x44u))
            return false;
        mix(hash, pose.address);
        mix(hash, pose.source_record);
        for (unsigned j = 0; j < (record.type == 0x13u ? 0x14u : 0x44u); j += 4u) {
            unsigned word = 0;
            if (!read_u32(m, pose.source_record + j, word))
                return false;
            mix(hash, word);
        }
        for (unsigned j = 0; j < 8u; ++j)
            if (!read_u32(m, pose.address + j * 4u, pose.words[j]))
                return false;
        if (!finite_pose(pose))
            return false;
        if (i != 0u) {
            for (unsigned axis = 0; axis < 3u; ++axis) {
                if (std::abs(std::bit_cast<float>(pose.words[axis])) > maximum_root_component)
                    return false;
            }
            std::uint16_t animated = 0;
            if (out.type != 3u &&
                (!read_u16(m, pose.source_record + 0x16u, animated) || animated != 0u))
                return false;
        }
    }
    if (!out.pose_count || (out.type == 3u && out.pose_count != animation_bones + 1u))
        return false;
    auto &plan = out.root_plan;
    plan.record = out.poses[0].record;
    plan.source_record = out.poses[0].source_record;
    if (!read_u16(m, plan.source_record + 0x12u, plan.original_source) ||
        plan.original_source != (out.type == 3u ? 1u : 2u))
        return false;
    unsigned detailed_scale = 0, render_scale = 0;
    if (!read_u32(m, 0x8009DBB0u, detailed_scale) ||
        detailed_scale != std::bit_cast<unsigned>(100.0f) ||
        !read_u32(m, 0x8009DBB4u, render_scale) || render_scale != std::bit_cast<unsigned>(10.0f))
        return false;
    mix(hash, detailed_scale);
    mix(hash, render_scale);
    plan.render_source = 2u;
    plan.normalized = out.type == 3u;
    // Readiness checks the actual slot's packed projection, view and norm.
    if (require_bank && !bank_ready(m, slot))
        return false;
    for (unsigned i = 0; i < out.pose_count; ++i) {
        const auto &pose = out.poses[i];
        for (const auto &other : s.allocations) {
            if (!other.node)
                continue;
            if (overlap(pose.address, 32u, other.node, node_bytes) ||
                overlap(pose.address, 32u, other.entity, entity_size(other.type)))
                return false;
            for (unsigned b = 0; b < 8u; ++b)
                if (other.bytes[b] && overlap(pose.address, 32u, other.buffers[b], other.bytes[b]))
                    return false;
        }
        if (overlap(pose.address, 32u, out.display_list, out.display_list_bytes))
            return false;
        for (unsigned j = 0; j < out.segment_count; ++j)
            if (overlap(pose.address, 32u, out.segment_records[j], 128u))
                return false;
        for (unsigned j = 0; j < out.pose_count; ++j) {
            const auto &other = out.poses[j];
            if ((i != j && overlap(pose.address, 32u, other.address, 32u)) ||
                overlap(pose.address, 32u, other.record, 0x18u) ||
                overlap(pose.address, 32u, other.source_record,
                        other.type == 0x13u ? 0x14u : 0x44u))
                return false;
        }
    }
    const float scale = out.type == 3u ? 0.1f : 1.0f;
    for (unsigned i = 0; i < 3u; ++i) {
        const float v = std::bit_cast<float>(out.poses[0].words[i]) * scale;
        if (!std::isfinite(v) || std::abs(v) > maximum_root_component)
            return false;
    }
    return true;
}

bool animation_preflight(unsigned char *m, const ActorSnapshot &actor, unsigned &clip,
                         unsigned &phase, std::uint64_t &resource) {
    unsigned mode = 0, motion = 0, style = 0;
    if (!read_u32(m, actor.entity + 0xcu, mode) || !read_u32(m, actor.entity + 0x304u, motion) ||
        !read_u32(m, actor.entity + 0x310u, phase) || !std::isfinite(std::bit_cast<float>(phase)) ||
        std::bit_cast<float>(phase) < 0.0f || !read_u32(m, animation_style, style) || style >= 6u)
        return false;
    clip = mode == 3u ? (motion == 1u ? 0x53u : 0x52u) : mode == 1u ? 0x54u : 0u;
    if (!clip)
        return false;
    resource = 14695981039346656037ull;
    mix(resource, clip);
    mix(resource, style);
    // Original18668 walks +2 links, while rendering walks +8. Require exactly
    // the same eleven bones, in order, before calling the unchecked guest math.
    unsigned cursor = actor.model;
    for (unsigned i = 0; i < actor.pose_count; ++i) {
        if (cursor != actor.poses[i].record)
            return false;
        std::uint16_t delta = 0;
        if (!read_u16(m, cursor + 2u, delta))
            return false;
        if (!delta)
            cursor = 0;
        else {
            const auto next = std::int64_t(cursor) + std::int16_t(delta) * 8;
            if (next < kRdramBegin || next > kRdramEnd - 0x18u)
                return false;
            cursor = unsigned(next);
        }
    }
    if (cursor)
        return false;
    const unsigned table = animation_table + clip * 132u + style * 11220u;
    for (unsigned bone = 0; bone < animation_bones; ++bone) {
        unsigned channel = 0;
        if (!read_u32(m, table + bone * 12u, channel) || !valid_guest_range(channel, 0x20u))
            return false;
        mix(resource, channel);
        for (unsigned kind = 0; kind < 2u; ++kind) {
            std::int8_t count = 0;
            unsigned keys = 0;
            if (!read_s8(m, channel + 0xeu + kind, count) || count < 0 || count == 1 ||
                !read_u32(m, table + bone * 12u + 4u + kind * 4u, keys))
                return false;
            mix(resource, unsigned(count));
            mix(resource, keys);
            if (!count)
                continue;
            float cache_time = 0;
            if (!read_float(m, channel + 0x14u + kind * 8u, cache_time) ||
                !std::isfinite(cache_time))
                return false;
            if (!valid_guest_range(keys, unsigned(count) * 8u))
                return false;
            int last = -1;
            for (int key = 0; key < count; ++key) {
                std::uint16_t t = 0;
                if (!read_u16(m, keys + key * 8u + (kind ? 6u : 4u), t) || std::int16_t(t) <= last)
                    return false;
                last = std::int16_t(t);
                unsigned first = 0, second = 0;
                if (!read_u32(m, keys + key * 8u, first) ||
                    !read_u32(m, keys + key * 8u + 4u, second) || (!kind && !first))
                    return false;
                mix(resource, first);
                mix(resource, second);
            }
        }
    }
    return true;
}
bool prepare_pedestrian(State &s, unsigned char *m, ActorSnapshot &actor,
                        const recomp_context &caller) {
    unsigned clip = 0, phase = 0;
    std::uint64_t resource = 0;
    if (!animation_preflight(m, actor, clip, phase, resource))
        return false;
    const unsigned stack = static_cast<unsigned>(caller.r29);
    if (stack < kRdramBegin + 0x800u || !valid_guest_range(stack - 0x800u, 0x800u))
        return false;
    for (unsigned i = 0; i < actor.pose_count; ++i)
        if (overlap(stack - 0x800u, 0x800u, actor.poses[i].address, 32u))
            return false;
    if (s.scratch.size() != kRdramSize)
        s.scratch.resize(kRdramSize);
    // At most one full copy per visual generation. Original animation changes
    // only private channels/poses; each next pedestrian samples current inputs.
    if (s.scratch_generation != s.generation) {
        std::memcpy(s.scratch.data(), m, kRdramSize);
        s.scratch_generation = s.generation;
    }
    auto *scratch = s.scratch.data();
    for (unsigned i = 0; i < actor.pose_count; ++i) {
        for (unsigned j = 0; j < 8u; ++j)
            write_u32(scratch, actor.poses[i].address + j * 4u, actor.poses[i].words[j]);
    }
    // Cached scratch must never turn an old resource pointer into a new graph.
    ActorSnapshot copied;
    if (!capture(s, scratch, actor.node, 0u, copied, false) ||
        copied.resource_signature != actor.resource_signature)
        return false;
    unsigned copied_clip = 0, copied_phase = 0;
    std::uint64_t copied_resource = 0;
    if (!animation_preflight(scratch, actor, copied_clip, copied_phase, copied_resource) ||
        copied_clip != clip || copied_phase != phase || copied_resource != resource)
        return false;
    recomp_context context = caller;
    context.f_odd = context.mips3_float_mode ? &context.f1.u32l : &context.f0.u32h;
    context.r29 = guest_address(stack - 0x100u);
    context.r4 = guest_address(actor.model);
    context.r5 = clip;
    context.r6 = 0x100u;
    context.r7 = phase;
    write_u32(scratch, stack - 0x100u + 0x10u, 0u);
    write_float(scratch, stack - 0x100u + 0x14u, 1.0f);
    func_80018668(scratch, &context);
    for (unsigned i = 0; i < actor.pose_count; ++i) {
        auto &pose = actor.poses[i];
        for (unsigned j = 0; j < 8u; ++j)
            if (!read_u32(scratch, pose.address + j * 4u, pose.words[j]))
                return false;
        if (!finite_pose(pose))
            return false;
    }
    return actor.poses[0].words == copied.poses[0].words;
}
} // namespace

extern "C" int rr64_world_distance_enabled() {
    static const bool fallback =
        rr64::presentation_options::environment_enabled("RR64_WORLD_DISTANCE");
    return rr64::presentation_options::enabled(rr64::presentation_options::world_distance,
                                               fallback);
}
extern "C" unsigned long long rr64_world_counter(unsigned index) {
    return index < counters.size() ? counters[index].load(std::memory_order_relaxed) : 0u;
}
extern "C" void rr64_world_end_actor() {
    binding.end();
    bound_mapping = nullptr;
    bound_node = bound_root = bound_stock = 0;
    normalized = false;
}
extern "C" void rr64_world_invalidate(unsigned char *m) {
    if (rr64::prediction::active()) return;
    if (!rr64_world_distance_enabled())
        return;
    rr64_world_end_actor();
    auto &s = state();
    std::lock_guard lock(s.mutex);
    map(s, m);
    expire(s);
}
extern "C" void rr64_world_observe_allocation(unsigned char *m, unsigned node, unsigned view,
                                              unsigned bytes) {
    // Private native constructors must not replace the live world's cache owner.
    if (rr64::prediction::active()) return;
    if (!rr64_world_distance_enabled() || view >= 4u || !bytes || bytes > 128u * 64u || bytes % 64u)
        return;
    unsigned type = 0, entity = 0;
    if (!identity(m, node, type, entity))
        return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    map(s, m);
    Allocation *selected = nullptr;
    for (auto &a : s.allocations) {
        if (a.node == node) {
            selected = &a;
            break;
        }
        if (!a.node && !selected)
            selected = &a;
    }
    if (!selected) {
        for (auto &a : s.allocations) {
            if (!member(m, a.node, a.type)) {
                selected = &a;
                *selected = {};
                break;
            }
        }
    }
    if (!selected)
        return;
    if (!view)
        *selected = {};
    selected->node = node;
    selected->type = type;
    selected->entity = entity;
    for (unsigned i = 0; i < 3u; ++i)
        if (!read_u32(m, node + 0x2cu + i * 4u, selected->models[i])) {
            *selected = {};
            return;
        }
    for (unsigned i = 0; i < 2u; ++i) {
        const unsigned index = view * 2u + i;
        if (!read_u32(m, node + 0x5cu + index * 4u, selected->buffers[index]) ||
            !valid_guest_range(selected->buffers[index], bytes)) {
            *selected = {};
            return;
        }
        selected->bytes[index] = bytes;
    }
    expire(s);
}
extern "C" void rr64_world_observe_roots(unsigned char *m, unsigned type) {
    if (!rr64_world_distance_enabled() || type < 3u || type > 5u ||
        !rr64::world::supported_scene(m))
        return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    map(s, m);
    for (unsigned i = 0; i < s.allocations.size(); ++i) {
        if (s.allocations[i].type != type)
            continue;
        s.roots[i] = {};
        ActorSnapshot captured;
        // Cached presentation builds the actual slot's camera bank later.
        if (!capture(s, m, s.allocations[i].node, 0u, captured, false))
            continue;
        RootStamp stamp;
        stamp.node = captured.node;
        stamp.entity = captured.entity;
        stamp.type = type;
        stamp.generation = s.generation;
        stamp.resource = captured.resource_signature;
        stamp.root = captured.poses[0].words;
        if (inputs(m, type, captured.entity, stamp.inputs))
            s.roots[i] = stamp;
    }
}
extern "C" void rr64_world_begin_draw(unsigned char *m) {
    rr64_world_end_actor();
    draw_active = rr64_world_distance_enabled() && rr64::world::supported_scene(m);
}
extern "C" void rr64_world_end_draw(unsigned char *m) {
    rr64_world_end_actor();
    draw_active = false;
    rr64_world_invalidate(m);
}
extern "C" unsigned rr64_world_actor_hidden(unsigned char *m, unsigned node, unsigned hidden,
                                            const void *caller) {
    rr64_world_end_actor();
    if (!draw_active || !caller || !rr64_world_distance_enabled() ||
        !rr64::world::supported_scene(m))
        return hidden;
    unsigned type = 0;
    if (!read_u32(m, node, type) || type < 3u || type > 5u)
        return hidden;
    const auto fallback = [&] {
        counters[3].fetch_add(1u, std::memory_order_relaxed);
        return hidden;
    };
    unsigned slot = 0, view = 0;
    if (!read_u32(m, 0x8009dbd4u, slot) || slot >= 2u || !read_u32(m, 0x8009db84u, view) || view)
        return fallback();
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.mapping != m)
        return fallback();
    ActorSnapshot actor;
    if (!capture(s, m, node, slot, actor)) {
        counters[4].fetch_add(1u, std::memory_order_relaxed);
        return fallback();
    }
    std::uint64_t current_inputs = 0;
    if (!inputs(m, actor.type, actor.entity, current_inputs))
        return fallback();
    const RootStamp *stamp = nullptr;
    for (const auto &candidate : s.roots)
        if (candidate.node == node && candidate.generation == s.generation) {
            stamp = &candidate;
            break;
        }
    if (!stamp || stamp->entity != actor.entity || stamp->resource != actor.resource_signature ||
        stamp->inputs != current_inputs || stamp->root != actor.poses[0].words)
        return fallback();
    if (actor.type == 3u &&
        !prepare_pedestrian(s, m, actor, *static_cast<const recomp_context *>(caller))) {
        counters[4].fetch_add(1u, std::memory_order_relaxed);
        return fallback();
    }
    if (!binding.begin(m, actor))
        return fallback();
    bound_mapping = m;
    bound_node = node;
    bound_root = actor.model;
    bound_stock = actor.stock_lod;
    normalized = actor.type == 3u;
    counters[type - 3u].fetch_add(1u, std::memory_order_relaxed);
    return 0u;
}
extern "C" unsigned rr64_world_select(unsigned char *m, unsigned node, unsigned stock_lod) {
    return binding.active() && m == bound_mapping && node == bound_node && stock_lod == bound_stock
               ? 0u
               : stock_lod;
}
extern "C" unsigned rr64_world_root_source(unsigned char *m, unsigned node, unsigned record,
                                           unsigned source) {
    return binding.active() && m == bound_mapping && node == bound_node && record == bound_root &&
                   normalized && source == 1u
               ? 2u
               : source;
}
extern "C" void rr64_world_scale_root_matrix(unsigned char *m, unsigned node, unsigned record,
                                             unsigned matrix) {
    if (!binding.active() || m != bound_mapping || node != bound_node || record != bound_root ||
        !normalized || !valid_guest_range(matrix, 64u))
        return;
    for (unsigned row = 0; row < 4u; ++row)
        for (unsigned column = 0; column < 3u; ++column) {
            float value = 0;
            if (read_float(m, matrix + (row * 4u + column) * 4u, value))
                write_float(m, matrix + (row * 4u + column) * 4u, value * 0.1f);
        }
}
