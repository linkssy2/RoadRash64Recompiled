#include "rr64_experimental_course_route.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_custom_cop.hpp"
#include "rr64_netplay.hpp"
#include "rr64_online_flow.hpp"
#include "rr64_prediction_rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

extern "C" void func_8001BDF8(unsigned char*, recomp_context*);
extern "C" void func_8001C084(unsigned char*, recomp_context*);

// The core owns this immutable view for the entire activation. It must not
// change packs while the guest game thread is building a course.
namespace rr64::experimental_course {
bool active() noexcept;
const RouteData* route_data() noexcept;
}

namespace {
using namespace rr64::engine;
using rr64::experimental_course::RouteData;
constexpr unsigned route_pointer = 0x800A6544;
constexpr unsigned route_count = 0x800A6540;
constexpr unsigned descriptor_pointer = 0x800D7620;
constexpr unsigned heap_zero_pointer = 0x800BBD00;
constexpr unsigned record_bytes = 16;
constexpr unsigned maximum_records = 2053; // Private converter: <=1024 curves.
struct DescriptorCoordinates {
    unsigned char* memory = nullptr;
    unsigned address = 0;
    std::array<unsigned, 6> original{}, written{};
    std::uint16_t original_lap_enabled = 0;
};
// Owned by the guest initialization/mode thread, never a frame worker.
DescriptorCoordinates descriptor_coordinates;

[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(message);
}

unsigned word(unsigned char* memory, unsigned address) {
    unsigned value = 0;
    if (!read_u32(memory, address, value)) fail("Experimental course: invalid guest address");
    return value;
}

unsigned big_word(const unsigned char* p) {
    return (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) |
           (unsigned(p[2]) << 8) | unsigned(p[3]);
}

float number(const unsigned char* p) {
    const unsigned bits = big_word(p);
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

float guest_float(unsigned char* memory, unsigned address) {
    float result = 0;
    if (!read_float(memory, address, result) || !std::isfinite(result))
        fail("Imported course: invalid guest coordinate");
    return result;
}

float distance(float ax, float az, float bx, float bz) {
    const float dx = ax - bx, dz = az - bz;
    return std::sqrt(dx * dx + dz * dz);
}

float length(const unsigned char* a) {
    const float ax = number(a + 8), az = number(a + 12);
    const float cx = number(a + 24), cz = number(a + 28);
    const float bx = number(a + 40), bz = number(a + 44);
    const float mx = (ax + cx) * .5f, mz = (az + cz) * .5f;
    const float nx = (bx + cx) * .5f, nz = (bz + cz) * .5f;
    return (distance(ax, az, mx, mz) + distance(mx, mz, nx, nz)) + distance(nx, nz, bx, bz);
}

bool close(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= .001f + std::abs(b) * .000002f;
}

void validate(const RouteData& data) {
    if (!data.records_be || data.record_count < 11 || data.record_count > maximum_records ||
        !(data.record_count & 1) || data.byte_count != data.record_count * record_bytes ||
        data.wrap_segment != data.record_count - 5 || data.finish_segment != data.wrap_segment ||
        data.initial_adjustment != 0 ||
        (data.prior_laps_required != 0 && data.prior_laps_required != 2 && data.prior_laps_required != 6) ||
        !std::isfinite(data.finish_parameter) || data.finish_parameter <= 0 || data.finish_parameter >= 1)
        fail("Experimental course: invalid closed-route metadata");
    for (unsigned i = 0; i < data.record_count; ++i) {
        const auto* p = data.records_be + i * record_bytes;
        const unsigned kind = i + 1 == data.record_count ? 3 : ((i & 1) ? 4 : 1);
        if (p[0] != kind || p[1] || p[2] || p[3] || !p[4] || !p[5] || p[6] || p[7] ||
            !std::isfinite(number(p + 8)) || !std::isfinite(number(p + 12)))
            fail("Experimental course: invalid route record");
    }
    if (std::memcmp(data.records_be, data.records_be + data.wrap_segment * record_bytes, 3 * record_bytes))
        fail("Experimental course: circuit seam does not match");
    float period = 0;
    for (unsigned segment = 0; segment < data.wrap_segment; segment += 2) {
        const float curve_length = length(data.records_be + segment * record_bytes);
        if (!std::isfinite(curve_length) || curve_length < .01f)
            fail("Experimental course: degenerate route curve");
        period += curve_length;
    }
    const float threshold = period + data.finish_parameter * length(data.records_be);
    const float finish = threshold + data.prior_laps_required * period;
    if (!close(data.lap_period, period) || !close(data.lap_threshold, threshold) ||
        !close(data.finish_threshold, finish))
        fail("Experimental course: inconsistent route lengths");
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(data.start[i]) || !std::isfinite(data.finish[i]))
            fail("Experimental course: invalid start or finish pose");
    }
    if ((data.record_heights == nullptr) != (data.height_count == 0) ||
        (data.height_count && data.height_count != data.record_count) ||
        (data.spawn_count && (data.spawn_count != 14 || !data.record_heights)))
        fail("Imported course: invalid height/grid metadata");
    for (unsigned i = 0; i < data.height_count; ++i)
        if (!std::isfinite(data.record_heights[i]))
            fail("Imported course: nonfinite route height");
    if (data.record_heights && std::memcmp(data.record_heights,
            data.record_heights + data.wrap_segment, 3 * sizeof(float)))
        fail("Imported course: height seam does not match");
    if ((data.recovery_support == nullptr) != (data.recovery_support_count == 0) ||
        (data.recovery_support_count &&
         (data.recovery_support_count != data.wrap_segment / 2 || !data.record_heights)))
        fail("Imported course: invalid recovery-support metadata");
    if (!std::isfinite(data.fall_floor) || std::abs(data.fall_floor) >= 8192)
        fail("Imported course: invalid void floor");
    unsigned supported = 0;
    for (unsigned i = 0; i < data.recovery_support_count; ++i) {
        if (data.recovery_support[i] > 1)
            fail("Imported course: unknown recovery-support value");
        supported += data.recovery_support[i];
    }
    if (data.recovery_support_count && !supported)
        fail("Imported course: no supported recovery curve");
    for (unsigned i = 0; i < data.spawn_count; ++i) {
        const auto& p = data.grid[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.z) ||
            !std::isfinite(p.height) || !std::isfinite(p.heading))
            fail("Imported course: nonfinite grid pose");
    }
    const float t = data.finish_parameter, u = 1 - t;
    for (unsigned i = 0; i < 2; ++i) {
        const float a = number(data.records_be + 8 + 4 * i);
        const float c = number(data.records_be + 24 + 4 * i);
        const float b = number(data.records_be + 40 + 4 * i);
        if (!close(data.finish[i], u * u * a + 2 * u * t * c + t * t * b))
            fail("Experimental course: finish pose misses its route curve");
    }
}

