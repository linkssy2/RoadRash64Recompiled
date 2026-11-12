// Offline exercise of RT64's real GameFrame matcher. No renderer is created.
#include "hle/rt64_workload_queue.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <numeric>
#include <unordered_map>

namespace {
unsigned failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
constexpr unsigned children = 3u;
using namespace RT64;
hlslpp::float4x4 translated(float x, float y, float z) {
    return hlslpp::float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1);
}
void seed(Workload& workload, const std::vector<unsigned>& identities, bool automatic) {
    workload.reset();
    auto& data = workload.drawData;
    data.transformGroups.clear(); data.worldTransformGroups.clear();
    data.worldTransforms.clear(); data.worldTransformVertexIndices.clear();
    TransformGroup camera{}; camera.matrixId = G_EX_ID_AUTO;
    data.transformGroups.push_back(camera);
    data.viewProjTransformGroups = {0u};
    const hlslpp::float4x4 identity = hlslpp::float4x4::identity();
    data.viewTransforms = {identity}; data.projTransforms = {identity}; data.viewProjTransforms = {identity};
    workload.fbPairCount = 1u;
    auto& pair = workload.fbPairs[0]; pair.projections.resize(1u); pair.projectionCount = 1u;
    auto& projection = pair.projections[0]; projection.reset();
    projection.type = Projection::Type::Perspective; projection.transformsIndex = 0u;
    for (unsigned id : identities) {
        TransformGroup group{};
        group.matrixId = automatic ? G_EX_ID_AUTO : 0x52520000u + id;
        group.ordering = automatic ? G_EX_ORDER_AUTO : G_EX_ORDER_LINEAR;
        data.transformGroups.push_back(group);
        for (unsigned child = 0; child < children; ++child) {
            const auto index = static_cast<unsigned>(data.worldTransforms.size());
            data.worldTransformGroups.push_back(static_cast<unsigned>(data.transformGroups.size() - 1u));
            // Repeated identical matrices/model draw calls make the ambiguity
            // deliberate; stable authored identity must survive draw reordering.
            data.worldTransforms.push_back(identity);
            data.worldTransformVertexIndices.push_back(0u);
            GameCall call{};
            call.callDesc.minWorldMatrix = call.callDesc.maxWorldMatrix = static_cast<uint16_t>(index);
            call.callDesc.triangleCount = 1u; call.callDesc.tileCount = 0u;
            projection.addGameCall(call);
        }
    }
    workload.presentationScene.raceActive = false; // Exact R18 results matching path.
}
std::uint64_t pairs(const GameFrame& frame, const Workload& current, const Workload& previous,
        std::uint64_t& automatic_pairs) {
    std::multimap<std::uint64_t, GameCallMap> current_map, previous_map;
    frame.buildCallHashMap(0u, current, current.fbPairs[0].projections[0], current_map);
    frame.buildCallHashMap(0u, previous, previous.fbPairs[0].projections[0], previous_map);
    std::uint64_t result = 0; automatic_pairs = 0;
    // Count the candidate range actually produced by RT64's key builders;
    // this is a deterministic complexity check, not a wall-clock benchmark.
    for (const auto& [hash, call] : current_map) {
        const auto range = previous_map.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            ++result;
            if (call.doTransformMatching && it->second.doTransformMatching) { ++automatic_pairs; }
        }
    }
    return result;
}
void run_case(const std::vector<unsigned>& previous_ids, const std::vector<unsigned>& current_ids,
        bool automatic, bool previously_matched) {
    auto queue = std::make_unique<WorkloadQueue>();
    seed(queue->workloads[0], previous_ids, automatic);
    seed(queue->workloads[1], current_ids, automatic);
    GameFrame previous, current;
    previous.workloads = {0u}; current.workloads = {1u};
    previous.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);
    current.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);
    previous.matched = previously_matched;
    if (previously_matched) {
        previous.buildTransformIdMap(queue->workloads[0], queue->workloads[0].transformIdMap,
            queue->workloads[0].transformIgnoredIds);
    }
    previous.perspectiveScenes.push_back(GameScene{{{0u, 0u, 0u}}});
    current.perspectiveScenes.push_back(GameScene{{{1u, 0u, 0u}}});
    std::uint64_t expensive_pairs = 0;
    const auto pair_count = pairs(current, queue->workloads[1], queue->workloads[0], expensive_pairs);
    unsigned shared = 0;
    for (auto id : current_ids) {
        if (std::find(previous_ids.begin(), previous_ids.end(), id) != previous_ids.end()) { ++shared; }
    }
    if (automatic) {
        check(pair_count == std::uint64_t(previous_ids.size()) * current_ids.size() * children * children,
            "identical AUTO instances expose the cross product in actual call hash maps");
        check(expensive_pairs == pair_count, "AUTO pairs all enter transform candidate matching");
    } else {
        check(pair_count == std::uint64_t(shared) * children * children,
            "explicit IDs restrict call candidates to the same authored placement");
        check(expensive_pairs == 0u, "LINEAR groups bypass automatic transform comparison");
    }
    bool velocities = false, tiles = false, look_at = false;
    current.match(nullptr, *queue, previous, nullptr, velocities, tiles, look_at);
    check(current.matched && !velocities && !tiles && !look_at,
        "real frame matching completes without GPU upload or texture work");
    if (!automatic) {
        const auto& transforms = current.frameMap.workloads[1].transforms;
        for (unsigned i = 0; i < current_ids.size(); ++i) {
            const auto found = std::find(previous_ids.begin(), previous_ids.end(), current_ids[i]);
            for (unsigned child = 0; child < children; ++child) {
                const auto& match = transforms[i * children + child];
                if (found == previous_ids.end()) {
                    check(!match.mapped, "newly visible identity cannot borrow an unrelated old pose");
                } else {
                    const auto expected = static_cast<unsigned>(found - previous_ids.begin()) * children + child;
                    check(match.mapped && match.prevTransformIndex == expected,
                        "actual matcher preserves authored identity and fixed child order across visibility/order changes");
                }
            }
        }
    }
    std::printf("matching %s previous=%zu current=%zu shared=%u call-pairs=%llu auto-pairs=%llu previous-matched=%u\n",
        automatic ? "AUTO" : "LINEAR", previous_ids.size(), current_ids.size(), shared,
        static_cast<unsigned long long>(pair_count), static_cast<unsigned long long>(expensive_pairs), previously_matched);
}
void run_motion_history() {
    auto queue = std::make_unique<WorkloadQueue>();
    GameFrame frames[3];
    const std::vector<unsigned> ids[3] = {{7u, 12u}, {12u, 7u}, {7u, 12u}};
    for (unsigned frame = 0; frame < 3u; ++frame) {
        auto& workload = queue->workloads[frame];
        seed(workload, ids[frame], false);
        auto& data = workload.drawData;
        data.viewTransforms[0] = translated(-2.0f * frame, 0.0f, 0.0f);
        data.viewProjTransforms[0] = data.viewTransforms[0];
        for (unsigned item = 0; item < ids[frame].size(); ++item) {
            const auto& group = data.transformGroups[item + 1u];
            check(group.ordering == G_EX_ORDER_LINEAR && group.decompose &&
                group.positionInterpolation == G_EX_COMPONENT_AUTO &&
                group.rotationInterpolation == G_EX_COMPONENT_AUTO &&
                group.scaleInterpolation == G_EX_COMPONENT_AUTO &&
                group.skewInterpolation == G_EX_COMPONENT_AUTO &&
                group.perspectiveInterpolation == G_EX_COMPONENT_AUTO &&
                group.vertexInterpolation == G_EX_COMPONENT_SKIP &&
                group.texcoordInterpolation == G_EX_COMPONENT_SKIP,
                "explicit identity preserves the existing default interpolation policy");
            for (unsigned child = 0; child < children; ++child) {
                // Distinct root/child positions, all moving eight units per
                // frame. AUTO needs prior velocity at this speed; a reset
                // rigid body would skip translation rather than interpolate.
                data.worldTransforms[item * children + child] = translated(
                    ids[frame][item] * 100.0f + 8.0f * frame + child * 3.0f,
                    child * 2.0f, 0.0f);
            }
        }
        frames[frame].workloads = {frame};
        frames[frame].frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);
        frames[frame].perspectiveScenes.push_back(GameScene{{{frame, 0u, 0u}}});
    }
    bool velocities = false, tiles = false, look_at = false;
    frames[1].match(nullptr, *queue, frames[0], nullptr, velocities, tiles, look_at);
    const auto& previous = frames[1].frameMap.workloads[1];
    check(frames[1].matched && previous.mapped, "middle frame has real matched history");
    for (const auto& transform : previous.transforms) {
        check(transform.mapped && !transform.rigidBody.lerpTranslation &&
            std::fabs(float(transform.rigidBody.linearVelocity.x) - 8.0f) < 0.0001f,
            "first moving frame records velocity while AUTO rejects abrupt startup");
    }
    frames[2].match(nullptr, *queue, frames[1], nullptr, velocities, tiles, look_at);
    const auto& current = frames[2].frameMap.workloads[2];
    check(frames[2].matched && current.mapped && !velocities && !tiles && !look_at,
        "three-frame history matches without GPU work");
    for (unsigned item = 0; item < ids[2].size(); ++item) {
        const auto old_item = static_cast<unsigned>(std::find(ids[1].begin(), ids[1].end(), ids[2][item]) - ids[1].begin());
        for (unsigned child = 0; child < children; ++child) {
            const auto index = item * children + child, old_index = old_item * children + child;
            const auto& transform = current.transforms[index];
            check(transform.mapped && transform.prevTransformIndex == old_index &&
                transform.rigidBody.lerpTranslation && transform.rigidBody.transforms[0].valid &&
                transform.rigidBody.transforms[1].valid,
                "reordered root and children retain both real decompositions and velocity history");
            const auto midpoint = transform.rigidBody.lerp(0.5f,
                queue->workloads[1].drawData.worldTransforms[old_index],
                queue->workloads[2].drawData.worldTransforms[index], false);
            check(std::fabs(float(midpoint[3].x) - (ids[2][item] * 100.0f + 12.0f + child * 3.0f)) < 0.001f &&
                std::fabs(float(midpoint[3].y) - child * 2.0f) < 0.001f,
                "actual rigid-body interpolation produces the root and child midpoint");
        }
    }
    const auto& camera = current.viewProjections[0];
    check(camera.mapped && camera.prevTransformIndex == 0u && camera.rigidBody.lerpTranslation &&
        camera.rigidBody.lerpRotation && !camera.rigidBody.lerpDecompose,
        "camera retains default simple interpolation policy");
    const auto midpoint = camera.rigidBody.lerp(0.5f, queue->workloads[1].drawData.viewTransforms[0],
        queue->workloads[2].drawData.viewTransforms[0], false);
    check(std::fabs(float(midpoint[3].x) + 3.0f) < 0.0001f,
        "actual camera interpolation produces its translated midpoint");
    std::puts("matching LINEAR three-frame motion: real history, reordered root/children and camera midpoint checked");
}
}
int main() {
#ifdef _WIN32
    _putenv_s("RR64_STABLE_PRESENTATION", "1");
#else
    setenv("RR64_STABLE_PRESENTATION", "1", 1);
#endif
    std::vector<unsigned> all(64u); std::iota(all.begin(), all.end(), 0u);
    auto reversed = all; std::reverse(reversed.begin(), reversed.end());
    std::vector<unsigned> partial{63u, 18u, 1u, 40u, 900u};
    run_case(all, reversed, true, false);
    run_case(all, reversed, false, false);
    run_case(all, reversed, false, true);
    run_case(all, partial, false, false);
    run_case(partial, reversed, false, false);
    run_motion_history();
    std::printf("World matching smoke: %s (%u failures); actual RT64 hash maps, LINEAR matching, visibility and native/interpolated transition\n",
        failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
