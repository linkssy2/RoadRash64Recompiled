#include "rr64_highlights.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_highlight_audio.hpp"
#include "rr64_highlight_recording.hpp"
#include "rr64_highlight_pose.hpp"
#include "rr64_highlight_weapon.hpp"
#include "rr64_highlight_traffic.hpp"
#include "rr64_highlight_timeline.hpp"
#include "rr64_highlight_camera.hpp"
#include "rr64_highlight_render_boundary.hpp"
#include "rr64_highlight_network.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_prediction_replay.hpp"
#include "rr64_online_flow.hpp"
#include "rr64_netplay.hpp"
#include "rr64_traffic_sync_capture.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_course_hazards.hpp"
#include "rr64_mk64_items.hpp"
#endif
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

extern "C" void func_800796F8(unsigned char *, recomp_context *);
extern "C" int rr64_online_wait_for_race(unsigned char *, unsigned);

namespace rr64::highlights {
namespace {
using namespace engine;
using Clock = std::chrono::steady_clock;
unsigned word(unsigned char *m, unsigned a) {
    unsigned v = 0;
    read_u32(m, a, v);
    return v;
}
unsigned half(unsigned char *m, unsigned a) {
    std::uint16_t v = 0;
    read_u16(m, a, v);
    return v;
}
float scalar(unsigned char *m, unsigned a) {
    float v = 0;
    read_float(m, a, v);
    return v;
}
template <std::size_t N> bool floats(unsigned char *m, unsigned a, std::array<float, N> &out) {
    for (unsigned i = 0; i < N; ++i)
        if (!read_float(m, a + i * 4, out[i]) || !std::isfinite(out[i]))
            return false;
    return true;
}
struct Links {
    unsigned bike = 0, rider = 0, route = 0;
    bool operator==(const Links &) const = default;
};
bool links(unsigned char *m, unsigned slot, Links &out) {
    if (slot >= maximum_racers)
        return false;
    const unsigned actor = 0x800D8570 + slot * 0x118;
    out = {word(m, actor + 0xE0), word(m, actor + 0xE4), word(m, actor + 0xE8)};
    return valid_guest_range(out.bike, bike::stride) &&
           valid_guest_range(out.rider, rider::stride) && valid_guest_range(out.route, 0x64) &&
           word(m, out.bike + 0x800) == out.rider && word(m, out.rider + 0x584) == out.bike;
}
// Reversible presentation writes only. All original bytes are restored before
// the frame returns to the results dispatcher; no guest allocation or rewind.
struct RenderWrites {
    struct Word {
        unsigned address = 0, bits = 0;
    };
    std::array<Word, 4096> original{};
    unsigned count = 0;
    unsigned char *memory = nullptr;
    bool save(unsigned address) {
        if (!memory || (address & 3) || !valid_guest_range(address, 4))
            return false;
        for (unsigned i = 0; i < count; ++i)
            if (original[i].address == address)
                return true;
        if (count == original.size())
            return false;
        original[count++] = {address, word(memory, address)};
        return true;
    }
    void put(unsigned address, unsigned bits) {
        if (save(address))
            write_u32(memory, address, bits);
    }
    void short_put(unsigned address, unsigned value) {
        if (save(address & ~3u))
            write_u16(memory, address, std::uint16_t(value));
    }
    template <std::size_t N> void vector(unsigned address, const std::array<float, N> &values) {
        for (unsigned i = 0; i < N; ++i)
            put(address + i * 4, std::bit_cast<unsigned>(values[i]));
    }
    void restore() {
        while (count) {
            const auto w = original[--count];
            write_u32(memory, w.address, w.bits);
        }
        memory = nullptr;
    }
};
struct DetailedPair {
    unsigned char *memory = nullptr;
    std::uint64_t generation = 0;
    unsigned epoch = 0, clock = 0, bike_node = 0, rider_node = 0;
    Pose bike{}, rider{};
    WeaponPose weapon{};
};
// One-way mailbox: producer LOD -> mailbox, consumer recorder -> mailbox.
// Publication never waits for the recorder mutex held across replay drawing.
struct DetailMailbox {
    std::mutex mutex;
    std::array<DetailedPair, maximum_racers> pairs{};
    std::uint64_t generation = 1;
};
DetailMailbox detail_mailbox;
void clear_detail() {
    std::lock_guard lock(detail_mailbox.mutex);
    ++detail_mailbox.generation;
    detail_mailbox.pairs = {};
}
bool copy_detail(unsigned char *m, unsigned slot, unsigned bike_node, unsigned rider_node,
                 Pose &bike_pose, Pose &rider_pose, WeaponPose *weapon = nullptr) {
    std::lock_guard lock(detail_mailbox.mutex);
    const auto &pair = detail_mailbox.pairs[slot];
    if (pair.memory != m || pair.generation != detail_mailbox.generation ||
        pair.epoch != word(m, 0x800A1830) || pair.clock != word(m, 0x800D7670) ||
        pair.bike_node != bike_node || pair.rider_node != rider_node)
        return false;
    bike_pose = pair.bike;
    rider_pose = pair.rider;
    if (weapon)
        *weapon = pair.weapon;
    return true;
}
struct State {
    std::recursive_mutex mutex;
    std::unique_ptr<Recorder> recorder;
    std::unique_ptr<Frame> capture, shown;
    unsigned char *memory = nullptr;
    bool racing = false, ended = false, holding = false, playing = false, disabled = false;
    bool draw = false, online = false, host = false, audio_stopped = false;
    bool traffic_drawn = false;
    unsigned round = 0, capture_epoch = ~0u, capture_clock = 0;
    bool capture_enrolled = false;
    bool block_dispatch = false, finish_pending = false;
    std::uint64_t tick = 0, last_time = 0, elapsed = 0;
    Clock::time_point started{}, waiting{};
    std::span<const Clip> playlist{};
    std::array<Clip, maximum_clips> selected{};
    highlight_network::Playlist received;
    Cursor cursor{};
    RenderWrites writes;
    Vec3 camera_origin{};
    std::array<Links, maximum_racers> native{};
    std::array<unsigned, maximum_racers> canonical{};
    std::array<float, maximum_racers> preceding_speed{};
    std::array<Links, maximum_racers> recording_links{};
    std::array<std::uint32_t, maximum_racers> recording_generation{};
    std::uint64_t recovery_cuts = 0, identity_cuts = 0;
    std::uint64_t replay_detailed = 0, replay_coarse = 0, replay_rejected = 0;
    struct BindingFailure {
        bool recorded = false, active = false;
        unsigned node = 0, canonical = 0;
        BindReport source{}, local{};
        Vec3 anchor{}, camera{};
    };
    std::array<BindingFailure, maximum_racers * 2> replay_failures{};
};
// The private-pose producer owns a different mutex. Its admission query must
// not acquire this recorder's lock while deciding whether to prepare a pair.
std::atomic<unsigned char *> detail_mapping{nullptr};
State &state() {
    static State s;
    return s;
}
thread_local bool draw_lock = false;
bool authoritative(const netplay::Status &status) {
    return !status.active || (status.connected && status.is_host && status.authoritative &&
                              !status.host_disconnected);
}
bool allocate(State &s) {
    if (s.recorder)
        return true;
    try {
        s.recorder = std::make_unique<Recorder>();
        s.capture = std::make_unique<Frame>();
        s.shown = std::make_unique<Frame>();
        return true;
    } catch (...) {
        s.recorder.reset();
        s.capture.reset();
        s.shown.reset();
        s.disabled = true;
        std::fprintf(stderr, "[highlights] Recording unavailable; race/results remain playable.\n");
        return false;
    }
}
void finish(State &s) {
    detail_mapping.store(nullptr, std::memory_order_release);
    // Flush bounded rejection samples at the existing game-thread handoff.
    // The render path only fills fixed storage; it never logs or allocates.
    for (unsigned i = 0; i < s.replay_failures.size(); ++i) {
        const auto &f = s.replay_failures[i];
        if (!f.recorded)
            continue;
        std::fprintf(stderr,
            "[highlights-bind] host=%u slot=%u canonical=%u rider=%u node=%08X active=%u reason=%s "
            "recorded=%u/%u/%u/%u/%016llX local=%u/%u/%u/%u/%016llX graph=%08X "
            "anchor=%.9g,%.9g,%.9g camera=%.9g,%.9g,%.9g scale=%.9g normalized=%u root=%.9g,%.9g,%.9g\n",
            unsigned(s.host), i / 2, f.canonical, i % 2, f.node, unsigned(f.active),
            bind_failure_name(f.local.failure), f.source.records, f.source.bones, f.source.lod, f.source.bank,
            static_cast<unsigned long long>(f.source.topology), f.local.records, f.local.bones, f.local.lod, f.local.bank,
            static_cast<unsigned long long>(f.local.topology), f.local.graph,
            f.anchor[0], f.anchor[1], f.anchor[2], f.camera[0], f.camera[1], f.camera[2],
            f.local.scale, unsigned(f.local.normalized), f.local.root[0], f.local.root[1], f.local.root[2]);
    }
    s.replay_failures = {};
    if (rr64::diagnostics::routine_enabled() &&
        (s.replay_detailed || s.replay_coarse || s.replay_rejected)) {
        std::fprintf(stderr, "[highlights] Replay detail: tier0=%llu coarse=%llu rejected=%llu.\n",
                     static_cast<unsigned long long>(s.replay_detailed),
                     static_cast<unsigned long long>(s.replay_coarse),
                     static_cast<unsigned long long>(s.replay_rejected));
    }
    s.replay_detailed = s.replay_coarse = s.replay_rejected = 0;
    s.holding = false;
    s.playing = false;
    s.finish_pending = false;
    s.cursor = {};
}
void report_clips(const State &s) {
    // The scan only describes already selected clips. Keep it entirely out of
    // ordinary race completion; it does not validate or select replay poses.
    if (!rr64::diagnostics::routine_enabled()) {
        return;
    }
    std::fprintf(stderr, "[highlights] Sealed %zu crash clips; race results fixed.\n",
                 s.playlist.size());
    std::array<std::uint64_t, 2> detailed{}, coarse{};
    for (const auto &clip : s.playlist) {
        for (const auto &frame : clip.frames) {
            for (const auto &racer : frame.racers) {
                if (!racer.active) continue;
                for (unsigned kind = 0; kind < 2; ++kind) {
                    const auto &pose = kind ? racer.rider_pose : racer.bike_pose;
                    if (pose.valid) {
                        ++(pose.lod == 0 ? detailed[kind] : coarse[kind]);
                    }
                }
            }
        }
    }
    std::fprintf(stderr, "[highlights] Recorded clip detail: bike tier0=%llu coarse=%llu; rider tier0=%llu coarse=%llu.\n",
                 static_cast<unsigned long long>(detailed[0]), static_cast<unsigned long long>(coarse[0]),
                 static_cast<unsigned long long>(detailed[1]), static_cast<unsigned long long>(coarse[1]));
    std::fprintf(stderr, "[highlights] Recording continuity: recovery-cuts=%llu identity-cuts=%llu.\n",
                 static_cast<unsigned long long>(s.recovery_cuts),
                 static_cast<unsigned long long>(s.identity_cuts));
}

void select_clips(State &s) {
    unsigned count = 0;
    for (const auto &original : s.recorder->seal()) {
        auto clip = original;
        const auto drawable = [&](const Frame &f) {
            const auto &r = f.racers[clip.slot];
            return r.active && r.bike_pose.valid && r.rider_pose.valid;
        };
        while (!clip.frames.empty() && !drawable(clip.frames.front()))
            clip.frames = clip.frames.subspan(1);
        while (!clip.frames.empty() && !drawable(clip.frames.back()))
            clip.frames = clip.frames.first(clip.frames.size() - 1);
        if (clip.frames.size() < 2 || clip.frames.front().time_us > clip.event_time_us ||
            clip.frames.back().time_us < clip.event_time_us ||
            clip.frames.back().time_us - clip.frames.front().time_us < 500000)
            continue;
        if (!std::all_of(clip.frames.begin(), clip.frames.end(), drawable))
            continue;
        s.selected[count++] = clip;
    }
    s.playlist = {s.selected.data(), count};
}
void trace_trajectories(const State &s, const netplay::Status &status) {
    const char *enabled = std::getenv("RR64_COURSE_PHYSICS_TRACE");
    if (!enabled || enabled[0] != '1' || enabled[1] != '\0')
        return;
    // Seal-time only: at most three subjects and 180 authored samples each.
    // IEEE-754 words preserve the exact recorded world coordinates, including
    // render-anchor offsets, without querying terrain or changing playback.
    unsigned index = 0;
    for (const auto &clip : s.playlist.first(std::min(s.playlist.size(), std::size_t(maximum_clips)))) {
        unsigned native = maximum_racers;
        for (unsigned i = 0; i < maximum_racers; ++i)
            if ((status.active ? online_flow::mapped_slot(i, status.local_slot, status.replicated_riders) : i) == clip.slot)
                native = i;
        std::fprintf(stderr, "[highlights-trajectory] {\"schema\":1,\"clip\":%u,\"slot\":%u,\"native_slot\":%u,\"event_tick\":%llu,\"event_us\":%llu,\"frames\":%zu,\"axis\":\"native_xyz_height_z\"}\n",
                     index, clip.slot, native, static_cast<unsigned long long>(clip.event_tick),
                     static_cast<unsigned long long>(clip.event_time_us), clip.frames.size());
        for (const auto &frame : clip.frames.first(std::min(clip.frames.size(), std::size_t(maximum_clip_frames)))) {
            const auto &r = frame.racers[clip.slot];
            std::array<char, 2048> row{};
            std::size_t used = 0;
            bool valid = true;
            const auto append = [&](const char *format, auto... args) {
                if (!valid)
                    return;
                const int n = std::snprintf(row.data() + used, row.size() - used, format, args...);
                valid = n >= 0 && std::size_t(n) < row.size() - used;
                if (valid)
                    used += std::size_t(n);
            };
            append("[highlights-trajectory] {\"clip\":%u,\"tick\":%llu,\"time_us\":%llu,\"generation\":%u,\"flags\":%u,\"model\":%u,\"character\":%u",
                         index, static_cast<unsigned long long>(frame.tick),
                         static_cast<unsigned long long>(frame.time_us), r.generation, r.crash_flags, r.model, r.character);
            const auto values = [&](const char *label, const auto &v) {
                append(",\"%s_bits\":[", label);
                for (unsigned i = 0; i < v.size(); ++i)
                    append("%s%u", i ? "," : "", std::bit_cast<unsigned>(v[i]));
                append("]");
            };
            values("bike_origin", r.bike_origin);
            values("rider_origin", r.rider_origin);
            values("bike_anchor", r.bike_anchor);
            values("rider_anchor", r.rider_anchor);
            values("bike_rotation", r.bike_rotation);
            values("rider_rotation", r.rider_rotation);
            append(",\"bike_lod\":%u,\"rider_lod\":%u,\"bike_bank\":%u,\"rider_bank\":%u}\n",
                         r.bike_pose.lod, r.rider_pose.lod, r.bike_pose.source_bank, r.rider_pose.source_bank);
            if (valid)
                std::fwrite(row.data(), 1, used, stderr);
        }
        ++index;
    }
}
void text(unsigned char *m, void *opaque, const char *label, float x, float y, float scale) {
    if (!opaque)
        return;
    auto *rdram = m; // Native memory macros use this binding.
    auto c = *static_cast<recomp_context *>(opaque);
    c.f_odd = &c.f0.u32h;
    c.r29 -= 160;
    if (!valid_guest_range(unsigned(c.r29), 160))
        return;
    unsigned n = 0;
    while (label[n] && n < 100) {
        MEM_B(48 + n, c.r29) = label[n];
        ++n;
    }
    MEM_B(48 + n, c.r29) = 0;
    MEM_W(32, c.r29) = 0xffff00ff;
    c.r4 = c.r29 + 48;
    c.r5 = guest_address(std::bit_cast<unsigned>(x));
    c.r6 = guest_address(std::bit_cast<unsigned>(y));
    c.r7 = c.r29 + 32;
    MEM_W(16, c.r29) = 3;
    MEM_W(20, c.r29) = std::bit_cast<unsigned>(scale);
    func_800796F8(m, &c);
}
unsigned find_slot(unsigned char *m, unsigned node, bool &rider_actor) {
    const unsigned kind = word(m, node), entity = word(m, node + 4);
    if (kind != 1 && kind != 2)
        return maximum_racers;
    rider_actor = kind == 2;
    for (unsigned slot = 0; slot < maximum_racers; ++slot) {
        Links current;
        if (!links(m, slot, current))
            continue;
        if ((rider_actor ? current.rider : current.bike) == entity)
            return slot;
    }
    return maximum_racers;
}
}

void reset() noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    end_actor();
    highlight_camera::end(s.memory);
    highlight_camera::reset(s.memory);
    s.writes.restore();
    reset_weapon_scratch();
    s.memory = nullptr;
    s.racing = s.ended = s.holding = s.playing = s.draw = s.block_dispatch = s.finish_pending = false;
    s.audio_stopped = false;
    s.traffic_drawn = false;
    s.playlist = {};
    s.tick = s.last_time = 0;
    s.preceding_speed = {};
    s.recording_links = {};
    s.recording_generation = {};
    s.recovery_cuts = s.identity_cuts = 0;
    clear_detail();
    detail_mapping.store(nullptr, std::memory_order_release);
    s.replay_detailed = s.replay_coarse = s.replay_rejected = 0;
    s.replay_failures = {};
    if (s.recorder)
        s.recorder->reset();
}
const netplay::CourseHazardState *render_hazards() noexcept {
    auto &s = state();
    return s.draw && s.playing && s.shown ? &s.shown->hazards : nullptr;
}
const mk64_items::Snapshot *render_items() noexcept {
    auto &s = state();
    return s.draw && s.playing && s.shown ? &s.shown->items : nullptr;
}
bool render_rider_anchors(unsigned slot, std::array<float, 3> &bike,
                          std::array<float, 3> &rider, bool &attached,
                          std::array<float, 3> &bike_origin) noexcept {
    auto &s = state();
    if (!s.draw || !s.playing || !s.shown || slot >= maximum_racers || !s.shown->racers[slot].active)
        return false;
    const auto &r = s.shown->racers[slot];
    bike = r.bike_anchor;
    rider = r.rider_anchor;
    bike_origin = r.bike_origin;
    attached = (r.crash_flags & (BikeAttached | RiderAttached)) == (BikeAttached | RiderAttached);
    return true;
}