struct Position { double x, z, height; };
struct Fall {
    unsigned char* memory = nullptr;
    const RouteData* course = nullptr;
    unsigned bike = 0, route = 0, recovery = 0, body_position = 0;
    float began = 0, latest = 0, settled = -1;
    Position start{}, target{};
    bool ready = false;
};
// Only live authoritative guest updates own this state; isolated prediction
// never advances it. Resetting/reloading a descriptor also resets all timers.
std::array<Fall, kMaximumRacers> falls{};
Position point(const RouteData& data, unsigned segment, double t) {
    const auto* records = data.records_be + segment * record_bytes;
    const double u = 1 - t;
    const auto value = [&](unsigned offset) {
        return u*u*number(records + offset) + 2*u*t*number(records + 16 + offset) +
               t*t*number(records + 32 + offset);
    };
    const double height = data.record_heights ?
        u*u*data.record_heights[segment] + 2*u*t*data.record_heights[segment + 1] +
        t*t*data.record_heights[segment + 2] : 0;
    return {value(8), value(12), height};
}
double distance_squared(Position a, Position b) {
    const double x = a.x-b.x, z = a.z-b.z, h = a.height-b.height;
    return x*x + z*z + h*h;
}
struct Projection { unsigned segment = 0; double t = 0, error = 0; Position position{}; };
Projection nearest(const RouteData& data, Position target, bool recovery) {
    if (!std::isfinite(target.x) || !std::isfinite(target.z) || !std::isfinite(target.height))
        fail("Imported course: nonfinite route-query position");
    Projection best;
    best.error = std::numeric_limits<double>::infinity();
    // Segment zero is a duplicate approach and is not reachable by the native
    // circuit walker. Recovery must use its equivalent at wrap instead.
    const unsigned first = recovery ? 2 : 0;
    const unsigned last = recovery ? data.wrap_segment : data.wrap_segment - 2;
    for (unsigned segment = first; segment <= last; segment += 2) {
        // The source navigation path can travel through air or above a lower
        // road. Do not reconstruct a resting bike on those route coordinates.
        // The reachable seam at wrap is the same curve as authored curve zero.
        const unsigned curve = segment == data.wrap_segment ? 0 : segment / 2;
        if (recovery && data.recovery_support_count && !data.recovery_support[curve]) continue;
        double local_error = std::numeric_limits<double>::infinity(), parameter = 0;
        for (unsigned sample = 0; sample <= 32; ++sample) {
            const double t = sample / 32.0;
            const double error = distance_squared(point(data, segment, t), target);
            if (error < local_error) { local_error = error; parameter = t; }
        }
        double lo = std::max(0.0, parameter - 1.0/32),
               hi = std::min(1.0, parameter + 1.0/32);
        for (unsigned step = 0; step < 24; ++step) {
            const double a = (2*lo+hi)/3, b = (lo+2*hi)/3;
            if (distance_squared(point(data, segment, a), target) <
                distance_squared(point(data, segment, b), target)) hi = b;
            else lo = a;
        }
        const double refined = (lo+hi)*.5;
        const double refined_error = distance_squared(point(data, segment, refined), target);
        if (refined_error < local_error) { local_error = refined_error; parameter = refined; }
        if (local_error < best.error)
            best = {segment, parameter, local_error, point(data, segment, parameter)};
    }
    if (!std::isfinite(best.error)) fail("Imported course: no reachable recovery point");
    return best;
}
const RouteData* extended_route() {
    if (!rr64::experimental_course::active()) return nullptr;
    const auto* data = rr64::experimental_course::route_data();
    return data && data->record_heights && data->height_count == data->record_count ? data : nullptr;
}

