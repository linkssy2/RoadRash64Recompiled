#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#endif
#include "rr64_world_packet_compiler.hpp"
#include "rr64_world_object_assets.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "recomp.h"

extern "C" void func_8000F958(std::uint8_t *, recomp_context *);
extern "C" void func_80015A90(std::uint8_t *, recomp_context *);
extern "C" void guMtxF2L(std::uint8_t *, recomp_context *);

namespace rr64::world {
namespace {
constexpr std::size_t model_table = 0x101d440u, placement_table = 0x1265c40u;
constexpr std::size_t descriptor_table = 0x1303200u, descriptor_end = 0x1304db0u;
constexpr std::size_t model_count = 373u, cell_count = 4900u, descriptor_count = 295u;
constexpr std::size_t maximum_blob = 16u * 1024u * 1024u;
constexpr std::uint32_t scratch_model = 0x80100000u, scratch_commands = 0x80300000u;
constexpr std::uint32_t scratch_pose = 0x80400000u, scratch_matrix = 0x80400100u;
constexpr std::uint32_t scratch_packed = 0x80400200u, scratch_stack = 0x807ff000u;
constexpr std::size_t maximum_model = 1024u * 1024u, maximum_commands = 65536u;
using Matrix = std::array<float, 16>;

void require(bool condition, const char *text) {
    if (!condition) {
        throw std::runtime_error(text);
    }
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    void range(std::size_t p, std::size_t n) const {
        require(p <= bytes.size() && n <= bytes.size() - p,
                "object reference exceeds containing record");
    }
    std::uint16_t half(std::size_t p) const {
        range(p, 2);
        return std::uint16_t((std::uint32_t(bytes[p]) << 8u) | bytes[p + 1]);
    }
    std::uint32_t word(std::size_t p) const {
        range(p, 4);
        return (std::uint32_t(bytes[p]) << 24u) | (std::uint32_t(bytes[p + 1]) << 16u) |
               (std::uint32_t(bytes[p + 2]) << 8u) | bytes[p + 3];
    }
    float number(std::size_t p) const {
        const auto n = std::bit_cast<float>(word(p));
        require(std::isfinite(n), "nonfinite object transform");
        return n;
    }
    Reader record(std::size_t p, std::size_t n) const {
        range(p, n);
        return {bytes.subspan(p, n)};
    }
    std::size_t relative(std::size_t p) const {
        const auto d = word(p);
        require(d != 0u && d <= bytes.size() - p, "invalid object relative pointer");
        return p + d;
    }
};
void put(std::vector<std::uint8_t> &m, std::uint32_t p, std::uint32_t w) {
    std::memcpy(m.data() + (p & 0x7fffffu), &w, 4);
}
std::uint32_t get(const std::vector<std::uint8_t> &m, std::uint32_t p) {
    std::uint32_t w;
    std::memcpy(&w, m.data() + (p & 0x7fffffu), 4);
    return w;
}
void copy(std::vector<std::uint8_t> &m, std::uint32_t p, std::span<const std::uint8_t> bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        m[((p & 0x7fffffu) + i) ^ 3u] = bytes[i];
    }
}
std::uint32_t append(ObjectAssets &out, std::span<const std::uint8_t> bytes) {
    const auto p = (out.bytes.size() + 7u) & ~std::size_t(7u);
    require(p <= maximum_blob && bytes.size() <= maximum_blob - p,
            "object cache exceeds fixed upload budget");
    out.bytes.resize(p, 0);
    out.bytes.insert(out.bytes.end(), bytes.begin(), bytes.end());
    return std::uint32_t(p);
}
void word(std::vector<std::uint8_t> &bytes, std::uint32_t w) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(std::uint8_t(w >> shift));
    }
}
void command(std::vector<std::uint8_t> &bytes, std::uint32_t a, std::uint32_t b) {
    word(bytes, a);
    word(bytes, b);
}
gpr guest(std::uint32_t p) {
    return static_cast<gpr>(static_cast<std::int32_t>(p));
}
Matrix identity() {
    Matrix m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}