static int wait_for_results(unsigned char *m, unsigned mode, bool native_blocked) {
    if (!m || prediction::active())
        return 0;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    detail_mapping.store(nullptr, std::memory_order_release);
    s.block_dispatch = false;
    const auto status = netplay::get_status();
    // The attract demo has its own mode family and is never enrolled.
    if (engine::is_live_race_mode(mode)) {
        if (status.active &&
            (!status.connected || !status.authoritative || status.host_disconnected))
            return 0;
        if (!s.racing || s.memory != m || s.round != status.game_setup.revision) {
            // Prewarm owned traffic resources at race entry, never at a crash
            // or on the graphics worker. Failure retains native presentation.
            prepare_traffic(m);
            highlight_camera::reset(nullptr);
            s.racing = true;
            s.ended = s.holding = s.playing = s.finish_pending = false;
            s.audio_stopped = false;
            s.memory = m;
            s.round = status.game_setup.revision;
            s.online = status.active;
            s.host = status.is_host;
            s.cursor = {};
            s.tick = s.last_time = 0;
            s.preceding_speed = {};
            s.recording_links = {};
            s.recording_generation = {};
            s.recovery_cuts = s.identity_cuts = 0;
            clear_detail();
            s.capture_enrolled = false;
            s.replay_detailed = s.replay_coarse = s.replay_rejected = 0;
            s.replay_failures = {};
            s.playlist = {};
            if (authoritative(status) && !s.disabled && allocate(s))
                s.recorder->begin(s.round + 1);
        }
        if (authoritative(status) && !s.disabled && s.recorder && !s.ended)
            detail_mapping.store(m, std::memory_order_release);
        return 0;
    }
    if (!engine::is_race_results_mode(mode)) {
        // Native mode39 is an intervening transition; retain its completed
        // race until results arrives. Menus/new game images cancel it.
        if (mode != 0x39) {
            highlight_camera::reset(m);
            s.racing = false;
            s.ended = false;
            finish(s);
        }
        return 0;
    }
    if (status.host_disconnected || (s.online && !status.connected)) {
        highlight_camera::reset(m);
        finish(s);
        return 0;
    }
    if (s.finish_pending) {
        // The graphics worker draws before this gate. Keep the final replay
        // through the skipped-results tick, then release all native producers
        // together so they prepare the next ordinary draw. Online barriers
        // must not leave presentation released while its update is still held.
        if (native_blocked) {
            s.block_dispatch = true;
            return 1;
        }
        finish(s);
        return 0;
    }
    if (!s.ended && s.racing) {
        s.ended = true;
        s.waiting = Clock::now();
        if (authoritative(status) && s.recorder && !s.disabled) {
            select_clips(s);
            s.holding = s.playing = duration(s.playlist) > 0;
            s.started = Clock::now();
            report_clips(s);
            trace_trajectories(s, status);
        }
        if (s.online) {
            s.holding = true;
            s.playing = false;
            if (s.host) {
                highlight_network::Blob blob;
                if (!s.playlist.empty() && !highlight_network::encode(s.playlist, blob))
                    s.playlist = {};
                if (!netplay::publish_highlight_playlist(std::move(blob), duration(s.playlist)))
                    finish(s);
            }
        }
    }
    if (!s.holding)
        return 0;
    s.block_dispatch = true;
    std::uint16_t pressed = 0;
    read_u16(m, engine::globals::controller_pressed_buttons, pressed);
    if (pressed & 0x8000) {
        if (!s.online)
            s.finish_pending = true;
        else if (s.host)
            netplay::host_skip_highlights();
    }
    if (s.online) {
        if (!s.host)
            if (auto blob = netplay::take_received_highlight_playlist()) {
                bool accepted = highlight_network::decode(*blob, s.received);
                if (accepted) {
                    try {
                        if (!s.shown)
                            s.shown = std::make_unique<Frame>();
                    } catch (...) {
                        accepted = false;
                    }
                }
                if (accepted)
                    s.playlist = s.received.view();
                netplay::acknowledge_highlight_decoded(accepted);
            }
        if (s.host)
            netplay::host_begin_highlights();
        const auto playback = netplay::highlight_status();
        if (playback.stage == highlight_network::Stage::Finished ||
            (playback.stage != highlight_network::Stage::Playing &&
             Clock::now() - s.waiting > std::chrono::seconds(65))) {
            s.finish_pending = true;
        } else {
            s.playing = playback.stage == highlight_network::Stage::Playing;
            s.elapsed = playback.elapsed_us;
        }
    } else if (s.playing) {
        s.elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s.started).count();
    }
    if (s.playing && (!s.finish_pending || !s.cursor.valid)) {
        const auto next = locate(s.playlist, s.elapsed);
        if (next.valid && sample(s.playlist[next.clip], next.source_us, *s.shown)) {
            s.cursor = next;
        } else {
            // Failed sampling leaves shown unchanged. Retain the last valid
            // camera/sample until the same coordinated results handoff.
            s.playing = s.cursor.valid;
            s.finish_pending = true;
        }
    }
    // Consume the skip edge on this frame; it must not also activate the
    // normal results screen's A Continue action underneath the replay.
    return 1;
}
} // namespace rr64::highlights