struct HeapPlan {
    unsigned allocation = 0;
    bool owns_previous = false;
};

HeapPlan inspect_heap(unsigned char* memory, unsigned requested, unsigned previous) {
    // Native 1BDF8 has no safe out-of-memory return, and 1C084 assumes the
    // pointer exists in this heap. Validate the complete chain before either
    // call, including a terminal busy header, instead of risking those loops.
    HeapPlan plan;
    unsigned header = word(memory, heap_zero_pointer);
    for (unsigned steps = 0; steps < 65536; ++steps) {
        if ((header & 7) || !valid_guest_range(header, 8))
            fail("Experimental course: malformed native heap chain");
        const unsigned size = word(memory, header);
        std::uint16_t alignment = 0;
        std::uint8_t busy = 0, padding = 0;
        read_u16(memory, header + 4, alignment);
        read_u8(memory, header + 6, busy);
        read_u8(memory, header + 7, padding);
        if (!size) {
            if (!busy) fail("Experimental course: invalid native heap terminator");
            if (previous && !plan.owns_previous)
                fail("Experimental course: previous route is not owned by the native heap");
            if (!plan.allocation) fail("Experimental course: insufficient native route memory");
            return plan;
        }
        const std::uint64_t payload = std::uint64_t(header) + 8 + padding;
        if (!alignment || alignment > 256 || (alignment & (alignment - 1)) ||
            padding >= alignment || (payload & (alignment - 1)) || (size & 7) ||
            payload > kRdramEnd || size > kRdramEnd - payload)
            fail("Experimental course: invalid native heap block");
        if (busy && payload == previous) plan.owns_previous = true;
        if (!busy && size >= requested && !plan.allocation)
            plan.allocation = static_cast<unsigned>(payload);
        header = static_cast<unsigned>(payload + size);
    }
    fail("Experimental course: native heap chain exceeds the validation bound");
}

struct RestoreContext {
    recomp_context& context;
    recomp_context saved;
    explicit RestoreContext(recomp_context& c) : context(c), saved(c) {}
    ~RestoreContext() { context = saved; }
};
} // namespace

void rr64::experimental_course::validate_route(const RouteData& data) {
    validate(data);
}

void rr64::experimental_course::reset_descriptor() noexcept {
    descriptor_coordinates = {};
    falls = {};
}

void rr64::experimental_course::restore_descriptor(unsigned char* memory) noexcept {
    const auto saved = descriptor_coordinates;
    reset_descriptor();
    if (!memory || saved.memory != memory || !valid_guest_range(saved.address, 24)) return;
    for (unsigned i = 0; i < saved.written.size(); ++i) {
        unsigned current = 0;
        if (!read_u32(memory, saved.address + i * 4, current) || current != saved.written[i])
            return;
    }
    for (unsigned i = 0; i < saved.original.size(); ++i)
        write_u32(memory, saved.address + i * 4, saved.original[i]);
    std::uint16_t lap_enabled = 0;
    if (read_u16(memory, 0x800D7680, lap_enabled) && lap_enabled == 1) {
        auto* rdram = memory;
        MEM_H(0, guest_address(0x800D7680)) = saved.original_lap_enabled;
    }
}

