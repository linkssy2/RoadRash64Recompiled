#include "rr64_traffic_roster.hpp"
#include "rr64_race_end_trace.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_replay.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_experimental_course.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
using namespace rr64;
constexpr unsigned capacity = 16;
constexpr unsigned drain_limit = 2;
constexpr unsigned actor_base = 0x800D8570;
constexpr unsigned actor_stride = 0x118;
constexpr std::array<unsigned, 5> counter_addresses{0x800D7648, 0x800D764C, 0x800D7658,
                                                    0x800D765C, 0x800D7678};

// Floating-point fields are copied as bits, including NaN/Inf, so diagnostics
// cannot hide corrupt values by replacing them with plausible zeroes.
struct Actor {
    unsigned controller = 0, role = 0, state = 0, bike = 0;
    unsigned segment = 0, lap = 0, parameter = 0, length = 0, base = 0;
    unsigned table_rank = 0, race_place = 0;
    unsigned health = 0, max_health = 0, crash_4cc = 0, x = 0, z = 0;
    std::uint16_t active = 0, ai = 0, eligible = 0, busted = 0, wrecked = 0;
    std::uint16_t finished = 0, flag_52 = 0, lockout = 0;
    bool state_valid = false, bike_valid = false;
};
struct Course {
    unsigned pointer = 0, count = 0, wrap = 0, prior_laps = 0;
    unsigned lap_threshold = 0, period = 0, finish_threshold = 0;
    unsigned finish_segment = 0, finish_parameter = 0, race_type = 0;
    bool operator==(const Course&) const = default;
};
// Reason bits are diagnostic observations, not rules that award a lap.
enum Reason : unsigned { Start = 1, Segment = 2, Lap = 4, Status = 8,
                         Recovery = 16, Identity = 32, MarkerCrossing = 64,
                         CoursePhysics = 128, Population = 256 };
struct WallContext {
    bool valid = false;
    unsigned actor = 0, kind = 0, course = 0, epoch = 0, frame = 0, native_frame = 0, elapsed = 0;
    unsigned event = 0, event_rows = 0, edge_stage = 0;
    rr64::race_end_trace::CourseWallContext query{};
};
struct WallObservation {
    unsigned checks = 0, static_hits = 0, dynamic_hits = 0, kind = 0, triangle = 0, hazard = 0;
    std::array<unsigned, 3> requested{}, resolved{}, normal{};
    unsigned normal_speed = 0;
    WallContext latest{};
};
struct Physics {
    // All floats retain their raw bits; interpreting them happens on the UI thread.
    std::array<unsigned, 3> position{}, velocity{}, predicted{}, predicted_velocity{}, radii{};
    std::array<unsigned, 3> rider_position{}, rider_velocity{};
    std::array<unsigned, 6> front{}, rear{}; // Query XY/height/aux-height, normal Z, surface.
    unsigned sphere_count = 0, bike_state = 0, recovery = 0, held = 0, changed = 0, pressed = 0;
    unsigned stick_x = 0, stick_y = 0, attached = 0, delta = 0;
    unsigned rider = 0, bike_missing_floor = 0, rider_missing_floor = 0;
    bool input_valid = false, rider_valid = false;
    WallObservation walls{};
};
struct Snapshot {
    unsigned sequence = 0, source = 0, pending = 0, target = 0, caller = 0;
    unsigned ticks = 0, race_elapsed = 0, race_type = 0, humans = 0, racers = 0, total = 0, count = 0;
    std::array<unsigned, counter_addresses.size()> counters{};
    std::array<Actor, engine::kMaximumRacers> actors{};
    Course course{};
    unsigned frame = 0, epoch = 0, reasons = 0, actor_mask = 0;
    std::array<unsigned, engine::kMaximumRacers> actor_reasons{};
    std::array<Actor, engine::kMaximumRacers> previous{};
    std::array<Physics, engine::kMaximumRacers> physics{};
    unsigned physics_sample = 0;
    unsigned traffic = 0, traffic_density = 0, traffic_interval = 0;
    unsigned event_entity = 0, event_detail = 0;
    std::array<std::array<unsigned, 17>, rr64::traffic::capacity> cars{};
};

// One guest-thread producer and one UI-thread consumer. Release/acquire pairs
// keep each entry immutable until the consumer finishes printing it.
std::array<Snapshot, capacity> entries{};
std::atomic<unsigned> write_position{0}, read_position{0}, dropped{0};
std::atomic<bool> enabled{false};
std::atomic<bool> population_enabled{false};
unsigned population_samples = 0, population_frames = 0;
std::array<unsigned, engine::kMaximumRacers> police_states{};
std::array<std::array<unsigned, 2>, rr64::traffic::capacity> traffic_targets{};
std::atomic<bool> course_enabled{false}, course_limit_notice{false};
constexpr unsigned course_period_frames = 30, course_sample_limit = 1200;
unsigned course_frames = 0, course_samples = 0; // Process/session cap, not reset each race.
std::array<WallObservation, engine::kMaximumRacers> wall_observations{};
constexpr unsigned wall_context_capacity = 64, wall_context_limit = 1024;
std::array<WallContext, wall_context_capacity> wall_contexts{};
std::atomic<unsigned> wall_context_write{0}, wall_context_read{0}, wall_context_dropped{0};
std::atomic<bool> wall_context_limit_notice{false};
std::array<std::array<WallContext, 4>, engine::kMaximumRacers> wall_history{};
unsigned wall_context_rows = 0, wall_context_course = 0, wall_context_events = 0;
unsigned next_sequence = 0; // Producer owned.
std::atomic<unsigned> authority_humans{0}; // UI publishes; guest only reads.
Course observed_course{};
std::array<Actor, engine::kMaximumRacers> previous_actors{};
unsigned previous_humans = 0, recovery_mask = 0, observed_frames = 0, race_epoch = 0;
bool observing = false, duplicate_start_curve = false;

