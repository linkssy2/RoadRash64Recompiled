#include "rr64_highlights.hpp"
#include "rr64_highlight_pose.hpp"
#include "rr64_highlight_camera.hpp"
#include "rr64_highlight_network.hpp"
#include "rr64_highlight_weapon.hpp"
#include "rr64_highlight_traffic.hpp"
#include "rr64_highlight_transition_clock.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
#include "rr64_prediction_replay.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <vector>

using namespace rr64;
using namespace rr64::engine;
namespace {
unsigned checks = 0, draws = 0, gate_calls = 0, publish_calls = 0, skip_calls = 0;
unsigned accepted_payloads = 0, rejected_payloads = 0;
bool finish_latched = false, online_blocked = false, client_delivery = false;
bool allow_host_begin = true;
netplay::Status session;
highlight_network::Status transport;
highlight_network::Blob published;
std::vector<unsigned> callback_order;
void check(bool yes, const char *label, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!yes) {
        std::cerr << "FAIL " << label << " (line " << where.line() << ")\n";
        std::exit(1);
    }
}
constexpr unsigned bike_entity = 0x80200000, rider_entity = 0x80202000;
constexpr unsigned bike_node = 0x80100000, rider_node = 0x80100200;
constexpr unsigned actor = 0x800d8570, route = 0x80204000;
constexpr std::array<unsigned, 3> results_callbacks{0x8006a600, 0x8006affc, 0x8006ff28};

void vector3(unsigned char *m, unsigned address, float x, float y, float z) {
    write_float(m, address, x); write_float(m, address + 4, y); write_float(m, address + 8, z);
}
float real(unsigned char *m, unsigned address) {
    float value = 0;
    check(read_float(m, address, value), "synthetic guest float readable");
    return value;
}

// Synthetic linked model records exercise the real recorder, portable pose
// validation, actor binding and camera restoration without shipping captures.
struct Scene {
    std::vector<unsigned char> memory = std::vector<unsigned char>(kRdramSize);
    unsigned char *data() { return memory.data(); }
    Scene() {
        auto *m = data();
        write_u32(m, 0x800a656c, 1); write_u16(m, actor + 0x24, 1);
        write_u32(m, actor + 0xe0, bike_entity); write_u32(m, actor + 0xe4, rider_entity);
        write_u32(m, actor + 0xe8, route);
        write_u32(m, bike_entity + 0x800, rider_entity); write_u32(m, rider_entity + 0x584, bike_entity);
        write_u16(m, bike_entity + 0x7f8, 1); write_u16(m, rider_entity + 0x57c, 1);
        write_float(m, bike_entity + 0x244 + 12, 1); write_float(m, rider_entity + 0x164 + 12, 1);
        vector3(m, bike_entity + 0x178, 20, 0, 0);
        write_u32(m, 0x800a1450, bike_node); write_u32(m, 0x800a1454, rider_node);
        write_float(m, 0x8009dbac, 10);
        // Replay camera deliberately invalidates the native viewport cache.
        // Start invalid so full-memory equality still tests every other byte.
        write_u16(m, 0x8009dba4, 1);
        for (unsigned kind = 1; kind <= 2; ++kind) {
            const unsigned n = kind == 1 ? bike_node : rider_node;
            const unsigned e = kind == 1 ? bike_entity : rider_entity;
            const unsigned graph = 0x80300000 + kind * 0x1000;
            const unsigned pose = 0x80400000 + kind * 0x1000;
            const unsigned source = 0x80500000 + kind * 0x1000;
            write_u32(m, n, kind); write_u32(m, n + actor_scene::entity, e);
            write_u32(m, e + 8, n);
            write_u32(m, n + actor_scene::lod_models, graph);
            write_u32(m, n + actor_scene::current_model, graph);
            constexpr std::array<unsigned, 5> types{0x13, 0x12, 0x10, 0x12, 0x10};
            constexpr std::array<unsigned, 5> flags{0x1000, 0x2000, 0, 0x2000, 0x4000};
            for (unsigned i = 0; i < types.size(); ++i) {
                const unsigned record = graph + i * 0x18;
                write_u16(m, record + 4, types[i]); write_u16(m, record + 8, i == 4 ? 0 : 3);
                write_u16(m, record + 10, flags[i]); write_u32(m, record + 12, pose + i * 0x40);
                write_u32(m, record + 20, source + i * 0x100); write_u32(m, source + i * 0x100, types[i]);
                for (unsigned j = 0; j < 7; ++j)
                    write_float(m, pose + i * 0x40 + j * 4, j < 3 ? float(i + j) : j == 6 ? 1.f : 0.f);
            }
            highlights::Pose captured;
            check(highlights::capture_entity(m, e, kind, captured), "synthetic native pose admitted by production capture");
        }
        // These callback targets are the retained native mode-table values.
        // The generated instruction slice itself performs every table lookup.
        for (unsigned mode : {0xbu, 0x14u, 0x19u, 0x1eu}) {
            const unsigned base = 0x800a1bdc + mode * 24;
            write_u32(m, base + 8, results_callbacks[0]); write_u32(m, base + 12, 0x8006a638);
            write_u32(m, base + 16, results_callbacks[1]);
            write_u32(m, base + 20, mode == 0xb ? 0x80071f94 : results_callbacks[2]);
        }
    }
};

