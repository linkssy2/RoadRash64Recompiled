#include "rr64_course_hazard_render.hpp"
#include "rr64_course_hazard_state.hpp"
#include "rr64_actor_pose.hpp"
#include "rr64_world_frustum.hpp"
#include "librecomp/addresses.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace rr64::course_hazards {
namespace {
using namespace rr64::engine;
using Matrix = std::array<float, 16>;
constexpr unsigned command_capacity = 16384, matrix_capacity = 8192, view_bytes = 24576;
constexpr unsigned guard = 0x48417afeu;
static_assert(maximum_render_hazards * 64u == matrix_capacity);
struct Model {
    HazardModelBounds bounds;
    unsigned display_list = 0;
};
struct Relocation {
    unsigned word, target;
};
struct Frame {
    unsigned base = 0;
    std::array<unsigned, 4> epoch{};
    std::array<bool, 4> issued{};
};
struct Cache {
    std::vector<std::uint8_t> bytes;
    std::vector<Model> models;
    std::vector<Relocation> relocations;
    unsigned char *mapping = nullptr;
    std::array<Frame, 2> frames{};
    HazardRenderStatistics stats;
    std::mutex mutex;
};
Cache &cache() {
    static Cache c;
    return c;
}
void word(unsigned char *rdram, unsigned address, unsigned value) {
    MEM_W(0, guest_address(address)) = value;
}
unsigned word(unsigned char *rdram, unsigned address) {
    return unsigned(MEM_W(0, guest_address(address)));
}
struct Writer {
    unsigned char *m;
    unsigned p, end;
    bool good = true;
    void command(unsigned a, unsigned b) {
        if (!good || end - p < 8) {
            good = false;
            return;
        }
        word(m, p, a);
        word(m, p + 4, b);
        p += 8;
    }
};
struct Reader {
    std::span<const std::uint8_t> data;
    size_t p = 0;
    bool good = true;
    unsigned u32() {
        if (!good || data.size() - p < 4) {
            good = false;
            return 0;
        }
        unsigned v = 0;
        for (unsigned n = 0; n < 4; ++n)
            v = (v << 8) | data[p++];
        return v;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    std::span<const std::uint8_t> take(size_t n) {
        if (!good || n > data.size() - p) {
            good = false;
            return {};
        }
        auto v = data.subspan(p, n);
        p += n;
        return v;
    }
};
void append_word(std::vector<std::uint8_t> &out, unsigned value) {
    for (int s = 24; s >= 0; s -= 8)
        out.push_back(std::uint8_t(value >> s));
}
unsigned append_bytes(std::vector<std::uint8_t> &out, std::span<const std::uint8_t> bytes) {
    while (out.size() & 7)
        out.push_back(0);
    const auto p = unsigned(out.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
    return p;
}
bool compile(std::span<const std::uint8_t> input, std::vector<std::uint8_t> &bytes,
             std::vector<Model> &models, std::vector<Relocation> &relocations, std::string &error) {
    const auto bad = [&](const char *message) {
        error = message;
        return false;
    };
    if (input.size() < 12 || input.size() > 32u * 1024u * 1024u ||
        std::memcmp(input.data(), "MKHZ0001", 8))
        return bad("Invalid hazard model asset");
    Reader r{input, 8};
    const auto count = r.u32();
    if (!count || count > netplay::kMaximumCourseHazardModels)
        return bad("Invalid hazard model count");
    for (unsigned index = 0; index < count; ++index) {
        const auto record_size = r.u32();
        if (!r.good || record_size > input.size() - r.p)
            return bad("Hazard model record bounds");
        const auto end = r.p + record_size;
        Model model{};
        if (r.u32() != index)
            return bad("Hazard model identity ordering");
        model.bounds.source_scale = r.f32();
        const auto batches = r.u32();
        if (!std::isfinite(model.bounds.source_scale) || model.bounds.source_scale <= 0 ||
            model.bounds.source_scale > 10 || !batches || batches > 512)
            return bad("Hazard model scale/batches");
        for (auto &v : model.bounds.minimum)
            v = r.f32();
        for (auto &v : model.bounds.maximum)
            v = r.f32();
        for (unsigned a = 0; a < 3; ++a)
            if (!std::isfinite(model.bounds.minimum[a]) ||
                !std::isfinite(model.bounds.maximum[a]) ||
                model.bounds.minimum[a] > model.bounds.maximum[a] ||
                std::abs(model.bounds.minimum[a]) > 32768 ||
                std::abs(model.bounds.maximum[a]) > 32768)
                return bad("Hazard model bounds");
        std::vector<std::array<unsigned, 2>> dl;
        std::vector<unsigned> pointer_words;
        const auto command = [&](unsigned a, unsigned b) { dl.push_back({a, b}); };
        const auto pointer = [&](unsigned a, unsigned b) {
            pointer_words.push_back(unsigned(dl.size()) * 8u + 4u);
            command(a, b);
        };
        for (unsigned group = 0; group < batches; ++group) {
            const unsigned flags = r.u32(), width = r.u32(), height = r.u32(), triangles = r.u32();
            if (!r.good || (flags & ~65535u) || std::popcount(flags & 56832u) > 1 || !triangles ||
                triangles > 16384 || model.bounds.triangles > 16384 - triangles)
                return bad("Hazard material/triangle bounds");
            const bool textured = (flags & 8) != 0;
            if (textured
                    ? (width < 4 || !height || width > 64 || height > 64 || (width & (width - 1)) ||
                       (height & (height - 1)) || width * height > 2048)
                    : (width || height || (flags & ~7u)))
                return bad("Hazard texture dimensions");
            const auto vertices = r.take(size_t(triangles) * 48u),
                       pixels = r.take(size_t(width) * height * 2u);
            if (!r.good || r.p > end)
                return bad("Hazard vertex/texture truncation");
            for (size_t p = 0; p < vertices.size(); p += 16) {
                for (unsigned a = 0; a < 3; ++a) {
                    const int value = std::int16_t((unsigned(vertices[p + a * 2]) << 8) |
                                                   vertices[p + a * 2 + 1]);
                    if (value < model.bounds.minimum[a] || value > model.bounds.maximum[a])
                        return bad("Vertex outside certified model bounds");
                }
                if (vertices[p + 6] || vertices[p + 7])
                    return bad("Unsupported hazard vertex flags");
            }
            const unsigned vertex_base = append_bytes(bytes, vertices),
                           texture_base = append_bytes(bytes, pixels);
            command(0xe7000000, 0);
            command(0xd9000000, 0x00200005u | ((flags & 1) ? 0x400u : 0));
            command(0xe300001f, 0x00082c00);
            command(0xe200001f, (flags & 8192) ? 0x00504dd8u
                                : (flags & 4)  ? 0x005049d8u
                                : (flags & 2)  ? 0x00553078u
                                               : 0x00552078u);
            if (textured) {
                command(0xd7000002, 0xffffffff);
                // Preserve source alpha choice: texture*shade, shade only,
                // or texture only. Values verified against pinned GBI macros.
                if (flags & 32768u)
                    command(0xfc309661, 0x552eff7f); // Source ENV->PRIM smoke/fire.
                else if (flags & 16384u)
                    command(0xfcff97ff, 0xff2dfeff); // Source primitive color/texture alpha.
                else if (flags & 4096u)
                    command(0xfc119623, 0xff2fffff); // MODULATEIA_PRIM
                else if (flags & 2048u)
                    command(0xfc147e28, 0x44fe793c); // BLENDRGBA (source GBI golden).
                else if (flags & 512u)
                    command(0xfc127e24, 0xfffff9fc); // MODULATEI
                else if (flags & 1024u)
                    command(0xfc127e24, 0xfffff3f9); // MODULATEIDECALA
                else
                    command(0xfc121824, 0xff33ffff); // MODULATEIA
                const unsigned format = (flags & 16) ? 3u : 0u, line = width / 4u;
                unsigned ms = 0, mt = 0;
                while ((1u << ms) < width)
                    ++ms;
                while ((1u << mt) < height)
                    ++mt;
                const unsigned cms = ((flags & 32) ? 1u : 0u) | ((flags & 64) ? 2u : 0u),
                               cmt = ((flags & 128) ? 1u : 0u) | ((flags & 256) ? 2u : 0u);
                pointer(0xfd100000u | (format << 21), texture_base);
                command(0xe8000000, 0);
                command(0xf5100000u | (format << 21), 0x07000000);
                command(0xe6000000, 0);
                command(0xf3000000,
                        0x07000000u | ((width * height - 1u) << 12) | ((2048u + line - 1u) / line));
                command(0xe7000000, 0);
                command(0xf5100000u | (format << 21) | (line << 9),
                        (cmt << 18) | (mt << 14) | (cms << 8) | (ms << 4));
                command(0xf2000000, ((width - 1u) * 4u << 12) | ((height - 1u) * 4u));
            } else {
                command(0xd7000000, 0x00010001);
                command(0xfcffffff, 0xfffe793c);
            }
            for (unsigned first = 0; first < triangles;) {
                const unsigned n = std::min(10u, triangles - first), verts = n * 3u;
                pointer(0x01000000u | (verts << 12) | (verts << 1), vertex_base + first * 48u);
                for (unsigned t = 0; t < n; ++t) {
                    const unsigned v = t * 3u;
                    command(0x05000000u | (v << 17) | ((v + 1) << 9) | ((v + 2) << 1), 0);
                }
                first += n;
            }
            model.bounds.triangles += triangles;
        }
        if (!r.good || r.p != end)
            return bad("Hazard model length mismatch");
        command(0xdf000000, 0);
        while (bytes.size() & 7)
            bytes.push_back(0);
        model.display_list = unsigned(bytes.size());
        for (const auto &v : dl) {
            append_word(bytes, v[0]);
            append_word(bytes, v[1]);
        }
        for (unsigned p : pointer_words) {
            const auto &v = dl[p / 8u];
            relocations.push_back({model.display_list + p, v[1]});
        }
        models.push_back(model);
        if (bytes.size() > 64u * 1024u * 1024u)
            return bad("Compiled hazard model budget");
    }
    if (!r.good || r.p != input.size())
        return bad("Trailing hazard model data");
    while (bytes.size() & 63u)
        bytes.push_back(0);
    error.clear();
    return true;
}
bool frame_ready(Cache &c, unsigned char *m, unsigned gfx) {
    if (c.mapping && c.mapping != m)
        return false;
    const unsigned size = unsigned(c.bytes.size()) + 4u * view_bytes + 16u;
    auto &f = c.frames[gfx];
    if (f.base) {
        for (unsigned i = 0; i < 4; ++i)
            if (word(m, f.base + size - 16u + i * 4u) != guard)
                return false;
        return true;
    }
    auto *host = static_cast<unsigned char *>(recomp::alloc(m, size));
    if (!host)
        return false;
    const auto offset = host - m;
    if (offset < 0x800000 || (offset & 7) || std::uint64_t(offset) + size > recomp::mem_size) {
        recomp::free(m, host);
        return false;
    }
    f.base = 0x80000000u + unsigned(offset);
    c.mapping = m;
    for (unsigned i = 0; i < c.bytes.size(); ++i)
        m[(unsigned(offset) + i) ^ 3u] = c.bytes[i];
    for (const auto &r : c.relocations)
        word(m, f.base + r.word, f.base + r.target);
    for (unsigned i = 0; i < 4; ++i)
        word(m, f.base + size - 16u + i * 4u, guard);
    return true;
}
struct Context {
    unsigned slot = 0, gfx = 0, epoch = 0, pointer = 0, view = 0;
    std::uint16_t normalize = 0;
    std::array<float, 3> camera{}, sector{};
    Matrix projection{}, modelview{};
};
bool context(unsigned char *m, Context &c) {
    unsigned base = 0, active = 0, count = 0, views = 0, scale = 0, width = 0;
    if (!read_u32(m, globals::active_viewport, c.view) || c.view >= 4u ||
        !read_u32(m, 0x8009DB88u, views) || views < 1u || views > 4u || c.view >= views ||
        !read_u32(m, globals::terrain_map_width, width) || width != 70u ||
        !read_u32(m, globals::actor_render_buffer_slot, c.slot) || c.slot > 1u ||
        !read_u32(m, 0x8009CBA4u, c.gfx) || c.gfx > 1u || !read_u32(m, 0x800A1830u, c.epoch) ||
        !read_u32(m, 0x800AC650u, c.pointer) || !read_u32(m, 0x800AC658u + c.gfx * 4u, base) ||
        !read_u32(m, 0x8009CB90u, active) || base != active || !read_u32(m, 0x800BC9A0u, count) ||
        (count != 0x4650u && count != 0x36b0u) || !read_u32(m, 0x8009DBB4u, scale) ||
        scale != std::bit_cast<unsigned>(10.f) ||
        !read_u16(m, 0x800B73F0u + c.view * 12u + c.slot * 2u, c.normalize) || !c.normalize)
        return false;
    const unsigned size = 0x140u + count * 8u;
    if (!valid_guest_range(base, size) || c.pointer < base + 0x148u ||
        c.pointer > base + size - 1064u || (c.pointer & 7u))
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!read_float(m, 0x800D69F8u + i * 4u, c.camera[i]) || !std::isfinite(c.camera[i]) ||
            !read_float(m, 0x800A4FDCu + i * 4u, c.sector[i]) || !std::isfinite(c.sector[i]))
            return false;
    Matrix4x4Snapshot p{}, v{};
    if (!decode_n64_matrix(m, 0x800B6668u + c.view * 0x180u + c.slot * 64u, p) ||
        !decode_n64_matrix(m, 0x800B6EE8u + c.view * 0x180u + c.slot * 64u, v))
        return false;
    c.projection = p.values;
    c.modelview = v.values;
    return p.values[0] != 0 && p.values[5] != 0 && p.values[11] != 0 && v.values[15] == 1;
}
// Original mtxf_rotate_zxy_translate basis, followed by MK(x,y,z)->RR(x,-z,y).
// Native source2 model units are10 times rider-world. Pack scale only applies
// to source-local geometry; translated world positions must not be scaled twice.
Matrix root_matrix(const HazardDrawState &box, const Context &c,
                   const std::array<std::uint16_t, 3> &angle, float shrink,
                   std::array<float, 3> offset = {}) {
    constexpr float radians = 6.2831853071795864769f / 65536.f;
    const float dx = c.camera[0] + c.sector[0] - box.position[0];
    const float dy = c.camera[1] + c.sector[1] - box.position[1];
    const float x = (angle[0] & 0xfff0u) * radians,
                y = box.billboard ? std::atan2(dx, -dy) : (angle[1] & 0xfff0u) * radians,
                z = (angle[2] & 0xfff0u) * radians;
    const float sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y),
                sz = std::sin(z), cz = std::cos(z);
    Matrix m{cy * cz + sx * sy * sz,
             cx * sz,
             -sy * cz + sx * cy * sz,
             0,
             -cy * sz + sx * sy * cz,
             cx * cz,
             sy * sz + sx * cy * cz,
             0,
             cx * sy,
             -sx,
             cx * cy,
             0,
             0,
             0,
             0,
             1};
    const float model_scale = 10.f * box.source_to_world_scale * shrink;
    for (unsigned r = 0; r < 3; ++r) {
        const auto old_y = m[r * 4 + 1];
        m[r * 4] *= model_scale;
        m[r * 4 + 1] = -m[r * 4 + 2] * model_scale;
        m[r * 4 + 2] = old_y * model_scale;
    }
    if (box.billboard == 2) {
        // Inverse camera rotation: source sprite X/Y align with this view's
        // right/up vectors. Per-view matrices prevent split-screen cross-talk.
        for (unsigned axis = 0; axis < 3; ++axis) {
            m[axis] = c.modelview[axis * 4] * model_scale;
            m[4 + axis] = c.modelview[axis * 4 + 1] * model_scale;
            m[8 + axis] = c.modelview[axis * 4 + 2] * model_scale;
        }
    }
    const std::array<float, 3> delta{offset[0] * box.source_to_world_scale,
                                     -offset[2] * box.source_to_world_scale,
                                     offset[1] * box.source_to_world_scale};
    for (unsigned i = 0; i < 3; ++i)
        m[12 + i] = ((box.position[i] + delta[i]) - c.camera[i] - c.sector[i]) * 10.f;
    return m;
}
} // namespace
bool install_hazard_asset(std::span<const std::uint8_t> input, std::string &error) {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (c.mapping) {
        error = "Hazard installation requires session reset";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    std::vector<Model> models;
    std::vector<Relocation> relocations;
    if (!compile(input, bytes, models, relocations, error))
        return false;
    c.bytes = std::move(bytes);
    c.models = std::move(models);
    c.relocations = std::move(relocations);
    return true;
}
void reset_hazard_render_session() {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    c.mapping = nullptr;
    c.frames = {};
    c.stats = {};
}
void clear_hazard_asset() {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (!c.mapping) {
        c.bytes.clear();
        c.models.clear();
        c.relocations.clear();
    }
}
bool hazard_model_bounds(unsigned model, HazardModelBounds &out) {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (model >= c.models.size())
        return false;
    out = c.models[model].bounds;
    return true;
}
HazardRenderStatistics hazard_render_statistics() {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    return c.stats;
}
bool draw_hazards(unsigned char *m, std::span<const HazardDrawState> states) {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (c.models.empty() || states.empty())
        return false;
    const auto fail = [&] {
        ++c.stats.refusals;
        return false;
    };
    if (!m || states.size() > maximum_render_hazards)
        return fail();
    std::array<bool, maximum_render_hazards> ids{};
    for (const auto &s : states) {
        if (s.id >= ids.size() || ids[s.id] || s.model >= c.models.size() || s.billboard > 2 ||
            s.opacity > 255 || s.tint > 0xffffff || s.environment_tint > 0xffffff ||
            !std::isfinite(s.scale) || s.scale <= 0 || s.scale > 64 ||
            !std::isfinite(s.source_to_world_scale) || s.source_to_world_scale <= 0 ||
            s.source_to_world_scale > 1)
            return fail();
        ids[s.id] = true;
        for (float p : s.position)
            if (!std::isfinite(p) || std::abs(p) > 100000.f)
                return fail();
    }
    Context ctx{};
    if (!context(m, ctx))
        return fail();
    auto &f = c.frames[ctx.gfx];
    if (f.issued[ctx.view] && f.epoch[ctx.view] == ctx.epoch)
        return fail();
    if (!frame_ready(c, m, ctx.gfx))
        return fail();
    c.stats.visible = c.stats.triangles = c.stats.command_bytes = c.stats.matrix_bytes = 0;
    const unsigned start = f.base + unsigned(c.bytes.size()) + ctx.view * view_bytes,
                   matrices = start + command_capacity;
    Writer w{m, start, matrices};
    unsigned count = 0, triangles = 0;
    rr64::world::WorldFrustum frustum(ctx.modelview, ctx.projection);
    for (unsigned op : {0x19u, 0x1bu, 0x29u, 0x1du, 0x1fu, 0x21u, 0x23u, 0x25u, 0x27u})
        w.command(0x64000000u | op, 0);
    w.command(0xdb0e0000, ctx.normalize);
    w.command(0xda380007, 0x800B6668u + ctx.view * 0x180u + ctx.slot * 64u);
    w.command(0xda380005, 0x800B6EE8u + ctx.view * 0x180u + ctx.slot * 64u);
    for (const auto &s : states) {
        if (!s.visible || !s.opacity)
            continue;
        const auto &model = c.models[s.model];
        const auto matrix = root_matrix(s, ctx, s.rotation, s.scale * model.bounds.source_scale);
        if (!frustum.intersects(model.bounds.minimum, model.bounds.maximum, matrix))
            continue;
        if ((count + 1u) * 64u > matrix_capacity)
            return fail();
        const unsigned address = matrices + count++ * 64u;
        for (unsigned i = 0; i < 16; ++i)
            word(m, address + i * 4u, std::bit_cast<unsigned>(matrix[i]));
        // Model identity separates unlike shapes. Six face frames share shape
        // but using separate groups also avoids stale texture interpolation.
        w.command(0x6400000c, 0x52600000u + ctx.view * 0x40000u + s.id * 2048u + s.model);
        w.command(0x02011555u, 0);
        w.command(0x64000030u, 2);
        w.command(0, address);
        w.command(0xfa000000u, (s.tint << 8) | s.opacity);
        w.command(0xfb000000u, (s.environment_tint << 8) | 0xffu);
        w.command(0xde000000u, f.base + model.display_list);
        w.command(0xd8380002u, 64);
        w.command(0x6400000du, 1);
        triangles += model.bounds.triangles;
    }
    if (!count)
        return false;
    w.command(0xe7000000, 0);
    w.command(0xd7000002, 0x80008000);
    for (unsigned op : {0x28u, 0x26u, 0x24u, 0x22u, 0x20u, 0x1eu, 0x2au, 0x1cu, 0x1au})
        w.command(0x64000000u | op, 0);
    w.command(0x6400002c, 0);
    w.command(0xe0525464, 0x20000000);
    w.command(0xdf000000, 0);
    if (!w.good)
        return fail();
    Writer bridge{m, ctx.pointer, ctx.pointer + 40};
    bridge.command(0xe7000000, 0);
    bridge.command(0xe0525464, 0x10000064);
    bridge.command(0x6400002c, 1);
    bridge.command(0xde000000, start);
    bridge.command(0xe7000000, 0);
    word(m, 0x800AC650, bridge.p);
    write_u32(m, 0x8009DB2C, 0);
    write_u32(m, 0x800B1A20, 0xffffffff);
    write_u32(m, 0x8009DBEC, 0xffffffff);
    f.issued[ctx.view] = true;
    f.epoch[ctx.view] = ctx.epoch;
    ++c.stats.draws;
    c.stats.visible = count;
    c.stats.triangles = triangles;
    c.stats.command_bytes = w.p - start;
    c.stats.matrix_bytes = count * 64u;
    return true;
}
} // namespace rr64::course_hazards