extern "C" int rr64_experimental_course_build_route(unsigned char* memory, void* opaque) noexcept(false) {
    if (!rr64::experimental_course::active()) return 0;
    const RouteData* data = rr64::experimental_course::route_data();
    if (!memory || !opaque || !data) fail("Experimental course: missing active route data");
    validate(*data);
    const unsigned descriptor = word(memory, descriptor_pointer);
    if ((descriptor & 3) || !valid_guest_range(descriptor, 0x4C))
        fail("Experimental course: invalid race descriptor");
    const unsigned previous = word(memory, route_pointer);
    const unsigned bytes = data->byte_count + record_bytes;
    const auto heap = inspect_heap(memory, bytes, previous);

    auto& context = *static_cast<recomp_context*>(opaque);
    RestoreContext saved(context);
    context.r4 = 0;
    context.r5 = bytes;
    func_8001BDF8(memory, &context);
    const unsigned allocation = static_cast<unsigned>(context.r2);
    if (allocation != heap.allocation || !valid_guest_range(allocation, bytes))
        fail("Experimental course: native route allocation changed unexpectedly");
    unsigned char* rdram = memory; // Required name for the guest memory macros.
    for (unsigned i = 0; i < data->byte_count; ++i)
        MEM_B(i, guest_address(allocation)) = data->records_be[i];
    for (unsigned i = data->byte_count; i < bytes; ++i)
        MEM_B(i, guest_address(allocation)) = 0;

    // All validation/allocation finished before publishing the replacement.
    // The native allocator retains ownership; normal rebuild/cleanup may free
    // this route exactly as it would a stock 66040 allocation.
    write_u32(memory, route_pointer, allocation);
    write_u32(memory, route_count, data->record_count);
    write_u32(memory, 0x800D7624, 1);
    write_u32(memory, 0x800D7628, data->finish_segment);
    write_float(memory, 0x800D762C, data->lap_threshold);
    write_float(memory, 0x800D7630, data->lap_period);
    unsigned prior_laps = data->prior_laps_required;
    if (data->native_laps) {
        const unsigned type = word(memory, 0x8009EAE4);
        // Custom Cop replaces the native type with eight. Its shared mode
        // keeps the pack's normal three-lap baseline, without changing the mode.
        prior_laps = type == 4 ? 6 : type == 3 ? 2 : type == 8 ? prior_laps : 0;
    }
    write_float(memory, 0x800D7634, data->lap_threshold + prior_laps * data->lap_period);
    write_float(memory, 0x800D7638, data->finish_parameter);
    write_u32(memory, 0x800D763C, data->wrap_segment);
    write_u32(memory, 0x800D7640, data->initial_adjustment);
    write_u32(memory, 0x800D7644, prior_laps);
    rr64::experimental_course::restore_descriptor(memory);
    descriptor_coordinates.memory = memory;
    descriptor_coordinates.address = descriptor + 0x34;
    read_u16(memory, 0x800D7680, descriptor_coordinates.original_lap_enabled);
    for (unsigned i = 0; i < 6; ++i)
        descriptor_coordinates.original[i] = word(memory, descriptor + 0x34 + i * 4);
    for (unsigned i = 0; i < 3; ++i) {
        write_float(memory, descriptor + 0x34 + i * 4, data->start[i]);
        write_float(memory, descriptor + 0x40 + i * 4, data->finish[i]);
    }
    for (unsigned i = 0; i < 6; ++i)
        descriptor_coordinates.written[i] = word(memory, descriptor + 0x34 + i * 4);
    // Tag and Deathmatch use points, so retain their native disabled lap flag.
    // Custom Cop still needs circuit finishes for racers to escape.
    const unsigned type = word(memory, 0x8009EAE4);
    if ((type != 6 && type != 7) || rr64_custom_cop_enabled())
        MEM_H(0, guest_address(0x800D7680)) = 1;
    if (previous) {
        context.r4 = 0;
        context.r5 = guest_address(previous);
        func_8001C084(memory, &context);
    }
    return 1;
}

extern "C" void rr64_experimental_course_spawn(unsigned char* memory, void* opaque) noexcept(false) {
    const auto* data = extended_route();
    if (!data || !data->spawn_count || !memory || !opaque) return;
    auto& context = *static_cast<recomp_context*>(opaque);
    const unsigned guest = unsigned(context.r21), stack = unsigned(context.r29);
    if (guest >= 14 || !valid_guest_range(stack, 0x80))
        fail("Imported course: invalid native spawn context");
    const auto status = rr64::netplay::get_status();
    const unsigned rank = status.active && status.connected ?
        rr64::online_flow::mapped_slot(guest, status.local_slot, status.replicated_riders) : guest;
    if (rank >= data->spawn_count) fail("Imported course: spawn outside checked grid");
    const unsigned actor = 0x800D8570 + guest * 0x118;
    const unsigned state = word(memory, actor + 0xE8), bike = word(memory, actor + 0xE0);
    if (!valid_guest_range(state, 0x64) || !valid_guest_range(bike, 0x868))
        fail("Imported course: missing native spawn actor");
    const auto& pose = data->grid[rank];
    const auto projected = nearest(*data, {pose.x, pose.z, pose.height}, false);
    float prefix = 0;
    for (unsigned segment = 0; segment < projected.segment; segment += 2)
        prefix += length(data->records_be + segment * record_bytes);
    // Grid rows may precede the first approach curve. Seed negative distance,
    // not nearly a completed lap, while leaving native lap evaluation intact.
    if (projected.segment != 0) prefix -= data->lap_period;
    write_u32(memory, state, projected.segment);
    write_u32(memory, state + 4, 0);
    write_float(memory, state + 8, float(projected.t));
    write_float(memory, state + 0xC, length(data->records_be + projected.segment * record_bytes));
    write_float(memory, state + 0x20, prefix);
    write_float(memory, stack + 0x18, pose.x);
    write_float(memory, stack + 0x1C, pose.z);
    write_float(memory, stack + 0x20, pose.heading);
    write_float(memory, stack + 0x48, 0);
    write_float(memory, stack + 0x4C, 0);
    // 41090 rebuilds the complete wheel/body pose. This only seeds its floor
    // search with the intended road deck instead of the stock global height.
    write_float(memory, bike + 0x174, pose.height);
}

