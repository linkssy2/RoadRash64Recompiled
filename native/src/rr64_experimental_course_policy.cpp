#include "rr64_experimental_course.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_race_pack.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>

extern "C" void rr64_course_progress_log(const char *format, ...);

namespace {
void load_progress(unsigned char *memory, const char *phase, unsigned destination) {
    static const bool enabled = [] {
        const char *value = std::getenv("RR64_COURSE_DIAGNOSTICS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    if (!enabled || !memory) return;
    static thread_local unsigned reports = 0;
    if (reports >= 128) return;
    ++reports;
    unsigned mode = 0, pending = 0, route = 0, records = 0;
    rr64::engine::read_u32(memory, rr64::engine::globals::main_mode, mode);
    rr64::engine::read_u32(memory, rr64::engine::globals::pending_mode, pending);
    rr64::engine::read_u32(memory, 0x800A6544u, route);
    rr64::engine::read_u32(memory, 0x800A6540u, records);
    const auto index = rr64::race_pack::selected_course();
    const auto entries = rr64::race_pack::menu_courses();
    const auto id = index && *index < entries.size() ? entries[*index].course_id : std::string_view("stock");
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    rr64_course_progress_log("[RR64-COURSE-PROGRESS] phase=%s time-ns=%lld mode=%u pending=%u destination=%u selected=%d course=%.*s active=%u route=%08X records=%u sample=%u\n",
             phase, static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
             mode, pending, destination, index ? static_cast<int>(*index) : -1,
             static_cast<int>(id.size()), id.data(), unsigned(rr64::experimental_course::active()), route, records, reports);
}
struct FloorIndices {
    unsigned x = 255, z = 255, sub_x = 0, sub_z = 0;
};

FloorIndices floor_indices(unsigned char *memory, unsigned query) {
    using namespace rr64::engine;
    float x = 0, z = 0;
    FloorIndices result;
    if (!read_float(memory, query, x) || !read_float(memory, query + 4, z) ||
        !std::isfinite(x) || !std::isfinite(z))
        return result;
    // Imported packs use the native 70x70 grid, 1000 raw units per cell.
    // Adding 35000 in float can round a point on the left of an edge onto
    // that edge. Compute indices in double; leave the actual query untouched.
    const double cell_x = std::floor(double(x) / 1000.0);
    const double cell_z = std::floor(double(z) / 1000.0);
    const double gx = cell_x + 35.0;
    const double gz = cell_z + 35.0;
    if (gx < 0 || gx >= 70 || gz < 0 || gz >= 70)
        return result;
    result.x = static_cast<unsigned>(gx);
    result.z = static_cast<unsigned>(gz);
    // Direct floor divisions also retain the side of zero for tiny negative
    // floats, where even a double addition to a large origin would round away.
    result.sub_x = static_cast<unsigned>(std::floor(double(x) / 125.0) - cell_x * 8.0);
    result.sub_z = static_cast<unsigned>(std::floor(double(z) / 125.0) - cell_z * 8.0);
    return result;
}
}

extern "C" void rr64_experimental_course_floor_indices(unsigned char *memory, void *opaque) {
    if (!memory || !opaque || !rr64::experimental_course::active())
        return;
    auto &context = *static_cast<recomp_context *>(opaque);
    const auto indices = floor_indices(memory, static_cast<unsigned>(context.r8));
    context.r9 = indices.x;
    context.r3 = indices.z;
}

extern "C" void rr64_experimental_course_floor_subindices(unsigned char *memory, void *opaque) {
    if (!memory || !opaque || !rr64::experimental_course::active())
        return;
    auto &context = *static_cast<recomp_context *>(opaque);
    const auto indices = floor_indices(memory, static_cast<unsigned>(context.r18));
    context.r7 = indices.sub_x;
    context.r6 = indices.sub_z;
}

// The combined bank keeps original and imported cells resident together. The
// native floor lookup must use the same owner filter as drawing, or leaving an
// imported track could collide with an invisible original-world floor.
extern "C" int rr64_experimental_course_floor_cell_allowed(unsigned cell) {
    return !rr64::experimental_course::installed() ||
           rr64::experimental_course::cell_allowed(cell);
}

// Pack selection does not change native mode IDs, player ownership or Custom
// Cop state. Only terrain/route ownership changes at the native loading boundary.
extern "C" void rr64_experimental_course_mode(unsigned char *memory, void *opaque) noexcept(false) {
    if (!rr64::experimental_course::installed() || !opaque)
        return;
    const auto &context = *static_cast<recomp_context *>(opaque);
    const unsigned destination = static_cast<unsigned>(context.r5);
    load_progress(memory, "mode-request", destination);
    // Mode 57 (0x39) is the native transition/fade, not a destination scene.
    // 48564 saves the real destination in 800A214C and inserts this mode;
    // 48400 later requests that saved destination. In particular, race setup
    // 19/29 goes through 57 after route construction. Clearing here restores
    // the stock descriptor before the imported riders can be constructed.
    // Let the real destination below decide whether to retain or unload it.
    if (destination == 57)
        return;
    if (destination == 17 || destination == 27) {
        load_progress(memory, "selection-load-begin", destination);
        rr64::experimental_course::restore_descriptor(memory);
        rr64::experimental_course::load_selection();
        load_progress(memory, "selection-load-end", destination);
    } // Thrash / shared multiplayer load.
    else if (!((destination >= 17 && destination <= 21) ||
               (destination >= 27 && destination <= 31))) {
        rr64::experimental_course::restore_descriptor(memory);
        rr64::experimental_course::load_stock();
        load_progress(memory, "stock-selected", destination);
    } // Campaign, attract and menu backgrounds.
}

extern "C" void rr64_experimental_course_prepare_race(unsigned char *memory) noexcept(false) {
    if (!memory || !rr64::experimental_course::active())
        return;
    using namespace rr64::engine;
    // This checkpoint is reached by the native setup after the route-loading
    // callback; it does not imply that terrain or renderer setup has finished.
    load_progress(memory, "race-setup-enter", 0);
    // Imported courses have their own route but no RR64 traffic/pedestrian/police
    // road graph. Do not spawn original-map actors against unrelated geometry.
    // Human cop roles, AI race entrants and saved menu preferences stay intact.
    write_u32(memory, 0x800A6570, 0);
    write_u16(memory, 0x800D8558, 0);
    write_u32(memory, 0x800A74B4, 0);
    write_u32(memory, 0x800A74B8, 0);
    // The native event scheduler uses both vehicle densities (+3C and +40).
    // Clearing only +40 leaves the +3C traffic stream eligible to spawn cars.
    write_u32(memory, 0x800D855C, 0);
    write_u32(memory, 0x800D8560, 0);
    write_u32(memory, 0x800D8564, 0);
    write_u16(memory, 0x800D7682, 0); // The source route is authored forward.
    load_progress(memory, "race-setup-end", 0);
}
