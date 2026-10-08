#include "rr64_shadow_tags.hpp"
#include "rr64_engine_layout.hpp"
#include <atomic>

namespace {
using namespace rr64::engine;
constexpr unsigned racers = 14, maximum = racers * 2 * 4, wrapper_bytes = 72;
constexpr unsigned identity_base = 0x52530000u, group_flags = 0x02011555u;
struct Owner {
    unsigned bike = 0, rider = 0, route = 0, generation = 0;
    bool operator==(const Owner &) const = default;
};
struct Identity {
    const void *memory = nullptr;
    Owner owner{};
    unsigned round = 0, resource = 0, id = 0;
};
struct Patch { unsigned address = 0, vertices = 0, id = 0; };
struct Frame {
    unsigned char *memory = nullptr;
    unsigned base = 0, capacity = 0, slot = 0, epoch = 0, round = 0, views = 0;
    std::array<Owner, racers> owners{};
    std::array<Patch, maximum> patches{};
    std::array<bool, maximum> used{};
    unsigned count = 0, start = 0, vertices = 0, id = 0;
};
thread_local Frame frame;
thread_local std::array<Identity, maximum> identities;
thread_local unsigned serial = 0;
thread_local const void *last_memory = nullptr;
thread_local unsigned last_epoch = 0;
std::atomic_uint64_t tagged_quads{0};
unsigned word(unsigned char *m, unsigned p) { unsigned v = 0; read_u32(m, p, v); return v; }
unsigned half(unsigned char *m, unsigned p) { std::uint16_t v = 0; read_u16(m, p, v); return v; }
bool buffer(unsigned char *m, unsigned &base, unsigned &capacity, unsigned &slot, unsigned &end) {
    slot = word(m, 0x8009cba4u);
    capacity = word(m, 0x800bc9a0u);
    if (!m || slot > 1 || (capacity != 0x4650u && capacity != 0x36b0u)) return false;
    base = word(m, 0x800ac658u + slot * 4);
    end = word(m, 0x800ac650u);
    // 1CB60 allocates 0x140 + count*8 for each graphics slot.
    return !(base & 7u) && valid_guest_range(base, 0x140u + capacity * 8u) &&
        word(m, 0x8009cb90u) == base && !(end & 7u) &&
        end >= base + 0x140u && end <= base + 0x140u + capacity * 8u;
}
bool same_frame(unsigned char *m, unsigned &end) {
    unsigned base = 0, capacity = 0, slot = 0;
    return m == frame.memory && buffer(m, base, capacity, slot, end) &&
        base == frame.base && capacity == frame.capacity && slot == frame.slot &&
        word(m, 0x800a1830u) == frame.epoch;
}
bool owner(unsigned char *m, unsigned slot, Owner &o) {
    const unsigned actor = 0x800d8570u + slot * 0x118u;
    if (!half(m, actor + 0x24u)) return false;
    o.bike = word(m, actor + 0xe0u); o.rider = word(m, actor + 0xe4u);
    o.route = word(m, actor + 0xe8u);
    // Entity +4 owns the actor table entry; the separate route +0 is not a racer ID.
    return valid_guest_range(o.bike, bike::stride) && valid_guest_range(o.rider, rider::stride) &&
        valid_guest_range(o.route, 0x64u) && word(m, o.bike + 4u) == actor &&
        word(m, o.rider + 4u) == actor && word(m, actor) == slot &&
        word(m, o.bike + 0x800u) == o.rider && word(m, o.rider + 0x584u) == o.bike;
}
unsigned next(unsigned char *m, unsigned p) {
    const int delta = static_cast<std::int16_t>(half(m, p + 8u));
    return delta ? unsigned(std::int64_t(p) + std::int64_t(delta) * 8) : 0u;
}
bool graph(unsigned char *m, unsigned root, unsigned part, unsigned &vertices, unsigned &resource) {
    // 47E08 clones only assets 1/2 from the supported 18B280 shadow block.
    // 1AD24 emits root -> child -> mesh; the raw type-11 container has no
    // runtime record. Only the child matrix reaches the one quad vertex load.
    const unsigned block = word(m, 0x800d42d8u);
    if (!valid_guest_range(block, 0x2100u) || word(m, block) != 0x40u ||
        word(m, block + 4) != 0x2100u || word(m, block + 12) != 5u) return false;
    resource = block + (part ? 0x1300u : 0xcc0u);
    const unsigned child = next(m, root), mesh = next(m, child);
    if (!valid_guest_range(root, 0x18u) || !valid_guest_range(child, 0x18u) ||
        !valid_guest_range(mesh, 0x18u) || root == child || root == mesh || child == mesh ||
        half(m, root + 4) != 0x13u || half(m, child + 4) != 0x12u ||
        half(m, mesh + 4) != 0x10u || next(m, mesh) ||
        (half(m, root + 10) & 1u) || !(half(m, mesh + 10) & 0x4000u) ||
        word(m, root + 20) != resource || word(m, child + 20) != resource + 0x50u ||
        word(m, mesh + 20) != resource + 0x100u ||
        word(m, resource) != 0x13u || half(m, resource + 18) != 1u ||
        word(m, resource + 0x50u) != 0x12u || word(m, resource + 0x100u) != 0x10u ||
        half(m, resource + 0x10cu) != 1u || half(m, resource + 0x128u) != 2u ||
        half(m, resource + 0x12au) != 4u || half(m, resource + 0x12eu) != 64u ||
        half(m, resource + 0x170u) != 0x0022u || half(m, resource + 0x172u) != 0x0860u)
        return false;
    vertices = resource + 0x130u;
    return true;
}
void command(unsigned char *m, unsigned &p, unsigned a, unsigned b) {
    write_u32(m, p, a); write_u32(m, p + 4, b); p += 8;
}
}
extern "C" void rr64_shadow_tags_reset() { frame = {}; }
void rr64::shadow_tags::begin(unsigned char *m, unsigned round, unsigned views,
                             const std::array<std::uint32_t, 14> &generations) {
    frame = {};
    unsigned end = 0;
    if (!views || views > 4 || !buffer(m, frame.base, frame.capacity, frame.slot, end)) return;
    frame.memory = m; frame.epoch = word(m, 0x800a1830u); frame.round = round; frame.views = views;
    if (last_memory != m || frame.epoch < last_epoch) identities = {};
    last_memory = m; last_epoch = frame.epoch;
    for (unsigned i = 0; i < racers; ++i) {
        Owner o;
        if (generations[i] && owner(m, i, o)) { o.generation = generations[i]; frame.owners[i] = o; }
    }
}
extern "C" void rr64_shadow_tags_before(unsigned char *m, unsigned node, unsigned root, unsigned part) {
    frame.start = 0;
    unsigned end = 0;
    if (part > 1 || !same_frame(m, end) || frame.count == maximum ||
        !valid_guest_range(node, actor_scene::node_minimum_size) || word(m, node) != 1u) return;
    const unsigned slot = word(m, node + 0x40u), view = word(m, 0x8009dba8u);
    if (slot >= racers || view >= frame.views || !frame.owners[slot].generation) return;
    Owner current;
    if (!owner(m, slot, current)) return;
    current.generation = frame.owners[slot].generation;
    if (current != frame.owners[slot] || word(m, node + 4) != current.bike ||
        word(m, current.bike + 8) != node ||
        word(m, (part ? 0x800dac88u : 0x800dac50u) + slot * 4) != root) return;
    unsigned resource = 0;
    if (!graph(m, root, part, frame.vertices, resource)) return;
    const unsigned index = (view * racers + slot) * 2 + part;
    if (frame.used[index]) return;
    auto &identity = identities[index];
    if (identity.memory != m || identity.owner != current || identity.round != frame.round ||
        identity.resource != resource || !identity.id) {
        // Never wrap and recycle a tag inside this process.
        if (serial == 0xffffu) return;
        identity = {m, current, frame.round, resource, identity_base + ++serial};
    }
    frame.used[index] = true;
    frame.id = identity.id; frame.start = end;
}
extern "C" void rr64_shadow_tags_after(unsigned char *m) {
    const unsigned start = frame.start; frame.start = 0;
    unsigned end = 0;
    if (!start || !same_frame(m, end) || end < start || end - start > 1024u) return;
    unsigned vertex = 0, matrices = 0, triangles = 0, pops = 0;
    for (unsigned p = start; p < end; p += 8) {
        const unsigned a = word(m, p), b = word(m, p + 4), op = a >> 24;
        if (op == 0xdau) {
            if (vertex) return;
            if (a == 0xda380003u && matrices == 0) ++matrices;
            else if (a == 0xda380000u && matrices == 1) ++matrices;
            else if (a != 0xda380007u && a != 0xda380005u) return;
        } else if (op == 1u) {
            if (a != 0x01004008u || b != frame.vertices || vertex || matrices != 2) return;
            vertex = p;
        } else if (op == 6u) {
            if (!vertex || a != 0x06000204u || b != 0x00040600u || triangles) return;
            triangles = 2;
        } else if (op == 0xd8u) {
            if (a != 0xd8380002u || b != 64u || triangles != 2 || pops++) return;
        } else if (op == 5u || op == 0xdeu || op == 0xdfu || op == 2u) return;
    }
    if (vertex && matrices == 2 && triangles == 2 && pops == 1)
        frame.patches[frame.count++] = {vertex, frame.vertices, frame.id};
}
extern "C" void rr64_shadow_tags_finalize(unsigned char *m, unsigned submitted_words) {
    unsigned end = 0;
    if (!frame.count || !same_frame(m, end)) { frame = {}; return; }
    // B0DC is reached only after the original full-sync/end commands and
    // submission-capacity gate. Confirm both independently before any write.
    const unsigned bytes = wrapper_bytes * frame.count;
    const unsigned submitted_end = frame.base + 0x140u + submitted_words * 4u;
    if (submitted_words < 4 || submitted_words > frame.capacity || (submitted_words & 1u) ||
        end < submitted_end || word(m, submitted_end - 16) != 0xe9000000u ||
        word(m, submitted_end - 12) || word(m, submitted_end - 8) != 0xdf000000u ||
        word(m, submitted_end - 4) ||
        bytes > frame.base + 0x140u + frame.capacity * 8u - end) { frame = {}; return; }
    for (unsigned i = 0; i < frame.count; ++i) {
        const auto &p = frame.patches[i];
        if (p.address < frame.base + 0x140u || p.address + 8 > submitted_end - 16 ||
            word(m, p.address) != 0x01004008u || word(m, p.address + 4) != p.vertices) {
            frame = {}; return;
        }
    }
    unsigned tail = end;
    for (unsigned i = 0; i < frame.count; ++i) {
        const auto &p = frame.patches[i];
        const unsigned wrapper = tail;
        command(m, tail, 0xe0525464u, 0x10000064u);
        command(m, tail, 0x6400000cu, p.id); command(m, tail, group_flags, 0);
        command(m, tail, 0xe0525464u, 0x20000000u);
        command(m, tail, 0x01004008u, p.vertices);
        command(m, tail, 0xe0525464u, 0x10000064u);
        command(m, tail, 0x6400000du, 1);
        command(m, tail, 0xe0525464u, 0x20000000u);
        command(m, tail, 0xdf000000u, 0);
        // The ordinary segment-zero call reaches this slot's allocated tail;
        // no extended addresses or original command relocation are involved.
        write_u32(m, p.address, 0xde000000u); write_u32(m, p.address + 4, wrapper);
    }
    // Share allocated tail storage with terrain's earlier finalizer. Native
    // task size was already calculated in registers and stays untouched.
    write_u32(m, 0x800ac650u, tail);
    tagged_quads.fetch_add(frame.count, std::memory_order_relaxed);
    frame = {};
}
extern "C" unsigned long long rr64_shadow_tags_count() {
    return tagged_quads.load(std::memory_order_relaxed);
}
