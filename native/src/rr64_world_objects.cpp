#include "rr64_world_objects.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_local_world_window.hpp"
#include "rr64_world_frustum.hpp"
#include "rr64_world_render.hpp"
#include "rr64_world_course_regions.hpp"
#include "rr64_actor_render_snapshot.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include <bit>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rr64::world {
namespace {
using namespace rr64::engine;
constexpr unsigned maximum_placements = 4257u, command_bytes = 256u * 1024u,
                   frame_bytes = 1024u * 1024u;
// Stable authored placement IDs make repeated models match directly instead of
// comparing every visually similar instance. Keep RT64's inherited component
// policies; only replace AUTO identity/order with explicit identity/LINEAR order.
constexpr unsigned object_id_base = 0x52520000u, object_group_flags = 0x02011555u;
static_assert(maximum_placements * 56u + 200u <= command_bytes);
struct Frame {
    unsigned char *host = nullptr;
    unsigned base = 0, commands = 0, matrices = 0;
    std::array<unsigned, 4> epoch{};
    std::array<bool, 4> issued{};
};
struct Animation {
    float elapsed = 0;
    unsigned frame = 0, synced_epoch = 0;
    bool synced = false;
};
struct Cache {
    unsigned char *mapping = nullptr;
    const unsigned char *rom = nullptr;
    ObjectAssets assets;
    std::array<Frame, 2> frames{};
    std::array<bool, maximum_placements> stock{};
    std::unordered_map<std::uint64_t, std::vector<unsigned>> placements;
    std::vector<Animation> animation;
    bool attempted = false, ready = false, drawing = false, clock_valid = false;
    unsigned clock_epoch = 0;
    ObjectStatistics stats{};
    std::mutex mutex;
};
Cache &cache() {
    static auto c = std::make_unique<Cache>();
    return *c;
}
void word(unsigned char *rdram, unsigned address, unsigned value) {
    MEM_W(0, guest_address(address)) = value;
}
void command(unsigned char *m, unsigned &p, unsigned a, unsigned b) {
    word(m, p, a);
    word(m, p + 4, b);
    p += 8;
}
std::uint64_t hash(unsigned descriptor, const std::array<float, 4> &q,
                   const std::array<float, 3> &p) {
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&](unsigned w) { h = (h ^ w) * 1099511628211ull; };
    mix(descriptor);
    for (float f : q)
        mix(std::bit_cast<unsigned>(f));
    for (float f : p)
        mix(std::bit_cast<unsigned>(f));
    return h;
}
void release(Cache &c) {
    for (auto &f : c.frames)
        if (f.host)
            recomp::free(c.mapping, f.host);
    c.frames = {};
    c.ready = false;
}
bool initialize(Cache &c, unsigned char *m) {
    const auto rom = recomp::get_rom();
    if (c.mapping != m || c.rom != rom.data()) {
        if (c.mapping == m)
            release(c);
        c.mapping = m;
        c.rom = rom.data();
        c.frames = {};
        c.assets = {};
        c.placements.clear();
        c.animation.clear();
        c.attempted = c.ready = c.clock_valid = false;
        c.stats = {};
    }
    if (c.attempted)
        return c.ready;
    c.attempted = true;
    std::string error;
    if (!build_object_assets(rom, c.assets, error, true, true)) {
        std::fprintf(stderr, "[RR64-WORLD] object cache refused: %s\n", error.c_str());
        return false;
    }
    if (c.assets.placements.size() > maximum_placements ||
        c.assets.bytes.size() > 16u * 1024u * 1024u)
        return false;
    for (unsigned i = 0; i < c.assets.placements.size(); ++i) {
        const auto &p = c.assets.placements[i];
        if (p.model_index >= c.assets.models.size() ||
            c.assets.models[p.model_index].source_bank != 2u)
            return false;
        c.placements[hash(p.descriptor, p.quaternion, p.position)].push_back(i);
    }
    const unsigned bytes = (unsigned(c.assets.bytes.size()) + 63u) & ~63u;
    for (auto &f : c.frames) {
        f.host = static_cast<unsigned char *>(recomp::alloc(m, bytes + 4u * frame_bytes));
        if (!f.host) {
            release(c);
            return false;
        }
        const auto offset = f.host - m;
        if (offset < 0x800000 ||
            std::uint64_t(offset) + bytes + 4u * frame_bytes > recomp::mem_size) {
            release(c);
            return false;
        }
        f.base = 0x80000000u + unsigned(offset);
        f.commands = f.base + bytes;
        f.matrices = f.commands + command_bytes;
        for (unsigned i = 0; i < c.assets.bytes.size(); ++i)
            m[(unsigned(offset) + i) ^ 3u] = c.assets.bytes[i];
        for (const auto &r : c.assets.relocations)
            word(m, f.base + r.word_offset, f.base + r.target_offset);
    }
    c.animation.resize(c.assets.textures.size());
    c.ready = true;
    c.stats.cached_models = unsigned(c.assets.models.size());
    c.stats.cached_placements = unsigned(c.assets.placements.size());
    c.stats.cached_bytes = 2u * (bytes + 4u * frame_bytes);
    if (rr64::diagnostics::routine_enabled()) {
        std::fprintf(stderr,
                     "[RR64-WORLD] object cache models=%u placements=%u bytes=%u triangle-batches=1\n",
                     c.stats.cached_models, c.stats.cached_placements, c.stats.cached_bytes);
    }
    return true;
}
bool context(unsigned char *m, unsigned &slot, unsigned &gfx, unsigned &epoch, unsigned &pointer,
             unsigned &camera_index, ObjectMatrix &view, ObjectMatrix &projection,
             std::array<float, 3> &camera, std::array<float, 3> &sector,
             std::array<float, 2> &eye) {
    unsigned width = 0, base = 0, active_base = 0, count = 0;
    if (!read_u32(m, globals::active_viewport, camera_index) || camera_index >= 4u ||
        !read_u32(m, globals::terrain_map_width, width) || width != 70u ||
        !read_u32(m, globals::actor_render_buffer_slot, slot) || slot > 1u ||
        !rr64_world_camera_ready(m, 2u, slot) || !read_u32(m, 0x8009CBA4u, gfx) || gfx > 1u ||
        !read_u32(m, 0x800A1830u, epoch) || !read_u32(m, 0x800AC650u, pointer) ||
        !read_u32(m, 0x800AC658u + gfx * 4u, base) || !read_u32(m, 0x8009CB90u, active_base) ||
        base != active_base || !read_u32(m, 0x800BC9A0u, count) ||
        (count != 0x4650u && count != 0x36b0u))
        return false;
    const unsigned size = 0x140u + count * 8u;
    if (!valid_guest_range(base, size) || pointer < base + 0x148u ||
        pointer > base + size - 1064u || (pointer & 7u))
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!read_float(m, 0x800D69F8u + i * 4u, camera[i]) || !std::isfinite(camera[i]) ||
            !read_float(m, 0x800A4FDCu + i * 4u, sector[i]) || !std::isfinite(sector[i]))
            return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!read_float(m, 0x800D6A28u + i * 4u, eye[i]) || !std::isfinite(eye[i]))
            return false;
    unsigned scale = 0;
    std::uint16_t norm = 0;
    if (!read_u32(m, 0x8009DBB4u, scale) || scale != std::bit_cast<unsigned>(10.0f) ||
        !read_u16(m, 0x800B73F0u + camera_index * 12u + slot * 2u, norm) || !norm)
        return false;
    Matrix4x4Snapshot p{}, v{};
    if (!decode_n64_matrix(m, 0x800B6668u + camera_index * 0x180u + slot * 64u, p) ||
        !decode_n64_matrix(m, 0x800B6EE8u + camera_index * 0x180u + slot * 64u, v))
        return false;
    projection = p.values;
    view = v.values;
    return p.values[0] != 0 && p.values[5] != 0 && p.values[11] != 0 && v.values[15] == 1;
}
void animate(Cache &c, unsigned char *m, unsigned epoch, Frame &f) {
    if (!c.clock_valid || c.clock_epoch != epoch) {
        float step = 0;
        if (!read_float(m, 0x80000C70u, step) || !std::isfinite(step) || step < 0 || step > 100)
            step = 0;
        for (unsigned i = 0; i < c.animation.size(); ++i) {
            const auto &t = c.assets.textures[i];
            auto &a = c.animation[i];
            if (a.synced && a.synced_epoch == epoch)
                continue;
            if (!(t.flags & 4u) || t.frame_count < 2u || !std::isfinite(t.frame_duration_ms) ||
                t.frame_duration_ms <= 0)
                continue;
            a.elapsed += step;
            if (a.elapsed >= t.frame_duration_ms) {
                a.elapsed -= t.frame_duration_ms;
                a.frame = (a.frame + 1u) % t.frame_count;
            }
        }
        c.clock_epoch = epoch;
        c.clock_valid = true;
    }
    for (unsigned view = 0; view < 4; ++view)
        if (f.issued[view] && f.epoch[view] == epoch)
            return;
    for (const auto &r : c.assets.relocations)
        if (r.texture_index < c.animation.size()) {
            const auto &t = c.assets.textures[r.texture_index];
            const auto &a = c.animation[r.texture_index];
            const std::uint64_t offset =
                std::uint64_t(r.target_offset) + std::uint64_t(a.frame) * t.frame_stride;
            if (offset < t.raw_offset + t.raw_size)
                word(m, f.base + r.word_offset, f.base + unsigned(offset));
        }
}
}
ObjectStatistics objects_statistics() noexcept {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    return c.stats;
}
void objects_reset_session() noexcept {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    c.mapping = nullptr;
    c.rom = nullptr;
    c.frames = {};
    c.assets = {};
    c.placements.clear();
    c.animation.clear();
    c.stock = {};
    c.attempted = c.ready = c.drawing = c.clock_valid = false;
    c.clock_epoch = 0;
    c.stats = {};
}
}
extern "C" void rr64_world_objects_begin(unsigned char *m) {
    auto &c = rr64::world::cache();
    std::lock_guard lock(c.mutex);
    c.drawing = false;
    c.stock.fill(false);
    c.stats.stock_placements = c.stats.visible_placements = c.stats.drawn_triangles = 0;
    if (!rr64_world_distance_enabled() ||
        !(rr64_draw_distance_enabled() && rr64::world::static_scene(m)))
        return;
    c.drawing = rr64::world::initialize(c, m);
}
extern "C" void rr64_world_objects_observe(unsigned char *m, unsigned placement) {
    using namespace rr64::world;
    using namespace rr64::engine;
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (!c.drawing || c.mapping != m || !valid_guest_range(placement, 0x30u))
        return;
    std::uint16_t descriptor = 0;
    std::array<float, 4> q{};
    std::array<float, 3> p{};
    if (!read_u16(m, placement + 0x28u, descriptor))
        return;
    for (unsigned i = 0; i < 4; ++i)
        if (!read_float(m, placement + i * 4u, q[i]))
            return;
    for (unsigned i = 0; i < 3; ++i)
        if (!read_float(m, placement + 0x10u + i * 4u, p[i]))
            return;
    const auto found = c.placements.find(hash(descriptor, q, p));
    bool matched = false;
    if (found != c.placements.end())
        for (unsigned index : found->second) {
            const auto &expected = c.assets.placements[index];
            if (expected.descriptor != descriptor || expected.quaternion != q ||
                expected.position != p)
                continue;
            matched = true;
            if (!c.stock[index]) {
                c.stock[index] = true;
                ++c.stats.stock_placements;
            }
        }
    if (!matched)
        ++c.stats.unmatched_stock;
}
extern "C" void rr64_world_objects_sample(unsigned char *m, unsigned placement, unsigned graph) {
    using namespace rr64::world;
    using namespace rr64::engine;
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (!c.drawing || c.mapping != m || !valid_guest_range(placement, 0x30u) ||
        !valid_guest_range(graph, 0x18u))
        return;
    std::uint16_t descriptor = 0, type = 0;
    unsigned source = 0, epoch = 0;
    if (!read_u16(m, placement + 0x28u, descriptor) || !read_u16(m, graph + 4u, type) ||
        type != 0x13u || !read_u32(m, graph + 0x14u, source) || !read_u32(m, 0x800A1830u, epoch))
        return;
    std::array<float, 4> q{};
    std::array<float, 3> p{};
    for (unsigned i = 0; i < 4; ++i)
        if (!read_float(m, placement + i * 4u, q[i]))
            return;
    for (unsigned i = 0; i < 3; ++i)
        if (!read_float(m, placement + 0x10u + i * 4u, p[i]))
            return;
    const auto found = c.placements.find(hash(descriptor, q, p));
    if (found == c.placements.end())
        return;
    unsigned model_index = ~0u;
    for (unsigned i : found->second) {
        const auto &candidate = c.assets.placements[i];
        if (candidate.descriptor == descriptor && candidate.quaternion == q &&
            candidate.position == p) {
            model_index = candidate.model_index;
            break;
        }
    }
    if (model_index >= c.assets.models.size())
        return;
    const auto &model = c.assets.models[model_index];
    if (source < kRdramBegin + model.root_source_offset)
        return;
    const unsigned base = source - model.root_source_offset;
    const auto rom = recomp::get_rom();
    if (rom.data() != c.rom || model.raw_rom_offset > rom.size() ||
        rom.size() - model.raw_rom_offset < 8u)
        return;
    const unsigned rp = model.raw_rom_offset + 4u;
    const unsigned expected_size = (unsigned(rom[rp]) << 24u) | (unsigned(rom[rp + 1u]) << 16u) |
                                   (unsigned(rom[rp + 2u]) << 8u) | rom[rp + 3u];
    unsigned signature = 0, size = 0;
    std::uint16_t source_type = 0, bank = 0;
    if (!valid_guest_range(base, expected_size) || !read_u32(m, base, signature) ||
        signature != 0x41u || !read_u32(m, base + 4u, size) || size != expected_size ||
        !read_u16(m, source + 2u, source_type) || source_type != 0x13u ||
        !read_u16(m, source + 0x12u, bank) || bank != 2u)
        return;
    for (unsigned i = 0; i < c.assets.textures.size(); ++i) {
        const auto &t = c.assets.textures[i];
        if (t.model_index != model_index || !(t.flags & 4u) || t.frame_count < 2u ||
            t.model_local_offset > size || t.raw_size > size - t.model_local_offset)
            continue;
        const unsigned address = base + t.model_local_offset;
        unsigned ts = 0, bytes = 0, stride = 0;
        std::uint16_t flags = 0, count = 0, frame = 0;
        float duration = 0, elapsed = 0;
        if (!read_u32(m, address, ts) || (ts != 0x16u && ts != 0x17u) ||
            !read_u32(m, address + 4u, bytes) || bytes != t.raw_size ||
            !read_u16(m, address + 0x24u, flags) || flags != t.flags ||
            !read_u16(m, address + 0x20u, count) || count != t.frame_count ||
            !read_u32(m, address + 0x3cu, stride) || stride != t.frame_stride ||
            !read_float(m, address + 0x28u, duration) || duration != t.frame_duration_ms ||
            !read_u16(m, address + 0x34u, frame) || frame >= count ||
            !read_float(m, address + 0x38u, elapsed) || !std::isfinite(elapsed) || elapsed < 0 ||
            elapsed >= duration + 100.0f)
            continue;
        c.animation[i] = {elapsed, frame, epoch, true};
        ++c.stats.texture_syncs;
    }
}
extern "C" void rr64_world_objects_draw(unsigned char *m) {
    using namespace rr64::world;
    using namespace rr64::engine;
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (!c.drawing || c.mapping != m || !rr64_world_distance_enabled() ||
        !(rr64_draw_distance_enabled() && rr64::world::static_scene(m)))
        return;
    c.drawing = false;
    unsigned slot = 0, gfx = 0, epoch = 0, pointer = 0, camera_index = 0;
    ObjectMatrix view{}, projection{};
    std::array<float, 3> camera{}, sector{};
    std::array<float, 2> eye{};
    if (!context(m, slot, gfx, epoch, pointer, camera_index, view, projection, camera, sector,
                 eye)) {
        ++c.stats.refusals;
        return;
    }
    const auto course = course_frame().read(m, epoch);
    auto &f = c.frames[gfx];
    if (f.issued[camera_index] && f.epoch[camera_index] == epoch) {
        ++c.stats.refusals;
        return;
    }
    const WorldFrustum frustum(view, projection);
    const unsigned commands = f.commands + camera_index * frame_bytes;
    const unsigned matrices = f.matrices + camera_index * frame_bytes;
    unsigned dl = commands, n = 0, triangles = 0;
    for (unsigned op : {0x19u, 0x1bu, 0x29u, 0x1du, 0x1fu, 0x21u, 0x23u, 0x25u, 0x27u})
        command(m, dl, 0x64000000u | op, 0);
    std::uint16_t norm = 0;
    read_u16(m, 0x800B73F0u + camera_index * 12u + slot * 2u, norm);
    command(m, dl, 0xdb0e0000u, norm);
    command(m, dl, 0xda380007u, 0x800B6668u + camera_index * 0x180u + slot * 64u);
    command(m, dl, 0xda380005u, 0x800B6EE8u + camera_index * 0x180u + slot * 64u);
    for (unsigned i = 0; i < c.assets.placements.size(); ++i) {
        if (c.stock[i])
            continue;
        const auto &p = c.assets.placements[i];
        const auto &model = c.assets.models[p.model_index];
        if ((p.cell_index >= course.size() && rr64_draw_distance_percent() < 100) ||
            (p.cell_index < course.size() && !course[p.cell_index]))
            continue;
        if (!model.vertices || !model.triangles)
            continue;
        ObjectMatrix root{};
        if (!object_matrix(p, camera, sector, eye, root) ||
            !frustum.intersects(model.minimum, model.maximum, root))
            continue;
        if (dl - commands + 160u > command_bytes ||
            std::uint64_t(n + 1u) * 64u > frame_bytes - command_bytes) {
            ++c.stats.refusals;
            return;
        }
        const unsigned address = matrices + n * 64u;
        for (unsigned k = 0; k < 16; ++k)
            word(m, address + k * 4u, std::bit_cast<unsigned>(root[k]));
        command(m, dl, 0x6400000cu, object_id_base + camera_index * 0x2000u + i);
        command(m, dl, object_group_flags, 0u);
        command(m, dl, 0x64000030u, 2u);
        command(m, dl, 0, address);
        command(m, dl, 0xde000000u, f.base + model.display_list_offset);
        command(m, dl, 0xd8380002u, 64u);
        command(m, dl, 0x6400000du, 1u);
        ++n;
        triangles += model.triangles;
    }
    c.stats.visible_placements = n;
    c.stats.drawn_triangles = triangles;
    if (!n)
        return;
    command(m, dl, 0xe7000000u, 0);
    for (unsigned op : {0x28u, 0x26u, 0x24u, 0x22u, 0x20u, 0x1eu, 0x2au, 0x1cu, 0x1au})
        command(m, dl, 0x64000000u | op, 0);
    command(m, dl, 0x6400002cu, 0);
    command(m, dl, 0xe0525464u, 0x20000000u);
    command(m, dl, 0xdf000000u, 0);
    animate(c, m, epoch, f);
    unsigned bridge = pointer;
    command(m, bridge, 0xe7000000u, 0);
    command(m, bridge, 0xe0525464u, 0x10000064u);
    command(m, bridge, 0x6400002cu, 1);
    command(m, bridge, 0xde000000u, commands);
    command(m, bridge, 0xe7000000u, 0);
    word(m, 0x800AC650u, bridge);
    // Texture tiles have no RT64 stack. Force the next stock material/root to
    // emit its complete state after this independent presentation list.
    write_u32(m, 0x8009DB2Cu, 0u);
    write_u32(m, 0x800B1A20u, 0xffffffffu);
    write_u32(m, 0x8009DBECu, 0xffffffffu);
    f.issued[camera_index] = true;
    f.epoch[camera_index] = epoch;
    ++c.stats.frames;
}
