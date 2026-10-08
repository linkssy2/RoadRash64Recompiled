#include "rr64_rider_skin_render.hpp"
#include "rr64_rider_skin_menu.hpp"
#include "rr64_rider_skin_diagnostics.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_rider_skins.hpp"
#include "rr64_mk64_items.hpp"
#include "rr64_mk64_item_lightning.hpp"
#include "rr64_engine_layout.hpp"
#include "librecomp/addresses.hpp"
#include <algorithm>
#include <array>
#include <mutex>

namespace rr64::rider_skins {
using mk64_items::MaterialCommand;
namespace {
std::mutex preview_trace_mutex;
std::array<PreviewTrace, 128> preview_trace_samples{};
unsigned preview_trace_written{}, preview_trace_read{};
std::atomic_uint preview_trace_lost{};
// Only the native render worker calls the sampler. The event worker drains
// copied records under a try-lock, so neither worker waits for logging.
struct PreviewTraceKey {
    unsigned root{}, mode{}, epoch{}, samples{};
    std::array<unsigned, 4> choices{};
};
std::array<PreviewTraceKey, 16> preview_trace_keys{};
unsigned preview_trace_attempts{};
void publish_preview_trace(const PreviewTrace &sample) noexcept {
    std::unique_lock lock(preview_trace_mutex, std::try_to_lock);
    if (!lock || preview_trace_written == preview_trace_samples.size()) {
        preview_trace_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    preview_trace_samples[preview_trace_written++] = sample;
}
}
bool preview_trace_enabled() noexcept {
    static const bool enabled = diagnostics::exact_enabled_value(
        std::getenv("RR64_RIDER_SKIN_PREVIEW_TRACE"));
    return enabled && diagnostics::detailed_enabled();
}
bool take_preview_trace(PreviewTrace &sample) noexcept {
    if (!preview_trace_enabled())
        return false;
    std::unique_lock lock(preview_trace_mutex, std::try_to_lock);
    if (!lock || preview_trace_read == preview_trace_written)
        return false;
    sample = preview_trace_samples[preview_trace_read++];
    return true;
}
unsigned preview_trace_dropped() noexcept {
    return preview_trace_lost.load(std::memory_order_relaxed);
}
unsigned replace_images(std::span<const MaterialCommand> source, std::span<MaterialCommand> output,
                        std::span<const ImageBinding> bindings) noexcept {
    if (source.empty() || source.size() > 4096 || output.size() < source.size() ||
        source.back() != MaterialCommand{0xdf000000, 0})
        return 0;
    // Both native actor paths emit flat lists. Refuse unknown control flow;
    // never chase or modify a shared nested list on another rider's behalf.
    for (unsigned i = 0; i + 1 < source.size(); ++i) {
        const unsigned op = source[i].first >> 24;
        if (op == 0xde || op == 0xdf || op == 0xdd || op == 0x04 || op == 0x64 || op == 0xe0)
            return 0;
    }
    unsigned changed = 0;
    for (unsigned i = 0; i < source.size(); ++i) {
        auto c = source[i];
        for (const auto &b : bindings) {
            if (c.first == 0xfd500000 && c.second == b.image && b.replacement_image) {
                c.second = b.replacement_image;
                ++changed;
                break;
            }
            if (c.first == 0xfd100000 && c.second == b.palette && b.replacement_palette) {
                c.second = b.replacement_palette;
                ++changed;
                break;
            }
        }
        output[i] = c;
    }
    return changed;
}

namespace {
using namespace engine;
constexpr unsigned far_staging_offset = 8704;
constexpr unsigned texture_stride = far_staging_offset + 2560;
constexpr unsigned texture_capacity = maximum_characters * texture_stride;
constexpr unsigned frame_capacity = 1024 * 1024, guard = 0x534b494e;
constexpr std::array<unsigned, 4> texture_offsets{0, 2560, 5120, 7680};
struct Frame {
    unsigned base{}, epoch{}, used{};
};
struct Preview {
    unsigned char *memory{};
    unsigned appearance{}, start{}, gfx{}, epoch{};
    bool sampled{};
    PreviewTrace trace{};
};
struct State {
    unsigned char *memory{};
    unsigned textures{};
    unsigned shell{};
    std::array<bool, maximum_characters> copied{};
    std::array<Frame, 2> frames{};
    std::array<Preview, 8> previews{};
    unsigned preview_depth{};
};
State state;
unsigned word(unsigned char *rdram, unsigned a) {
    return unsigned(MEM_W(0, guest_address(a)));
}
void put(unsigned char *m, unsigned a, unsigned v) {
    auto *rdram = m;
    MEM_W(0, guest_address(a)) = v;
}
unsigned native_word(unsigned char *m, unsigned a) {
    unsigned v = 0;
    read_u32(m, a, v);
    return v;
}
unsigned half(unsigned char *m, unsigned a) {
    std::uint16_t v = 0;
    read_u16(m, a, v);
    return v;
}
bool sample_preview(unsigned char *m, unsigned root, PreviewTrace &sample) {
    if (!m || !preview_trace_enabled() || preview_trace_attempts >= 128)
        return false;
    std::array<unsigned, 4> choices{};
    bool chosen = false;
    for (unsigned i = 0; i < choices.size(); ++i) {
        choices[i] = menu_selection(i);
        chosen |= choices[i] != 0;
    }
    if (!chosen)
        return false;
    const unsigned mode = native_word(m, globals::main_mode), epoch = native_word(m, 0x800a1830);
    auto *key = &preview_trace_keys.back();
    for (auto &candidate : preview_trace_keys)
        if (!candidate.samples || (candidate.root == root && candidate.mode == mode)) {
            key = &candidate;
            break;
        }
    if (key->root != root || key->mode != mode || key->choices != choices)
        *key = {root, mode, epoch, 0, choices};
    if (key->samples >= 3 || (key->samples && unsigned(epoch - key->epoch) < 2))
        return false;
    ++key->samples;
    key->epoch = epoch;
    ++preview_trace_attempts;
    sample.root = root;
    sample.mode = mode;
    sample.epoch = epoch;
    sample.choices = choices;
    sample.handler = mode < kModeRecordCount
        ? native_word(m, globals::mode_records + mode * kModeRecordSize + 8) : 0;
    sample.pool = native_word(m, 0x800d13c8);
    sample.head = native_word(m, 0x800a1454);
    sample.gfx = native_word(m, 0x8009cba4);
    sample.base = sample.gfx < 2 ? native_word(m, 0x800ac658 + sample.gfx * 4) : 0;
    sample.active_base = native_word(m, 0x8009cb90);
    sample.capacity = native_word(m, 0x800bc9a0);
    sample.start = native_word(m, 0x800ac650);
    for (unsigned i = 0; i < 4; ++i)
        sample.donors[i] = native_word(m, 0x8009f670 + i * 4);
    unsigned node = sample.head;
    for (auto &actor : sample.actors) {
        if (!node || (node & 3) || !valid_guest_range(node, 0x44))
            break;
        actor = {node, native_word(m, node), native_word(m, node + 4),
                 native_word(m, node + 0x40), native_word(m, node + 0x28)};
        node = native_word(m, node + 0x3c);
    }
    return true;
}
bool graphics(unsigned char *m, unsigned &gfx, unsigned &epoch) {
    return m && read_u32(m, 0x8009cba4, gfx) && gfx < 2 && read_u32(m, 0x800a1830, epoch);
}
unsigned allocate(unsigned char *m, unsigned bytes) {
    if (state.memory && state.memory != m)
        return 0;
    auto *p = static_cast<unsigned char *>(recomp::alloc(m, bytes + 16));
    if (!p)
        return 0;
    const auto offset = p - m;
    if (offset < 0x800000 || (offset & 7) ||
        std::uint64_t(offset) + bytes + 16 > recomp::mem_size) {
        recomp::free(m, p);
        return 0;
    }
    state.memory = m;
    const unsigned base = kRdramBegin + unsigned(offset);
    for (unsigned i = 0; i < 4; ++i)
        put(m, base + bytes + i * 4, guard);
    return base;
}
bool intact(unsigned char *m, unsigned base, unsigned bytes) {
    if (!base || state.memory != m)
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (word(m, base + bytes + i * 4) != guard)
            return false;
    return true;
}
bool textures(unsigned char *m, unsigned id, const Appearance &a) {
    if (!id || id > maximum_characters || (a.donor != 0 && a.donor != 10))
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned w = i == 3 ? 32 : 64, h = i == 3 ? 16 : 32;
        if (a.textures[i].width != w || a.textures[i].height != h ||
            a.textures[i].bytes.size() != w * h + 512)
            return false;
    }
    if (!state.textures)
        state.textures = allocate(m, texture_capacity);
    if (!intact(m, state.textures, texture_capacity))
        return false;
    if (!state.copied[id - 1]) {
        // Catalog installation is immutable for a running game session. Copy
        // once, using guest byte order; rendering performs no host allocation.
        for (unsigned i = 0; i < 4; ++i) {
            const auto &data = a.textures[i].bytes;
            const unsigned base = state.textures + (id - 1) * texture_stride + texture_offsets[i];
            for (unsigned j = 0; j < data.size(); ++j)
                m[((base - kRdramBegin) + j) ^ 3] = data[j];
        }
        // Native LOD2 reuses the 64x32 body material, but its unchanged UVs
        // address the 32x16 far layout. Stage the authored far image inside
        // that native tile, padding its edges for filtering. Keep the native
        // 64-byte row stride, tile commands and 2048-byte palette offset.
        const auto &far = a.textures[3].bytes;
        const unsigned staged =
            state.textures - kRdramBegin + (id - 1) * texture_stride + far_staging_offset;
        for (unsigned y = 0; y < 32; ++y)
            for (unsigned x = 0; x < 64; ++x)
                m[(staged + y * 64 + x) ^ 3] = far[std::min(y, 15u) * 32 + std::min(x, 31u)];
        for (unsigned i = 0; i < 512; ++i)
            m[(staged + 2048 + i) ^ 3] = far[512 + i];
        state.copied[id - 1] = true;
    }
    return true;
}
std::array<unsigned, 4> donor_headers(unsigned char *m, const Appearance &a) {
    std::array<unsigned, 4> result{};
    const unsigned skin = native_word(m, 0x800a1578 + a.donor * 8);
    if (skin != (a.donor == 10 ? 33u : 0u))
        return result;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned p = native_word(m, 0x800d3840 + (skin * 4 + i) * 8);
        const unsigned w = i == 3 ? 32 : 64, h = i == 3 ? 16 : 32;
        // The native loader owns/frees these pointers at each menu/race
        // transition. Resolve them anew instead of caching recycled addresses.
        if (valid_guest_range(p, 64 + w * h + 512) && !(p & 7) && native_word(m, p) == 0x16 &&
            native_word(m, p + 0x2c) == w && native_word(m, p + 0x30) == h &&
            native_word(m, p + 0x34) == 8 && half(m, p + 0x22) == 512 &&
            half(m, p + 0x26) == w * h && !(half(m, p + 0x24) & 0x8004))
            result[i] = p;
    }
    return result;
}
ImageBinding binding(unsigned id, unsigned slot, unsigned source, bool far_body = false) {
    const unsigned size = slot == 3 && !far_body ? 512 : 2048;
    const unsigned offset = far_body ? far_staging_offset : texture_offsets[slot];
    const unsigned replacement = state.textures + (id - 1) * texture_stride + offset;
    return {source, source + size, replacement, replacement + size};
}
unsigned frame_copy(unsigned char *m, unsigned gfx, unsigned epoch,
                    std::span<const MaterialCommand> commands) {
    auto &f = state.frames[gfx];
    if (!f.base)
        f.base = allocate(m, frame_capacity);
    if (!intact(m, f.base, frame_capacity))
        return 0;
    if (f.epoch != epoch) {
        f.epoch = epoch;
        f.used = 0;
    }
    const unsigned bytes = unsigned(commands.size()) * 8;
    if (bytes > frame_capacity - f.used)
        return 0;
    const unsigned result = f.base + f.used;
    for (unsigned i = 0; i < commands.size(); ++i) {
        put(m, result + i * 8, commands[i].first);
        put(m, result + i * 8 + 4, commands[i].second);
    }
    f.used += bytes;
    return result;
}
unsigned add_shell(unsigned char *m, std::span<MaterialCommand> commands,
                   unsigned count, unsigned torso) {
    // Insert only after a complete near-LOD torso batch. Its bone matrix and
    // texture remain active here, so the shell follows every native pose.
    unsigned last_triangle = 0;
    bool active = false;
    for (unsigned i = 0; i < count; ++i) {
        const auto [op, address] = commands[i];
        if (op == 0xfd500000) {
            if (active && address != torso) break;
            active = address == torso;
        }
        if (active && (op >> 24 == 5 || op >> 24 == 6)) last_triangle = i;
        if (last_triangle && (op >> 24 == 0xda || op >> 24 == 0xdf)) break;
    }
    constexpr unsigned added = 5, vertex_bytes = 9 * 16;
    if (!last_triangle || count + added + 2 > commands.size()) return count;
    // The attachment overwrites the RSP vertex cache. A following native batch
    // must reload vertices before drawing; otherwise keep the original list.
    for (unsigned i = last_triangle + 1; i < count; ++i) {
        const unsigned op = commands[i].first >> 24;
        if (op == 1 || op == 0xdf) break;
        if (op == 5 || op == 6) return count;
    }
    if (!state.shell) {
        state.shell = allocate(m, vertex_bytes);
        if (!state.shell) return count;
        // Authored eight-sided dome, not geometry extracted from another game.
        constexpr std::array<std::array<int, 3>, 9> positions{{
            {-43,0,8}, {-24,0,35}, {-24,18,27}, {-24,25,8},
            {-24,18,-11}, {-24,0,-19}, {-24,-18,-11},
            {-24,-25,8}, {-24,-18,27}}};
        for (unsigned i = 0; i < positions.size(); ++i) {
            const auto &p = positions[i];
            const unsigned v = state.shell + i * 16;
            // This allocation is outside the original 8 MiB guest range.
            put(m, v, (unsigned(std::uint16_t(p[0])) << 16) | std::uint16_t(p[1]));
            put(m, v + 4, unsigned(std::uint16_t(p[2])) << 16);
            put(m, v + 8, (unsigned((48 + p[1] * 15 / 25) * 64) << 16) |
                          unsigned((p[2] + 19) * 31 * 64 / 54));
            put(m, v + 12, 0x810000ff); // Back-facing normal, opaque.
        }
    }
    if (!intact(m, state.shell, vertex_bytes)) return count;
    const unsigned at = last_triangle + 1;
    std::move_backward(commands.begin() + at, commands.begin() + count,
                       commands.begin() + count + added);
    commands[at] = {0x01009012, state.shell};
    for (unsigned pair = 0; pair < 4; ++pair) {
        const unsigned first = pair * 2 + 1, second = first + 1;
        const unsigned next = second == 8 ? 1 : second + 1;
        commands[at + 1 + pair] = {0x06000000 | (first * 2 << 8) | second * 2,
                                   (second * 2 << 8) | next * 2};
    }
    return count + added;
}
unsigned separate_head(unsigned char *m, unsigned gfx, unsigned epoch, unsigned id,
                       unsigned vertex_base, std::span<MaterialCommand> commands,
                       unsigned count) {
    using Vertex = std::array<unsigned, 4>;
    std::array<Vertex, 32> cache{};
    std::array<bool, 32> valid{};
    std::array<MaterialCommand, 6144> result{};
    unsigned used = 0, changed = 0;
    unsigned loaded = 0;
    MaterialCommand reload{};
    int slot = -1;
    bool overwritten = false;
    const std::array<unsigned, 3> images{binding(id, 0, 0).replacement_image,
        binding(id, 2, 0).replacement_image, binding(id, 2, 0, true).replacement_image};
    constexpr std::array<int, 3> left{26 * 64, 36 * 64, 18 * 64};
    constexpr std::array<int, 3> width{38 * 64, 27 * 64, 9 * 64};
    auto emit = [&](MaterialCommand c) {
        if (used >= result.size()) return false;
        result[used++] = c;
        return true;
    };
    auto stage = [&](std::span<const Vertex> vertices) {
        std::array<MaterialCommand, 64> words{};
        for (unsigned i = 0; i < vertices.size(); ++i) {
            words[i * 2] = {vertices[i][0], vertices[i][1]};
            words[i * 2 + 1] = {vertices[i][2], vertices[i][3]};
        }
        return frame_copy(m, gfx, epoch, std::span(words).first(vertices.size() * 2));
    };
    auto restore = [&] {
        if (!overwritten) return true;
        overwritten = false;
        return emit(reload);
    };
    auto indices = [](unsigned word) {
        return std::array<unsigned, 3>{(word >> 17) & 127, (word >> 9) & 127,
                                      (word >> 1) & 127};
    };
    auto head = [&](const std::array<unsigned, 3> &triangle) {
        if (slot < 0 || loaded < 3) return false;
        for (unsigned index : triangle) {
            if (index >= loaded || !valid[index]) return false;
            const auto &v = cache[index];
            const int s = std::int16_t(v[2] >> 16);
            // Detailed showroom and animated meshes do not share the reference
            // model's XYZ bounds. The head's material/UV island is stable.
            if (s < left[slot] - 32 || s > left[slot] + width[slot]) return false;
        }
        return true;
    };
    auto triangle = [&](unsigned packed, bool is_head) {
        if (!is_head) return restore() && emit({0x05000000 | (packed & 0xffffff), 0});
        const auto ids = indices(packed);
        int low = 32767, high = -32768, side = 0;
        for (unsigned i = 0; i < loaded; ++i) {
            const int y = std::int16_t(cache[i][0]);
            low = std::min(low, y);
            high = std::max(high, y);
        }
        for (unsigned index : ids) side += 2 * std::int16_t(cache[index][0]) - low - high;
        std::array<Vertex, 3> vertices{cache[ids[0]], cache[ids[1]], cache[ids[2]]};
        for (auto &v : vertices) {
            const int s = std::int16_t(v[2] >> 16);
            const unsigned remapped = left[slot] +
                std::clamp(s - left[slot], 0, width[slot] - 1) / 2 +
                (side > 0 ? width[slot] / 2 : 0);
            // Preserve authored T: the live high-detail head already spans
            // this strip, unlike the low-detail reference used by offline previews.
            v[2] = (remapped << 16) | (v[2] & 0xffff);
        }
        const unsigned address = stage(vertices);
        if (!address || !emit({0x01003006, address}) || !emit({0x05000204, 0})) return false;
        overwritten = true;
        ++changed;
        return true;
    };
    for (unsigned i = 0; i < count; ++i) {
        const auto c = commands[i];
        const unsigned op = c.first >> 24;
        if (c.first == 0xfd500000) {
            slot = -1;
            for (unsigned s = 0; s < images.size(); ++s)
                if (c.second == images[s]) slot = int(s);
        }
        if (op == 1) {
            if (!restore()) return 0;
            const unsigned n = (c.first >> 12) & 255, end = (c.first >> 1) & 127;
            if (!n || n > 32 || end > 32 || end < n) return 0;
            // Native head batches load from cache index zero. Keep partial or
            // unfamiliar batches untouched; only overwrite slots we can restore
            // with the exact original load under the same bone matrix.
            loaded = end == n ? n : 0;
            reload = c;
            unsigned address = c.second;
            if ((address >> 24) == 5) {
                if (!vertex_base) return 0;
                address = vertex_base + (address & 0xffffff);
            } else if (!(address & 0x80000000)) {
                if (address >= 0x800000) return 0;
                address |= 0x80000000;
            }
            if (!valid_guest_range(address, n * 16)) return 0;
            for (unsigned v = 0; v < n; ++v) {
                for (unsigned w = 0; w < 4; ++w)
                    cache[end - n + v][w] = native_word(m, address + v * 16 + w * 4);
                valid[end - n + v] = true;
            }
        }
        if (op == 5 || op == 6) {
            const bool first = head(indices(c.first)), second = op == 6 && head(indices(c.second));
            if (first || second) {
                if (!triangle(c.first, first) || (op == 6 && !triangle(c.second, second))) return 0;
                continue;
            }
            if (!restore()) return 0;
        }
        if ((op == 0xda || op == 0xdb || op == 0xdf) && !restore()) return 0;
        if (!emit(c)) return 0;
    }
    if (!changed || used + 2 > commands.size()) return 0;
    std::copy_n(result.begin(), used, commands.begin());
    return used;
}
unsigned finish_list(unsigned char *m, unsigned gfx, unsigned epoch,
                     std::span<MaterialCommand> commands, unsigned count, unsigned node) {
#ifdef RR64_EXPERIMENTAL_COURSE
    mk64_items::RiderState effect{};
    unsigned clock = 0;
    if (node && mk64_items::render_effect(m, node, effect, clock)) {
        const auto lightning = mk64_items::lightning_visual(effect, clock);
        if (effect.star_until > clock || effect.boo_until > clock || lightning.active) {
            std::array<MaterialCommand, 1536> transformed{};
            constexpr std::array<unsigned, 6> colors{0xff5050, 0xffdf50, 0x70ff70,
                                                     0x50dfff, 0x8080ff, 0xff70df};
            const bool ghost = effect.boo_until > clock;
            const unsigned color = effect.star_until > clock ? colors[(clock / 3) % colors.size()]
                                   : lightning.active        ? lightning.tint
                                                             : 0xffffff;
            const unsigned n = mk64_items::compile_material(commands.first(count), transformed,
                                                            (color << 8) | (ghost ? 90u : 255u),
                                                            ghost, lightning.active);
            return n ? frame_copy(m, gfx, epoch, std::span(transformed).first(n)) : 0;
        }
    }
#endif
    if (commands.size() < count + 2)
        return 0;
    commands[count - 1] = {0x6400002c, 0}; // Scoped extended addresses end with this actor.
    commands[count] = {0xe0525464, 0x20000000};
    commands[count + 1] = {0xdf000000, 0};
    return frame_copy(m, gfx, epoch, commands.first(count + 2));
}
bool call_range(unsigned char *m, unsigned call, unsigned bytes = 24) {
    unsigned gfx = 0, epoch = 0;
    if (!graphics(m, gfx, epoch) || (call & 7))
        return false;
    const unsigned base = native_word(m, 0x800ac658 + gfx * 4), count = native_word(m, 0x800bc9a0);
    return native_word(m, 0x8009cb90) == base && (count == 0x4650 || count == 0x36b0) &&
           valid_guest_range(base, 0x140 + count * 8) && call >= base + 0x148 &&
           call <= base + 0x140 + count * 8 - 1064 && bytes <= base + 0x140 + count * 8 - call;
}
void branch(unsigned char *m, unsigned call, unsigned list) {
    put(m, call, 0xe0525464);
    put(m, call + 4, 0x10000064);
    put(m, call + 8, 0x6400002c);
    put(m, call + 12, 1);
    put(m, call + 16, 0xde000000);
    put(m, call + 20, list);
}
}
void reset_render_session() noexcept {
    state = {};
}
}