void reset() {
    highlights::reset();
    highlight_test::Clock::current = {};
    session = {}; transport = {}; published = {};
    finish_latched = online_blocked = client_delivery = false;
    allow_host_begin = true;
    callback_order.clear();
}
void record(Scene &s, unsigned live_mode = 0x1d, bool crash = true) {
    auto *m = s.data();
    write_u16(m, globals::controller_pressed_buttons, 0); write_u16(m, globals::gameplay_pause_state, 0);
    write_u32(m, globals::main_mode, live_mode); write_u32(m, globals::pending_mode, live_mode);
    check(rr64_highlights_frame_gate(m, live_mode, nullptr) == 0, "live mode enrolls through actual ordered gates");
    for (unsigned i = 0; i < 240; ++i) {
        write_float(m, 0x800d7670, float(i) / 30);
        vector3(m, bike_entity + 0x16c, float(i), 10, 1);
        vector3(m, bike_entity + 0x53c, float(i), 10, 1);
        vector3(m, rider_entity + 0x8c, float(i), 10, 2);
        vector3(m, rider_entity + 0x5dc, float(i), 10, 2);
        if (crash && i == 90)
            rr64_highlights_crash(m, bike_entity);
        rr64_highlights_capture(m);
    }
}
void expect_callbacks(bool blocked, unsigned result_mode = 0x1e) {
    if (blocked) {
        check(callback_order.empty(), "held native dispatcher skips all three producers");
    } else {
        check(callback_order == std::vector<unsigned>{results_callbacks[0], results_callbacks[1],
                  result_mode == 0xb ? 0x80071f94 : results_callbacks[2]},
              "release runs native update, prepare, then results exactly once");
    }
}
struct Draw {
    std::array<float, 6> camera{};
    float bike_x = 0;
    bool operator==(const Draw &) const = default;
};
Draw worker(Scene &s, bool replay) {
    const auto before = s.memory;
    auto *m = s.data();
    rr64_highlights_draw_begin(m);
    check(highlight_camera::active() == replay, "worker retains prepared replay until native producers can resume");
    Draw result;
    if (replay) {
        rr64_highlight_camera_apply(m); rr64_highlights_camera_origin(m);
        check(rr64_highlights_viewport_inset(2) == 0, "replay worker uses full viewport");
        for (unsigned i = 0; i < 3; ++i) {
            result.camera[i] = real(m, 0x800d6a28 + i * 4);
            result.camera[i + 3] = real(m, 0x800d69f8 + i * 4);
        }
        result.bike_x = real(m, bike_entity + 0x16c);
        for (unsigned n : {bike_node, rider_node}) {
            check(!rr64_highlights_hidden(m, n, 1), "recorded rider and bike remain visible on held exit frame");
            check(rr64_highlights_actor(m, n, 0), "held exit frame binds actual recorded actor graph");
            rr64_highlights_actor_end();
        }
    } else {
        check(rr64_highlights_viewport_inset(2) == 2, "ordinary draw restores native viewport inset");
    }
    rr64_highlights_draw_end(m, nullptr);
    check(before == s.memory, "worker restores all guest actor, camera and layout bytes");
    check(!highlight_camera::active() && !highlights::actor_bound(), "worker leaves no live presentation override");
    ++draws;
    return result;
}
}