namespace {
bool progress_owner(unsigned char* memory, unsigned state) {
    const auto status = rr64::prediction::status_for_rules();
    for (unsigned slot = 0; slot < kMaximumRacers; ++slot) {
        const unsigned actor = 0x800D8570 + slot * 0x118;
        if (word(memory, actor + 0xE8) != state) continue;
        std::uint16_t ai = 1;
        read_u16(memory, actor + 0x26, ai);
        const unsigned controller = word(memory, actor + 8);
        const bool native_ai = ai == 1 && controller == 0xFFFFFFFF &&
            word(memory, actor + 0x20) != 7;
        // Authoritative simulation uses canonical actor slots, including human
        // riders beyond the four native controller slots. Replay uses its saved
        // rule view and its private RDRAM; there is no host-only lap cache.
        if (status.active && status.authoritative) {
            if (!status.connected) return false;
            if (status.authority_humans & (1u << slot)) return true;
            // Only the live host advances AI. Guest replicas and private human
            // prediction must not acquire ownership of their route state.
            return native_ai && status.is_host && status.phase == rr64::netplay::Phase::Race &&
                !rr64::prediction::active();
        }
        if (ai == 0 && controller < 4) return true;
        // Imported AI can reacquire a curve behind its saved position after an
        // ordinary road crash. The native forward-only accumulator interprets
        // that as almost a full circuit, falsely ranking it ahead of humans.
        // Use the same signed progress for locally simulated racers; stock
        // courses, police, temporary route records and online replicas retain
        // their existing paths.
        return native_ai && !status.active && !rr64::prediction::active();
    }
    return false;
}

int update_progress(unsigned char* memory, recomp_context& context, unsigned state,
                    unsigned target, float target_t, bool recovery) {
    if (!rr64::experimental_course::active()) return 0;
    const auto* data = rr64::experimental_course::route_data();
    if (!memory || !data || !valid_guest_range(state, 0x64) || !progress_owner(memory, state))
        return 0;
    const unsigned wrap = data->wrap_segment, old = word(memory, state);
    if (word(memory, route_count) != data->record_count || word(memory, 0x800D763C) != wrap ||
        (old & 1) || old > wrap || (target & 1) || target > wrap ||
        !std::isfinite(target_t))
        fail("Imported course: invalid race-progress route");
    const float old_t = std::clamp(guest_float(memory, state + 8), 0.0f, 1.0f);
    target_t = std::clamp(target_t, 0.0f, 1.0f);
    const float old_length = length(data->records_be + old * record_bytes);
    const float target_length = length(data->records_be + target * record_bytes);
    const float old_base = guest_float(memory, state + 0x20);
    const unsigned curves = wrap / 2, from = old == wrap ? 0 : old / 2,
                   to = target == wrap ? 0 : target / 2;
    int steps = int(to) - int(from);
    if (steps > int(curves / 2)) steps -= int(curves);
    if (steps < -int(curves / 2)) steps += int(curves);
    // 507B0 visits consecutive curves; its fifth advance returns failure at
    // 508C8..508D8. A larger jump, or the two equally long directions around
    // the circuit, is relocation/ambiguous projection, not driven traversal.
    const bool continuous = std::abs(steps) <= 4 && unsigned(std::abs(steps)) * 2 < curves;
    float base_delta = 0;
    if (continuous) {
        unsigned cursor = from;
        for (int n = 0; n < std::abs(steps); ++n) {
            if (steps > 0) {
                base_delta += length(data->records_be + cursor * 2 * record_bytes);
                cursor = (cursor + 1) % curves;
            } else {
                cursor = (cursor + curves - 1) % curves;
                base_delta -= length(data->records_be + cursor * 2 * record_bytes);
            }
        }
    }
    const double old_progress = double(old_base) + double(old_t) * old_length;
    double next_base = double(old_base) + base_delta;
    double displacement = next_base + double(target_t) * target_length - old_progress;
    const double next_progress = next_base + double(target_t) * target_length;
    if (!continuous || (recovery && displacement > 0 && next_progress >= data->lap_threshold)) {
        // Rejoining another branch/off-road section is not backwards travel.
        // Preserve the existing lap phase, rather than rounding every forward
        // rejoin down by one whole circuit. Zero and wrap name the same curve;
        // only consecutive traversal may add a new circuit at that seam.
        double prefix = 0, old_prefix = 0;
        for (unsigned curve = 0; curve < std::max(from, to); ++curve) {
            const double curve_length = length(data->records_be + curve * 2 * record_bytes);
            if (curve < to) prefix += curve_length;
            if (curve < from) old_prefix += curve_length;
        }
        const double phase = std::round((double(old_base) - old_prefix) / data->lap_period);
        next_base = prefix + phase * data->lap_period;
        // Neither a recovery nor an ambiguous projection may cross the finish
        // on its own. Keep the native lap evaluator and subsequent ordinary
        // forward circuit intact, including after a near-finish recovery.
        const double relocated = next_base + double(target_t) * target_length;
        if (relocated >= data->lap_threshold)
            next_base -= (std::floor((relocated - data->lap_threshold) / data->lap_period) + 1)
                         * data->lap_period;
        displacement = next_base + double(target_t) * target_length - old_progress;
    }
    write_u32(memory, state, target);
    write_float(memory, state + 8, target_t);
    write_float(memory, state + 0xC, target_length);
    write_float(memory, state + 0x20, float(next_base));
    // Preserve the native skipped-distance statistic for genuine multi-curve
    // changes. Native 68D0C reads +20, +8 and +C, then maintains completed laps
    // at +4 and the finish threshold used by 6EF50..6EF78.
    if (std::abs(steps) >= 2)
        write_float(memory, state + 0x10, guest_float(memory, state + 0x10) + float(displacement));
    context.f0.fl = float(displacement);
    return 1;
}
} // namespace