extern "C" int rr64_highlights_wait(unsigned char *m, unsigned mode) {
    return rr64::highlights::wait_for_results(m, mode, false);
}

extern "C" int rr64_highlights_frame_gate(unsigned char *m, unsigned mode, void *context) {
    // Always service both gates in this order. Online must latch the original
    // finish mode before publishing footage, even on the first results frame.
    // Highlights must observe disconnection even if online is holding updates.
    const bool online_wait = rr64_online_wait_for_race(m, mode) != 0;
    const bool replay_wait = rr64::highlights::wait_for_results(m, mode, online_wait) != 0;
    auto &s = rr64::highlights::state();
    std::lock_guard lock(s.mutex);
    // Holding skips native bike updates, including stale brake-loop expiry.
    // Stop once before drawing/waiting for the reel on every participant.
    if (s.holding && s.memory == m && !s.audio_stopped)
        s.audio_stopped = rr64::highlights::stop_race_loops(m, context);
    return online_wait || replay_wait;
}

extern "C" int rr64_highlights_presenting() {
    auto &s = rr64::highlights::state();
    std::lock_guard lock(s.mutex);
    return s.holding;
}
extern "C" int rr64_highlights_needs_detail(unsigned char *m) {
    return m && !rr64::prediction::active() &&
           rr64::highlights::detail_mapping.load(std::memory_order_acquire) == m;
}
extern "C" void rr64_highlights_capture_detail(unsigned char *m, unsigned char *prepared,
                                               unsigned bike_node, unsigned rider_node) {
    using namespace rr64;
    using namespace highlights;
    if (!rr64_highlights_needs_detail(m) || m == prepared)
        return;
    if (half(m, engine::globals::gameplay_pause_state))
        return;
    DetailedPair pair;
    {
        std::lock_guard lock(detail_mailbox.mutex);
        pair.generation = detail_mailbox.generation;
    }
    bool bike_is_rider = false, rider_is_rider = false;
    const auto slot = find_slot(m, bike_node, bike_is_rider);
    if (slot >= maximum_racers || bike_is_rider ||
        find_slot(m, rider_node, rider_is_rider) != slot || !rider_is_rider)
        return;
    if (!capture_prepared_node(m, prepared, bike_node, pair.bike) ||
        !capture_prepared_node(m, prepared, rider_node, pair.rider))
        return;
    pair.epoch = word(prepared, 0x800A1830);
    pair.clock = word(prepared, 0x800D7670);
    pair.bike_node = bike_node;
    pair.rider_node = rider_node;
    // Capture children after the same private native attack producer that
    // prepared the rider, including AI outside the live camera's range.
    capture_weapon(prepared, word(prepared, rider_node + 4), pair.weapon);
    pair.memory = m;
    std::lock_guard lock(detail_mailbox.mutex);
    if (rr64_highlights_needs_detail(m) && pair.generation == detail_mailbox.generation &&
        pair.epoch == word(m, 0x800A1830) && pair.clock == word(m, 0x800D7670))
        detail_mailbox.pairs[slot] = pair;
}
extern "C" int rr64_highlights_block_dispatch() {
    auto &s = rr64::highlights::state();
    std::lock_guard lock(s.mutex);
    return s.block_dispatch;
}
extern "C" void rr64_highlights_crash(unsigned char *m, unsigned bike) {
    using namespace rr64;
    using namespace highlights;
    if (prediction::active())
        return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.racing || s.ended || !s.recorder || m != s.memory ||
        !authoritative(netplay::get_status()))
        return;
    for (unsigned i = 0; i < maximum_racers; ++i) {
        Links l;
        if (!links(m, i, l) || l.bike != bike)
            continue;
        Vec3 v{};
        if (!floats(m, bike + 0x178, v))
            return;
        float speed = 0;
        for (float component : v)
            speed += component * component;
        const auto status = netplay::get_status();
        const auto canonical =
            status.active ? online_flow::mapped_slot(i, status.local_slot, status.replicated_riders)
                          : i;
        // A solid wall can stop a major crash almost completely before this
        // native detachment hook. Retain the preceding authored speed so that
        // a stopped hard hit is not ranked below a minor moving spill.
        s.recorder->mark_crash(canonical, 1 + std::max(std::sqrt(speed), s.preceding_speed[canonical]));
        return;
    }
}
extern "C" void rr64_highlights_recovery(unsigned char *m, unsigned actor) {
    using namespace rr64;
    using namespace highlights;
    if (!m || prediction::active() || actor < 0x800D8570 ||
        (actor - 0x800D8570) % 0x118 != 0)
        return;
    const unsigned slot = (actor - 0x800D8570) / 0x118;
    if (slot >= maximum_racers)
        return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    Links current;
    if (!s.racing || s.ended || !s.recorder || s.memory != m ||
        !authoritative(netplay::get_status()) || !links(m, slot, current))
        return;
    // 68E20:69464 is the common committed relocation path, including AI.
    // Even a nearby reset must be a presentation cut, not a motion blend.
    ++s.recording_generation[slot];
    ++s.recovery_cuts;
}
extern "C" void rr64_highlights_capture(unsigned char *m) {
    using namespace rr64;
    using namespace highlights;
    if (prediction::active())
        return;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    s.capture_enrolled = false;
    if (!s.racing || s.ended || !s.recorder || s.memory != m ||
        !authoritative(netplay::get_status()) || half(m, engine::globals::gameplay_pause_state))
        return;
    const float elapsed = scalar(m, 0x800D7670);
    if (!std::isfinite(elapsed) || elapsed < 0 || elapsed > 86400)
        return;
    const auto now = static_cast<std::uint64_t>(double(elapsed) * 1000000);
    if (s.tick && now < s.last_time) {
        s.recorder->begin(s.round + 1);
        s.tick = s.last_time = 0;
        s.recording_links = {};
        s.recording_generation = {};
        s.recovery_cuts = s.identity_cuts = 0;
        clear_detail();
    }
    if (s.tick && now < s.last_time + 32000)
        return; // bounded ~30Hz, independent of render FPS
    auto &frame = *s.capture;
    frame = {};
    frame.time_us = now;
    frame.tick = s.tick + 1;
    unsigned count = word(m, 0x800A656C);
    if (!count || count > maximum_racers)
        return;
    const auto status = netplay::get_status();
    for (unsigned i = 0; i < count; ++i) {
        const unsigned actor = 0x800D8570 + i * 0x118;
        if (!half(m, actor + 0x24))
            continue;
        Links l;
        if (!links(m, i, l))
            return;
        const auto canonical =
            status.active ? online_flow::mapped_slot(i, status.local_slot, status.replicated_riders)
                          : i;
        auto &r = frame.racers[canonical];
        r.active = true;
        r.model = word(m, actor + 0x18);
        r.character = word(m, actor + 0x1C);
        r.weapon = word(m, l.rider + 0x5B0);
        if (!(s.recording_links[i] == l)) {
            if (s.recording_links[i].bike)
                ++s.identity_cuts;
            ++s.recording_generation[i];
            s.recording_links[i] = l;
        }
        // Native 6E5E0:6EBEC rewrites route+40 with current standings rank.
        // Overtakes do not interrupt motion; only recovery/ownership does.
        r.generation = s.recording_generation[i];
        r.crash_flags = (half(m, l.bike + 0x7F8) ? BikeAttached : 0u) |
                        (half(m, l.rider + 0x57C) ? RiderAttached : 0u) |
                        (half(m, l.rider + 0x57E) ? Ejected : 0u) |
                        (half(m, l.bike + 0x7F6) ? DriveLockout : 0u);
        if (!floats(m, l.bike + 0x16C, r.bike_origin) ||
            !floats(m, l.rider + 0x8C, r.rider_origin) ||
            !floats(m, l.bike + 0x53C, r.bike_anchor) ||
            !floats(m, l.rider + 0x5DC, r.rider_anchor) ||
            !floats(m, l.bike + 0x244, r.bike_rotation) ||
            !floats(m, l.rider + 0x164, r.rider_rotation))
            return;
        // The dispatcher waits for the drawing worker before this update;
        // private preparation publishes synchronously before capture. Prefer
        // its current validated pair; absent/stale pairs retain stock poses,
        // including held off-camera animation and the current weapon state.
        if (!copy_detail(m, i, word(m, l.bike + 8), word(m, l.rider + 8), r.bike_pose,
                         r.rider_pose, &r.held_weapon)) {
            capture_entity(m, l.bike, 1, r.bike_pose);
            capture_entity(m, l.rider, 2, r.rider_pose);
            capture_weapon(m, l.rider, r.held_weapon);
        }
        Vec3 velocity{};
        if (floats(m, l.bike + 0x178, velocity)) {
            float speed = 0;
            for (float component : velocity)
                speed += component * component;
            if (std::isfinite(speed))
                s.preceding_speed[canonical] = std::sqrt(speed);
        }
    }
    world_sync::Snapshot traffic;
    if (world_sync::capture_traffic(m, 1, frame.tick, traffic))
        frame.traffic = traffic.traffic;
#ifdef RR64_EXPERIMENTAL_COURSE
    frame.hazards = course_hazards::capture_state();
    frame.items = mk64_items::capture_state();
#endif
    if (s.recorder->push(frame)) {
        s.tick = frame.tick;
        s.last_time = now;
        s.capture_epoch = word(m, 0x800A1830);
        s.capture_clock = word(m, 0x800D7670);
        s.capture_enrolled = true;
    }
}