extern "C" void highlight_dispatch_native(unsigned char *, recomp_context *);
extern "C" void fixture_callback(unsigned target, unsigned char *, recomp_context *) {
    check(target == 0x8006a600 || target == 0x8006affc || target == 0x8006ff28 || target == 0x80071f94,
          "bounded native dispatcher never duplicates the earlier worker draw");
    callback_order.push_back(target);
}
extern "C" void func_800796F8(unsigned char *, recomp_context *) {}
extern "C" void func_80080768(unsigned char *, recomp_context *c) { c->r2 = 0; }
extern "C" void func_800806B4(unsigned char *, recomp_context *) {}
extern "C" int rr64_online_wait_for_race(unsigned char *, unsigned mode) {
    ++gate_calls;
    if (session.active && is_race_results_mode(mode))
        finish_latched = true;
    return online_blocked;
}
namespace rr64::netplay {
Status get_status() { return session; }
bool publish_highlight_playlist(highlight_network::Blob blob, std::uint64_t duration) {
    check(finish_latched, "online finish mode latched before actual playlist publication");
    ++publish_calls; published = std::move(blob);
    transport.stage = highlight_network::Stage::Waiting; transport.duration_us = duration;
    return duration != 0;
}
std::optional<highlight_network::Blob> take_received_highlight_playlist() {
    if (!client_delivery) return {};
    client_delivery = false;
    return published;
}
void acknowledge_highlight_decoded(bool accepted) { ++(accepted ? accepted_payloads : rejected_payloads); }
highlight_network::Status highlight_status() { return transport; }
bool host_begin_highlights() {
    if (!allow_host_begin || transport.stage != highlight_network::Stage::Waiting) return false;
    transport.stage = highlight_network::Stage::Playing;
    return true;
}
bool host_skip_highlights() { ++skip_calls; transport.stage = highlight_network::Stage::Finished; return true; }
}
namespace rr64::highlights {
// No attack props participate in this handoff test. Actor pose/camera/codec
// implementations remain real; these stubs avoid linking an unrelated renderer.
bool capture_weapon(unsigned char *, unsigned, WeaponPose &out) noexcept { out = {}; return false; }
bool draw_weapon(unsigned char *, void *, const WeaponPose &, const Vec3 &, const Quaternion &, const Vec3 &,
                 unsigned, float, const Vec3 &) noexcept { return false; }
void reset_weapon_scratch() noexcept {}
// Resource ownership and historical traffic rendering have their own native
// fixture. This dispatcher regression deliberately exercises their refusal path.
bool prepare_traffic(unsigned char *) noexcept { return false; }
bool draw_traffic(unsigned char *, const std::array<world_sync::Traffic, rr64::traffic::capacity> &, std::uint64_t) noexcept { return false; }
}
namespace {
void dispatch(Scene &s, unsigned mode = 0x1e, bool press = false) {
    auto *m = s.data();
    write_u32(m, globals::main_mode, mode); write_u32(m, globals::pending_mode, mode);
    write_u16(m, globals::controller_pressed_buttons, press ? 0x8000 : 0);
    recomp_context context{}; context.f_odd = &context.f0.u32h; context.r29 = guest_address(0x807eff00);
    callback_order.clear();
    highlight_dispatch_native(m, &context);
}
void online(bool host) {
    session.active = session.connected = session.authoritative = session.replicated_riders = true;
    session.is_host = host; session.local_slot = 0;
    session.phase = netplay::Phase::Race; session.game_setup.revision = 7;
}
void offline_modes() {
    // Native worker 4E938/4E94C completes BEFORE this frame's 4E9BC gate.
    // Render before each dispatch: an after-dispatch test misses the bug.
    for (const auto modes : {std::array<unsigned, 2>{0x09, 0x0b}, {0x0a, 0x0b}, {0x12, 0x14},
                            {0x13, 0x14}, {0x17, 0x19}, {0x18, 0x19}, {0x1c, 0x1e}, {0x1d, 0x1e}}) {
        reset(); Scene s; record(s, modes[0]);
        worker(s, false); dispatch(s, modes[1]); expect_callbacks(true, modes[1]);
        check(rr64_highlights_presenting(), "results begins real recorded reel in every live mode family");
        highlight_test::Clock::advance(std::chrono::seconds(1));
        worker(s, true); dispatch(s, modes[1]); expect_callbacks(true, modes[1]);
        const auto last = worker(s, true);
        dispatch(s, modes[1], true); expect_callbacks(true, modes[1]);
        check(rr64_highlights_presenting(), "skip frame retains replay while native producers remain blocked");
        highlight_test::Clock::advance(std::chrono::seconds(3));
        const auto held = worker(s, true);
        check(held == last, "skip handoff preserves last sample and camera rather than advancing or exposing raw world");
        dispatch(s, modes[1]); expect_callbacks(false, modes[1]);
        check(!rr64_highlights_presenting() && !rr64_highlights_block_dispatch(), "next eligible gate releases presentation and all producers together");
        worker(s, false); dispatch(s, modes[1]); expect_callbacks(false, modes[1]);
    }
}
highlight_network::Blob host_exit() {
    reset(); Scene s; online(true); record(s);
    worker(s, false); dispatch(s); expect_callbacks(true);
    check(!published.bytes.empty(), "real host recording encoded for guest transition test");
    const auto payload = published;
    transport.elapsed_us = 1000000;
    worker(s, true); dispatch(s); expect_callbacks(true);
    const auto last = worker(s, true);
    const auto skips = skip_calls;
    dispatch(s, 0x1e, true); expect_callbacks(true);
    check(skip_calls == skips + 1 && rr64_highlights_presenting(), "host skip retains shared presentation for held dispatcher frame");
    online_blocked = true;
    for (unsigned i = 0; i < 3; ++i) {
        transport.elapsed_us += 1000000;
        check(worker(s, true) == last, "online barrier retains last valid camera and actors across delayed release");
        dispatch(s); expect_callbacks(true);
    }
    online_blocked = false;
    worker(s, true); dispatch(s); expect_callbacks(false);
    worker(s, false);
    return payload;
}
void guest_exit(const highlight_network::Blob &payload) {
    reset(); Scene s; online(false); record(s);
    published = payload; client_delivery = true; transport.stage = highlight_network::Stage::Playing;
    worker(s, false); dispatch(s); expect_callbacks(true);
    const auto skips = skip_calls;
    worker(s, true); dispatch(s, 0x1e, true); expect_callbacks(true);
    check(skip_calls == skips && rr64_highlights_presenting(), "guest A cannot independently release host reel");
    const auto last = worker(s, true);
    transport.stage = highlight_network::Stage::Finished;
    dispatch(s); expect_callbacks(true);
    check(rr64_highlights_presenting(), "guest shared-finish edge preserves render state while producers are held");
    check(worker(s, true) == last, "guest retains real decoded actors through finished edge");
    dispatch(s); expect_callbacks(false);
    worker(s, false);
}
void cancellation(const highlight_network::Blob &payload) {
    for (unsigned kind = 0; kind < 3; ++kind) {
        reset(); Scene s; online(false); record(s);
        published = payload; client_delivery = true; transport.stage = highlight_network::Stage::Playing;
        worker(s, false); dispatch(s); expect_callbacks(true);
        worker(s, true); transport.stage = highlight_network::Stage::Finished; dispatch(s); expect_callbacks(true);
        worker(s, true);
        if (kind == 0) {
            highlights::reset();
        } else if (kind == 1) {
            session.host_disconnected = true; online_blocked = true;
            check(rr64_highlights_frame_gate(s.data(), 0x1e, nullptr), "disconnect still services both gates while online blocked");
        } else {
            check(rr64_highlights_wait(s.data(), 0x04) == 0, "leaving race context cancels pending presentation");
        }
        check(!rr64_highlights_presenting() && !rr64_highlights_block_dispatch(), "reset, disconnect or menu cancels pending replay state");
        worker(s, false);
        // A fresh race without events must not inherit the previous cursor.
        session = {}; online_blocked = false;
        check(rr64_highlights_wait(s.data(), 0x04) == 0, "fresh race leaves old results context");
        record(s, 0x1d, false);
        worker(s, false); dispatch(s); expect_callbacks(false);
        check(!rr64_highlights_presenting(), "fresh event-free race cannot replay stale cursor or playlist");
    }
}
void no_clip_and_timeout(const highlight_network::Blob &payload) {
    reset(); Scene s; record(s, 0x1d, false);
    worker(s, false); dispatch(s); expect_callbacks(false);
    check(!rr64_highlights_presenting(), "empty offline playlist does not add transition frame");

    reset(); Scene host; online(true); record(host, 0x1d, false);
    worker(host, false); dispatch(host); expect_callbacks(false);
    check(!rr64_highlights_presenting(), "empty host playlist immediately runs native results");

    reset(); Scene guest; online(false); record(guest);
    published = payload; published.checksum ^= 1; client_delivery = true;
    transport.stage = highlight_network::Stage::Waiting;
    const auto rejects = rejected_payloads;
    worker(guest, false); dispatch(guest); expect_callbacks(true);
    check(rejected_payloads == rejects + 1, "malformed guest payload rejected by real codec");
    worker(guest, false); // Waiting never fabricated a replay sample.
    highlight_test::Clock::advance(std::chrono::seconds(66));
    dispatch(guest); expect_callbacks(true);
    check(rr64_highlights_presenting(), "timeout is a pending handoff rather than releasing a blocked frame");
    worker(guest, false); dispatch(guest); expect_callbacks(false);
    worker(guest, false);

    // Existing valid sample followed by transport stall uses the same exit
    // contract. Waiting has no replay camera until playback is active again.
    reset(); Scene stalled; online(false); record(stalled);
    published = payload; client_delivery = true; transport.stage = highlight_network::Stage::Playing;
    worker(stalled, false); dispatch(stalled); expect_callbacks(true);
    const auto last = worker(stalled, true);
    transport.stage = highlight_network::Stage::Waiting;
    highlight_test::Clock::advance(std::chrono::seconds(66));
    dispatch(stalled); expect_callbacks(true);
    check(worker(stalled, true) == last, "timeout edge retains an already valid replay sample");
    dispatch(stalled); expect_callbacks(false); worker(stalled, false);
}
}
int main() {
    offline_modes();
    const auto payload = host_exit();
    guest_exit(payload);
    cancellation(payload);
    no_clip_and_timeout(payload);
    highlights::reset();
    std::cout << "{\"checks\":" << checks << ",\"worker_draws\":" << draws
              << ",\"ordered_gates\":" << gate_calls << ",\"published_playlists\":" << publish_calls
              << ",\"accepted_payloads\":" << accepted_payloads << ",\"rejected_payloads\":" << rejected_payloads
              << ",\"synthetic_assets\":true,\"real_dispatcher\":true,\"worker_before_gate\":true}\n";
}
