#include "rr64_rival_engine.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_highlights.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" void func_800571DC(unsigned char *, recomp_context *);
extern "C" void func_800806B4(unsigned char *, recomp_context *);
extern "C" void func_800807C0(unsigned char *, recomp_context *);
extern "C" void func_80080820(unsigned char *, recomp_context *);

namespace {
using namespace rr64;
using Vec = std::array<float, 3>;
constexpr unsigned actors = 0x800d8570, actor_stride = 0x118;
// Fourteen racers including the listener, plus one row for an RPM-loop handoff.
// These rows are appended to the original sound pool; ordinary effects/music
// retain their original capacity and cannot evict a passing bike's engine.
constexpr unsigned maximum_engines = 13, extra_voices = maximum_engines + 1;
constexpr unsigned voice_stride = 0x13c, extra_heap = 0x14000;
// The ROM table contains all 32 bike models, including late choppers, the
// two Insanity bikes and cop variants. Reserved entries still need validation.
constexpr unsigned engine_profile_count = 32;
constexpr float full_volume_distance = 12.f, audible_distance = 320.f;
std::atomic<double> volume_percent{35.0};
std::atomic<bool> engines_enabled{true};
#ifdef RR64_RIVAL_ENGINE_TEST_TRACE
extern "C" void rr64_rival_engine_test_scan(unsigned);
#define RR64_RIVAL_SCAN(kind) rr64_rival_engine_test_scan(kind)
#else
#define RR64_RIVAL_SCAN(kind) ((void)0)
#endif
// Each native row has a monotonically assigned handle. A reused row cannot
// inherit ownership just because its address matches an earlier rival voice.
std::array<std::atomic<std::uint64_t>, 64> owned_rows{};
std::atomic<unsigned char *> owned_memory{nullptr};
std::atomic<std::uint64_t> admitted{0}, denied{0};
struct VoicePool {
    unsigned base = 0, count = 0, first = 0, free = 0, owned = 0;
    bool valid = false;
};
struct Bike {
    unsigned slot = 14, bike = 0, rider = 0, controller = ~0u;
    Vec position{}, velocity{};
    bool ready = false, motion_valid = false;
};
struct Listener {
    unsigned slot = 14, bike = 0, rider = 0;
    Vec position{}, forward{}, velocity{};
    bool attached = false, motion_valid = false;
};
struct Engine {
    Bike source{};
    Listener listener{};
    float gain = 0, pan = 128, doppler = 0;
    unsigned handle = ~0u;
};
struct Runtime {
    unsigned char *memory = nullptr;
    std::array<Engine, maximum_engines> engines{};
    float clock = 0;
    bool observed = false;
    bool announced = false;
    unsigned eligible_peak = 0, voice_peak = 0;
    std::uint64_t admission_start = 0, denial_start = 0, pan_updates = 0;
};
thread_local Runtime runtime;
struct Scope {
    unsigned char *memory = nullptr;
    void *context = nullptr;
    unsigned bike = 0, slot = 14;
    gpr stack = 0;
    float gain = 0, pan = 128, doppler = 0;
    unsigned replacing = 0;
};
thread_local Scope producer;
struct Binding {
    unsigned char *memory = nullptr;
    void *context = nullptr;
    gpr stack = 0;
    bool enabled = false;
};
thread_local Binding allocation, child;
bool diagnostics() {
    static const bool enabled = []() {
        const char *v = std::getenv("RR64_DIAGNOSTICS");
        return v && *v && *v != '0';
    }();
    return enabled;
}

unsigned word(unsigned char *m, unsigned a) {
    unsigned v = 0;
    engine::read_u32(m, a, v);
    return v;
}
unsigned half(unsigned char *m, unsigned a) {
    std::uint16_t v = 0;
    engine::read_u16(m, a, v);
    return v;
}
bool vector(unsigned char *m, unsigned a, Vec &v) {
    for (unsigned i = 0; i < 3; ++i)
        if (!engine::read_float(m, a + 4 * i, v[i]) || !std::isfinite(v[i]) ||
            std::abs(v[i]) > 1000000)
            return false;
    return true;
}
unsigned cache(unsigned slot) {
    return engine::racer_audio::cache + slot * engine::racer_audio::row_stride;
}
bool owned(unsigned char *m, unsigned row, unsigned handle, const VoicePool &pool) {
    if (!handle || handle == ~0u || owned_memory.load(std::memory_order_acquire) != m ||
        row < pool.base || (row - pool.base) % voice_stride ||
        (row - pool.base) / voice_stride >= pool.count)
        return false;
    return owned_rows[(row - pool.base) / voice_stride].load(std::memory_order_acquire) ==
           (std::uint64_t(row) << 32 | handle);
}
VoicePool pool(unsigned char *m) {
    VoicePool p;
    p.base = word(m, 0x800df6ec);
    p.count = word(m, 0x800df6e4);
    if (p.count <= 4 || p.count > owned_rows.size() || (p.base & 3) ||
        !engine::valid_guest_range(p.base, p.count * voice_stride) ||
        word(m, 0x800df6f0) != p.base + 4 * voice_stride)
        return p;
    p.valid = true;
    p.first = rr64_rival_engine_effect_limit(p.count);
    for (unsigned i = 4; i < p.count; ++i) {
        const auto row = p.base + i * voice_stride;
        if (!word(m, row + 4) && i >= p.first)
            ++p.free;
        else if (owned(m, row, word(m, row + 0x44), p))
            ++p.owned;
    }
    return p;
}
bool owned_handle(unsigned char *m, unsigned handle, const VoicePool &p, bool retiring = false) {
    if (!p.valid)
        return false;
    for (unsigned i = 4; i < p.count; ++i) {
        const auto row = p.base + i * voice_stride;
        if (word(m, row + 4) && word(m, row + 0x44) == handle && owned(m, row, handle, p) &&
            (!retiring || word(m, row + 0x10) != ~0u))
            return true;
    }
    return false;
}
bool read_bike(unsigned char *m, unsigned slot, Bike &out) {
    RR64_RIVAL_SCAN(1);
    if (slot >= 14)
        return false;
    const unsigned actor = actors + slot * actor_stride;
    Bike b;
    b.slot = slot;
    b.bike = word(m, actor + 0xe0);
    b.rider = word(m, actor + 0xe4);
    b.controller = word(m, actor + 8);
    if (!half(m, actor + 0x24) || word(m, actor) != slot || (b.bike & 3) || (b.rider & 3) ||
        !engine::valid_guest_range(b.bike, engine::bike::stride) ||
        !engine::valid_guest_range(b.rider, engine::rider::stride) ||
        word(m, b.bike + 4) != actor || word(m, b.rider + 4) != actor ||
        word(m, b.bike + engine::bike::rider_pointer) != b.rider ||
        word(m, b.rider + engine::rider::bike_pointer) != b.bike ||
        !vector(m, b.bike + engine::bike::body_position, b.position))
        return false;
    b.ready = !half(m, b.bike + engine::bike::drive_control_lockout) &&
              half(m, b.bike + engine::bike::rider_attached);
    b.motion_valid = vector(m, b.bike + 0x178, b.velocity);
    out = b;
    return true;
}
unsigned sound_profile(unsigned char *m, const Bike &bike) {
    RR64_RIVAL_SCAN(2);
    // Only sound sources need a playable profile. A local listener must remain
    // valid even when its bike has a reserved/missing sound-table entry.
    const unsigned type = word(m, bike.bike);
    if (type >= engine_profile_count)
        return 0;
    const unsigned profile = word(m, 0x800a4b48 + type * 4);
    const unsigned bank = word(m, 0x800df718);
    if ((profile & 3) || !engine::valid_guest_range(profile, 0x4c) ||
        !engine::valid_guest_range(bank, 4))
        return 0;
    const unsigned effects = word(m, bank);
    // Model 22's pointer addresses unrelated packed data in the original ROM.
    // Validate all four authored engine effects before invoking native 571DC.
    for (unsigned offset = 0; offset < 16; offset += 4)
        if (word(m, profile + offset) >= effects)
            return 0;
    return profile;
}
bool rival(const Bike &b, const netplay::PhysicsRules &rules) {
    if (rules.active) {
        const unsigned local = rules.replicated_riders ? 0 : rules.local_slot;
        return b.slot != local;
    }
    return static_cast<int>(b.controller) < 0;
}
bool valid_context(const recomp_context &ctx) {
    const auto sp = static_cast<unsigned>(ctx.r29);
    return !(sp & 7) && engine::valid_guest_range(sp - 0x200, 0x210);
}
recomp_context call_context(const recomp_context &ctx) {
    return ctx;
}
void stop(unsigned char *m, recomp_context &call, Engine &e) {
    const auto p = pool(m);
    if (e.source.slot < 14 && owned_handle(m, e.handle, p)) {
        call.r4 = engine::guest_address(e.handle);
        call.r5 = 0;
        func_800806B4(m, &call);
        if (word(m, cache(e.source.slot)) == e.handle)
            engine::write_u32(m, cache(e.source.slot), ~0u);
    }
    e = {};
}
void reset(unsigned char *m, recomp_context &call) {
    if (runtime.observed && diagnostics())
        std::fprintf(
            stderr,
            "[RR64-RIVAL-ENGINE] stopped eligible_peak=%u voice_peak=%u admitted=%llu budget_denied=%llu pan_updates=%llu\n",
            runtime.eligible_peak, runtime.voice_peak,
            static_cast<unsigned long long>(admitted.load() - runtime.admission_start),
            static_cast<unsigned long long>(denied.load() - runtime.denial_start),
            static_cast<unsigned long long>(runtime.pan_updates));
    if (runtime.memory == m)
        for (auto &e : runtime.engines)
            stop(m, call, e);
    runtime = {};
    runtime.memory = m;
}
float approach(float current, float target, float maximum) {
    return current + std::clamp(target - current, -maximum, maximum);
}
unsigned listeners(unsigned char *m, const netplay::PhysicsRules &rules,
                   std::array<Listener, 4> &out) {
    RR64_RIVAL_SCAN(0);
    const auto count = rules.active ? 1u : std::clamp(word(m, engine::local_race::humans), 1u, 4u);
    unsigned used = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto view = rules.active ? (rules.replicated_riders ? 0 : rules.local_slot) : i;
        if (view >= 4)
            continue;
        Bike b;
        const unsigned slot = word(m, 0x800a657c + view * 4);
        if (!read_bike(m, slot, b))
            continue;
        Listener l;
        l.slot = slot;
        l.bike = b.bike;
        l.rider = b.rider;
        l.attached = half(m, b.rider + engine::rider::bike_attached) != 0;
        if (!vector(m, l.attached ? b.bike + engine::bike::body_position : b.rider + 0x8c,
                    l.position))
            continue;
        // 36B78 copies bike+178 into rider+98 while attached; after eject the
        // latter is the body's own integrator velocity, not its abandoned bike.
        l.motion_valid = vector(m, l.attached ? b.bike + 0x178 : b.rider + 0x98, l.velocity);
        // 1626C stores eye, target and up separately for each view. This follows the
        // camera after an eject, without borrowing the abandoned bike's heading.
        Vec eye{}, target{};
        if (vector(m, 0x800b7418 + view * 0x24, eye) && vector(m, 0x800b7424 + view * 0x24, target))
            for (unsigned j = 0; j < 3; ++j)
                l.forward[j] = target[j] - eye[j];
        float length = std::hypot(l.forward[0], l.forward[1]);
        if (length < .001f) {
            Vec front{}, rear{};
            if (vector(m, b.bike + engine::bike::front_wheel_position, front) &&
                vector(m, b.bike + engine::bike::rear_wheel_position, rear))
                for (unsigned j = 0; j < 3; ++j)
                    l.forward[j] = front[j] - rear[j];
            length = std::hypot(l.forward[0], l.forward[1]);
        }
        if (length >= .001f) {
            l.forward[0] /= length;
            l.forward[1] /= length;
        } else
            l.forward = {0, 1, 0};
        out[used++] = l;
    }
    return used;
}
struct Candidate {
    Bike source{};
    Listener listener{};
    float gain = 0, pan = 128, rank = 0, doppler = 0;
    bool doppler_valid = false;
};
bool doppler_offset(unsigned char *m, const Bike &b, const Listener &l, float &offset) {
    if (!b.motion_valid || !l.motion_valid)
        return false;
    const float dx = b.position[0] - l.position[0], dy = b.position[1] - l.position[1];
    const float distance = std::hypot(dx, dy);
    if (distance < .001f)
        return false;
    // Original traffic 57D48..57DB4 uses horizontal radial velocity and these
    // ROM-authored pitch constants. No physical unit conversion is assumed.
    float scale = 0, rate = 0, minimum = 0;
    if (!engine::read_float(m, 0x80005b94, scale) || !std::isfinite(scale) || scale >= 0 ||
        !engine::read_float(m, 0x80005b98, rate) || !std::isfinite(rate) || rate <= 0 ||
        !engine::read_float(m, 0x80005b9c, minimum) || !std::isfinite(minimum) || minimum >= 0 ||
        minimum < -12.f)
        return false;
    const float radial =
        (dx * (b.velocity[0] - l.velocity[0]) + dy * (b.velocity[1] - l.velocity[1])) / distance;
    const float shifted = (radial * scale) * rate;
    if (!std::isfinite(shifted))
        return false;
    offset = std::clamp(shifted, minimum, -minimum);
    return true;
}
bool continuous_listener(const Listener &before, const Listener &now) {
    if (before.slot != now.slot || before.bike != now.bike || before.rider != now.rider ||
        before.attached != now.attached)
        return false;
    float distance = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const float d = now.position[i] - before.position[i];
        distance += d * d;
    }
    return distance <= 80.f * 80.f;
}
bool scoped(const Binding &b, unsigned char *m, void *ctx, gpr stack) {
    return b.enabled && b.memory == m && b.context == ctx && b.stack == stack;
}
}