Matrix product(const Matrix &a, const Matrix &b) {
    Matrix c{};
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t col = 0; col < 4; ++col)
            for (std::size_t k = 0; k < 4; ++k) {
                c[r * 4 + col] += a[r * 4 + k] * b[k * 4 + col];
            }
    return c;
}
void quaternion(const Reader &r, std::size_t p) {
    float norm = 0.0f;
    for (std::size_t k = 0; k < 4; ++k) {
        const auto v = r.number(p + k * 4u);
        norm += v * v;
    }
    require(std::abs(norm - 1.0f) < 0.002f, "object quaternion is not normalized");
}

struct ModelBuilder {
    ObjectAssets &out;
    const Reader model;
    std::vector<std::uint8_t> &memory;
    ObjectModelAsset metadata;
    std::uint32_t upload;
    std::vector<std::uint8_t> commands;
    std::vector<ObjectAssetRelocation> relocations;
    std::unordered_map<std::size_t, std::uint32_t> textures;
    std::unordered_set<std::size_t> visited;
    bool batch_triangles = false;

    std::uint32_t texture(std::size_t p) {
        if (const auto i = textures.find(p); i != textures.end()) {
            return i->second;
        }
        model.range(p, 0x40u);
        const auto size = model.word(p + 4u);
        model.range(p, size);
        const auto type = model.word(p), width = model.word(p + 0x2cu),
                   height = model.word(p + 0x30u);
        const auto bits = model.word(p + 0x34u), stride = model.word(p + 0x3cu);
        const auto frames = model.half(p + 0x20u), flags = model.half(p + 0x24u),
                   pixel_end = model.half(p + 0x26u);
        require((type == 0x16u || type == 0x17u) && size >= 0x40u && size <= 131072u &&
                    width > 0u && width <= 256u && height > 0u && height <= 256u &&
                    (bits == 4u || bits == 8u) && frames >= 1u && frames <= 64u && stride > 0u,
                "invalid embedded object texture header");
        const auto pixels = (width * height * bits + 7u) / 8u;
        const auto palette = (flags & 0x8000u) ? 0u : (bits == 4u ? 32u : 512u);
        require(pixel_end >= pixels && std::uint64_t(pixel_end) + palette <= stride &&
                    0x40ull + std::uint64_t(stride) * frames <= size,
                "object texture frame exceeds record");
        const auto duration = (flags & 4u) ? model.number(p + 0x28u) : 0.0f;
        require(!(flags & 4u) || duration > 0.0f, "invalid animated object texture duration");
        const auto index = std::uint32_t(out.textures.size());
        out.textures.push_back({upload + std::uint32_t(p), size, stride, frames, flags, duration,
                                std::uint32_t(out.models.size()), std::uint32_t(p)});
        textures.emplace(p, index);
        // F594 advances only the private texture clock. Zero time means its
        // original writer emits frame zero, which remains relocatable later.
        if (flags & 4u) {
            put(memory, scratch_model + std::uint32_t(p) + 0x34u, bits & 0xffffu);
            put(memory, scratch_model + std::uint32_t(p) + 0x38u, 0u);
        }
        return index;
    }