bool imported_course() {
#ifdef RR64_EXPERIMENTAL_COURSE
    return rr64::experimental_course::active();
#else
    return false;
#endif
}
void saturating_add(unsigned& value, unsigned amount) {
    value += std::min(amount, std::numeric_limits<unsigned>::max() - value);
}
unsigned float_bits(float value) {
    unsigned bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

unsigned word(unsigned char* memory, unsigned address) {
    std::uint32_t value = 0;
    engine::read_u32(memory, address, value);
    return value;
}
std::uint16_t half(unsigned char* memory, unsigned address) {
    std::uint16_t value = 0;
    engine::read_u16(memory, address, value);
    return value;
}
bool valid_pointer(unsigned pointer, unsigned size) {
    return (pointer & 3) == 0 && engine::valid_guest_range(pointer, size);
}
bool terminal_mode(unsigned mode) {
    // Decimal 30 is multiplayer results (0x1E), not hexadecimal 0x30.
    // func_8003FE48 also requests the failure modes 0x1F and 0x30..0x33.
    return engine::is_race_results_mode(mode) || mode == 0x1F ||
           (mode >= 0x30 && mode <= 0x33);
}
double floating(unsigned bits) {
    float value = 0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
Course course(unsigned char* memory) {
    return {word(memory, 0x800A6544), word(memory, 0x800A6540), word(memory, 0x800D763C),
            word(memory, 0x800D7644), word(memory, 0x800D762C), word(memory, 0x800D7630),
            word(memory, 0x800D7634), word(memory, 0x800D7628), word(memory, 0x800D7638),
            word(memory, 0x800D8524)};
}
bool identical_start_curve(unsigned char* memory, const Course& track) {
    if (track.count < 3 || track.count > 8192 || track.wrap < 2 || (track.wrap & 1) ||
        track.wrap > track.count - 3 || !valid_pointer(track.pointer, track.count * 16)) return false;
    for (unsigned offset : {8u, 12u, 24u, 28u, 40u, 44u})
        if (word(memory, track.pointer + offset) != word(memory, track.pointer + track.wrap * 16 + offset))
            return false;
    return true;
}

Actor capture_actor(unsigned char* memory, unsigned slot) {
    Actor actor{};
    const unsigned address = actor_base + slot * actor_stride;
    actor.controller = word(memory, address + 0x08);
    actor.role = word(memory, address + 0x20);
    actor.active = half(memory, address + 0x24);
    actor.ai = half(memory, address + 0x26);
    actor.state = word(memory, address + 0xE8);
    actor.bike = word(memory, address + 0xE0);
    actor.state_valid = valid_pointer(actor.state, 0x54);
    actor.bike_valid = valid_pointer(actor.bike, engine::bike::stride);
    if (actor.state_valid) {
        actor.segment = word(memory, actor.state);
        actor.lap = word(memory, actor.state + 4);
        actor.parameter = word(memory, actor.state + 8);
        actor.length = word(memory, actor.state + 0xC);
        actor.base = word(memory, actor.state + 0x20);
        // Native 6E5E0 owns both fields; the HUD displays +44, not +40.
        actor.table_rank = word(memory, actor.state + 0x40);
        actor.race_place = word(memory, actor.state + 0x44);
        actor.eligible = half(memory, actor.state + 0x48);
        actor.busted = half(memory, actor.state + 0x4C);
        actor.wrecked = half(memory, actor.state + 0x4E);
        actor.finished = half(memory, actor.state + 0x50);
        actor.flag_52 = half(memory, actor.state + 0x52);
    }
    if (actor.bike_valid) {
        actor.health = word(memory, actor.bike + engine::bike::durability_current);
        actor.max_health = word(memory, actor.bike + engine::bike::durability_capacity);
        actor.crash_4cc = word(memory, actor.bike + 0x4CC);
        actor.lockout = half(memory, actor.bike + engine::bike::drive_control_lockout);
        actor.x = word(memory, actor.bike + 0x16C);
        actor.z = word(memory, actor.bike + 0x170);
    }
    return actor;
}

Physics capture_physics(unsigned char* memory, unsigned slot, const Actor& actor) {
    Physics p{};
    p.walls = wall_observations[slot];
    p.delta = word(memory, engine::globals::physics_delta);
    if (actor.controller < 4) {
        p.input_valid = true;
        p.held = half(memory, engine::globals::controller_buttons + actor.controller * 2);
        p.changed = half(memory, engine::globals::controller_changed_buttons + actor.controller * 2);
        p.pressed = half(memory, engine::globals::controller_pressed_buttons + actor.controller * 2);
        std::uint8_t x = 0, y = 0;
        engine::read_u8(memory, engine::globals::controller_stick_x + actor.controller, x);
        engine::read_u8(memory, engine::globals::controller_stick_y + actor.controller, y);
        p.stick_x = x; p.stick_y = y;
    }
    if (actor.state_valid) p.recovery = word(memory, actor.state + 0x40);
    if (!actor.bike_valid) return p;
    const unsigned body = actor.bike + 0x108;
    for (unsigned axis = 0; axis < 3; ++axis) {
        p.position[axis] = word(memory, body + 0x64 + axis * 4);
        p.velocity[axis] = word(memory, body + 0x70 + axis * 4);
        p.predicted[axis] = word(memory, body + 0x8C + axis * 4);
        p.predicted_velocity[axis] = word(memory, body + 0x98 + axis * 4);
        p.radii[axis] = word(memory, body + 0x54 + axis * 4);
    }
    const auto floor_cache = [&](unsigned address, std::array<unsigned, 6>& out) {
        for (unsigned i = 0; i < 4; ++i) out[i] = word(memory, address + i * 4);
        out[4] = word(memory, address + 0x54);
        out[5] = half(memory, address + 0x64);
    };
    floor_cache(actor.bike + 0x5C0, p.front);
    floor_cache(actor.bike + 0x704, p.rear);
    p.sphere_count = word(memory, body + 0x2C);
    p.bike_state = word(memory, actor.bike + 0x100);
    p.attached = half(memory, actor.bike + engine::bike::rider_attached);
    p.bike_missing_floor = half(memory, actor.bike + 0x7FA);
    p.rider = word(memory, actor_base + slot * actor_stride + 0xE4);
    p.rider_valid = valid_pointer(p.rider, engine::rider::stride) &&
        word(memory, p.rider + 4) == actor_base + slot * actor_stride &&
        word(memory, p.rider + engine::rider::bike_pointer) == actor.bike;
    if (p.rider_valid) {
        // Detached bodies own their physical pose; the abandoned bike cannot
        // explain a stuck rider or whether its landing timer should advance.
        for (unsigned axis = 0; axis < 3; ++axis) {
            p.rider_position[axis] = word(memory, p.rider + 0x8C + axis * 4);
            p.rider_velocity[axis] = word(memory, p.rider + 0x98 + axis * 4);
        }
        p.rider_missing_floor = half(memory, p.rider + 0x308);
    }
    return p;
}

void print_wall_context(const WallContext& sample, const char* reason, unsigned sequence = 0) {
    if (!sample.valid) return;
    std::array<char, 3072> line{};
    std::size_t used = 0;
    bool valid = true;
    const auto append = [&](const char* format, auto... args) {
        if (!valid) return;
        const int n = std::snprintf(line.data() + used, line.size() - used, format, args...);
        valid = n >= 0 && std::size_t(n) < line.size() - used;
        if (valid) used += std::size_t(n);
    };
    const auto& q = sample.query;
    append("[RR64-COURSE-WALL-QUERY] reason=%s sequence=%u epoch=%u observed_frame=%u native_frame=%u elapsed_bits=%08X course=%08X slot=%u kind=%u body=%08X route=%08X event=%u edge_stage=%u event_rows=%u event_limit=%u static_hits=%u dynamic_hits=%u triangle=%u hazard=%u triangle_tests=%u dt_bits=%08X spheres=%u",
           reason, sequence, sample.epoch, sample.frame, sample.native_frame, sample.elapsed, sample.course,
           sample.actor, sample.kind, q.body, q.route, sample.event, sample.edge_stage, sample.event_rows, wall_context_limit,
           q.static_contacts, q.dynamic_contacts, q.triangle, q.hazard, q.triangle_tests, float_bits(q.delta), q.sphere_count);
    const auto vector = [&](const char* label, const auto& v) {
        append(" %s_bits=", label);
        for (unsigned i = 0; i < v.size(); ++i) append("%s%08X", i ? "," : "", float_bits(v[i]));
    };
    vector("previous", q.previous); vector("requested", q.requested); vector("resolved", q.resolved);
    vector("point", q.point); vector("normal", q.normal);
    for (unsigned i = 0; i < q.sphere_count; ++i) {
        append(" sphere%u_bits=", i);
        for (unsigned axis = 0; axis < 4; ++axis) append("%s%08X", axis ? "," : "", float_bits(q.spheres[i][axis]));
    }
    append("\n");
    if (valid) std::fwrite(line.data(), 1, used, stderr);
}
void print_physics(const Snapshot& sample) {
    std::fprintf(stderr, "[RR64-COURSE-PHYSICS] sequence=%u sample=%u limit=%u epoch=%u observed_frame=%u "
        "mode=%u pending=%u course=%08X actor_mask=%04X frame_timing_sum=%.9g race_elapsed=%.9g\n",
        sample.sequence, sample.physics_sample, course_sample_limit, sample.epoch, sample.frame,
        sample.source, sample.pending, sample.course.pointer, sample.actor_mask, floating(sample.ticks), floating(sample.race_elapsed));
    for (unsigned slot = 0; slot < sample.count; ++slot) {
        if (!(sample.actor_mask & (1u << slot))) continue;
        const auto& a = sample.actors[slot]; const auto& p = sample.physics[slot];
        // One bounded write per actor prevents renderer-thread log messages
        // from splitting the record between its vector fields. UI consumer only.
        std::array<char, 3072> line{};
        std::size_t used = 0;
        bool truncated = false;
        const auto append = [&](const char* format, auto... values) {
            if (truncated) return;
            const int count = std::snprintf(line.data() + used, line.size() - used, format, values...);
            if (count < 0 || static_cast<std::size_t>(count) >= line.size() - used) {
                truncated = true;
                return;
            }
            used += static_cast<std::size_t>(count);
        };
        const auto vector = [&](const char* label, const std::array<unsigned, 3>& v) {
            append(" %s=(%.9g,%.9g,%.9g) %s_bits=%08X,%08X,%08X", label,
                floating(v[0]), floating(v[1]), floating(v[2]), label, v[0], v[1], v[2]);
        };
        append("[RR64-COURSE-PHYSICS-ACTOR] sequence=%u slot=%u controller=%d bike=%08X "
            "valid=%u input_valid=%u held=%04X changed=%04X pressed=%04X stick=%d,%d state=%u attached=%u "
            "lockout=%u health=%.9g crash4cc_bits=%08X segment=%u t=%.9g recovery=%u dt_bits=%08X spheres=%u",
            sample.sequence, slot, static_cast<std::int32_t>(a.controller), a.bike, unsigned(a.bike_valid),
            unsigned(p.input_valid), p.held, p.changed, p.pressed, int(static_cast<std::int8_t>(p.stick_x)),
            int(static_cast<std::int8_t>(p.stick_y)), p.bike_state, p.attached, a.lockout,
            floating(a.health), a.crash_4cc, a.segment, floating(a.parameter), p.recovery, p.delta, p.sphere_count);
        vector("position", p.position); vector("velocity", p.velocity); vector("predicted", p.predicted);
        vector("predicted_velocity", p.predicted_velocity); vector("radii", p.radii);
        for (unsigned i = 0; i < 2; ++i) {
            const auto& f = i ? p.rear : p.front;
            append(" %s_cache_bits=%08X,%08X,%08X,%08X normal_z_bits=%08X surface=%u",
                i ? "rear" : "front", f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        const auto& w = p.walls;
        append(" wall_checks=%u static_hits=%u dynamic_hits=%u last_kind=%u triangle=%u hazard=%u normal_speed_bits=%08X",
            w.checks, w.static_hits, w.dynamic_hits, w.kind, w.triangle, w.hazard, w.normal_speed);
        vector("requested", w.requested); vector("resolved", w.resolved); vector("normal", w.normal);
        append(" rider=%08X rider_valid=%u bike_missing_floor=%u rider_missing_floor=%u eligible=%u busted=%u wrecked=%u finished=%u",
            p.rider, unsigned(p.rider_valid), p.bike_missing_floor, p.rider_missing_floor,
            a.eligible, a.busted, a.wrecked, a.finished);
        vector("rider_position", p.rider_position); vector("rider_velocity", p.rider_velocity);
        if (truncated || used >= line.size() - 1) {
            constexpr char suffix[] = " truncated=1\n";
            used = std::min(used, line.size() - sizeof(suffix));
            std::memcpy(line.data() + used, suffix, sizeof(suffix) - 1);
            used += sizeof(suffix) - 1;
        } else line[used++] = '\n';
        std::fwrite(line.data(), 1, used, stderr);
        print_wall_context(w.latest, "periodic", sample.sequence);
    }
}

void print(const Snapshot& sample) {
    if (sample.reasons == Population) {
        std::fprintf(stderr, "[RR64-POPULATION] sequence=%u phase=%u cars=%u density=%.9g interval=%.9g\n",
            sample.sequence, sample.target, sample.traffic, floating(sample.traffic_density), floating(sample.traffic_interval));
        for (const auto& car : sample.cars) {
            if (!car[0]) continue;
            std::fprintf(stderr, "[RR64-TRAFFIC-CAR] sequence=%u entity=%08X type=%u segment=%u route=%.9g direction=%.9g speed=%.9g target=%.9g active=%u x=%.9g y=%.9g\n",
                sample.sequence, car[0], car[1], car[2], floating(car[3]), floating(car[4]),
                floating(car[5]), floating(car[6]), car[7], floating(car[8]), floating(car[9]));
            std::fprintf(stderr, "[RR64-TRAFFIC-ROAD] sequence=%u entity=%08X kind=%u next_kind=%u flags=%04X lane=%.9g state=%u index=%u width=%.9g\n",
                sample.sequence, car[0], car[10], car[11], car[12], floating(car[13]), car[14], car[15], floating(car[16]));
        }
        if (sample.event_entity)
            std::fprintf(stderr, "[RR64-POPULATION-CAUSE] sequence=%u phase=%u entity=%08X site=%08X before_road_bits=%08X route_records=%u\n",
                sample.sequence, sample.target, sample.event_entity, sample.caller, sample.event_detail, sample.course.count);
        print_physics(sample);
        return;
    }
    if (sample.reasons == CoursePhysics) { print_physics(sample); return; }
    std::fprintf(stderr,
        "[%s] sequence=%u source=%u source_hex=%02X pending=%u target=%u target_hex=%02X "
        "raw_ra=%08X total_ticks_bits=%08X frame_timing_sum=%.9g race_elapsed=%.9g type=%u humans=%u racers=%u total=%u captured=%u "
        "epoch=%u observed_frame=%u reasons=%u actor_mask=%04X "
        "gD7648=%u gD764C=%u gD7658=%u gD765C=%u gD7678_bits=%08X\n",
        sample.reasons ? "RR64-LAP-EVENT" : "RR64-RACE-END",
        sample.sequence, sample.source, sample.source, sample.pending, sample.target, sample.target,
        sample.caller, sample.ticks, floating(sample.ticks), floating(sample.race_elapsed), sample.race_type, sample.humans, sample.racers,
        sample.total, sample.count, sample.epoch, sample.frame, sample.reasons, sample.actor_mask,
        sample.counters[0], sample.counters[1],
        sample.counters[2], sample.counters[3], sample.counters[4]);
    const auto& track = sample.course;
    std::fprintf(stderr,
        "[RR64-RACE-COURSE] sequence=%u course=%08X records=%u wrap=%u prior_laps_required=%u "
        "lap_threshold=%.9g lap_threshold_bits=%08X period=%.9g period_bits=%08X "
        "finish_threshold=%.9g finish_threshold_bits=%08X finish_segment=%u finish_t=%.9g finish_t_bits=%08X\n",
        sample.sequence, track.pointer, track.count, track.wrap, track.prior_laps,
        floating(track.lap_threshold), track.lap_threshold, floating(track.period), track.period,
        floating(track.finish_threshold), track.finish_threshold, track.finish_segment,
        floating(track.finish_parameter), track.finish_parameter);
    for (unsigned slot = 0; slot < sample.count; ++slot) {
        if (sample.reasons && !(sample.actor_mask & (1u << slot))) continue;
        const auto& actor = sample.actors[slot];
        std::fprintf(stderr,
            "[%s] sequence=%u slot=%u actor=%08X controller=%d role=%u active=%u ai=%u "
            "state=%08X state_valid=%u segment=%u lap=%u t=%.9g t_bits=%08X "
            "length=%.9g length_bits=%08X base=%.9g base_bits=%08X "
            "table_rank=%u race_place=%u eligible=%u busted=%u wrecked=%u finished=%u flag52=%u "
            "bike=%08X bike_valid=%u health=%.9g health_bits=%08X max_health=%.9g max_health_bits=%08X "
            "crash4cc_bits=%08X lockout=%u x=%.9g x_bits=%08X z=%.9g z_bits=%08X\n",
            sample.reasons ? "RR64-LAP-ACTOR" : "RR64-RACE-END-ACTOR",
            sample.sequence, slot, actor_base + slot * actor_stride,
            static_cast<std::int32_t>(actor.controller), actor.role, unsigned(actor.active), unsigned(actor.ai),
            actor.state, unsigned(actor.state_valid), actor.segment, actor.lap,
            floating(actor.parameter), actor.parameter, floating(actor.length), actor.length,
            floating(actor.base), actor.base, actor.table_rank, actor.race_place,
            unsigned(actor.eligible), unsigned(actor.busted),
            unsigned(actor.wrecked), unsigned(actor.finished), unsigned(actor.flag_52),
            actor.bike, unsigned(actor.bike_valid), floating(actor.health), actor.health,
            floating(actor.max_health), actor.max_health, actor.crash_4cc, unsigned(actor.lockout),
            floating(actor.x), actor.x, floating(actor.z), actor.z);
        if (sample.reasons) {
            const auto& before = sample.previous[slot];
            std::fprintf(stderr,
                "[RR64-LAP-CHANGE] sequence=%u slot=%u reasons=%u previous_segment=%u previous_lap=%u "
                "previous_t=%.9g previous_t_bits=%08X previous_base=%.9g previous_base_bits=%08X "
                "previous_finished=%u\n", sample.sequence, slot, sample.actor_reasons[slot],
                before.segment, before.lap, floating(before.parameter), before.parameter,
                floating(before.base), before.base, unsigned(before.finished));
        }
    }
}
} // namespace

static void enqueue(unsigned char* memory, unsigned target_mode, unsigned return_address,
                    unsigned reasons = 0, unsigned actor_mask = 0, const unsigned* slot_reasons = nullptr,
                    unsigned event_entity = 0, unsigned event_detail = 0) {
    const unsigned source = word(memory, engine::globals::main_mode);
    const unsigned pending = word(memory, engine::globals::pending_mode);
    const unsigned write = write_position.load(std::memory_order_relaxed);
    if (write - read_position.load(std::memory_order_acquire) >= capacity) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& sample = entries[write % capacity];
    sample = {};
    sample.sequence = ++next_sequence;
    sample.course = course(memory);
    sample.frame = observed_frames;
    sample.epoch = race_epoch;
    sample.reasons = reasons;
    sample.actor_mask = actor_mask;
    if (reasons && reasons != CoursePhysics && reasons != Population) {
        sample.previous = previous_actors;
        for (unsigned slot = 0; slot < engine::kMaximumRacers; ++slot)
            sample.actor_reasons[slot] = slot_reasons[slot];
    }
    sample.source = source;
    sample.pending = pending;
    sample.target = target_mode;
    sample.caller = return_address;
    sample.event_entity = event_entity;
    sample.event_detail = event_detail;
    sample.ticks = word(memory, engine::globals::total_ticks);
    sample.race_elapsed = word(memory, 0x800D7670);
    sample.race_type = word(memory, 0x800D8524);
    sample.humans = word(memory, engine::local_race::humans);
    sample.racers = word(memory, engine::local_race::racers);
    sample.total = word(memory, 0x800A656C);
    sample.count = std::min(sample.total, engine::kMaximumRacers);
    for (unsigned index = 0; index < counter_addresses.size(); ++index)
        sample.counters[index] = word(memory, counter_addresses[index]);
    for (unsigned slot = 0; slot < sample.count; ++slot)
        sample.actors[slot] = capture_actor(memory, slot);
    if (reasons == Population) {
        sample.traffic = word(memory, 0x800A6528);
        sample.traffic_density = word(memory, 0x800D8560);
        sample.traffic_interval = word(memory, 0x800D84E8 + 0x14);
        for (unsigned i = 0; i < std::min(sample.traffic, rr64::traffic::storage(memory)); ++i) {
            const unsigned car = rr64::traffic::entity(memory, i);
            if (!valid_pointer(car, 0x338)) continue;
            sample.cars[i] = {car, word(memory, car), word(memory, car + 0x14),
                word(memory, car + 0x24), word(memory, car + 0xC), word(memory, car + 0xC0),
                word(memory, car + 0x328), half(memory, car + 0x334),
                word(memory, car + 0xA8), word(memory, car + 0xAC)};
            auto& row = sample.cars[i];
            row[10] = row[11] = row[12] = ~0u;
            const unsigned segment = row[2], route = sample.course.pointer;
            if (segment < sample.course.count && sample.course.count - segment > 2 &&
                sample.course.count <= 65536 && valid_pointer(route, sample.course.count * 16)) {
                std::uint8_t kind = 0, next = 0;
                engine::read_u8(memory, route + segment * 16, kind);
                engine::read_u8(memory, route + segment * 16 + 32, next);
                row[10] = kind; row[11] = next;
                row[12] = half(memory, route + segment * 16 + 18);
            }
            row[13] = word(memory, car + 8);
            row[14] = word(memory, car + 0x3C);
            row[15] = word(memory, car + 0x28);
            row[16] = word(memory, car + 0x34);
        }
    }
    if (reasons == CoursePhysics || reasons == Population) {
        sample.physics_sample = course_samples;
        for (unsigned slot = 0; slot < sample.count; ++slot)
            if (actor_mask & (1u << slot))
                sample.physics[slot] = capture_physics(memory, slot, sample.actors[slot]);
    }
    write_position.store(write + 1, std::memory_order_release);
}

extern "C" void rr64_trace_population(unsigned char* memory, unsigned phase) {
    if (!population_enabled.load(std::memory_order_relaxed) || !memory ||
        prediction::active() || population_samples >= 1200) return;
    if (!engine::is_live_race_mode(word(memory, engine::globals::main_mode))) {
        police_states = {}; traffic_targets = {}; population_frames = 0;
        return;
    }
    if (half(memory, engine::globals::gameplay_pause_state)) return;
    unsigned mask = 0;
    bool changed = phase != 0;
    const unsigned count = std::min(word(memory, 0x800A656C), engine::kMaximumRacers);
    for (unsigned slot = 0; slot < count; ++slot) {
        const unsigned actor = actor_base + slot * actor_stride;
        if (word(memory, actor + 0x20) != 7) continue;
        mask |= 1u << slot;
        const unsigned bike = word(memory, actor + 0xE0);
        unsigned state = half(memory, actor + 0x24);
        if (valid_pointer(bike, engine::bike::stride))
            state |= unsigned(half(memory, bike + 0x7F8)) << 8 |
                     unsigned(half(memory, bike + 0x7F6)) << 16;
        changed |= state != police_states[slot];
        police_states[slot] = state;
    }
    // State edges plus one sample per 120 observations; UI drains the fixed queue.
    if (phase == 0 && ++population_frames < 120 && !changed) return;
    population_frames = 0;
    ++population_samples;
    enqueue(memory, phase, 0, Population, mask);
}

extern "C" void rr64_trace_police_crash(unsigned char* memory, unsigned bike, unsigned site) {
    if (!population_enabled.load(std::memory_order_relaxed) || !memory ||
        prediction::active() || population_samples >= 1200 ||
        !engine::is_live_race_transition(word(memory, engine::globals::main_mode),
                                        word(memory, engine::globals::pending_mode))) return;
    const unsigned count = std::min(word(memory, 0x800A656C), engine::kMaximumRacers);
    for (unsigned slot = 0; slot < count; ++slot) {
        const unsigned actor = actor_base + slot * actor_stride;
        if (word(memory, actor + 0x20) != 7 || word(memory, actor + 0xE0) != bike ||
            !valid_pointer(bike, engine::bike::stride)) continue;
        ++population_samples;
        // Include other riders: a crash edge alone cannot identify a nearby attacker.
        enqueue(memory, 3, site, Population, (1u << count) - 1, nullptr, bike);
        return;
    }
}

extern "C" void rr64_trace_traffic_target(unsigned char* memory, unsigned car, unsigned before_road) {
    if (!population_enabled.load(std::memory_order_relaxed) || !memory ||
        prediction::active() || population_samples >= 1200 || !valid_pointer(car, 0x338) ||
        !engine::is_live_race_transition(word(memory, engine::globals::main_mode),
                                        word(memory, engine::globals::pending_mode))) return;
    const unsigned index = word(memory, car + 0x28);
    if (index >= traffic_targets.size() || rr64::traffic::entity(memory, index) != car) return;
    const unsigned target = word(memory, car + 0x328);
    auto& prior = traffic_targets[index];
    const bool stop = (target & 0x7FFFFFFFu) == 0;
    const bool changed = prior[0] != car ? stop : target != prior[1] &&
        (stop || (prior[1] & 0x7FFFFFFFu) == 0);
    prior = {car, target};
    if (!changed) return;
    ++population_samples;
    enqueue(memory, 4, 0x80076BB0, Population, 0, nullptr, car, before_road);
}

extern "C" void rr64_trace_race_end(unsigned char* memory, unsigned target_mode,
                                     unsigned return_address) {
    if (!enabled.load(std::memory_order_relaxed) || !memory || prediction::active() ||
        !terminal_mode(target_mode)) return;
    if (!engine::is_live_race_mode(word(memory, engine::globals::main_mode)) ||
        word(memory, engine::globals::pending_mode) == target_mode) return;
    enqueue(memory, target_mode, return_address);
}

void rr64::race_end_trace::set_authority_humans(unsigned mask) {
    authority_humans.store(mask & 0x3FFFu, std::memory_order_relaxed);
}

void rr64::race_end_trace::observe_course_wall(unsigned char* memory, unsigned actor,
    unsigned kind, unsigned static_contacts, unsigned dynamic_contacts, unsigned triangle,
    unsigned hazard, const float* requested, const float* resolved, const float* normal,
    float normal_speed) {
    if (!course_enabled.load(std::memory_order_relaxed) || course_samples >= course_sample_limit ||
        !memory || actor >= engine::kMaximumRacers || kind >= 4 || !requested || !resolved || !normal ||
        prediction::active() || !imported_course() ||
        half(memory, engine::globals::gameplay_pause_state) ||
        !engine::is_live_race_transition(word(memory, engine::globals::main_mode),
                                         word(memory, engine::globals::pending_mode))) return;
    auto& w = wall_observations[actor];
    saturating_add(w.checks, 1);
    saturating_add(w.static_hits, static_contacts);
    saturating_add(w.dynamic_hits, dynamic_contacts);
    if (!static_contacts && !dynamic_contacts) return; // Retain last contact, not last clear step.
    w.kind = kind; w.triangle = triangle; w.hazard = hazard;
    w.normal_speed = float_bits(normal_speed);
    for (unsigned i = 0; i < 3; ++i) {
        w.requested[i] = float_bits(requested[i]); w.resolved[i] = float_bits(resolved[i]);
        w.normal[i] = float_bits(normal[i]);
    }
}

static bool human(unsigned char* memory, unsigned slot, unsigned online_mask) {
    const unsigned address = actor_base + slot * actor_stride;
    if (!half(memory, address + 0x24)) return false;
    if (online_mask) return (online_mask & (1u << slot)) != 0;
    return !half(memory, address + 0x26) && word(memory, address + 8) < 4;
}

void rr64::race_end_trace::observe_course_wall_context(unsigned char* memory, unsigned actor,
    unsigned kind, const CourseWallContext& query) {
    if (!course_enabled.load(std::memory_order_relaxed) || course_samples >= course_sample_limit ||
        !memory || actor >= engine::kMaximumRacers || kind >= 4 || query.sphere_count > 3 ||
        !query.sphere_count || prediction::active() || !imported_course() ||
        half(memory, engine::globals::gameplay_pause_state) ||
        !engine::is_live_race_transition(word(memory, engine::globals::main_mode),
                                         word(memory, engine::globals::pending_mode)) ||
        !human(memory, actor, authority_humans.load(std::memory_order_relaxed))) return;
    const unsigned track = word(memory, 0x800A6544);
    if (wall_context_course != track) {
        wall_context_course = track;
        wall_history = {};
        wall_context_rows = wall_context_events = 0;
    }
    WallContext current;
    current.valid = true; current.actor = actor; current.kind = kind; current.course = track;
    current.epoch = race_epoch; current.frame = observed_frames;
    current.native_frame = word(memory, 0x800A1830); current.elapsed = word(memory, 0x800D7670);
    current.query = query;
    auto& previous = wall_history[actor][kind];
    const bool same_owner = previous.valid && previous.query.body == query.body && previous.query.route == query.route;
    const bool contact = query.static_contacts || query.dynamic_contacts;
    const bool previous_contact = previous.query.static_contacts || previous.query.dynamic_contacts;
    if (contact && (!same_owner || !previous_contact || previous.query.triangle != query.triangle ||
                    previous.query.hazard != query.hazard)) {
        current.event = ++wall_context_events;
        current.edge_stage = 1;
        const unsigned needed = same_owner ? 2 : 1;
        // Humans alone consume this budget; unrelated AI contacts cannot
        // exhaust late-race wall evidence. Periodic latest context is uncapped.
        if (wall_context_rows + needed <= wall_context_limit) {
            const unsigned write = wall_context_write.load(std::memory_order_relaxed);
            const unsigned read = wall_context_read.load(std::memory_order_acquire);
            wall_context_rows += needed;
            if (write - read + needed <= wall_context_capacity) {
                unsigned next = write;
                if (same_owner) {
                    auto before = previous;
                    before.event = current.event; before.event_rows = wall_context_rows; before.edge_stage = 0;
                    wall_contexts[next++ % wall_context_capacity] = before;
                }
                current.event_rows = wall_context_rows;
                wall_contexts[next++ % wall_context_capacity] = current;
                wall_context_write.store(next, std::memory_order_release);
            } else wall_context_dropped.fetch_add(needed, std::memory_order_relaxed);
            if (wall_context_rows == wall_context_limit)
                wall_context_limit_notice.store(true, std::memory_order_release);
        } else if (wall_context_rows < wall_context_limit) {
            wall_context_rows = wall_context_limit;
            wall_context_limit_notice.store(true, std::memory_order_release);
        }
    }
    current.event_rows = wall_context_rows;
    wall_observations[actor].latest = current;
    previous = current;
}

extern "C" void rr64_trace_race_recovery(unsigned char* memory, unsigned actor) {
    if (!enabled.load(std::memory_order_relaxed) || !memory || prediction::active() ||
        actor < actor_base || (actor - actor_base) % actor_stride) return;
    const unsigned slot = (actor - actor_base) / actor_stride;
    if (slot >= engine::kMaximumRacers || !engine::is_live_race_mode(word(memory, engine::globals::main_mode))) return;
    if (human(memory, slot, authority_humans.load(std::memory_order_relaxed)))
        recovery_mask |= 1u << slot;
}

extern "C" void rr64_trace_lap_frame(unsigned char* memory) {
    rr64_trace_population(memory, 0);
    const bool lap_enabled = enabled.load(std::memory_order_relaxed);
    const bool physics_enabled = course_enabled.load(std::memory_order_relaxed);
    if ((!lap_enabled && !physics_enabled) || !memory || prediction::active()) return;
    const unsigned mode = word(memory, engine::globals::main_mode);
    const Course track = course(memory);
    // This extra history diagnoses circuit lap counting. Open-road races keep
    // their existing terminal snapshot, without per-segment diagnostic traffic.
    if (!engine::is_live_race_mode(mode) || !track.wrap) {
        observing = false;
        previous_humans = recovery_mask = 0;
        course_frames = 0; wall_observations = {};
        return;
    }
    const unsigned total = std::min(word(memory, 0x800A656C), engine::kMaximumRacers);
    const unsigned online_mask = authority_humans.load(std::memory_order_relaxed);
    std::array<Actor, engine::kMaximumRacers> current{};
    std::array<unsigned, engine::kMaximumRacers> reasons{};
    unsigned human_mask = 0, event_mask = 0, combined = 0;
    const bool starting = !observing || !(track == observed_course);
    if (starting) {
        duplicate_start_curve = identical_start_curve(memory, track);
        previous_actors = {};
        previous_humans = 0;
        wall_history = {}; wall_context_rows = wall_context_events = 0;
        wall_context_course = track.pointer;
    }
    for (unsigned slot = 0; slot < total; ++slot) {
        if (!human(memory, slot, online_mask)) continue;
        human_mask |= 1u << slot;
        auto& now = current[slot];
        now = capture_actor(memory, slot);
        const auto& before = previous_actors[slot];
        unsigned changed = starting ? unsigned(Start) : 0;
        if (!starting && !(previous_humans & (1u << slot))) changed |= Identity;
        if (!starting && (previous_humans & (1u << slot))) {
            if (now.state != before.state || now.bike != before.bike ||
                now.state_valid != before.state_valid || now.bike_valid != before.bike_valid)
                changed |= Identity;
            if (now.segment != before.segment) changed |= Segment;
            if (now.lap != before.lap) changed |= Lap;
            if (now.finished != before.finished || now.busted != before.busted ||
                now.wrecked != before.wrecked || now.eligible != before.eligible)
                changed |= Status;
            const auto marker_segment = [&](unsigned segment) {
                return duplicate_start_curve && segment == 0 ? track.wrap : segment;
            };
            const double boundary = floating(track.finish_parameter);
            const double old_t = floating(before.parameter), new_t = floating(now.parameter);
            if (!(changed & Identity) && now.state_valid && before.state_valid &&
                std::isfinite(boundary) && boundary >= 0 && boundary <= 1 &&
                std::isfinite(old_t) && std::isfinite(new_t) &&
                marker_segment(before.segment) == track.finish_segment &&
                marker_segment(now.segment) == track.finish_segment &&
                ((old_t <= boundary && new_t > boundary) || (old_t > boundary && new_t <= boundary)))
                changed |= MarkerCrossing;
        }
        if (recovery_mask & (1u << slot)) changed |= Recovery;
        reasons[slot] = changed;
        if (changed) event_mask |= 1u << slot;
        combined |= changed;
    }
    if (!human_mask) return; // Wait for initialized human actors before start.
    if (starting) { ++race_epoch; observed_frames = 0; }
    ++observed_frames;
    if (lap_enabled && combined)
        enqueue(memory, word(memory, engine::globals::pending_mode), 0, combined, event_mask, reasons.data());
    const bool capture_course = physics_enabled && imported_course() &&
        engine::is_live_race_transition(mode, word(memory, engine::globals::pending_mode)) &&
        !half(memory, engine::globals::gameplay_pause_state);
    if (!capture_course) {
        course_frames = 0; wall_observations = {};
    } else if (course_samples < course_sample_limit) {
        if (starting) course_frames = 0;
        if (starting || ++course_frames >= course_period_frames) {
            course_frames = 0;
            ++course_samples;
            enqueue(memory, word(memory, engine::globals::pending_mode), 0, CoursePhysics, human_mask);
            wall_observations = {};
            if (course_samples == course_sample_limit)
                course_limit_notice.store(true, std::memory_order_release);
        }
    }
    observed_course = track;
    observing = true;
    previous_actors = current;
    previous_humans = human_mask;
    recovery_mask = 0;
}

void rr64::race_end_trace::drain() {
    // Environment/stdio work stays on the UI thread. The launcher invokes UI
    // updates before a race can begin, so the producer needs only one atomic.
    static const bool trace_enabled = [] {
        const auto flag = [](const char* name) {
            const char* value = std::getenv(name);
            return value && value[0] && value[0] != '0';
        };
        return flag("RR64_RUNTIME_TRACE") || flag("RR64_DIAGNOSTICS");
    }();
    enabled.store(trace_enabled, std::memory_order_relaxed);
    static const bool physics_enabled = [] {
        const char* value = std::getenv("RR64_COURSE_PHYSICS_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    course_enabled.store(physics_enabled, std::memory_order_relaxed);
    static const bool populations = [] {
        const char* value = std::getenv("RR64_POPULATION_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    population_enabled.store(populations, std::memory_order_relaxed);
    if (!trace_enabled && !physics_enabled && !populations) return;
    unsigned read = read_position.load(std::memory_order_relaxed);
    const unsigned write = write_position.load(std::memory_order_acquire);
    const unsigned count = std::min(write - read, drain_limit);
    for (unsigned index = 0; index < count; ++index) {
        print(entries[read % capacity]);
        read_position.store(++read, std::memory_order_release);
    }
    const unsigned lost = dropped.exchange(0, std::memory_order_relaxed);
    if (lost)
        std::fprintf(stderr, "[RR64-RACE-END-LOSS] dropped=%u capacity=%u\n", lost, capacity);
    if (course_limit_notice.exchange(false, std::memory_order_acquire))
        std::fprintf(stderr, "[RR64-COURSE-PHYSICS-LIMIT] samples=%u period_frames=%u reached=1\n",
            course_sample_limit, course_period_frames);
    unsigned context_read = wall_context_read.load(std::memory_order_relaxed);
    const unsigned context_write = wall_context_write.load(std::memory_order_acquire);
    for (unsigned i = 0, count = std::min(context_write - context_read, 4u); i < count; ++i) {
        print_wall_context(wall_contexts[context_read % wall_context_capacity], "contact-edge");
        wall_context_read.store(++context_read, std::memory_order_release);
    }
    const unsigned context_lost = wall_context_dropped.exchange(0, std::memory_order_relaxed);
    if (context_lost)
        std::fprintf(stderr, "[RR64-COURSE-WALL-LOSS] dropped=%u capacity=%u\n", context_lost, wall_context_capacity);
    if (wall_context_limit_notice.exchange(false, std::memory_order_acquire))
        std::fprintf(stderr, "[RR64-COURSE-WALL-LIMIT] rows=%u human_only=1 periodic_continues=1\n", wall_context_limit);
}