extern "C" void rr64_highlights_draw_begin(unsigned char *m) {
    using namespace rr64;
    using namespace highlights;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (draw_lock)
        return;
    s.draw = false;
    s.traffic_drawn = false;
    if (!s.holding || !s.playing || m != s.memory || !s.cursor.valid) {
        // Existing native recovery/ownership generations are copied while the
        // recorder is locked. This marks known camera cuts; it does not grant
        // permission to bypass AUTO motion rejection for an unchanged phase.
        if (s.racing && !s.ended && s.recorder && m == s.memory &&
            !prediction::active() && !netplay::get_status().active)
            highlight_render::begin_normal(m, s.round, s.recording_generation);
        else
            rr64_highlight_render_begin(m, 0);
        return;
    }
    // The native update and graphics worker share guest memory. Keep the
    // selected sample immutable until this complete draw restores its bytes.
    s.mutex.lock();
    draw_lock = true;
    s.writes.restore();
    s.writes.memory = m;
    s.draw = true;
    const auto status = netplay::get_status();
    for (unsigned i = 0; i < maximum_racers; ++i) {
        s.native[i] = {};
        s.canonical[i] =
            status.active ? online_flow::mapped_slot(i, status.local_slot, status.replicated_riders)
                          : i;
        Links l;
        if (!links(m, i, l))
            continue;
        s.native[i] = l;
        const auto &r = s.shown->racers[s.canonical[i]];
        if (!r.active)
            continue;
        s.writes.vector(l.bike + 0x16C, r.bike_origin);
        s.writes.vector(l.bike + 0x53C, r.bike_anchor);
        s.writes.vector(l.bike + 0x404, r.bike_origin);
        s.writes.vector(l.bike + 0x244, r.bike_rotation);
        s.writes.vector(l.rider + 0x8C, r.rider_origin);
        s.writes.vector(l.rider + 0x5DC, r.rider_anchor);
        s.writes.vector(l.rider + 0x164, r.rider_rotation);
    }
    // Match retained traffic by race-scoped identity, never by allocation slot.
    const unsigned cars = std::min(word(m, 0x800A6528), world_sync::capacity);
    for (unsigned i = 0; i < cars; ++i) {
        const unsigned entity = word(m, 0x800D76E0 + i * 4);
        if (!valid_guest_range(entity, 0x360))
            continue;
        const unsigned id = word(m, entity + 4);
        for (const auto &car : s.shown->traffic)
            if (car.active && car.id == id) {
                s.writes.vector(entity + 0xA8, car.motion);
                s.writes.vector(entity + 0x310, car.directions);
                s.writes.vector(entity + 0xA8, car.position);
                break;
            }
    }
    // Everyone watches the same single camera, including local split-screen.
    s.writes.put(0x800A4F24, 0);
    s.writes.put(0x8009DB88, 1);
    s.writes.short_put(0x800A65C2, 0);
    s.writes.short_put(engine::globals::gameplay_pause_state, 1);
    const auto &clip = s.playlist[s.cursor.clip];
    const auto &subject = s.shown->racers[clip.slot];
    Vec3 forward{1, 0, 0};
    const auto &first = clip.frames.front().racers[clip.slot];
    for (unsigned axis = 0; axis < 2; ++axis)
        forward[axis] = subject.bike_origin[axis] - first.bike_origin[axis];
    // Camera helper saves its globals; its hook runs before actor culling and
    // source-bank root/projection setup, keeping all consumers in one frame.
    highlight_camera::View view;
    if (highlight_camera::make_view(subject.rider_origin, forward, s.cursor.angle, view))
        highlight_camera::begin(m, view);
    // Carry the camera cut with its submitted graphics, including the world
    // rebase. A renderer-side read of live replay state can see a later frame.
    rr64_highlight_render_begin(m, highlight_camera::active()
        ? 1u + unsigned(s.cursor.clip) * 3u + s.cursor.angle : 0u);
}
extern "C" void rr64_highlights_camera_origin(unsigned char *m) {
    auto &s = rr64::highlights::state();
    if (s.draw)
        rr64::highlights::floats(m, 0x800D69F8, s.camera_origin);
}
extern "C" void rr64_highlights_traffic_draw(unsigned char *m) {
    using namespace rr64::highlights;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.draw && s.playing && s.memory == m && s.shown && !s.traffic_drawn)
        s.traffic_drawn = draw_traffic(m, s.shown->traffic, s.shown->time_us);
}
extern "C" unsigned rr64_highlights_lod(unsigned char *m, unsigned node, unsigned original) {
    using namespace rr64::highlights;
    auto &s = state();
    if (!s.draw || !s.playing)
        return original;
    bool rider_actor = false;
    const unsigned slot = find_slot(m, node, rider_actor);
    if (slot >= maximum_racers)
        return original;
    const auto &r = s.shown->racers[s.canonical[slot]];
    const auto &pose = rider_actor ? r.rider_pose : r.bike_pose;
    return r.active && pose.valid ? pose.lod : original;
}
extern "C" int rr64_highlights_actor(unsigned char *m, unsigned node, unsigned tier) {
    using namespace rr64;
    using namespace highlights;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    bool rider_actor = false;
    const unsigned slot = find_slot(m, node, rider_actor);
    if (slot >= maximum_racers)
        return 1;
    if (s.draw && s.playing) {
        const auto &r = s.shown->racers[s.canonical[slot]];
        const auto &pose = rider_actor ? r.rider_pose : r.bike_pose;
        BindReport report;
        const bool accepted = r.active &&
                              begin_actor(m, node, tier, pose, rider_actor ? r.rider_anchor : r.bike_anchor,
                                          rider_actor ? r.rider_rotation : r.bike_rotation, s.camera_origin,
                                          &report);
        if (accepted)
            ++(tier == 0 ? s.replay_detailed : s.replay_coarse);
        else {
            ++s.replay_rejected;
            // Fixed per-race budget: one first failure for each native actor.
            // A total rejection count alone cannot distinguish a graph/source
            // mismatch from a stale camera or a missing local pose allocation.
            auto &f = s.replay_failures[slot * 2 + unsigned(rider_actor)];
            if (!f.recorded) {
                f.recorded = true;
                f.active = r.active;
                f.node = node;
                f.canonical = s.canonical[slot];
                f.local = report;
                f.source.records = pose.record_count;
                f.source.bones = pose.count;
                f.source.lod = pose.lod;
                f.source.bank = pose.source_bank;
                f.source.topology = pose.topology;
                f.anchor = rider_actor ? r.rider_anchor : r.bike_anchor;
                f.camera = s.camera_origin;
            }
        }
        return accepted;
    }
    if (s.racing && !s.ended && s.recorder && s.capture_enrolled && m == s.memory &&
        s.capture_epoch == word(m, 0x800A1830) && s.capture_clock == word(m, 0x800D7670) &&
        authoritative(netplay::get_status())) {
        Pose pose, bike_pose, rider_pose;
        WeaponPose weapon;
        const auto status = netplay::get_status();
        const auto canonical = status.active ? online_flow::mapped_slot(slot, status.local_slot,
                                                                        status.replicated_riders)
                                             : slot;
        Links pair;
        const bool detailed = links(m, slot, pair) &&
                              copy_detail(m, slot, word(m, pair.bike + 8), word(m, pair.rider + 8),
                                          bike_pose, rider_pose, &weapon);
        if (detailed)
            s.recorder->update_weapon(s.tick, canonical, weapon);
        if (detailed)
            pose = rider_actor ? rider_pose : bike_pose;
        if (detailed || capture_node(m, node, tier, pose))
            s.recorder->update_pose(s.tick, canonical, rider_actor, pose);
    }
    return 1;
}
extern "C" int rr64_highlights_weapon_draw(unsigned char *m, void *context, unsigned node) {
    using namespace rr64::highlights;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.holding)
        return 0; // Exact original held-weapon behavior outside highlights.
    // Never fall back to a terminal-frame inventory model. That model may be
    // absent, a different weapon, or an allocation reused since this recording.
    if (s.draw && s.playing && s.memory == m && s.shown) {
        bool rider_actor = false;
        const auto slot = find_slot(m, node, rider_actor);
        if (slot < maximum_racers && rider_actor) {
            const auto &r = s.shown->racers[s.canonical[slot]];
            if (r.active && r.rider_pose.valid) {
                const bool shrunk = s.shown->items.enabled &&
                    s.shown->items.riders[s.canonical[slot]].shrink_until > s.shown->items.clock;
                const bool attached =
                    (r.crash_flags & (BikeAttached | RiderAttached)) == (BikeAttached | RiderAttached);
                draw_weapon(m, context, r.held_weapon, r.rider_anchor, r.rider_rotation,
                            s.camera_origin, r.rider_pose.source_bank, shrunk ? .5f : 1.f,
                            attached ? r.bike_origin : r.rider_anchor);
            }
        }
    }
    return 1;
}
extern "C" void rr64_highlights_actor_end() {
    rr64::highlights::end_actor();
}
extern "C" unsigned rr64_highlights_root_source(unsigned char *m, unsigned node, unsigned record, unsigned original) {
    return rr64::highlights::root_source(m, node, record, original);
}
extern "C" int rr64_highlights_scale_root_matrix(unsigned char *m, unsigned node, unsigned record, unsigned matrix) {
    return rr64::highlights::scale_root_matrix(m, node, record, matrix);
}
extern "C" unsigned rr64_highlights_hidden(unsigned char *m, unsigned node, unsigned original) {
    using namespace rr64::highlights;
    auto &s = state();
    if (!s.draw || !s.playing)
        return original;
    bool rider_actor = false;
    const unsigned slot = find_slot(m, node, rider_actor);
    if (slot < maximum_racers) {
        const auto &r = s.shown->racers[s.canonical[slot]];
        return !(r.active && (rider_actor ? r.rider_pose.valid : r.bike_pose.valid));
    }
    if (word(m, node) == 4) {
        // The owned historical pass is complete before scene actors draw.
        // Suppress terminal traffic only after that pass succeeds.
        if (s.traffic_drawn)
            return 1;
        const unsigned entity = word(m, node + 4), id = word(m, entity + 4);
        for (const auto &car : s.shown->traffic)
            if (car.active && car.id == id && car.kind == word(m, entity))
                return original;
        // Final-frame traffic is not historical footage. Do not invent a car
        // that was absent from this recorded instant or recycle its identity.
        return 1;
    }
    return original;
}
extern "C" void rr64_highlights_draw_end(unsigned char *m, void *context) {
    using namespace rr64::highlights;
    auto &s = state();
    std::lock_guard lock(s.mutex);
    end_actor();
    rr64_highlight_render_end(m);
    if (s.draw) {
        rr64::highlight_camera::end(m);
        s.writes.restore();
        s.draw = false;
    }
    if (s.holding) {
        text(m, context, s.playing ? "CRASH HIGHLIGHTS" : "PREPARING HIGHLIGHTS", 12, 12, .55f);
        text(m, context, s.online && !s.host ? "HOST CONTROLS PLAYBACK" : "A: CONTINUE", 12,
             224, .35f);
    }
    if (draw_lock) {
        draw_lock = false;
        s.mutex.unlock();
    }
}