extern "C" int rr64_experimental_course_progress(unsigned char* memory, void* opaque) {
    if (!memory || !opaque || !rr64::experimental_course::active()) return 0;
    auto& context = *static_cast<recomp_context*>(opaque);
    if (unsigned(context.r4) != word(memory, route_pointer)) return 0;
    float t;
    const unsigned bits = unsigned(context.r7);
    std::memcpy(&t, &bits, sizeof(t));
    return update_progress(memory, context, unsigned(context.r5), unsigned(context.r6), t, false);
}

extern "C" int rr64_experimental_course_recovery_progress(unsigned char* memory, void* opaque) {
    if (!memory || !opaque || !rr64::experimental_course::active()) return 0;
    auto& context = *static_cast<recomp_context*>(opaque);
    const unsigned stack = unsigned(context.r29), state = unsigned(context.r16);
    if (!valid_guest_range(stack, 0x60) || word(memory, unsigned(context.r17) + 0xE8) != state)
        return 0;
    return update_progress(memory, context, state, word(memory, stack + 0x58),
                           guest_float(memory, stack + 0x5C), true);
}

extern "C" int rr64_experimental_course_recovery_point(unsigned char* memory, void* opaque) noexcept(false) {
    const auto* data = extended_route();
    if (!data || !memory || !opaque) return 0;
    auto& context = *static_cast<recomp_context*>(opaque);
    const unsigned actor = unsigned(context.r17), bike = unsigned(context.r21), stack = unsigned(context.r29);
    if (actor < 0x800D8570 || (actor - 0x800D8570) % 0x118 ||
        (actor - 0x800D8570) / 0x118 >= 14 || !valid_guest_range(bike, 0x868) ||
        !valid_guest_range(stack, 0xC0)) return 0;
    auto& fall = falls[(actor - 0x800D8570) / 0x118];
    const Position current{guest_float(memory, bike + 0x16C),
        guest_float(memory, bike + 0x170), guest_float(memory, bike + 0x174)};
    // A detached body can fall while its abandoned bike remains on the road.
    // Recover nearest the falling body in that case, not the old bike position.
    const bool replay = rr64::prediction::active();
    const bool falling = !replay && fall.ready && fall.memory == memory && fall.course == data && fall.bike == bike;
    const auto projected = nearest(*data, falling ? fall.target : current, true);
    if (!replay) fall = {};
    write_u32(memory, stack + 0x58, projected.segment);
    write_float(memory, stack + 0x5C, float(projected.t));
    write_float(memory, bike + 0x174, float(projected.position.height));
    context.r7 = guest_address(stack + 0x48);
    context.f24.fl = 0;
    context.f28.fl = 0;
    return 1;
}