    void mesh(std::size_t p, const Matrix &accumulated) {
        model.range(p, 0x28u);
        const auto mesh_size = model.word(p + 4u);
        model.range(p, mesh_size);
        const auto packet_count = model.half(p + 0xcu), back = model.half(p + 0xeu);
        std::uint32_t texture_index = 0xffffffffu;
        if (back != 0xffffu) {
            require(std::size_t(back) * 8u <= p, "object texture back pointer underflows");
            texture_index = texture(p - std::size_t(back) * 8u);
        }
        std::size_t packet = p + 0x28u;
        for (std::size_t i = 0; i < packet_count; ++i) {
            model.range(packet, 8u);
            const auto triangles = model.half(packet), vertices = model.half(packet + 2u);
            const auto size = model.half(packet + 4u), vertex_bytes = model.half(packet + 6u);
            require(vertices > 0u && vertices <= 32u && size >= 8u &&
                        vertex_bytes >= vertices * 16u &&
                        8u + std::size_t(vertex_bytes) + triangles * 2u <= size,
                    "invalid object geometry packet");
            require(packet <= p + mesh_size && size <= p + mesh_size - packet,
                    "object packet exceeds mesh");
            for (std::size_t t = 0; t < triangles; ++t) {
                const auto v = model.half(packet + 8u + vertex_bytes + t * 2u);
                require(((v >> 10u) & 31u) < vertices && ((v >> 5u) & 31u) < vertices &&
                            (v & 31u) < vertices,
                        "object triangle references a missing vertex");
            }
            for (std::size_t v = 0; v < vertices; ++v) {
                std::array<float, 3> point{};
                for (std::size_t k = 0; k < 3; ++k)
                    point[k] =
                        std::bit_cast<std::int16_t>(model.half(packet + 8u + v * 16u + k * 2u));
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto n = point[0] * accumulated[k] + point[1] * accumulated[4 + k] +
                                   point[2] * accumulated[8 + k] + accumulated[12 + k];
                    require(std::isfinite(n), "object transformed bound is nonfinite");
                    metadata.minimum[k] = std::min(metadata.minimum[k], n);
                    metadata.maximum[k] = std::max(metadata.maximum[k], n);
                }
            }
            metadata.vertices += vertices;
            metadata.triangles += triangles;
            ++out.packets;
            packet += size;
        }
        put(memory, 0x800ac650u, scratch_commands);
        put(memory, 0x8009db2cu, 0u);
        put(memory, 0x800b1a20u, 0xffffffffu);
        put(memory, 0x8009db20u, 0u);
        put(memory, 0x800b1a1cu, 0u);
        std::fill(memory.begin() + (scratch_commands & 0x7fffffu),
                  memory.begin() + (scratch_commands & 0x7fffffu) + maximum_commands + 64u, 0xa5u);
        recomp_context ctx{};
        ctx.r4 = guest(scratch_model + std::uint32_t(p));
        ctx.r5 = 0;
        ctx.r29 = guest(scratch_stack);
        ctx.f_odd = &ctx.f0.u32h;
        func_8000F958(memory.data(), &ctx);
        const auto end = get(memory, 0x800ac650u);
        require(end >= scratch_commands && end <= scratch_commands + maximum_commands &&
                    ((end - scratch_commands) & 7u) == 0u,
                "object original material writer exceeds command budget");
        require(
            std::all_of(memory.begin() + (scratch_commands & 0x7fffffu) + maximum_commands,
                        memory.begin() + (scratch_commands & 0x7fffffu) + maximum_commands + 64u,
                        [](auto b) { return b == 0xa5u; }),
            "object command guard was modified");
        for (auto c = scratch_commands; c < end; c += 8u) {
            const auto a = get(memory, c);
            auto b = get(memory, c + 4u);
            const auto opcode = a >> 24u;
            if (batch_triangles && (opcode == 0x05u || opcode == 0x06u) &&
                (c == scratch_commands || (get(memory, c - 8u) >> 24u) < 0x05u ||
                 (get(memory, c - 8u) >> 24u) > 0x06u)) {
                std::uint32_t count = 0u;
                for (auto next = c; next < end; next += 8u) {
                    const auto next_opcode = get(memory, next) >> 24u;
                    if (next_opcode != 0x05u && next_opcode != 0x06u) {
                        break;
                    }
                    ++count;
                }
                require(count <= 32768u, "object triangle batch exceeds packet bound");
                command(commands, 0x64000034u, count);
            }
            if (opcode == 0x01u || opcode == 0xfdu) {
                const auto physical = b & 0x1fffffffu;
                require(physical >= (scratch_model & 0x1fffffffu) &&
                            physical - (scratch_model & 0x1fffffffu) < model.bytes.size(),
                        "object writer emitted external asset pointer");
                const auto offset = physical - (scratch_model & 0x1fffffffu);
                auto animated = 0xffffffffu;
                if (opcode == 0xfdu && texture_index != 0xffffffffu) {
                    const auto &t = out.textures[texture_index];
                    require(upload + offset >= t.raw_offset + 0x40u &&
                                upload + offset < t.raw_offset + t.raw_size,
                            "object texture pointer outside its embedded texture");
                    if (t.frame_count > 1u && (t.flags & 4u)) {
                        animated = texture_index;
                    }
                }
                relocations.push_back(
                    {std::uint32_t(commands.size() + 4u), upload + offset, animated});
                b = 0u;
            }
            command(commands, a, b);
        }
    }

    void walk(std::size_t p, const Matrix &parent, std::size_t depth = 0u) {
        require(depth <= 16u && visited.size() < 128u && visited.insert(p).second,
                "object graph is cyclic or exceeds record budget");
        const auto type = model.word(p);
        std::size_t children = 0u, entries = 0u;
        auto accumulated = parent;
        if (type == 0x10u) {
            mesh(p, parent);
            return;
        }
        if (type == 0x13u) {
            require(depth == 0u, "nested object root is unsupported");
            model.range(p, 0x40u);
            metadata.source_bank = model.half(p + 0x12u);
            require(metadata.source_bank == 1u || metadata.source_bank == 2u,
                    "object source bank is unsupported");
            children = model.half(p + 0xcu);
            entries = p + 0x40u;
        } else if (type == 0x11u) {
            model.range(p, 0x38u);
            children = model.half(p + 0xcu);
            entries = p + 0x38u;
        } else if (type == 0x12u) {
            model.range(p, 0x58u);
            require(model.half(p + 0x16u) == 0u, "animated object child is unsupported");
            quaternion(model, p + 0x28u);
            for (std::size_t k = 0; k < 3; ++k) {
                model.number(p + 0x38u + k * 4u);
            }
            copy(memory, scratch_pose, model.bytes.subspan(p + 0x38u, 12u));
            copy(memory, scratch_pose + 12u, model.bytes.subspan(p + 0x28u, 16u));
            recomp_context ctx{};
            ctx.r4 = guest(scratch_pose + 12u);
            ctx.r5 = guest(scratch_pose);
            ctx.r6 = guest(scratch_matrix);
            ctx.r29 = guest(scratch_stack);
            ctx.f_odd = &ctx.f0.u32h;
            func_80015A90(memory.data(), &ctx);
            Matrix local{};
            for (std::size_t k = 0; k < 16; ++k) {
                local[k] =
                    std::bit_cast<float>(get(memory, scratch_matrix + std::uint32_t(k) * 4u));
                require(std::isfinite(local[k]) && local[k] > -32768.0f && local[k] < 32768.0f,
                        "object child matrix exceeds fixed format");
            }
            ctx.r4 = guest(scratch_matrix);
            ctx.r5 = guest(scratch_packed);
            guMtxF2L(memory.data(), &ctx);
            std::array<std::uint8_t, 64> packed{};
            for (std::size_t k = 0; k < 64; ++k) {
                packed[k] = memory[((scratch_packed & 0x7fffffu) + k) ^ 3u];
            }
            // Culling must use the exact quantized child matrices emitted to
            // the GPU, rather than the pre-pack float values near a clip edge.
            const Reader packed_reader{packed};
            for (std::size_t k = 0; k < 16; ++k) {
                local[k] = float(std::bit_cast<std::int16_t>(packed_reader.half(k * 2u))) +
                           float(packed_reader.half(32u + k * 2u)) / 65536.0f;
            }
            accumulated = product(local, parent);
            const auto matrix_offset = append(out, packed);
            relocations.push_back({std::uint32_t(commands.size() + 4u), matrix_offset});
            command(commands, 0xda380000u, 0u);
            children = model.half(p + 0x10u);
            entries = p + 0x58u + 24u * model.half(p + 0x18u) + 16u * model.half(p + 0x20u);
        } else {
            throw std::runtime_error("unsupported object graph source type");
        }
        require(children <= 128u, "object graph child count exceeds budget");
        model.range(entries, children * 12u);
        for (std::size_t i = 0; i < children; ++i) {
            const auto e = entries + i * 12u;
            walk(model.relative(e), accumulated, depth + 1u);
        }
        if (type == 0x12u) {
            command(commands, 0xd8380002u, 64u);
        }
    }
};

