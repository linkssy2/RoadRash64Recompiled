#include "rr64_native.hpp"
#include "rr64_engine_layout.hpp"
#include "rr64_weapon_diagnostics.hpp"
#ifdef RR64_EXPERIMENTAL_COURSE
#include "rr64_mk64_items.hpp"
#endif
#include <mutex>
#include <array>
#include <bit>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C" void func_80016A18(unsigned char *, recomp_context *);
namespace {
std::array<std::atomic_ullong, 8> counters{};
bool diagnostics() {
    static const bool enabled = [] {
        const char *v = std::getenv("RR64_WEAPON_DIAGNOSTICS");
        return v && std::strcmp(v, "1") == 0;
    }();
    return enabled;
}
void count(unsigned i) {
    if (diagnostics())
        counters[i].fetch_add(1, std::memory_order_relaxed);
}
std::mutex reportMutex;
rr64::weapon::Report report;
thread_local rr64::weapon::Sample pending;
thread_local bool pendingValid = false;
thread_local unsigned char *weaponMapping = nullptr;
thread_local unsigned weaponView = 0, weaponRecord = 0, weaponParent = 0, weaponGraph = 0;
thread_local unsigned preparedSource = 0;
thread_local unsigned onlineParent=0,onlineGraph=0;
thread_local unsigned char *onlineMapping=nullptr;
thread_local unsigned char *savedMapping = nullptr;
thread_local unsigned savedPose = 0;
thread_local std::array<unsigned, 7> savedWords{};
void restore_pose() {
    if (savedMapping)
        for (unsigned i = 0; i < savedWords.size(); ++i)
            rr64::engine::write_u32(savedMapping, savedPose + i * 4, savedWords[i]);
    savedMapping = nullptr;
}
bool same_view(unsigned char *m) {
    unsigned view = 0;
    return m == weaponMapping &&
           rr64::engine::read_u32(m, rr64::engine::globals::active_viewport, view) &&
           view == weaponView;
}
}
extern "C" void rr64_weapon_begin(unsigned char *m, unsigned node, unsigned graph) {
    restore_pose();
    onlineParent=node;onlineGraph=graph;onlineMapping=m;
    count(0);
    weaponMapping = nullptr;
    weaponRecord = 0;
    preparedSource = 0;
    pendingValid = false;
    using namespace rr64::engine;
    unsigned views = 0, type = 0, rider = 0, owned = 0;
    if (!rr64_render_only_max_lod_enabled() || !read_u32(m, 0x8009DB88u, views) || views < 2 ||
        views > 4 || !read_u32(m, globals::active_viewport, weaponView) || weaponView >= views ||
        !read_u32(m, node, type) || type != 2 || !read_u32(m, node + 4, rider) ||
        !read_u32(m, rider + 0x5BCu, owned) || !graph || graph != owned ||
        !valid_guest_range(graph, 0x18))
        return;
    weaponMapping = m;
    weaponParent = node;
    weaponGraph = graph;
    count(1);
    // Distant riders can skip the weapon pose producer for this viewport.
    // The shared weapon then retains the previous viewport's camera-relative
    // root. Reproduce 5EB50's root copy at draw time, without touching children
    // (attack animation) or leaving render-only state in the simulation.
    unsigned model = 0, parentPose = 0, weaponPose = 0;
    if (!read_u32(m, node + 0x28, model) || !read_u32(m, model + 0xc, parentPose) ||
        !read_u32(m, graph + 0xc, weaponPose) || parentPose == weaponPose ||
        !valid_guest_range(parentPose, 28) || !valid_guest_range(weaponPose, 28))
        return;
    std::array<unsigned, 7> current{};
    for (unsigned i = 0; i < current.size(); ++i) {
        if (!read_u32(m, parentPose + i * 4, current[i]) ||
            !std::isfinite(std::bit_cast<float>(current[i])) ||
            !read_u32(m, weaponPose + i * 4, savedWords[i]))
            return;
    }
    // The detailed rider is already unbound here. A distant stock graph can
    // have an unprepared (even zero) root, particularly for playable cops.
    // Borrow the certified current-view root instead; retain authored units
    // and select its source bank below. Children keep their attack animation.
    if (rr64_lod_weapon_root(m, node, current.data(), &preparedSource)) count(7);
    savedMapping = m;
    savedPose = weaponPose;
    for (unsigned i = 0; i < current.size(); ++i)
        write_u32(m, weaponPose + i * 4, current[i]);
    count(6);
}
extern "C" void rr64_weapon_alt_begin(unsigned char *m, unsigned node, unsigned graph) {
    count(4);
    rr64_weapon_begin(m, node, graph);
}
extern "C" unsigned long long rr64_weapon_counter(unsigned i) {
    return i < counters.size() ? counters[i].exchange(0, std::memory_order_relaxed) : 0;
}
extern "C" void rr64_weapon_end() {
    onlineMapping=nullptr;onlineParent=onlineGraph=0;
    restore_pose();
    weaponMapping = nullptr;
    weaponRecord = 0;
    preparedSource = 0;
    pendingValid = false;
}
extern "C" void rr64_weapon_source(unsigned char *m, void *context, unsigned record) {
    weaponRecord = 0;
    if (!context || !same_view(m))
        return;
    count(2);
    using namespace rr64::engine;
    unsigned sourceRecord = 0, parentModel = 0, parentSourceRecord = 0;
    std::uint16_t source = 0, parentSource = 0;
    // 5EB50 copies the CURRENT rider model's pose into the weapon root.
    // That copied pose retains the current model's coordinate units.
    if (record != weaponGraph || !read_u32(m, record + 0x14u, sourceRecord) ||
        !read_u16(m, sourceRecord + 0x12u, source) || source > 2 ||
        !read_u32(m, weaponParent + 0x28u, parentModel) ||
        !read_u32(m, parentModel + 0x14u, parentSourceRecord) ||
        !read_u16(m, parentSourceRecord + 0x12u, parentSource) || parentSource < 1 ||
        parentSource > 2)
        return;
    if (preparedSource) parentSource = static_cast<std::uint16_t>(preparedSource);
    if (source == parentSource) return;
    auto *c = static_cast<recomp_context *>(context);
    const auto saved = *c;
    c->r4 = parentSource;
    func_80016A18(m, c);
    *c = saved;
    weaponRecord = record;
    count(5);
}
extern "C" void rr64_weapon_matrix(unsigned char *m, unsigned record, unsigned address) {
    if (onlineMapping==m && record==onlineGraph)
        rr64_online_presentation_matrix(m,onlineParent,record,address,preparedSource ? preparedSource : 3u);
#ifdef RR64_EXPERIMENTAL_COURSE
    if (onlineMapping == m && record == onlineGraph)
        rr64::mk64_items::scale_weapon_matrix(m, onlineParent, record, address,
                                               preparedSource ? preparedSource : 3u);
#endif
    pendingValid = false;
    if (diagnostics() && same_view(m)) {
        using namespace rr64::engine;
        pending = {};
        pending.node = weaponParent;
        pending.graph = weaponGraph;
        pending.record = record;
        pending.view = weaponView;
        unsigned sr = 0, psr = 0, pose = 0;
        bool valid = read_u32(m, record + 0x14, sr) && read_u16(m, sr + 0x12, pending.source) &&
                     read_u32(m, weaponParent + 0x28, pending.model) &&
                     read_u32(m, pending.model + 0x14, psr) &&
                     read_u16(m, psr + 0x12, pending.parentSource) &&
                     read_u32(m, pending.model + 0xc, pose) &&
                     read_u16(m, weaponParent + 0x44, pending.lod) &&
                     read_float(m, weaponParent + 8 + weaponView * 4, pending.distanceSquared);
        for (unsigned i = 0; i < 3; ++i)
            valid = read_float(m, address + 48 + i * 4, pending.translation[i]) &&
                    read_float(m, pose + i * 4, pending.parent[i]) && valid;
        pendingValid = valid;
    }
    if (!same_view(m) || weaponRecord != record || !record)
        return;
    // Source selection now matches the units already in the copied pose.
    // No matrix scaling: rescaling here would preserve the original mismatch.
    weaponRecord = 0;
    count(3);
    (void)address;
}
extern "C" void rr64_weapon_packed(unsigned char *m, unsigned record, unsigned address) {
    if (!pendingValid || !same_view(m) || pending.record != record)
        return;
    pendingValid = false;
    // N64 Mtx stores signed integer halves first, then fractional halves.
    for (unsigned i = 0; i < 3; ++i) {
        std::uint16_t hi = 0, lo = 0;
        if (!rr64::engine::read_u16(m, address + 24 + i * 2, hi) ||
            !rr64::engine::read_u16(m, address + 56 + i * 2, lo))
            return;
        pending.packed[i] = float(std::bit_cast<std::int16_t>(hi)) + float(lo) / 65536.f;
    }
    std::lock_guard lock(reportMutex);
    ++report.examined;
    for (unsigned i = 0; i < report.size; ++i) {
        auto &s = report.samples[i];
        if (s.node == pending.node && s.view == pending.view && s.record == pending.record) {
            s = pending;
            return;
        }
    }
    if (report.size < report.samples.size())
        report.samples[report.size++] = pending;
    else
        ++report.omitted;
}
rr64::weapon::Report rr64::weapon::take_report() {
    std::lock_guard lock(reportMutex);
    auto out = report;
    report = {};
    return out;
}