extern "C" int rr64_experimental_course_fall_action(unsigned char* memory, unsigned actor) {
    // 0: preserve ordinary native recovery; 1: keep falling; 2: request the
    // existing native recovery path; 3: supported selected body, retain native
    // timers but ignore the other body's missing-floor flag; 4: healthy human
    // mounted on real ground, exempt only from native offroad recovery. No guest writes.
    const auto* data = extended_route();
    if (!data || !data->delayed_fall_recovery || !memory || actor < 0x800D8570 ||
        (actor - 0x800D8570) % 0x118 || (actor - 0x800D8570) / 0x118 >= falls.size()) return 0;
    auto& fall = falls[(actor - 0x800D8570) / 0x118];
    const auto half = [&](unsigned address) { std::uint16_t v = 0; read_u16(memory, address, v); return v; };
    const unsigned bike = word(memory, actor + 0xE0), rider = word(memory, actor + 0xE4);
    const unsigned route = word(memory, actor + 0xE8);
    const bool replay = rr64::prediction::active();
    const auto clear = [&] { if (!replay) fall = {}; return 0; };
    if (!half(actor + 0x24) || !valid_guest_range(bike, 0x868) ||
        !valid_guest_range(rider, 0x5F0) || !valid_guest_range(route, 0x64) ||
        word(memory, bike + 4) != actor || word(memory, rider + 4) != actor ||
        word(memory, bike + rr64::engine::bike::rider_pointer) != rider ||
        word(memory, rider + rr64::engine::rider::bike_pointer) != bike ||
        (!half(route + 0x48) && !rr64_custom_cop_is_player(memory, actor)) ||
        half(route + 0x4C) || half(route + 0x4E) || half(route + 0x50))
        return clear();
    float health = 0;
    if (!read_float(memory, bike + 0x4F8, health) || !std::isfinite(health) || health < 0) return clear();
    if (!is_live_race_transition(word(memory, globals::main_mode), word(memory, globals::pending_mode)))
        return clear();
    if (half(globals::gameplay_pause_state)) return 0;
    const bool detached = !half(bike + rr64::engine::bike::rider_attached);
    const bool unsupported = detached ? half(rider + 0x308) != 0 : half(bike + 0x7FA) != 0;
    const bool other_unsupported = detached ? half(bike + 0x7FA) != 0 : half(rider + 0x308) != 0;
    const auto supported = [&] {
        clear();
        const auto rules = rr64::prediction::status_for_rules();
        const unsigned slot = (actor - 0x800D8570) / 0x118;
        const bool human = rules.active && rules.authoritative
            ? rules.connected && (rules.authority_humans & (1u << slot)) != 0
            : !half(actor + 0x26) && word(memory, actor + 8) < 4;
        // Imported Thrash can have one human without local_players being active.
        // Keep its real-ground exploration consistent with split-screen/online
        // roaming. Detached riders and native crash/terminal timers still run.
        if (human && !detached && half(rider + 0x57C) && !half(bike + 0x7F6))
            return 4;
        // 6B2FC updates/detaches the bike, 6B304 runs recovery, and only then
        // 6B30C refreshes the detached rider's contact. The abandoned bike or
        // stale rider flag must not trigger 40044..4005C's immediate recovery
        // while our selected body is supported. Keep native crash timers.
        return other_unsupported ? 3 : 0;
    };
    const unsigned position = detached ? rider + 0x8C : bike + 0x16C;
    float xyz[3]{}, elapsed = 0, vertical_speed = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (!read_float(memory, position + i * 4, xyz[i]) || !std::isfinite(xyz[i])) return clear();
    if (!read_float(memory, position + 0x14, vertical_speed) || !std::isfinite(vertical_speed) ||
        !read_float(memory, 0x800D7670, elapsed) || !std::isfinite(elapsed) || elapsed < 0 || elapsed > 1000000)
        return clear();
    const Position here{xyz[0], xyz[1], xyz[2]};
    // 14DE4 reports a floor anywhere below the query; 7FA/308 are not contact
    // flags. Its accepted triangle pointer is query+34 (cleared on a miss),
    // with the selected height at query+8 in terrain units. Require proximity
    // before cancelling a fall or starting the post-impact clock. Otherwise a
    // lower platform visible at an airborne apex can trigger a midair reset.
    bool touching = unsupported && here.height <= data->fall_floor + 2.0;
    const auto near_floor = [&](unsigned query) {
        float height = 0;
        return word(memory, query + 0x34) && read_float(memory, query + 8, height) &&
            std::isfinite(height) && std::abs(height) < 32768 &&
            std::abs(here.height - double(height) * .25) <= 2.0;
    };
    if (!unsupported) {
        touching = detached ? near_floor(rider + 0x230) || near_floor(rider + 0x29C)
                            : near_floor(bike + 0x698) || near_floor(bike + 0x770) ||
                              near_floor(bike + 0x62C);
    }
    const unsigned recovery = word(memory, route + 0x40);
    // A newly ejected body must land for itself, even if its bike hit first.
    const bool same = fall.memory == memory && fall.course == data && fall.bike == bike &&
        fall.route == route && fall.recovery == recovery && fall.body_position == position &&
        elapsed >= fall.latest;
    // Route height is race navigation, not a boundary on explorable terrain.
    // Royal's real lower grass was previously treated as a settled fall and
    // warped back to its elevated road after two seconds. Accept finite native
    // contact at any elevation; a floor far below an airborne body does not
    // qualify, and a missing query at the synthetic void bottom still recovers.
    if (!unsupported && touching) return supported();
    // Clients/replay run identical native contact but only the live host can
    // decide a reset. Replay cannot consume a pending live body's target.
    if (replay) return 1;
    const auto status = rr64::netplay::get_physics_rules();
    if (status.active && (!status.connected || !status.authoritative || !status.is_host ||
                          status.phase != rr64::netplay::Phase::Race)) return 1;
    if (!same)
        fall = {memory, data, bike, route, recovery, position, elapsed, elapsed, -1, here, here, false};
    fall.latest = elapsed;
    fall.target = here;
    // Let native gravity/contact reach actual lower terrain or the course's
    // void floor first; a jump across a short gap must never start a warp.
    if (touching && std::abs(vertical_speed) <= 2.0f) {
        if (fall.settled < 0) fall.settled = elapsed;
    }
    // This is a post-impact delay, not a requirement to remain perfectly still.
    // Native ragdoll contact keeps applying small bouncing/standing impulses;
    // restarting the clock on each impulse can trap a healthy rider for 30s.
    // A supported road landing above already clears the entire pending fall.
    // A long void fall and gross coordinate excursion remain bounded safety
    // exits, not the normal two-second post-impact recovery rule.
    fall.ready = (fall.settled >= 0 && elapsed - fall.settled >= 2.0f) ||
                 elapsed - fall.began >= 30.0f || std::abs(here.x) >= 8750 ||
                 std::abs(here.z) >= 8750 || std::abs(here.height) >= 8192;
    return fall.ready ? 2 : 1;
}