void append_model(const Reader &rom, ObjectAssets &out, std::vector<std::uint8_t> &memory,
                  std::size_t i, bool batch_triangles) {
    require(i < model_count, "object resource index exceeds table");
    const auto entry = model_table + 0x40u + i * 12u, p = rom.relative(entry);
    const auto size = rom.word(entry + 4u);
    require(size >= 0x40u && size <= maximum_model, "object raw model exceeds budget");
    const auto raw = rom.record(p, size);
    require(raw.word(0) == 0x41u && raw.word(4) == size, "invalid raw object model header");
    const auto root = std::size_t(raw.half(0xeu)) * 8u;
    raw.range(root, 0x40u);
    copy(memory, scratch_model, raw.bytes);
    const auto upload = append(out, raw.bytes);
    ModelBuilder builder{out, raw, memory, {}, upload};
    builder.metadata.raw_rom_offset = std::uint32_t(p);
    builder.batch_triangles = batch_triangles;
    builder.metadata.root_source_offset = std::uint32_t(root);
    builder.metadata.minimum.fill(std::numeric_limits<float>::infinity());
    builder.metadata.maximum.fill(-std::numeric_limits<float>::infinity());
    builder.walk(root, identity());
    // Original entries163/250 are deliberate 192-byte empty roots. They
    // retain their indices and empty display lists; no geometry is invented.
    if (builder.metadata.vertices == 0u) {
        builder.metadata.minimum.fill(0.0f);
        builder.metadata.maximum.fill(0.0f);
    }
    command(builder.commands, 0xdf000000u, 0u);
    builder.metadata.display_list_offset = append(out, builder.commands);
    builder.metadata.display_list_size = std::uint32_t(builder.commands.size());
    for (auto r : builder.relocations) {
        r.word_offset += builder.metadata.display_list_offset;
        out.relocations.push_back(r);
    }
    out.vertices += builder.metadata.vertices;
    out.triangles += builder.metadata.triangles;
    out.models.push_back(builder.metadata);
}

