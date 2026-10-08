// Actual RT64 frame matching and projection processing; no window or GPU.
#include "hle/rt64_rr64_camera_boundary.h"
#include "hle/rt64_rr64_matching_evidence.h"
#include "render/rt64_projection_processor.h"
#include "render/rt64_transform_processor.h"
#include "rr64_highlight_render_boundary.hpp"
#include "rr64_engine_layout.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using namespace RT64;
unsigned failures = 0, checks = 0;
bool old_normal_boundary = false;
struct NormalInput {
    unsigned round = 7;
    std::array<std::uint32_t, 14> generations{};
};
void check(bool ok, const char *why) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); }
}
unsigned read(unsigned char *m, unsigned address) {
    unsigned result = 0; rr64::engine::read_u32(m, address, result); return result;
}
void put(unsigned char *m, unsigned address, unsigned value) {
    rr64::engine::write_u32(m, address, value);
}
unsigned packet(unsigned char *m, unsigned epoch, unsigned key, unsigned views = 1,
                const NormalInput *normal = nullptr) {
    constexpr unsigned base = 0x80200000u, start = base + 0x140u;
    put(m, 0x800a1830u, epoch); put(m, 0x8009cba4u, 0);
    put(m, 0x800ac658u, base); put(m, 0x8009cb90u, base);
    put(m, 0x800bc9a0u, 0x4650u); put(m, 0x800ac650u, start);
    if (normal && !old_normal_boundary)
        rr64::highlight_render::begin_normal(m, normal->round, normal->generations);
    else rr64_highlight_render_begin(m, key);
    for (unsigned i = 0; i < views; ++i) rr64_highlight_render_projection(m);
    const unsigned end = read(m, 0x800ac650u);
    if (end == start) {
        rr64_highlight_render_end(m);
        check(read(m, 0x800ac650u) == start, "ordinary pre-replay draw emits no extra commands");
        return 0;
    }
    const unsigned id = read(m, start + 12);
    check(end - start == views * 32u, "bounded four-command projection marker per native load");
    for (unsigned p = start; p < end; p += 32) {
        check(read(m, p) == 0xe0525464u && read(m, p + 4) == 0x10000064u &&
              read(m, p + 8) == 0x6400000cu && read(m, p + 12) == id &&
              read(m, p + 16) == 0xaaau && read(m, p + 24) == 0xe0525464u &&
              read(m, p + 28) == 0x20000000u,
              "every view/source carries the same submitted identity and simple interpolation policy");
    }
    rr64_highlight_render_end(m);
    check(read(m, 0x800ac650u) == end + 32 && read(m, end + 8) == 0x6400000cu &&
          read(m, end + 12) == G_EX_ID_AUTO && read(m, end + 16) == 2u,
          "draw exit restores AUTO without pushing or leaking projection stack entries");
    return id;
}
hlslpp::float4x4 translated(float x) {
    return hlslpp::float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, x,0,0,1);
}
bool same(const hlslpp::float4x4 &a, const hlslpp::float4x4 &b) {
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
            if (std::fabs(a[i][j] - b[i][j]) > .00001f) return false;
    return true;
}
float determinant3(const hlslpp::float4x4 &a) {
    return a[0][0] * (a[1][1]*a[2][2] - a[1][2]*a[2][1]) -
           a[0][1] * (a[1][0]*a[2][2] - a[1][2]*a[2][0]) +
           a[0][2] * (a[1][0]*a[2][1] - a[1][1]*a[2][0]);
}
void seed(Workload &w, unsigned marker, const hlslpp::float4x4 &view, float root, unsigned views) {
    w.reset();
    auto &d = w.drawData;
    d.transformGroups.clear(); d.worldTransformGroups.clear();
    d.worldTransforms.clear(); d.worldTransformVertexIndices.clear();
    TransformGroup camera;
    camera.matrixId = marker ? marker : G_EX_ID_AUTO;
    camera.decompose = false;
    camera.positionInterpolation = camera.rotationInterpolation =
        camera.scaleInterpolation = camera.skewInterpolation =
        camera.perspectiveInterpolation = G_EX_COMPONENT_INTERPOLATE;
    d.transformGroups.push_back(camera);
    d.viewProjTransformGroups.assign(views + 1, 0);
    const auto identity = hlslpp::float4x4::identity();
    d.viewTransforms.assign(views + 1, view);
    d.projTransforms.assign(views + 1, identity);
    d.viewProjTransforms.assign(views + 1, view);
    d.viewportOrigins.assign(views + 1, G_EX_ORIGIN_NONE);
    d.rspViewports.resize(views + 1); d.viewportClipRatios.resize((views + 1) * 4);
    w.fbPairCount = 1;
    auto &pair = w.fbPairs[0]; pair.projectionCount = views; pair.projections.resize(views);
    for (unsigned i = 0; i < views; ++i) {
        auto &p = pair.projections[i]; p.reset();
        p.type = Projection::Type::Perspective; p.transformsIndex = i + 1;
        p.scissorRect = {0, 0, 1280, 960};
        TransformGroup world;
        world.matrixId = 0x52520000u + i; world.ordering = G_EX_ORDER_LINEAR;
        world.decompose = false;
        world.positionInterpolation = world.rotationInterpolation = G_EX_COMPONENT_INTERPOLATE;
        d.transformGroups.push_back(world);
        d.worldTransformGroups.push_back(i + 1);
        d.worldTransforms.push_back(translated(root + i));
        d.worldTransformVertexIndices.push_back(0);
        GameCall call{}; call.callDesc.minWorldMatrix = call.callDesc.maxWorldMatrix = i;
        call.callDesc.triangleCount = 1; p.addGameCall(call);
    }
    // Also exercise the non-retained path: the cut must not depend on a global
    // race flag or on the optional interpolation certificate being enabled.
    w.presentationScene.raceActive = false;
}
void frame(GameFrame &f, unsigned index, unsigned views) {
    f.workloads = {index}; f.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);
    for (unsigned i = 0; i < views; ++i)
        f.perspectiveScenes.push_back(GameScene{{{index, 0, i}}});
}
void render_projection(WorkloadQueue &q, GameFrame &cur, const GameFrame &prev, float weight) {
    ProjectionProcessor processor;
    ProjectionProcessor::ProcessParams p;
    p.workloadQueue = &q; p.curFrame = &cur;
    // Exactly threadRenderFrame's matchedFrame gate, including the aspect
    // adjustment path that still processes an unmatched native endpoint.
    p.prevFrame = cur.matched ? &prev : nullptr;
    p.curFrameWeight = weight; p.prevFrameWeight = 0; p.aspectRatioScale = 1;
    processor.process(p);
}
void run_pair(unsigned old_id, unsigned new_id, unsigned views, bool expect_cut, bool sharp) {
    auto q = std::make_unique<WorkloadQueue>();
    const auto old_view = sharp ? hlslpp::float4x4(-1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,-10,20,1)
                               : translated(-2);
    const auto new_view = sharp ? hlslpp::float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-10,20,1)
                               : translated(-4);
    seed(q->workloads[0], old_id, old_view, 1000, views);
    seed(q->workloads[1], new_id, new_view, 0, views);
    GameFrame previous, current; frame(previous, 0, views); frame(current, 1, views);
    bool velocity = true, tiles = true, lookat = true;
    current.match(nullptr, *q, previous, nullptr, velocity, tiles, lookat);
    check(!velocity && !tiles && !lookat, "boundary/matching has no GPU upload or tile work");
    if (expect_cut) {
        check(!current.matched && !current.frameMap.workloads[1].mapped &&
              !current.rr64InterpolationCompatible && current.rr64GeometryRejectionReasons == 1,
              "camera cut rejects the whole submitted pair before world matching");
        check(current.frameMap.workloads[1].transforms.empty(), "sector-relative world transforms cannot interpolate across cut");
    } else {
        check(current.matched && current.frameMap.workloads[1].mapped,
              "same clip/ordinary frames retain actual matching");
        for (unsigned i = 0; i < views; ++i)
            check(current.frameMap.workloads[1].transforms[i].mapped &&
                  current.frameMap.workloads[1].viewProjections[i + 1].mapped,
                  "same phase keeps both camera and object interpolation");
    }
    for (float weight : {.25f, .5f, .75f, 1.f}) {
        render_projection(*q, current, previous, weight);
        for (unsigned i = 1; i <= views; ++i) {
            const auto &actual = q->workloads[1].drawData.modViewTransforms[i];
            if (expect_cut) {
                check(same(actual, new_view), "actual projection processor uses complete authored results camera at every output weight");
            } else if (!sharp) {
                check(std::fabs(actual[3][0] - (-2.f - 2.f * weight)) < .0001f,
                      "within-clip camera remains smooth at every output weight");
            } else if (weight == .5f) {
                check(std::fabs(determinant3(actual)) < .00001f,
                      "old unmarked cut reproduces a singular synthetic camera despite valid authored endpoint cameras");
            }
        }
    }
}
void normal_scene(unsigned char *m, unsigned views) {
    using namespace rr64::engine;
    put(m, globals::main_mode, 0x09); put(m, globals::pending_mode, 0x09);
    put(m, 0x800a4f24, views == 1 ? 0 : views == 2 ? 1 : 2); put(m, 0x800a6578, views);
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned actor = 0x800d8570 + i * 0x118;
        const unsigned bike_address = 0x80280000 + i * 0x2000, rider_address = bike_address + 0x1000;
        put(m, 0x800a4fa0 + i * 4, i + 1); put(m, 0x800a657c + i * 4, i);
        write_u16(m, actor + 0x24, 1); put(m, actor + 0xe0, bike_address); put(m, actor + 0xe4, rider_address);
        put(m, bike_address + bike::rider_pointer, rider_address);
        put(m, rider_address + rider::bike_pointer, bike_address);
        put(m, bike_address + 4, actor); put(m, rider_address + 4, actor);
        write_u16(m, bike_address + bike::rider_attached, 1);
        write_u16(m, rider_address + rider::bike_attached, 1);
        write_u16(m, rider_address + rider::ejected, 0);
        write_u16(m, bike_address + bike::drive_control_lockout, 0);
        for (unsigned j = 0; j < 4; ++j) write_float(m, bike_address + 0x244 + j * 4, j == 3 ? 1.f : 0.f);
        for (unsigned j = 0; j < 3; ++j) write_float(m, bike_address + 0x7dc + j * 4, float(i * 100 + j));
    }
}
void distinct_cameras(unsigned marker, unsigned views) {
    auto q = std::make_unique<WorkloadQueue>();
    for (unsigned f = 0; f < 2; ++f) {
        auto &w = q->workloads[f]; seed(w, marker, translated(0), 1000, views);
        for (unsigned i = 0; i < views; ++i) {
            const auto view = translated(float(100 * i + (f ? 4 : 2)));
            w.drawData.viewTransforms[i + 1] = w.drawData.viewProjTransforms[i + 1] = view;
            const int32_t x = int32_t((i & 1) * 640), y = int32_t((i / 2) * 480);
            w.fbPairs[0].projections[i].scissorRect = {x, y, x + 640, y + 480};
        }
    }
    GameFrame previous, current; frame(previous, 0, views); frame(current, 1, views);
    bool velocity = false, tiles = false, lookat = false;
    current.match(nullptr, *q, previous, nullptr, velocity, tiles, lookat);
    check(current.matched, "normal shared phase preserves distinct camera matching");
    for (unsigned i = 1; i <= views; ++i) {
        const auto &map = current.frameMap.workloads[1].viewProjections[i];
        check(map.mapped && map.prevTransformIndex == i, "phase identity never cross-maps distinct native viewports");
    }
    render_projection(*q, current, previous, .5f);
    for (unsigned i = 1; i <= views; ++i)
        check(std::fabs(q->workloads[1].drawData.modViewTransforms[i][3][0] - float(100 * (i - 1) + 3)) < .0001f,
              "each distinct camera retains its own midpoint and scissor association");
}
void unchanged_phase_auto_jump(unsigned marker) {
    auto q = std::make_unique<WorkloadQueue>();
    seed(q->workloads[0], marker, translated(0), 0, 1);
    seed(q->workloads[1], marker, translated(0), 1000, 1);
    for (unsigned f = 0; f < 2; ++f)
        q->workloads[f].drawData.transformGroups[1].positionInterpolation = G_EX_COMPONENT_AUTO;
    GameFrame previous, current; frame(previous, 0, 1); frame(current, 1, 1);
    bool velocity = false, tiles = false, lookat = false;
    current.match(nullptr, *q, previous, nullptr, velocity, tiles, lookat);
    check(current.matched && current.frameMap.workloads[1].transforms[0].mapped &&
          !current.frameMap.workloads[1].transforms[0].rigidBody.lerpTranslation,
          "unchanged normal marker leaves arbitrary same-mode jumps subject to original AUTO rejection");
}
void normal_boundaries(unsigned char *m) {
    using namespace rr64::engine;
    unsigned initial_four = 0;
    for (unsigned views = 1; views <= 4; ++views) {
        normal_scene(m, views); NormalInput normal;
        for (unsigned i = 0; i < 4; ++i) normal.generations[i] = 10 + i;
        unsigned epoch = 100 * views;
        const auto initial = packet(m, epoch++, 0, views, &normal);
        if (views == 4) initial_four = initial;
        check(initial && packet(m, epoch++, 0, views, &normal) == initial, "known ordinary phase is stable");
        distinct_cameras(initial, views);
        if (views == 3) {
            ++normal.generations[3];
            check(packet(m, epoch++, 0, 4, &normal) != initial,
                  "three-player layout observes the fourth native camera's reset generation");
        }
        ++normal.generations[views - 1];
        auto current = packet(m, epoch++, 0, views, &normal);
        check(current != initial, "same-mode native committed recovery changes submitted phase");
        run_pair(initial, current, views, true, true);
        check(packet(m, epoch++, 0, views, &normal) == current, "steady post-recovery pair resumes matching");
        const unsigned selected = views - 1;
        put(m, 0x800a4fa0 + selected * 4, 8);
        auto next = packet(m, epoch++, 0, views, &normal);
        check(next != current, "native camera mode change creates a boundary"); current = next;
        const unsigned owner = views % 4;
        put(m, 0x800a657c + selected * 4, owner);
        next = packet(m, epoch++, 0, views, &normal);
        check(next != current, "camera owner change creates a boundary"); current = next;
        const unsigned bike_address = 0x80280000 + owner * 0x2000;
        write_u16(m, bike_address + bike::drive_control_lockout, 1);
        next = packet(m, epoch++, 0, views, &normal);
        check(next != current, "mode8 alternate-anchor flag change creates a boundary"); current = next;
        write_float(m, bike_address + 0x7dc, 10000.f);
        check(packet(m, epoch++, 0, views, &normal) == current,
              "finite moved anchor alone is not misrepresented as a continuity certificate");
        unchanged_phase_auto_jump(current);
        put(m, 0x800a4fa0 + selected * 4, 9);
        next = packet(m, epoch++, 0, views, &normal);
        check(next != current, "scanned-owner camera enters an explicitly unproved phase"); current = next;
        ++normal.round;
        next = packet(m, epoch++, 0, views, &normal);
        check(next != current, "new recording round cannot inherit preceding camera phase");
        check(packet(m, epoch++, 0, views) != next, "leaving recorded local scope creates a boundary");
    }
    if (RT64::RR64MatchingEvidence::enabled()) {
        bool found = false;
        RT64::RR64MatchingEvidence::buffer.drain([&](const auto &r) {
            if (r.category != 3 || r.cameraMarker != initial_four || r.cameraViews != 4 ||
                r.cameraValidMask != 15 || r.cameraReplayKey) return;
            found = true;
            for (unsigned i = 0; i < 4; ++i)
                check(r.cameraOwners[i] == i && r.cameraGenerations[i] == 10 + i,
                      "emitted four-view phase evidence preserves distinct authored owners and reset generations");
        });
        check(found, "bounded phase evidence captures an emitted four-view normal key");
    }
}
}
int main(int argc, char **argv) {
    old_normal_boundary = argc > 1 && std::strcmp(argv[1], "--old-normal-boundary") == 0;
    std::vector<unsigned char> memory(rr64::engine::kRdramSize);
    auto *m = memory.data();
    check(packet(m, 1, 0) == 0, "ordinary racing is untouched before highlights");
    const auto replay = packet(m, 2, 1);
    check(replay != 0 && packet(m, 3, 1, 4) == replay, "same clip keeps submitted marker for all viewports");
    const auto angle = packet(m, 4, 2);
    const auto results = packet(m, 5, 0, 4);
    check(angle != replay && results != angle && results != 0, "clip/camera changes and exit establish distinct boundaries");
    check(packet(m, 6, 0) == results, "results camera resumes ordinary smooth matching immediately");
    check(packet(m, 1, 0) == 0, "guest restart retires the old replay marker");
    const auto restarted = packet(m, 2, 1);
    check(restarted != replay, "same-mapping restart never reuses preceding replay identity");
    put(m, 0x800ac650u, 0x807ffff8u);
    rr64_highlight_render_begin(m, 2); rr64_highlight_render_projection(m); rr64_highlight_render_end(m);
    check(read(m, 0x800ac650u) == 0x807ffff8u, "invalid native command capacity is left untouched");
    for (unsigned views = 1; views <= 4; ++views) {
        run_pair(0, 0, views, false, true); // Concrete old camera interpolation failure.
        run_pair(replay, results, views, true, true);
        run_pair(0, replay, views, true, true);
        run_pair(results, 0, views, true, true);
        run_pair(replay, angle, views, true, true);
        run_pair(replay, replay, views, false, false);
        run_pair(results, results, views, false, false);
    }
    normal_boundaries(m);
    std::printf("Highlight camera boundary: %s (%u checks, %u failures); actual RT64 matcher/projection, all views, no assets or window\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