namespace rr64::rival_engine {
void set_enabled(bool value) noexcept {
    // UI callbacks only publish the preference. Native voice cleanup belongs
    // on the next game/audio update, never on the UI thread.
    engines_enabled.store(value, std::memory_order_relaxed);
}
bool get_enabled() noexcept {
    return engines_enabled.load(std::memory_order_relaxed);
}
void set_volume_percent(double value) noexcept {
    volume_percent.store(std::isfinite(value) ? std::clamp(value, 0.0, 100.0) : 35.0,
                         std::memory_order_relaxed);
}
double get_volume_percent() noexcept {
    return volume_percent.load(std::memory_order_relaxed);
}
}

extern "C" void rr64_rival_engine_gate(unsigned char *m, void *raw) {
    if (!m || !raw || rr64::prediction::active())
        return;
    const auto rules = rr64::netplay::get_physics_rules();
    if (!rules.active || !rules.connected || rules.phase != rr64::netplay::Phase::Race)
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    const unsigned actor = static_cast<unsigned>(ctx.r2);
    if (actor < actors || (actor - actors) % actor_stride || (actor - actors) / actor_stride >= 14)
        return;
    const auto slot = (actor - actors) / actor_stride;
    if (slot != (rules.replicated_riders ? 0u : rules.local_slot))
        ctx.r3 = static_cast<gpr>(-1);
}
extern "C" int rr64_rival_engine_gain(unsigned char *m, void *raw) {
    if (!raw || rr64::prediction::active() || producer.memory != m || producer.context != raw)
        return 0;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if (static_cast<unsigned>(ctx.r4) != producer.bike)
        return 0;
    const auto gain = static_cast<unsigned>(ctx.r5), pan = static_cast<unsigned>(ctx.r6),
               priority = static_cast<unsigned>(ctx.r7);
    if ((gain | pan | priority) & 3u || !engine::valid_guest_range(gain, 4) ||
        !engine::valid_guest_range(pan, 4) || !engine::valid_guest_range(priority, 4))
        return 0;
    engine::write_float(m, gain, producer.gain);
    engine::write_float(m, pan, producer.pan);
    engine::write_u32(m, priority, 0);
    return 1;
}
extern "C" void rr64_rival_engine_threshold(unsigned char *m, void *raw) {
    if (raw && !prediction::active() && producer.memory == m && producer.context == raw)
        // Stock 58600 discards gains <=18, including a 35% idle engine even up
        // close. Rival voices fade through quiet gains; retain integer silence.
        static_cast<recomp_context *>(raw)->f0.fl = 1.f;
}
extern "C" void rr64_rival_engine_pitch(unsigned char *m, void *raw) {
    if (!raw || prediction::active() || producer.memory != m || producer.context != raw ||
        producer.slot >= 14)
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if (ctx.r29 != producer.stack || static_cast<unsigned>(ctx.r18) != producer.bike ||
        !std::isfinite(ctx.f20.fl) || !owned_handle(m, word(m, cache(producer.slot)), pool(m)))
        return;
    // 571DC recalculates the bike's RPM pitch every call. Add exactly once
    // before 80890 combines it with the authored sound pitch. Start cues skip
    // this steady-engine branch and retain their original authored pitch.
    ctx.f20.fl += producer.doppler;
}
extern "C" void rr64_rival_engine_transition(unsigned char *m, void *raw) {
    if (!raw || prediction::active() || producer.memory != m || producer.context != raw ||
        producer.slot >= 14)
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if (ctx.r29 != producer.stack || static_cast<unsigned>(ctx.r18) != producer.bike ||
        static_cast<unsigned>(ctx.r17) != cache(producer.slot))
        return;
    const auto handle = word(m, cache(producer.slot));
    const auto effect = word(m, cache(producer.slot) + 4);
    const auto voices = pool(m);
    if (static_cast<unsigned>(ctx.r16) == effect || !owned_handle(m, handle, voices))
        return;
    float gain = 0;
    // 58600 rejects replacements at its quiet-start threshold before the
    // allocator. Keep the existing loop and let its normal volume update fade
    // it; stopping it first creates a gap when distant bikes change RPM clips.
    if (!engine::read_float(m, static_cast<unsigned>(ctx.r29) + 0x18, gain) ||
        !std::isfinite(gain) || gain <= 1.f) {
        ctx.r16 = effect;
        return;
    }
    // Native 574C0 stops the old RPM loop before starting its replacement.
    // Keep it playing if no transition row is available: otherwise a full pack
    // can stop every engine, then deny every restart until the worker releases.
    if (voices.free && voices.owned < extra_voices)
        producer.replacing = handle;
    else
        ctx.r16 = effect;
}
extern "C" void rr64_rival_engine_frame(unsigned char *m, void *raw) {
    if (!m || !raw || prediction::active())
        return;
    const auto &ctx = *static_cast<recomp_context *>(raw);
    if (!rr64::rival_engine::get_enabled()) {
        // Stop owned voices once, including native script children sharing
        // their handle. Subsequent disabled frames do not read listeners,
        // bike profiles, voice pools, race rules or sample data.
        if (runtime.memory == m && runtime.observed && valid_context(ctx)) {
            auto call = call_context(ctx);
            call.f_odd = &call.f0.u32h;
            reset(m, call);
        }
        return;
    }
    if (!valid_context(ctx))
        return;
    auto call = call_context(ctx);
    call.f_odd = &call.f0.u32h;
    if (runtime.memory != m)
        runtime = {};
    runtime.memory = m;
    if (owned_memory.exchange(m, std::memory_order_acq_rel) != m)
        for (auto &row : owned_rows)
            row.store(0, std::memory_order_release);
    const auto rules = netplay::get_physics_rules();
    float clock = 0;
    const bool live = engine::is_live_race_transition(word(m, engine::globals::main_mode),
                                                      word(m, engine::globals::pending_mode)) &&
                      !half(m, engine::globals::gameplay_pause_state) &&
                      !rr64_highlights_presenting() &&
                      (!rules.active || (rules.connected && rules.phase == netplay::Phase::Race)) &&
                      engine::read_float(m, 0x800a1820, clock) && std::isfinite(clock);
    if (!live) {
        reset(m, call);
        return;
    }
    if (runtime.observed && clock < runtime.clock)
        reset(m, call);
    if (!runtime.observed) {
        runtime.admission_start = admitted.load();
        runtime.denial_start = denied.load();
        if (diagnostics())
            std::fprintf(
                stderr,
                "[RR64-RIVAL-ENGINE] armed volume=%.1f max_engines=13 dedicated_rows=14 range=320 profiles=32\n",
                rr64::rival_engine::get_volume_percent());
    }
    const float dt = runtime.observed ? std::clamp(clock - runtime.clock, 0.f, .1f) : 1.f / 60.f;
    // Reach 95% of a gain change in about 180 ms at every volume. A fixed
    // full-scale step erased quiet, distant voices in a single update.
    const float gain_blend = -std::expm1(-dt / .06f);
    runtime.clock = clock;
    runtime.observed = true;
    std::array<Listener, 4> ears{};
    const auto ear_count = listeners(m, rules, ears);
    if (!ear_count) {
        reset(m, call);
        return;
    }
    std::array<Candidate, 14> candidates{};
    unsigned count = 0;
    const float volume = float(rr64::rival_engine::get_volume_percent() / 100.0);
    // Use the same ROM gain as the local human engine at 100%. The old fixed
    // 98.56 ceiling reduced rivals to 70% even before distance attenuation.
    float local_gain = 0;
    if (!engine::read_float(m, 0x80005CA0u, local_gain) || !std::isfinite(local_gain) ||
        local_gain <= 0 || local_gain > 255.f) {
        reset(m, call);
        return;
    }
    for (unsigned slot = 0; slot < 14; ++slot) {
        Bike b;
        if (!read_bike(m, slot, b) || !rival(b, rules) || !b.ready || !sound_profile(m, b))
            continue;
        Candidate c;
        c.source = b;
        for (unsigned i = 0; i < ear_count; ++i) {
            float sum = 0;
            for (unsigned j = 0; j < 3; ++j) {
                const float d = b.position[j] - ears[i].position[j];
                sum += d * d;
            }
            const float distance = std::sqrt(sum);
            const float rank = 1.f / (full_volume_distance + distance);
            // Nearby bikes reach the slider's full level, followed by a longer
            // smooth fade. Selection and native voice headroom remain bounded.
            const float t = std::clamp((distance - full_volume_distance) /
                                           (audible_distance - full_volume_distance),
                                       0.f, 1.f);
            const float gain = (1.f - t * t * (3.f - 2.f * t)) * volume;
            if (rank <= c.rank)
                continue;
            c.rank = rank;
            c.gain = gain;
            c.listener = ears[i];
            c.doppler_valid = doppler_offset(m, b, ears[i], c.doppler);
            const float dx = b.position[0] - ears[i].position[0],
                        dy = b.position[1] - ears[i].position[1];
            const float length = std::hypot(dx, dy);
            const float side =
                length > .001f ? (dx * ears[i].forward[1] - dy * ears[i].forward[0]) / length : 0;
            // Native pan is a multiplier: authored engine pan 63 * this /128.
            c.pan = 128.f + 112.f * std::clamp(side, -1.f, 1.f);
        }
        if (c.gain <= .001f)
            continue;
        candidates[count++] = c;
    }
    runtime.eligible_peak = std::max(runtime.eligible_peak, count);
    const unsigned selected = std::min(count, maximum_engines);
    for (auto &e : runtime.engines)
        if (e.source.slot < 14) {
            Bike current;
            if (!read_bike(m, e.source.slot, current) || current.bike != e.source.bike ||
                current.rider != e.source.rider || !rival(current, rules) || !current.ready ||
                !sound_profile(m, current)) {
                stop(m, call, e);
                continue;
            }
            float squared = 0;
            for (unsigned i = 0; i < 3; ++i) {
                const float d = current.position[i] - e.source.position[i];
                squared += d * d;
            }
            if (squared > 80.f * 80.f) {
                stop(m, call, e);
                continue;
            }
            e.source = current;
            const Candidate *target = nullptr;
            for (unsigned i = 0; i < selected; ++i)
                if (candidates[i].source.slot == e.source.slot)
                    target = &candidates[i];
            const float target_gain = target ? target->gain : 0;
            e.gain += (target_gain - e.gain) * gain_blend;
            if (std::abs(target_gain - e.gain) < .001f)
                e.gain = target_gain;
            if (target) {
                e.pan = approach(e.pan, target->pan, 112.f * dt / .12f);
                if (target->doppler_valid && continuous_listener(e.listener, target->listener))
                    e.doppler = approach(e.doppler, target->doppler, 12.f * dt / .18f);
                else
                    e.doppler = 0;
                e.listener = target->listener;
            } else
                e.doppler = approach(e.doppler, 0, 12.f * dt / .18f);
            if (!target && e.gain <= .001f) {
                stop(m, call, e);
                continue;
            }
        }
    for (unsigned i = 0; i < selected; ++i) {
        bool existing = false;
        for (const auto &e : runtime.engines)
            existing |= e.source.slot == candidates[i].source.slot;
        if (existing)
            continue;
        for (auto &e : runtime.engines)
            if (e.source.slot >= 14) {
                e.source = candidates[i].source;
                e.listener = candidates[i].listener;
                e.pan = candidates[i].pan;
                e.gain = candidates[i].gain * gain_blend;
                break;
            }
    }
    for (auto &e : runtime.engines)
        if (e.source.slot < 14 && e.gain > .001f) {
            const unsigned old = word(m, cache(e.source.slot));
            if (old != ~0u && old && !owned_handle(m, old, pool(m))) {
                // A remote human may already have a native engine from an earlier
                // ownership state. Stop that exact cache entry before taking over.
                call.r4 = engine::guest_address(old);
                call.r5 = 0;
                func_800806B4(m, &call);
                engine::write_u32(m, cache(e.source.slot), ~0u);
            }
            producer = {m,
                        &call,
                        e.source.bike,
                        e.source.slot,
                        static_cast<gpr>(ADD32(ctx.r29, -0x50)),
                        e.gain * local_gain,
                        e.pan,
                        e.doppler};
            call.r4 = engine::guest_address(e.source.bike);
            func_800571DC(m, &call);
            producer = {};
            e.handle = word(m, cache(e.source.slot));
            if (owned_handle(m, e.handle, pool(m))) {
                // The native race-start transient returns early while its handle
                // lives, bypassing 571DC's normal continuous volume update. Retain
                // that original cue/profile multiplier while still fading it.
                const auto profile = sound_profile(m, e.source);
                float transient_scale = 0;
                if (word(m, cache(e.source.slot) + 4) == word(m, profile) &&
                    engine::read_float(m, profile + 0x20, transient_scale) &&
                    std::isfinite(transient_scale)) {
                    call.r4 = engine::guest_address(e.handle);
                    call.r5 = static_cast<unsigned>(
                        std::clamp(e.gain * local_gain * transient_scale, 0.f, 255.f));
                    func_800807C0(m, &call);
                }
                call.r4 = engine::guest_address(e.handle);
                call.r5 = static_cast<unsigned>(e.pan);
                func_80080820(m, &call);
                ++runtime.pan_updates;
            }
        }
    const auto voices = pool(m);
    runtime.voice_peak = std::max(runtime.voice_peak, voices.owned);
    if (voices.owned && !runtime.announced) {
        runtime.announced = true;
        if (diagnostics())
            std::fprintf(
                stderr,
                "[RR64-RIVAL-ENGINE] first-voice eligible=%u voices=%u free=%u pan_updates=%llu\n",
                count, voices.owned, voices.free,
                static_cast<unsigned long long>(runtime.pan_updates));
    }
}
extern "C" void rr64_rival_engine_mode(unsigned char *m, void *raw, unsigned next) {
    if (!m || !raw || prediction::active() || engine::is_live_race_mode(next))
        return;
    const auto &ctx = *static_cast<recomp_context *>(raw);
    if (!valid_context(ctx))
        return;
    auto call = call_context(ctx);
    call.f_odd = &call.f0.u32h;
    reset(m, call);
}
extern "C" void rr64_rival_engine_recovery(unsigned char *m, void *raw, unsigned actor) {
    if (!m || !raw || prediction::active() || runtime.memory != m)
        return;
    const auto &ctx = *static_cast<recomp_context *>(raw);
    if (!valid_context(ctx))
        return;
    auto call = call_context(ctx);
    call.f_odd = &call.f0.u32h;
    for (auto &e : runtime.engines)
        if (actors + e.source.slot * actor_stride == actor)
            stop(m, call, e);
}
extern "C" void rr64_rival_engine_audio_reset() {
    if (prediction::active())
        return;
    for (auto &row : owned_rows)
        row.store(0, std::memory_order_release);
}
extern "C" unsigned rr64_rival_engine_effect_limit(unsigned count) {
    // Both original quality presets are recognized; unknown layouts stay native.
    return count == 8 + 4 + extra_voices || count == 16 + 4 + extra_voices
               ? count - extra_voices : count;
}
extern "C" void rr64_rival_engine_heap(unsigned char *, void *raw) {
    if (!raw || prediction::active())
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if ((ctx.r5 == 0x17c00 || ctx.r5 == 0x1e828) && ctx.r3 == ctx.r5)
        ctx.r3 = ctx.r5 += extra_heap;
}
extern "C" void rr64_rival_engine_audio_init(unsigned char *m, void *raw) {
    if (!m || !raw || prediction::active())
        return;
    rr64_rival_engine_audio_reset();
    const auto config = static_cast<unsigned>(static_cast<recomp_context *>(raw)->r4);
    if ((config & 3) || !engine::valid_guest_range(config, 0x44))
        return;
    const auto effects = word(m, config + 4);
    const auto heap = word(m, config + 0x14);
    if (!((effects == 8 && heap == 0x17c00 + extra_heap) ||
          (effects == 16 && heap == 0x1e828 + extra_heap)))
        return;
    engine::write_u32(m, config + 4, effects + extra_voices);
    // Two cached sample blocks per added voice, plus room in both command lists.
    engine::write_u32(m, config + 0x38, word(m, config + 0x38) + 2 * extra_voices);
    engine::write_u32(m, config + 0x30, word(m, config + 0x30) * 2);
}
extern "C" void rr64_rival_engine_allocation_range(unsigned char *m, void *raw, unsigned begin) {
    if (!m || !raw || prediction::active())
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    const auto count = static_cast<unsigned>(ctx.r2);
    const auto first = rr64_rival_engine_effect_limit(count);
    if (first == count)
        return;
    if (scoped(allocation, m, raw, ctx.r29)) {
        if (begin) {
            ctx.r5 = first;
            ctx.r4 = engine::guest_address(word(m, 0x800df6ec) + first * voice_stride);
        }
    } else
        ctx.r2 = first;
}
extern "C" int rr64_rival_engine_allocate(unsigned char *m, void *raw) {
    allocation = {};
    if (!m || !raw || prediction::active())
        return 0;
    auto &ctx = *static_cast<recomp_context *>(raw);
    const bool enabled =
        (producer.memory == m && producer.context == raw) || scoped(child, m, raw, ctx.r29);
    if (!enabled)
        return 0;
    // A queued native script can outlive the game-thread stop request. Keep
    // identifying owned parents while disabled so their children cannot start.
    if (!rr64::rival_engine::get_enabled()) {
        ++denied;
        ctx.r2 = 0;
        return 1;
    }
    const auto p = pool(m);
    // One temporary row bridges an owned loop's pending stop. Queued releases
    // still occupy rows; neither a new source nor a script child may use it.
    const bool replacement = producer.memory == m && producer.context == raw &&
                             owned_handle(m, producer.replacing, p, true);
    if (!p.valid || !p.free ||
        p.owned >= maximum_engines + unsigned(replacement)) {
        ++denied;
        ctx.r2 = 0;
        return 1;
    }
    // The fifth native argument is allocator priority; parent and script-child
    // rows both stay at the lowest priority, below original engine priority 100.
    if (!engine::write_u32(m, static_cast<unsigned>(ctx.r29) + 0x10, 0)) {
        ctx.r2 = 0;
        return 1;
    }
    allocation = {m, raw, static_cast<gpr>(ADD32(ctx.r29, -0x38)), true};
    return 0;
}
extern "C" void rr64_rival_engine_no_steal(unsigned char *m, void *raw) {
    if (!raw || prediction::active())
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if (scoped(allocation, m, raw, ctx.r29)) {
        ctx.r6 = 0;
        ctx.r18 = 0;
    }
}
extern "C" void rr64_rival_engine_allocated(unsigned char *m, void *raw) {
    if (!raw || prediction::active())
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if (!scoped(allocation, m, raw, ctx.r29))
        return;
    const auto p = pool(m);
    const unsigned row = static_cast<unsigned>(ctx.r16), handle = static_cast<unsigned>(ctx.r2);
    if (p.valid && row >= p.base + 4 * voice_stride && (row - p.base) % voice_stride == 0 &&
        (row - p.base) / voice_stride < p.count) {
        owned_rows[(row - p.base) / voice_stride].store(std::uint64_t(row) << 32 | handle,
                                                        std::memory_order_release);
        ++admitted;
    }
}
extern "C" void rr64_rival_engine_child(unsigned char *m, void *raw, unsigned begin) {
    child = {};
    if (!begin || !m || !raw || prediction::active())
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    const auto p = pool(m);
    const unsigned row = static_cast<unsigned>(ctx.r16);
    if (p.valid && word(m, row + 4) && owned(m, row, word(m, row + 0x44), p))
        child = {m, raw, ctx.r29, true};
}
extern "C" void rr64_rival_engine_child_adopt(unsigned char *m, void *raw) {
    if (!raw || prediction::active())
        return;
    auto &ctx = *static_cast<recomp_context *>(raw);
    if (!scoped(child, m, raw, ctx.r29))
        return;
    const auto p = pool(m);
    const unsigned row = static_cast<unsigned>(ctx.r3) - 0x7c;
    if (p.valid && owned(m, row, word(m, row + 0x44), p))
        owned_rows[(row - p.base) / voice_stride].store(
            std::uint64_t(row) << 32 | static_cast<unsigned>(ctx.r2), std::memory_order_release);
}