void build(const Reader &rom, ObjectAssets &out, bool batch_triangles) {
    require((rom.bytes.size() == 0x2000000u
#ifdef RR64_EXPERIMENTAL_COURSE
             || (rr64::experimental_course::installed() && rom.bytes.size()>0x2000000u && rom.bytes.size()<=0x4000000u)
#endif
             ) && rom.word(0) == 0x80371240u,
            "object cache needs the supported big-endian 32MiB ROM");
    require(rom.word(model_table) == 0x40u && rom.word(model_table + 0xcu) == model_count &&
                rom.word(placement_table) == 0x48u && rom.word(placement_table + 0xcu) == 70u &&
                rom.word(descriptor_table + 4u) == descriptor_count,
            "object table dimensions differ from supported ROM");
    std::vector<std::uint8_t> memory(8u * 1024u * 1024u);
    // Original arithmetic/material constants only; no guest mapping is read.
    copy(memory, 0x80000000u, rom.bytes.subspan(0xc00u, 0xa0000u));
    put(memory, 0x80000c70u, 0u);
    out.models.reserve(model_count);
    for (std::size_t i = 0; i < model_count; ++i)
        append_model(rom, out, memory, i, batch_triangles);
    std::array<std::size_t, descriptor_count> descriptors{};
    auto p = descriptor_table + 8u;
    for (auto &d : descriptors) {
        rom.range(p, 8u);
        d = p;
        p += 8u + 16u * rom.bytes[p] + 14u * rom.bytes[p + 1u];
    }
    require(p == descriptor_end, "object descriptor sequence changed");
    for (std::size_t cell = 0; cell < cell_count; ++cell) {
        const auto entry = placement_table + 0x10u + cell * 4u;
        if (rom.word(entry) == 0u) {
            continue;
        }
        const auto base = rom.relative(entry);
        const auto data = rom.record(base, rom.word(base + 4u));
        const auto count = data.word(0);
        require(count <= 32u, "object cell placement count exceeds observed bound");
        data.range(0x88u, count * 0x30u);
        for (std::size_t i = 0; i < count; ++i) {
            const auto r = 0x88u + i * 0x30u;
            ObjectPlacementAsset item;
            item.raw_rom_offset = std::uint32_t(base + r);
            item.cell_index = std::uint32_t(cell);
            item.placement_index = std::uint32_t(i);
            item.descriptor = data.half(r + 0x28u);
            require(item.descriptor < descriptor_count, "object placement descriptor is invalid");
            const auto d = descriptors[item.descriptor];
            item.category = rom.bytes[d + 4u];
            require(item.category < 6u, "object descriptor category is invalid");
            std::uint32_t model = rom.bytes[d + 3u];
            for (std::size_t c = 0; c < item.category; ++c) {
                model += rom.word(0xa8334u + c * 4u);
            }
            require(model < model_count, "object descriptor model is invalid");
            item.model_index = std::uint16_t(model);
            item.billboard = item.category == 1u;
            quaternion(data, r);
            for (std::size_t k = 0; k < 4; ++k) {
                item.quaternion[k] = data.number(r + k * 4u);
            }
            for (std::size_t k = 0; k < 3; ++k) {
                item.position[k] = data.number(r + 0x10u + k * 4u);
            }
            out.placements.push_back(item);
        }
    }
    require(out.placements.size() == 4257u, "object placement inventory changed");
}
} // namespace

