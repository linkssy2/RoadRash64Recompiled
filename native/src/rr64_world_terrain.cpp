#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#endif
#include "rr64_world_terrain.hpp"
#include "rr64_terrain_graphics_pool.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_world_camera.hpp"
#include "rr64_local_world_window.hpp"
#include "rr64_world_frustum.hpp"
#include "rr64_world_render.hpp"
#include "rr64_world_course_regions.hpp"
#include "rr64_actor_render_snapshot.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>

namespace rr64::world {
Matrix terrain_matrix(const TerrainCellAsset &cell, float x, float y) noexcept {
    // All 3,289 roots in the supported ROM are identity quaternions. The
    // loader validates that certificate instead of silently dropping rotation.
    return {1,
            0,
            0,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            0.5f,
            0,
            cell.authored_origin[0] - x,
            cell.authored_origin[1] - y,
            0,
            1};
}
namespace {
using Vector = std::array<float, 4>;
Vector transform(const Vector &v, const Matrix &m) {
    Vector out{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            out[c] += v[r] * m[r * 4 + c];
    return out;
}
}
bool terrain_in_frustum(const TerrainCellAsset &cell, const Matrix &model, const Matrix &view,
                        const Matrix &projection) noexcept {
    // Conservative AABB/clip-plane test: a cell is culled only when all eight
    // corners lie outside the same plane. Intersecting and enclosing cells stay.
    unsigned outside_all = 63u;
    for (unsigned corner = 0; corner < 8; ++corner) {
        Vector p{(corner & 1) ? cell.maximum[0] : cell.minimum[0],
                 (corner & 2) ? cell.maximum[1] : cell.minimum[1],
                 (corner & 4) ? cell.maximum[2] : cell.minimum[2], 1};
        p = transform(transform(transform(p, model), view), projection);
        if (!std::all_of(p.begin(), p.end(), [](float f) { return std::isfinite(f); }))
            return false;
        unsigned mask = 0;
        for (unsigned i = 0; i < 3; ++i) {
            // RT64 expands the original 4:3 projection to at most 16:9 in
            // this port (rt64_workload_queue.cpp). Include those side strips
            // even in Original/Stretch mode; the GPU does the final clipping.
            const float limit = p[3] * (i == 0u ? 4.0f / 3.0f : 1.0f);
            if (p[i] < -limit)
                mask |= 1u << (i * 2u);
            if (p[i] > limit)
                mask |= 2u << (i * 2u);
        }
        outside_all &= mask;
    }
    return outside_all == 0;
}
namespace {
using namespace rr64::engine;
constexpr unsigned cells_count = 4900u, frame_bytes = 640u * 1024u;
constexpr unsigned command_bytes = 288u * 1024u;
constexpr unsigned terrain_id_base = 0x52510000u;
// Preserve RT64's default component policies, while matching each authored
// cell directly in fixed transform order instead of searching similar draws.
constexpr unsigned matching_group_flags = 0x02011555u;
static_assert(cells_count * 56u + 88u <= command_bytes);
static_assert(cells_count * 64u <= frame_bytes - command_bytes);
struct StockCall {
    unsigned command = 0, target = 0, replacement = 0;
};
struct Frame {
    unsigned char *host = nullptr;
    unsigned base = 0, assets = 0, commands = 0, matrices = 0;
    std::array<unsigned, 4> last_epoch{};
    std::array<bool, 4> issued{};
    std::array<std::vector<StockCall>, 4> pending;
};
struct Cache {
    unsigned char *mapping = nullptr;
    const unsigned char *rom = nullptr;
    TerrainAssets assets;
    std::vector<TerrainBounds> bounds;
    CourseRegions regions;
    CourseRegions::Mask allowed{};
    bool course_scoped = false;
    std::array<CourseRegions::Mask, 4> windowHistory{};
    std::array<double, 4> windowPercent{};
    CourseRegions::Mask window{};
    std::array<Frame, 2> frames{};
    bool attempted = false, ready = false, drawing = false;
    unsigned grid = 0;
    std::array<bool, cells_count> stock{};
    std::array<unsigned, cells_count> cell_ordinals{};
    std::array<bool, cells_count> static_materials{};
    std::array<StockCall, cells_count> stock_calls{};
    bool replace_stock = false;
    unsigned stock_epoch = 0, stock_view = 0, stock_graphics_slot = 0;
    TerrainStatistics stats{};
    std::mutex mutex;
};
Cache &cache() {
    static auto c = std::make_unique<Cache>();
    return *c;
}
bool course_diagnostics() {
    static const bool enabled = [] {
        const char *value = std::getenv("RR64_COURSE_DIAGNOSTICS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
TerrainViewEvidence *capture_view(Cache &c, unsigned char *m, unsigned index, unsigned epoch,
                                  float x, float y) {
    if (!course_diagnostics())
        return nullptr;
    std::array<unsigned, 19> setup{};
    if (!read_u32(m, globals::main_mode, setup[0]) || !read_u32(m, globals::pending_mode, setup[1]))
        return nullptr;
    for (unsigned i = 0; i < globals::multiplayer_game_setup_words.size(); ++i)
        if (!read_u32(m, globals::multiplayer_game_setup_words[i], setup[i + 2]))
            return nullptr;
    auto &evidence = c.stats.evidence;
    if (!evidence.enabled || evidence.raw_setup != setup) {
        const unsigned generation = evidence.generation + 1;
        evidence = {};
        evidence.enabled = true;
        evidence.generation = generation;
        evidence.raw_setup = setup;
    }
    auto &v = evidence.views[index];
    v.valid = true;
    v.epoch = epoch;
    v.x = x;
    v.y = y;
    v.stock = c.stats.stock_cells;
    v.excluded = c.stats.course_excluded_cells;
    v.extended = 0;
    v.triangles = 0;
    v.extended_last.fill(0);
    v.stock_last.fill(0);
    // Union of stock draws AFTER the existing island filter, not unfiltered
    // engine selection. Never infer ownership from cells missing in this set.
    for (unsigned i = 0; i < cells_count; ++i)
        if (c.stock[i]) {
            v.stock_union[i / 64] |= 1ull << (i % 64);
            v.stock_last[i / 64] |= 1ull << (i % 64);
        }
    return &v;
}
void word(unsigned char *rdram, unsigned addr, unsigned value) {
    MEM_W(0, guest_address(addr)) = value;
}
void command(unsigned char *m, unsigned &p, unsigned a, unsigned b) {
    word(m, p, a);
    word(m, p + 4, b);
    p += 8;
}
void cell_commands(unsigned char *m, unsigned &dl, unsigned matrix, unsigned assets,
                   unsigned camera, const TerrainCellAsset &cell, float x, float y) {
    const auto root = terrain_matrix(cell, x, y);
    for (unsigned i = 0; i < 16; ++i)
        word(m, matrix + i * 4u, std::bit_cast<unsigned>(root[i]));
    command(m, dl, 0x6400000cu, terrain_id_base + camera * 0x2000u + cell.cell_index);
    command(m, dl, matching_group_flags, 0u);
    command(m, dl, 0x64000030u, 2u); // Float LOAD+PUSH, exactly as in the distant pass.
    command(m, dl, 0u, matrix);
    command(m, dl, 0xde000000u, assets + cell.display_list_offset);
    command(m, dl, 0xd8380002u, 0x40u);
    command(m, dl, 0x6400000du, 1u);
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
        // Mapping replacement destroys its heap. Only free in a still-live
        // mapping; guest race/menu streaming never owns these allocations.
        if (c.mapping == m)
            release(c);
        c.mapping = m;
        c.rom = rom.data();
        c.frames = {};
        c.assets = {};
        c.bounds.clear();
        c.windowHistory = {};
        c.attempted = false;
        c.ready = false;
        c.stats = {};
    }
    if (c.attempted)
        return c.ready;
    c.attempted = true;
    std::string error;
    if (!build_terrain_assets(rom, c.assets, error, true, true)) {
        std::fprintf(stderr, "[RR64-WORLD] terrain cache refused: %s\n", error.c_str());
        return false;
    }
    c.cell_ordinals.fill(~0u);
    for (unsigned ordinal = 0; ordinal < c.assets.cells.size(); ++ordinal) {
        const auto &cell = c.assets.cells[ordinal];
        if (cell.root_quaternion != std::array<float, 4>{0, 0, 0, 1} ||
            cell.cell_index >= cells_count || c.cell_ordinals[cell.cell_index] != ~0u) {
            std::fprintf(stderr, "[RR64-WORLD] terrain root certificate refused\n");
            return false;
        }
        c.cell_ordinals[cell.cell_index] = ordinal;
    }
    c.static_materials.fill(true);
    // Stock animated materials keep their original producer until their live
    // texture phase has a certificate. The distant cache currently binds frame 0.
    // ponytail: one startup scan per animated binding; index ranges if it becomes costly.
    for (const auto &r : c.assets.relocations)
        if (r.texture_index < c.assets.textures.size() &&
            c.assets.textures[r.texture_index].frame_count > 1u)
            for (const auto &cell : c.assets.cells)
                if (r.word_offset >= cell.display_list_offset &&
                    r.word_offset < cell.display_list_offset + cell.display_list_size)
                    c.static_materials[cell.cell_index] = false;
    CourseRegions::Mask occupied{};
    c.bounds.clear();
    c.bounds.reserve(c.assets.cells.size());
    for (const auto &cell : c.assets.cells)
        c.bounds.emplace_back(cell.minimum, cell.maximum);
    for (const auto &cell : c.assets.cells)
        occupied[cell.cell_index] = true;
    c.regions.build(occupied);
    const unsigned bytes = (unsigned(c.assets.bytes.size()) + 63u) & ~63u;
    // Per-original-graphics-buffer copies keep animated bindings and matrices
    // immutable while RT64 consumes the other buffer. Never allocate per race.
    for (auto &frame : c.frames) {
        for (auto &pending : frame.pending)
            pending.reserve(c.assets.cells.size());
        frame.host = static_cast<unsigned char *>(recomp::alloc(m, bytes + 4u * frame_bytes));
        if (!frame.host) {
            release(c);
            return false;
        }
        const auto offset = frame.host - m;
        if (offset < 0x800000 ||
            std::uint64_t(offset) + bytes + 4u * frame_bytes > recomp::mem_size) {
            release(c);
            return false;
        }
        frame.base = 0x80000000u + unsigned(offset);
        frame.assets = frame.base;
        frame.commands = frame.base + bytes;
        frame.matrices = frame.commands + command_bytes;
        for (unsigned i = 0; i < c.assets.bytes.size(); ++i)
            m[(unsigned(offset) + i) ^ 3u] = c.assets.bytes[i];
        for (const auto &r : c.assets.relocations)
            word(m, frame.assets + r.word_offset, frame.assets + r.target_offset);
    }
    c.ready = true;
    c.stats.cached_cells = unsigned(c.assets.cells.size());
    c.stats.cached_triangles = c.assets.triangles;
    c.stats.cached_bytes = 2u * (bytes + 4u * frame_bytes);
    if (rr64::diagnostics::routine_enabled()) {
        std::fprintf(stderr,
                     "[RR64-WORLD] terrain cache cells=%u triangles=%u bytes=%u triangle-batches=1\n",
                     c.stats.cached_cells, c.stats.cached_triangles, c.stats.cached_bytes);
    }
    return true;
}
bool frame_context(unsigned char *m, unsigned &slot, unsigned &graphics_slot, unsigned &epoch,
                   unsigned &pointer, unsigned &camera_index, Matrix &view, Matrix &projection,
                   float &origin_x, float &origin_y) {
    unsigned width = 0, base = 0, count = 0, active_base = 0;
    if (!read_u32(m, globals::active_viewport, camera_index) || camera_index >= 4u ||
        !read_u32(m, globals::terrain_map_width, width) || width != 70u ||
        !read_u32(m, 0x8009dbd4u, slot) || slot > 1u || !rr64_world_camera_ready(m, 0u, slot) ||
        !read_u32(m, 0x8009cba4u, graphics_slot) || graphics_slot > 1u ||
        !read_u32(m, 0x800a1830u, epoch) || !read_u32(m, 0x800ac650u, pointer) ||
        !read_u32(m, 0x800ac658u + graphics_slot * 4u, base) ||
        !read_u32(m, 0x8009cb90u, active_base) || base != active_base ||
        !read_u32(m, 0x800bc9a0u, count) || (count != 0x4650u && count != 0x36b0u))
        return false;
    const unsigned size = 0x140u + count * 8u;
    // Keep a 1024-byte tail for the original remainder and final commands.
    if (!valid_guest_range(base, size) || pointer < base + 0x148u ||
        pointer > base + size - 1040u || (pointer & 7u))
        return false;
    unsigned reset = 0, value = 0;
    if (!read_u32(m, pointer - 8u, reset) || reset != 0xfa000000u ||
        !read_u32(m, pointer - 4u, value) || value != 0u)
        return false;
    if (!read_float(m, 0x800dde80u, origin_x) || !read_float(m, 0x800dde84u, origin_y) ||
        !std::isfinite(origin_x) || !std::isfinite(origin_y))
        return false;
    Matrix4x4Snapshot p{}, v{};
    if (!decode_n64_matrix(m, 0x800b6568u + camera_index * 0x180u + slot * 0x40u, p) ||
        !decode_n64_matrix(m, 0x800b6de8u + camera_index * 0x180u + slot * 0x40u, v))
        return false;
    projection = p.values;
    view = v.values;
    return p.values[0] != 0 && p.values[5] != 0 && p.values[11] != 0 && v.values[15] == 1;
}
}
TerrainStatistics terrain_statistics() noexcept {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    return c.stats;
}
void terrain_reset_session() noexcept {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    // init_heap follows the on_init callback and reclaims the old allocation
    // arena. Addresses from the preceding run must never be reused or freed.
    c.mapping = nullptr;
    c.rom = nullptr;
    c.frames = {};
    c.assets = {};
    c.bounds.clear();
    c.windowHistory = {};
    c.windowHistory = {};
    c.stock.fill(false);
    c.stock_calls = {};
    c.replace_stock = false;
    c.attempted = false;
    c.ready = false;
    c.drawing = false;
    c.grid = 0;
    c.stats = {};
}
}
extern "C" int rr64_terrain_streaming_bounded(unsigned char *m) {
    using namespace rr64::engine;
    unsigned grid = 0, width = 0;
    if (!m || !rr64_world_distance_enabled() || !rr64_draw_distance_enabled() ||
        !rr64::world::static_scene(m) ||
        !read_u32(m, globals::terrain_map_width, width) || width != 70u ||
        !read_u32(m, globals::terrain_cell_grid, grid) ||
        !valid_guest_range(grid, 4900u * terrain::cell_stride))
        return 0;
    auto &c = rr64::world::cache();
    std::lock_guard lock(c.mutex);
    // This is the same once-per-ROM cache used by the draw pass. Prepare it
    // before the first streamed frame, so a cold or refused cache never makes
    // a promise to cover terrain which it cannot actually render.
    return rr64::world::initialize(c, m) && c.ready && !c.assets.cells.empty();
}
extern "C" unsigned rr64_terrain_streaming_range(unsigned char *m, unsigned original_bits) {
    return rr64::terrain_graphics::streaming_range(original_bits,
                                                  rr64_terrain_streaming_bounded(m) != 0);
}
extern "C" void rr64_world_terrain_begin(unsigned char *m) {
    auto &c = rr64::world::cache();
    std::lock_guard lock(c.mutex);
    unsigned epoch = 0;
    rr64::engine::read_u32(m, 0x800a1830u, epoch);
    rr64::world::CourseRegions::Mask all{};
    all.fill(true);
    unsigned initialViews = 0;
    if (rr64::engine::read_u32(m, 0x8009DB88u, initialViews) && initialViews >= 1 &&
        rr64_draw_distance_enabled())
        all.fill(false);
    rr64::world::course_frame().publish(m, epoch, all);
    c.drawing = false;
    c.course_scoped = false;
    c.allowed.fill(true);
    c.window.fill(true);
    c.stock.fill(false);
    c.stock_calls = {};
    c.replace_stock = false;
    c.stats.stock_cells = 0;
    c.stats.replaced_stock_cells = 0;
    c.stats.course_excluded_cells = 0;
    c.stats.stock_course_excluded = 0;
    if (!rr64_world_distance_enabled() ||
        !(rr64_draw_distance_enabled() && rr64::world::static_scene(m)))
        return;
    if (!rr64::engine::read_u32(m, rr64::engine::globals::terrain_cell_grid, c.grid) ||
        !rr64::engine::valid_guest_range(c.grid, 4900u * 16u))
        return;
    c.drawing = rr64::world::initialize(c, m);
    c.replace_stock = c.drawing &&
        rr64::engine::read_u32(m, 0x800a1830u, c.stock_epoch) &&
        rr64::engine::read_u32(m, rr64::engine::globals::active_viewport, c.stock_view) &&
        rr64::engine::read_u32(m, 0x8009cba4u, c.stock_graphics_slot);
    unsigned mode = 0, pending = 0;
    float x = 0, y = 0;
    using namespace rr64::engine;
    if (c.drawing && !c.assets.cells.empty() && read_u32(m, globals::main_mode, mode) &&
        read_u32(m, globals::pending_mode, pending) && mode >= 0x1cu && mode <= 0x1eu &&
        pending >= 0x1cu && pending <= 0x1eu &&
        read_float(m, globals::terrain_camera_position, x) &&
        read_float(m, globals::terrain_camera_position + 4u, y) && std::isfinite(x) &&
        std::isfinite(y)) {
        // Scope BEFORE stock rendering. Visibility during a jump must never
        // make a disconnected island eligible. Height is deliberately absent.
        double best = std::numeric_limits<double>::infinity();
        unsigned nearest = 0;
        for (const auto &cell : c.assets.cells) {
            const double dx = double(cell.authored_origin[0]) - x,
                         dy = double(cell.authored_origin[1]) - y;
            const double distance = dx * dx + dy * dy;
            if (distance < best) {
                best = distance;
                nearest = cell.cell_index;
            }
        }
        rr64::world::CourseRegions::Mask anchor{};
        anchor[nearest] = true;
        c.allowed = c.regions.select(anchor);
        c.course_scoped = true;
        for (const auto &cell : c.assets.cells)
            if (!c.allowed[cell.cell_index])
                ++c.stats.course_excluded_cells;
        rr64::world::course_frame().publish(m, epoch, c.allowed);
    }
#ifdef RR64_EXPERIMENTAL_COURSE
    if(rr64::experimental_course::installed()) {
        for(unsigned i=0;i<c.allowed.size();++i)
            c.allowed[i]=(rr64::experimental_course::active() || c.allowed[i]) && rr64::experimental_course::cell_allowed(i);
        c.course_scoped=true;
        rr64::world::course_frame().publish(m,epoch,c.allowed);
    }
#endif
    unsigned views = 0, viewIndex = 0, slot = 0;
    if (c.drawing && read_u32(m, 0x8009DB88u, views) && views >= 1 &&
        rr64_draw_distance_enabled()) {
        const double percent = rr64_draw_distance_percent();
        c.window.fill(false);
        Matrix4x4Snapshot camera{};
        double eyeX = 0, eyeY = 0;
        if (read_u32(m, globals::active_viewport, viewIndex) && viewIndex < 4 &&
            read_u32(m, globals::actor_render_buffer_slot, slot) && slot < 2 &&
            decode_n64_matrix(m, 0x800b6de8u + viewIndex * 0x180u + slot * 64u, camera) &&
            read_float(m, globals::terrain_camera_position, x) &&
            read_float(m, globals::terrain_camera_position + 4, y) &&
            rr64::world::terrain_eye(camera.values, eyeX, eyeY)) {
            eyeX += x;
            eyeY += y;
            auto &previous = c.windowHistory[viewIndex];
            // A slider reduction must take effect now, rather than retaining
            // the former range inside the motion-hysteresis margin.
            if (c.windowPercent[viewIndex] != percent) {
                previous.fill(false);
                c.windowPercent[viewIndex] = percent;
            }
            if (percent >= 100) {
                // Full range replaces the radial mask below. Record the setting
                // transition so lowering the slider still clears hysteresis.
                c.window.fill(true);
                previous = c.window;
            } else {
                double fullRadius = 0;
                for (const auto &cell : c.assets.cells) {
                    if (!c.allowed[cell.cell_index])
                        continue;
                    const double dx = std::max(
                        std::abs(cell.authored_origin[0] + double(cell.minimum[0]) - eyeX),
                        std::abs(cell.authored_origin[0] + double(cell.maximum[0]) - eyeX));
                    const double dy = std::max(
                        std::abs(cell.authored_origin[1] + double(cell.minimum[1]) - eyeY),
                        std::abs(cell.authored_origin[1] + double(cell.maximum[1]) - eyeY));
                    fullRadius = std::max(fullRadius, std::hypot(dx, dy));
                }
                const double radius = fullRadius * std::clamp(percent, 0.0, 100.0) / 100.0;
                for (const auto &cell : c.assets.cells) {
                    const auto i = cell.cell_index;
                    c.window[i] = rr64::world::window_cell(
                        eyeX, eyeY, cell.authored_origin[0] + double(cell.minimum[0]),
                        cell.authored_origin[1] + double(cell.minimum[1]),
                        cell.authored_origin[0] + double(cell.maximum[0]),
                        cell.authored_origin[1] + double(cell.maximum[1]), previous[i], radius);
                }
                previous = c.window;
            }
        }
        // Endpoint restores the pre-window full-map extension. Frustum and
        // course-island checks still exclude invisible or unrelated terrain.
        if (percent >= 100)
            c.window.fill(true);
        auto visible = c.allowed;
        for (unsigned i = 0; i < visible.size(); ++i)
            visible[i] = visible[i] && c.window[i];
        rr64::world::course_frame().publish(m, epoch, visible);
    }
}
extern "C" unsigned rr64_world_terrain_stock_state(unsigned char *m, unsigned record,
                                                   unsigned state) {
#ifdef RR64_EXPERIMENTAL_COURSE
    if(rr64::experimental_course::installed() && state==5u) {
        unsigned grid=0;
        if(rr64::engine::read_u32(m,rr64::engine::globals::terrain_cell_grid,grid) &&
           record>=grid && (record-grid)%16u==0 &&
           !rr64::experimental_course::cell_allowed((record-grid)/16u))return 0u;
    }
#endif
    auto &c = rr64::world::cache();
    std::lock_guard lock(c.mutex);
    if (state != 5u || !c.drawing || !c.course_scoped || c.mapping != m || record < c.grid ||
        (record - c.grid) % 16u)
        return state;
    const unsigned index = (record - c.grid) / 16u;
    if (index >= c.allowed.size() || c.allowed[index])
        return state;
    ++c.stats.stock_course_excluded;
    return 0u; // Change only the branch input, never the live cell's state.
}
extern "C" void rr64_world_terrain_observe(unsigned char *m, unsigned record) {
    using namespace rr64::engine;
    auto &c = rr64::world::cache();
    std::lock_guard lock(c.mutex);
    if (!c.drawing || c.mapping != m)
        return;
    if (record < c.grid || (record - c.grid) % 16u) {
        c.replace_stock = false;
        return;
    }
    const auto index = (record - c.grid) / 16u;
    if (index < c.stock.size() && !c.stock[index]) {
        c.stock[index] = true;
        ++c.stats.stock_cells;
    }
    if (!c.replace_stock)
        return;
    unsigned pointer = 0, base = 0, count = 0, opcode = 0, target = 0, root = 0;
    if (index >= c.stock.size() || c.cell_ordinals[index] == ~0u || !c.static_materials[index] ||
        c.stock_graphics_slot > 1u || c.stock_view >= 4u ||
        !read_u32(m, 0x800ac650u, pointer) ||
        !read_u32(m, 0x800ac658u + c.stock_graphics_slot * 4u, base) ||
        !read_u32(m, 0x800bc9a0u, count) || (count != 0x4650u && count != 0x36b0u) ||
        !valid_guest_range(base, 0x140u + count * 8u) || pointer < base + 0x148u ||
        pointer > base + 0x140u + count * 8u || (pointer & 7u) ||
        !read_u32(m, pointer - 8u, opcode) || opcode != 0xde000000u ||
        !read_u32(m, pointer - 4u, target) || !valid_guest_range(target, 8u) ||
        !read_u32(m, record, root) || !valid_guest_range(root, 0x58u)) {
        c.replace_stock = false;
        return;
    }
    const auto &cell = c.assets.cells[c.cell_ordinals[index]];
    for (unsigned i = 0; i < 6; ++i) {
        float value = 0;
        const float expected = i < 2u ? cell.authored_origin[i] : cell.root_quaternion[i - 2u];
        if (!read_float(m, root + 0x1cu + i * 4u, value) || value != expected) {
            c.replace_stock = false;
            return;
        }
    }
    auto &call = c.stock_calls[index];
    // Repeated observation of one call is harmless. Two draws of the same cell
    // cannot safely share one authored identity, so retain the entire stock pass.
    if (call.command && (call.command != pointer - 8u || call.target != target))
        c.replace_stock = false;
    else
        call = {pointer - 8u, target, 0u};
}
extern "C" void rr64_world_terrain_draw(unsigned char *m) {
    using namespace rr64::world;
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (!c.drawing || c.mapping != m || !rr64_world_distance_enabled() ||
        !(rr64_draw_distance_enabled() && rr64::world::static_scene(m)))
        return;
    c.drawing = false;
    unsigned slot = 0, graphics_slot = 0, epoch = 0, pointer = 0, camera_index = 0;
    float x = 0, y = 0;
    Matrix view{}, projection{};
    if (!frame_context(m, slot, graphics_slot, epoch, pointer, camera_index, view, projection, x,
                       y)) {
        ++c.stats.refusals;
        return;
    }
    const auto &allowed = c.allowed;
    auto &f = c.frames[graphics_slot];
    if (f.issued[camera_index] && f.last_epoch[camera_index] == epoch) {
        ++c.stats.refusals;
        return;
    }
    auto &pending = f.pending[camera_index];
    pending.clear();
    auto *evidence = capture_view(c, m, camera_index, epoch, x, y);
    if (evidence) {
        evidence->view = view;
        evidence->projection = projection;
        evidence->view_width = float(rr64::view_width.load(std::memory_order_relaxed));
    }
    const WorldFrustum frustum(view, projection);
    const unsigned commands = f.commands + camera_index * frame_bytes;
    const unsigned matrices = f.matrices + camera_index * frame_bytes;
    // Prepare every stock replacement before changing a single original call.
    // Mixed stock/compiled materials could inherit stale texture/tile state.
    bool replace_stock = c.replace_stock && c.stock_epoch == epoch &&
        c.stock_view == camera_index && c.stock_graphics_slot == graphics_slot &&
        std::uint64_t(c.assets.cells.size()) * 56u +
            std::uint64_t(c.stats.stock_cells) * 96u + 88u <= command_bytes;
    unsigned base = 0;
    rr64::engine::read_u32(m, 0x800ac658u + graphics_slot * 4u, base);
    for (unsigned i = 0; replace_stock && i < c.stock.size(); ++i) {
        if (!c.stock[i])
            continue;
        const auto &call = c.stock_calls[i];
        unsigned opcode = 0, target = 0;
        replace_stock = c.cell_ordinals[i] != ~0u && call.command >= base + 0x140u &&
            call.command < pointer - 8u &&
            rr64::engine::read_u32(m, call.command, opcode) && opcode == 0xde000000u &&
            rr64::engine::read_u32(m, call.command + 4u, target) && target == call.target;
    }
    unsigned dl = commands;
    command(m, dl, 0x64000019u, 0);
    command(m, dl, 0x6400001bu, 0);
    command(m, dl, 0x64000029u, 0);
    unsigned n = 0, triangles = 0;
    for (unsigned cellOrdinal = 0; cellOrdinal < c.assets.cells.size(); ++cellOrdinal) {
        const auto &cell = c.assets.cells[cellOrdinal];
        if (c.stock[cell.cell_index] || !allowed[cell.cell_index] || !c.window[cell.cell_index] ||
            !cell.triangles)
            continue;
        if (!frustum.intersectsTerrain(c.bounds[cellOrdinal], cell.authored_origin[0] - x,
                                       cell.authored_origin[1] - y))
            continue;
        if (evidence)
            evidence->extended_last[cell.cell_index / 64] |= 1ull << (cell.cell_index % 64);
        const unsigned address = matrices + n * 64u;
        cell_commands(m, dl, address, f.assets, camera_index, cell, x, y);
        ++n;
        triangles += cell.triangles;
    }
    command(m, dl, 0xe7000000u, 0);
    command(m, dl, 0x6400002au, 0);
    command(m, dl, 0x6400001cu, 0);
    command(m, dl, 0x6400001au, 0);
    command(m, dl, 0x6400002cu, 0);
    command(m, dl, 0xe0525464u, 0x20000000u);
    command(m, dl, 0xfa000000u, 0);
    command(m, dl, 0xdf000000u, 0);
    unsigned replaced = 0;
    if (replace_stock) {
        for (const auto &cell : c.assets.cells) {
            if (!c.stock[cell.cell_index])
                continue;
            auto &call = c.stock_calls[cell.cell_index];
            call.replacement = dl;
            command(m, dl, 0xe0525464u, 0x10000064u);
            command(m, dl, 0x6400002cu, 1u);
            for (unsigned op : {0x19u, 0x1bu, 0x29u})
                command(m, dl, 0x64000000u | op, 0u);
            cell_commands(m, dl, matrices + (n + replaced) * 64u, f.assets,
                          camera_index, cell, x, y);
            command(m, dl, 0xe7000000u, 0u);
            for (unsigned op : {0x2au, 0x1cu, 0x1au})
                command(m, dl, 0x64000000u | op, 0u);
            command(m, dl, 0x6400002cu, 0u);
            command(m, dl, 0xe0525464u, 0x20000000u);
            command(m, dl, 0xdf000000u, 0u);
            pending.push_back(call);
            ++replaced;
        }
    }
    if (evidence) {
        evidence->extended = n;
        evidence->triangles = triangles;
    }
    if (!n && !replaced) {
        c.stats.visible_cells = 0;
        c.stats.drawn_triangles = 0;
        return;
    }
    // Replace the just-written FA reset with a three-command bridge. Exactly
    // 16 bytes are added, inside the certified current original Gfx allocation.
    unsigned bridge = pointer;
    if (n) {
        bridge -= 8u;
        command(m, bridge, 0xe0525464u, 0x10000064u);
        command(m, bridge, 0x6400002cu, 1u);
        command(m, bridge, 0xde000000u, commands);
    }
    // Stock calls remain untouched until the completed frame proves enough
    // unused guest storage for their extended-address trampolines.
    word(m, 0x800ac650u, bridge);
    f.issued[camera_index] = true;
    f.last_epoch[camera_index] = epoch;
    c.stats.visible_cells = n;
    c.stats.drawn_triangles = triangles;
    ++c.stats.frames;
}
extern "C" void rr64_world_terrain_finalize(unsigned char *m, unsigned submitted_words) {
    using namespace rr64::world;
    using namespace rr64::engine;
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    unsigned gfx = 0, epoch = 0, base = 0, active_base = 0, count = 0, cursor = 0;
    if (c.mapping != m || !c.ready || !read_u32(m, 0x8009cba4u, gfx) || gfx > 1u)
        return;
    auto &f = c.frames[gfx];
    const auto discard = [&] { for (auto &pending : f.pending) pending.clear(); };
    unsigned sync = 0, sync_value = 0, end = 0, end_value = 0;
    if (!read_u32(m, 0x800a1830u, epoch) ||
        !read_u32(m, 0x800ac658u + gfx * 4u, base) ||
        !read_u32(m, 0x8009cb90u, active_base) || active_base != base ||
        !read_u32(m, 0x800bc9a0u, count) || (count != 0x4650u && count != 0x36b0u) ||
        !valid_guest_range(base, 0x140u + count * 8u) ||
        submitted_words < 4u || submitted_words > count || (submitted_words & 1u) ||
        !read_u32(m, 0x800ac650u, cursor) ||
        cursor < base + 0x140u + submitted_words * 4u ||
        cursor > base + 0x140u + count * 8u || (cursor & 7u)) {
        discard();
        return;
    }
    const unsigned submitted_end = base + 0x140u + submitted_words * 4u;
    if (!read_u32(m, submitted_end - 16u, sync) || sync != 0xe9000000u ||
        !read_u32(m, submitted_end - 12u, sync_value) || sync_value ||
        !read_u32(m, submitted_end - 8u, end) || end != 0xdf000000u ||
        !read_u32(m, submitted_end - 4u, end_value) || end_value) {
        discard();
        return;
    }
    for (unsigned view = 0; view < f.pending.size(); ++view) {
        auto &pending = f.pending[view];
        bool valid = f.issued[view] && f.last_epoch[view] == epoch &&
            pending.size() * 24u <= base + 0x140u + count * 8u - cursor;
        for (const auto &call : pending) {
            unsigned opcode = 0, target = 0;
            valid = valid && call.command >= base + 0x140u &&
                call.command + 8u <= submitted_end - 16u &&
                read_u32(m, call.command, opcode) && opcode == 0xde000000u &&
                read_u32(m, call.command + 4u, target) && target == call.target;
        }
        if (valid) {
            for (const auto &call : pending) {
                // The original main-list DF stays in place. Only stock calls
                // enter this tail; no later actor/scenery commands can collide.
                word(m, call.command + 4u, cursor);
                command(m, cursor, 0xe0525464u, 0x10000064u);
                command(m, cursor, 0x6400002cu, 1u);
                command(m, cursor, 0xde010000u, call.replacement);
            }
            c.stats.replaced_stock_cells += unsigned(pending.size());
        }
        pending.clear();
    }
    // Share the remaining tail with other finalizers. The original submission
    // length was already computed by 8000B00C and remains unchanged.
    word(m, 0x800ac650u, cursor);
}