extern "C" unsigned rr64_rider_skin_actor_list(unsigned char *m, unsigned node, unsigned original,
                                               unsigned lod, unsigned vertex_base) {
    using namespace rr64::rider_skins;
    using namespace rr64::engine;
    const unsigned id = rr64_rider_skin_actor_selection(m, node);
    const auto *a = appearance(id);
    unsigned gfx = 0, epoch = 0;
    if (!a || !graphics(m, gfx, epoch) || !valid_guest_range(node, 0x17c) ||
        !valid_guest_range(original, 8) || (original & 7))
        return original;
    if (lod > 2)
        return original;
    const unsigned material_count = half(m, node + 0x4a + lod * 2);
    if (!material_count || material_count > 8 || !textures(m, id, *a))
        return original;
    const auto headers = donor_headers(m, *a);
    std::array<ImageBinding, 8> bindings{};
    unsigned size = 0;
    for (unsigned i = 0; i < material_count; ++i) {
        const unsigned p = native_word(m, node + 0x7c + lod * 32 + i * 4);
        for (unsigned slot = 0; slot < 4; ++slot)
            if (p && p == headers[slot])
                bindings[size++] = binding(id, slot, (7 + i) << 24, lod == 2 && slot == 2);
    }
    if (!size)
        return original;
    std::array<MaterialCommand, 1024> source{};
    std::array<MaterialCommand, 1536> changed{};
    unsigned count = 0;
    for (; count < source.size(); ++count) {
        const unsigned p = original + count * 8;
        if (!valid_guest_range(p, 8))
            return original;
        source[count] = {native_word(m, p), native_word(m, p + 4)};
        if (source[count].first == 0xdf000000) {
            ++count;
            break;
        }
    }
    if (!replace_images(std::span(source).first(count), changed, std::span(bindings).first(size)))
        return original;
    if (a->turtle_shell && lod == 0)
        count = add_shell(m, changed, count, binding(id, 1, 0).replacement_image);
    if (a->dual_head) {
        count = separate_head(m, gfx, epoch, id, vertex_base, changed, count);
        if (!count) return original;
    }
    const auto list = finish_list(m, gfx, epoch, changed, count, node);
    return list ? list : original;
}
extern "C" unsigned rr64_rider_skin_actor_call(unsigned char *m, unsigned node, unsigned original,
                                               unsigned call, unsigned lod) {
    using namespace rr64::rider_skins;
    const unsigned cursor = m ? native_word(m, 0x800ac650) : 0;
    if (!call_range(m, call) || (cursor != call && cursor != call + 8) ||
        native_word(m, call) != 0xde000000)
        return original;
    unsigned vertex_base = 0;
    const auto *skin = appearance(rr64_rider_skin_actor_selection(m, node));
    if (skin && skin->dual_head) {
        // Native 11F8C establishes segment 5 before the actor call. Search only
        // this validated command buffer, never guessed global segment state.
        unsigned gfx = 0, epoch = 0;
        graphics(m, gfx, epoch);
        const unsigned start = native_word(m, 0x800ac658 + gfx * 4) + 0x140;
        for (unsigned p = call, n = 0; p >= start + 8 && n < 128; ++n) {
            p -= 8;
            if (native_word(m, p) == 0xdb060014) {
                vertex_base = native_word(m, p + 4);
                break;
            }
        }
    }
    const unsigned list = rr64_rider_skin_actor_list(m, node, original, lod, vertex_base);
    if (list == original) {
#ifdef RR64_EXPERIMENTAL_COURSE
        return rr64_mk64_items_actor_call(m, node, original, call);
#else
        return original;
#endif
    }
    branch(m, call, list);
    return list;
}
extern "C" void rr64_rider_skin_preview_begin(unsigned char *m, unsigned root) {
    using namespace rr64::rider_skins;
    if (state.preview_depth >= state.previews.size()) {
        ++state.preview_depth;
        return;
    }
    auto &p = state.previews[state.preview_depth++];
    p = {};
    const unsigned id = rr64_rider_skin_preview_selection(m, root);
    p.sampled = sample_preview(m, root, p.trace);
    p.trace.appearance = id;
    if (!id || !graphics(m, p.gfx, p.epoch)) {
        p.trace.reason = 1; // Ownership or graphics state rejected at entry.
        return;
    }
    p.start = native_word(m, 0x800ac650);
    if (!call_range(m, p.start)) {
        p.trace.reason = 2; // Active native command buffer did not match.
        return;
    }
    p.memory = m;
    p.appearance = id;
    // Force each preview to emit its own first texture load. Native caches
    // otherwise may omit it when adjacent local players share the same donor.
    put(m, 0x8009db2c, 0);
}
extern "C" void rr64_rider_skin_preview_end(unsigned char *m) {
    using namespace rr64::rider_skins;
    if (!state.preview_depth)
        return;
    const unsigned depth = --state.preview_depth;
    if (depth >= state.previews.size())
        return;
    Preview p = state.previews[depth];
    state.previews[depth] = {};
    struct Report {
        Preview &preview;
        ~Report() {
            if (preview.sampled)
                publish_preview_trace(preview.trace);
        }
    } report{p};
    p.trace.end = m ? native_word(m, 0x800ac650) : 0;
    unsigned gfx = 0, epoch = 0;
    if (p.memory != m || !p.appearance || !graphics(m, gfx, epoch) || p.gfx != gfx ||
        p.epoch != epoch) {
        if (!p.trace.reason)
            p.trace.reason = 3; // Frame/owner changed during traversal.
        return;
    }
    put(m, 0x8009db2c, 0);
    const unsigned end = native_word(m, 0x800ac650);
    if (end < p.start + 24 || (end & 7) || !call_range(m, p.start, end - p.start)) {
        p.trace.reason = 4;
        return;
    }
    const unsigned count = (end - p.start) / 8;
    p.trace.commands = count;
    if (count >= 4096) {
        p.trace.reason = 5;
        return;
    }
    const auto *a = appearance(p.appearance);
    if (!a || !textures(m, p.appearance, *a)) {
        p.trace.reason = 6;
        return;
    }
    const auto headers = donor_headers(m, *a);
    p.trace.headers = headers;
    std::array<ImageBinding, 8> bindings{};
    unsigned size = 0;
    for (unsigned slot = 0; slot < 4; ++slot)
        if (headers[slot]) {
            const auto b = binding(p.appearance, slot, headers[slot] + 64);
            bindings[size++] = b;
            auto physical = b;
            physical.image &= 0x1fffffff;
            physical.palette &= 0x1fffffff;
            bindings[size++] = physical;
        }
    std::array<MaterialCommand, 4096> source{};
    std::array<MaterialCommand, 4112> changed{};
    for (unsigned i = 0; i < count; ++i) {
        source[i] = {native_word(m, p.start + i * 8), native_word(m, p.start + i * 8 + 4)};
        if (p.sampled) {
            const unsigned op = source[i].first >> 24;
            if (op == 0xfd && p.trace.image_count < p.trace.images.size())
                p.trace.images[p.trace.image_count++] = {source[i].first, source[i].second};
            if (!p.trace.rejected_opcode &&
                (op == 0xde || op == 0xdf || op == 0xdd || op == 0x04 || op == 0x64 || op == 0xe0))
                p.trace.rejected_opcode = source[i].first;
        }
    }
    source[count] = {0xdf000000, 0};
    const unsigned replaced = replace_images(std::span(source).first(count + 1), changed,
                                             std::span(bindings).first(size));
    p.trace.replacements = replaced;
    if (!replaced) {
        p.trace.reason = 7; // Unmatched images or unsupported commands.
        return;
    }
    unsigned final_count = a->turtle_shell
        ? add_shell(m, changed, count + 1, binding(p.appearance, 1, 0).replacement_image)
        : count + 1;
    if (a->dual_head)
        final_count = separate_head(m, gfx, epoch, p.appearance, 0, changed, final_count);
    if (!final_count) { p.trace.reason = 8; return; }
    const unsigned list = finish_list(m, gfx, epoch, changed, final_count, 0);
    p.trace.list = list;
    if (!list) {
        p.trace.reason = 8;
        return;
    }
    // This range was just emitted for this single native preview. Redirect it
    // to an immutable per-frame copy; shared models and the next preview stay intact.
    branch(m, p.start, list);
    put(m, 0x800ac650, p.start + 24);
}