bool build_object_model_assets(std::span<const std::uint8_t> bytes,
                               std::span<const unsigned> resources, ObjectAssets &output,
                               std::string &error) noexcept {
    try {
        const Reader rom{bytes};
        require(bytes.size() >= 0x2000000u && bytes.size() <= 0x4000000u &&
                    rom.word(0) == 0x80371240u && rom.word(model_table) == 0x40u &&
                    rom.word(model_table + 0xcu) == model_count && resources.size() <= model_count,
                "selected object cache needs the supported Road Rash resource table");
        std::vector<std::uint8_t> memory(8u * 1024u * 1024u);
        copy(memory, 0x80000000u, bytes.subspan(0xc00u, 0xa0000u));
        put(memory, 0x80000c70u, 0u);
        ObjectAssets candidate;
        candidate.models.reserve(resources.size());
        for (auto resource : resources) append_model(rom, candidate, memory, resource, false);
        output = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
    catch (...) { error = "unknown selected object compilation failure"; return false; }
}
bool build_object_assets(std::span<const std::uint8_t> rom, ObjectAssets &output,
                         std::string &error, bool batch_triangles, bool compile_packets) noexcept {
    try {
        ObjectAssets candidate;
        build({rom}, candidate, batch_triangles);
        if (compile_packets && batch_triangles) {
            PacketCompiler compiler;
            compiler.compile(candidate, candidate.models);
        }
        output = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "unknown object asset compilation failure";
        return false;
    }
}
bool build_object_assets(std::span<const std::uint8_t> rom, ObjectAssets &output,
                         std::string &error) noexcept {
    return build_object_assets(rom, output, error, false);
}
bool build_object_assets(std::span<const std::uint8_t> rom, ObjectAssets &output,
                         std::string &error, bool batch_triangles) noexcept {
    return build_object_assets(rom, output, error, batch_triangles, false);
}
} // namespace rr64::world