extern "C" void rr64_experimental_course_missing_floor(unsigned char* memory, void* opaque, unsigned kind) {
    const auto* data = extended_route();
    if (!memory || !opaque || !data || !data->delayed_fall_recovery) return;
    auto& context = *static_cast<recomp_context*>(opaque);
    if (kind == 0) {
        // Native 3B450 through 3B478 forms three penetrations against a zero plane.
        // The remaining two wheel contacts are adjusted by kinds 3 and 4.
        context.f8.fl += data->fall_floor;
        context.f4.fl += data->fall_floor;
        context.f2.fl += data->fall_floor;
        context.f22.fl = data->fall_floor;
        // 3B888 normally aligns the entire bike instantly when support changes.
        // Losing real road support must enter ordinary airborne contact instead;
        // Kind 5 also rejects downward alignment when a lower floor reappears.
        // s6 is only the saved previous 7FA value, not a persistent guest flag.
        context.r22 = 1;
    } else if (kind == 1 || kind == 2) {
        std::uint16_t missing = 0;
        const unsigned rider = unsigned(context.r20);
        if (valid_guest_range(rider, 0x5F0) && read_u16(memory, rider + 0x308, missing) && missing) {
            if (kind == 1) context.f24.fl += data->fall_floor;
            else context.f22.fl += data->fall_floor;
        }
    } else if (kind == 3) {
        // 3B574: this branch is reached only for the missing front-wheel query.
        context.f2.fl = data->fall_floor;
    } else if (kind == 4) {
        // 3B6B8: zero-normal stores are already complete. Set rear support
        // height and penetration without changing its upward contact normal.
        context.f0.fl += data->fall_floor;
        context.f20.fl = data->fall_floor;
        write_float(memory, unsigned(context.r18) + 0x550, data->fall_floor);
    } else if (kind >= 6 && kind <= 9) {
        // The native partial-query miss paths initialize penetration to zero.
        // That is safe on continuous stock terrain, but at an imported ledge
        // it beats every genuinely negative airborne contact. In a wheelie,
        // the raised body probe then looks like an impact and 3C668 ejects the
        // rider. Give only the missed point the same bottom as a complete miss.
        // Later native object-contact overrides still supply real collisions.
        const unsigned bike = unsigned(context.r18), stack = unsigned(context.r29);
        if (!valid_guest_range(bike, 0x868) || !valid_guest_range(stack, 0xA0)) return;
        constexpr unsigned query_offsets[] = {0x5C0, 0x704, 0x770, 0x62C};
        constexpr unsigned depth_offsets[] = {0x18, 0x24, 0x28, 0x1C};
        const unsigned index = kind - 6, depth_address = stack + depth_offsets[index];
        // 14DE4 clears query+34 on failure. Accepted zero-depth ground contact
        // and the native disabled-probe sentinel (-10) must remain untouched.
        if (word(memory, bike + query_offsets[index] + 0x34) ||
            guest_float(memory, depth_address) != 0) return;
        float bottom = 0;
        switch (kind) {
        case 6: bottom = guest_float(memory, bike + 0x344) - guest_float(memory, bike + 0x304); break;
        case 7: bottom = guest_float(memory, stack + 0x88) - guest_float(memory, bike + 0x34); break;
        case 8: bottom = guest_float(memory, stack + 0x98) - guest_float(memory, bike + 0x38); break;
        case 9: bottom = guest_float(memory, bike + 0x40C) - guest_float(memory, bike + 0x3CC); break;
        }
        if (!std::isfinite(bottom) || !std::isfinite(data->fall_floor)) return;
        write_float(memory, depth_address, data->fall_floor - bottom);
        // Keep native wheel-height/normal estimates for steering and pitch.
        // A missing sample has no new slope; joining it to a far-away void
        // bottom would invent a steep ramp across the edge of a platform.
    } else if (kind == 5) {
        // 3B888: finding a lower platform changes 7FA before the bike has
        // actually landed. Native alignment would add the negative average
        // wheel penetration directly to its height. Let ordinary contact and
        // gravity handle that descent; retain upward penetration correction.
        const unsigned stack = unsigned(context.r29);
        float front = 0, rear = 0;
        if (context.r2 != context.r22 && valid_guest_range(stack, 0x20) &&
            read_float(memory, stack + 0x18, front) &&
            read_float(memory, stack + 0x1C, rear) &&
            std::isfinite(front) && std::isfinite(rear) &&
            std::isfinite(front + rear) && front + rear < 0)
            context.r22 = context.r2;
    }
}

extern "C" void rr64_experimental_course_ground_hint(unsigned char* memory, void* opaque) noexcept(false) {
    const auto* data = extended_route();
    if (!data || !memory || !opaque) return;
    auto& context = *static_cast<recomp_context*>(opaque);
    const unsigned stack = unsigned(context.r29), bike = unsigned(context.r18);
    if (!valid_guest_range(stack, 0xE8) || !valid_guest_range(bike, 0x868)) return;
    // Spawn/recovery already seeds bike.height with the chosen road deck.
    // Projecting each wheel onto the simplified route again can select a lower
    // segment (Choco's starting approach), making a valid grid spawn underground.
    // Keep that deck through both native wheel queries; 41090 still computes
    // the complete pose and contacts. Ordinary in-place reconstruction likewise
    // retains the bike's current height instead of pulling it toward the route.
    const float height = guest_float(memory, bike + 0x174);
    const float query_height = (height + 1) * 4;
    if (!std::isfinite(height) || !std::isfinite(query_height)) return;
    write_float(memory, stack + 0x18, query_height);
}
