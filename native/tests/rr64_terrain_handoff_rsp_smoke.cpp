// Reuse the existing renderer-free State/RSP fixture; do not create a GPU device.
#define main rr64_triangle_fixture_unused_main
#include "rr64_rsp_triangle_batch_smoke.cpp"
#undef main
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_game_frame.h"

int rr64_terrain_driver_smoke_main();
namespace {
std::unique_ptr<Fixture> handoff;
UserConfiguration handoffUser;
std::array<Snapshot, 2> materialState;
unsigned captured = 0;
uint32_t readWord(const std::vector<uint8_t> &ram, unsigned address) {
    uint32_t value;
    std::memcpy(&value, ram.data() + (address & 0x7fffffffu), sizeof(value));
    return value;
}
void writeWord(std::vector<uint8_t> &ram, unsigned address, uint32_t value) {
    std::memcpy(ram.data() + (address & 0x7fffffffu), &value, sizeof(value));
}
bool sameMatrix(const hlslpp::float4x4 &a, const hlslpp::float4x4 &b) {
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            if (float(a[row][col]) != float(b[row][col])) return false;
    return true;
}
GameFrame matchPair() {
    auto &queue = *handoff->queue;
    GameFrame previous, current;
    const uint32_t before = 0, after = 1;
    previous.set(queue, &before, 1);
    current.set(queue, &after, 1);
    require(previous.perspectiveScenes.size() == 1 && current.perspectiveScenes.size() == 1 &&
        previous.orthographicScenes.empty() && current.orthographicScenes.empty(),
        "decoded stock/native calls must form real compatible perspective scenes");
    bool velocities = false, tiles = false, lookAt = false;
    current.match(nullptr, queue, previous, nullptr, velocities, tiles, lookAt);
    require(!velocities && !tiles && !lookAt, "static handoff must not require GPU velocity uploads");
    return current;
}
}

void rr64_terrain_rsp_handoff(const std::vector<uint8_t> &memory, unsigned entry, unsigned role) {
    require(role < 2 && captured == role, "capture native then stock exactly once");
    if (!handoff) {
        GBI_EXTENDED::initialize();
        handoff = std::make_unique<Fixture>();
    }
    auto &f = *handoff;
    f.ram = memory;
    f.state->RDRAM = f.ram.data();
    f.state->ext.userConfig = &handoffUser;
    f.queue->writeCursor = role;
    auto &workload = f.queue->workloads[role];
    workload.reset();
    f.state->reset();
    f.state->clearExtended();
    f.state->rdramCheckPending = false; // No GPU/RAM framebuffer ownership exists in this fixture.
    f.interpreter->setup(f.state.get());
    f.interpreter->extendedOpCode = 0;
    f.gbi = {};
    f.gbi.ucode = GBIUCode::F3DEX2;
    GBI_RDP::setup(&f.gbi, true);
    GBI_F3DEX2::setup(&f.gbi);
    // The finalized stock stream retains its real FullSync/END footer.
    // Omit GPU submission only; decode every material, matrix and draw command.
    f.gbi.map[0xe9] = [](State *, DisplayList **) {};
    auto &rsp = f.rsp();
    rsp.setGBI(&f.gbi);
    rsp.viewportStack[0].scale = {160, 120, 512};
    rsp.viewportStack[0].translate = {160, 120, 512};
    rsp.geometryModeStack[0] = G_SHADE | 0x400u; // Exercise inherited native back-face culling.
    f.rdp().colorImage = {};
    f.rdp().colorImage.width = 320;
    f.rdp().colorImage.siz = G_IM_SIZ_16b;
    f.rdp().colorImage.changed = true;
    f.rdp().depthImage = {};
    f.rdp().scissorRectStack[0] = {0, 0, 1280, 960};

    // The driver uses an orthographic-like culling fixture. Seed a genuine
    // perspective camera through the real RSP matrix reader for this proof.
    const hlslpp::float4x4 projection(1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,-1,0);
    const hlslpp::float4x4 view(1,0,0,0, 0,1,0,0, 0,0,1,0,
        role ? .125f : 0.0f, 0, 100, 1);
    const hlslpp::float4x4 caller(1,0,0,0, 0,1,0,0, 0,0,1,0,
        role ? 73.0f : -91.0f, 12, 5, 1);
    auto matrix = [&](unsigned address, const hlslpp::float4x4 &value, unsigned params) {
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned col = 0; col < 4; ++col) {
                const float component = value[row][col];
                uint32_t bits;
                std::memcpy(&bits, &component, sizeof(bits));
                writeWord(f.ram, address + (row * 4 + col) * 4, bits);
            }
        rsp.matrixFloat(address, uint8_t(params));
    };
    matrix(0x80620000u, projection, rsp.projMask | rsp.loadMask);
    matrix(0x80620040u, view, rsp.projMask);
    matrix(0x80620080u, caller, rsp.loadMask);
    const auto end = readWord(f.ram, 0x800ac650u);
    writeWord(f.ram, end, 0xdf000000u);
    writeWord(f.ram, end + 4, 0);
    f.interpreter->processDisplayLists(entry & 0x7fffffffu,
        reinterpret_cast<DisplayList *>(f.ram.data() + (entry & 0x7fffffffu)));
    f.state->flush();
    require(f.interpreter->extendedOpCode == 0 && !f.state->extended.extendRDRAM &&
        rsp.modelMatrixStackSize == 1 && sameMatrix(rsp.modelMatrixStack[0], caller),
        "full emitted chain must restore extended addressing and the caller model stack");
    require(rsp.extended.modelMatrixIdStackSize == 1 && rsp.otherModeStackSize == 1 &&
        rsp.geometryModeStackSize == 1 && rsp.geometryModeStack[0] == (G_SHADE | 0x400u),
        "full emitted chain must balance model ID/material scopes and restore inherited culling");
    require(workload.drawData.faceIndices.size() == 9 && workload.drawData.vertexCount() > 0,
        "the real RSP must submit all three compiled terrain triangles");
    require(workload.drawData.worldTransforms.size() == 2 &&
        workload.drawData.transformGroups[workload.drawData.worldTransformGroups[1]].matrixId == 0x52510000u,
        "both roles must submit one world transform with the same authored terrain ID");

    // FullSync assigns consecutive mesh ranges before GPU submission. Repeat
    // only that bookkeeping here; all geometry and call state came from RSP/RDP.
    unsigned face = 0;
    for (unsigned fb = 0; fb < workload.fbPairCount; ++fb)
        for (unsigned p = 0; p < workload.fbPairs[fb].projectionCount; ++p) {
            auto &proj = workload.fbPairs[fb].projections[p];
            for (unsigned c = 0; c < proj.gameCallCount; ++c) {
                auto &call = proj.gameCalls[c];
                call.meshDesc.faceIndicesStart = face;
                face += call.callDesc.triangleCount * 3;
                auto material = call.callDesc;
                // Draw bounds move with the camera; submission IDs/dirty bits
                // describe traversal rather than the rendered material.
                material.uid = material.callIndex = material.drawStatusChanges = 0;
                material.rect.reset();
                materialState[role].call(material);
            }
        }
    for (const auto &tile : workload.drawData.rdpTiles) {
        auto &s = materialState[role];
        s.add(tile.fmt); s.add(tile.siz); s.add(tile.stride); s.add(tile.address);
        s.add(tile.palette); s.add(tile.masks); s.add(tile.maskt);
        s.f(tile.shifts); s.f(tile.shiftt); s.f(tile.uls); s.f(tile.ult);
        s.f(tile.lrs); s.f(tile.lrt); s.add(tile.cms); s.add(tile.cmt);
    }
    for (const auto &op : workload.drawData.loadOperations) {
        auto &s = materialState[role];
        s.add(op.type); s.add(op.texture.address); s.add(op.texture.width);
        s.add(op.texture.fmt); s.add(op.texture.siz);
        s.add(op.tile.fmt); s.add(op.tile.siz); s.add(op.tile.line); s.add(op.tile.tmem);
        s.add(op.tile.uls); s.add(op.tile.ult); s.add(op.tile.lrs); s.add(op.tile.lrt);
    }
    require(face == workload.drawData.faceIndices.size(), "decoded call ranges cover every submitted face");
    workload.presentationScene.raceActive = true;
    ++captured;
}

int main() {
#ifdef _WIN32
    _putenv_s("RR64_STABLE_PRESENTATION", "1");
#else
    setenv("RR64_STABLE_PRESENTATION", "1", 1);
#endif
    if (rr64_terrain_driver_smoke_main() != 0) return 1;
    require(captured == 2, "driver must provide both actual emitted list paths");
    auto &a = handoff->queue->workloads[0].drawData;
    auto &b = handoff->queue->workloads[1].drawData;
    require(a.posFloats == b.posFloats && a.faceIndices == b.faceIndices && a.tcFloats == b.tcFloats &&
        a.normColBytes == b.normColBytes && a.worldIndices == b.worldIndices &&
        a.viewProjIndices == b.viewProjIndices && materialState[0].values == materialState[1].values,
        "stock wrapper and appended path must submit identical geometry, attributes, materials and texture loads");
    auto expectedRoot = a.worldTransforms[1];
    expectedRoot[3][0] = -.25f;
    require(float(a.worldTransforms[1][3][0]) == 0 && sameMatrix(expectedRoot, b.worldTransforms[1]),
        "actual stock producer must preserve the authored root with the new camera-relative origin");
    auto frame = matchPair();
    require(frame.matched && frame.rr64InterpolationCompatible && frame.rr64GeometryRejectionReasons == 0 &&
        frame.frameMap.workloads[1].transforms[1].mapped,
        "strict production matcher must accept compiled stock/native handoff");
    const auto camera = b.viewProjIndices.front();
    const auto &mapped = frame.frameMap.workloads[1];
    require(mapped.transforms[1].rigidBody.lerpTranslation && mapped.viewProjections[camera].mapped &&
        mapped.viewProjections[camera].rigidBody.lerpTranslation,
        "moving handoff must retain world and camera translation interpolation");
    const auto midWorld = mapped.transforms[1].rigidBody.lerp(.5f, a.worldTransforms[1], b.worldTransforms[1], true);
    const auto midView = mapped.viewProjections[camera].rigidBody.lerp(.5f,
        a.viewTransforms[camera], b.viewTransforms[camera], true);
    const hlslpp::float4 vertex(b.posFloats[0], b.posFloats[1], b.posFloats[2], 1);
    auto clip = [&](const hlslpp::float4x4 &world, const hlslpp::float4x4 &view) {
        return hlslpp::mul(hlslpp::mul(hlslpp::mul(vertex, world), view), b.projTransforms[camera]);
    };
    const auto before = clip(a.worldTransforms[1], a.viewTransforms[camera]);
    const auto after = clip(b.worldTransforms[1], b.viewTransforms[camera]);
    const auto midpoint = clip(midWorld, midView);
    require(std::abs(float(after.x - before.x) + .125f) < .00001f &&
        std::abs(float(midpoint.x - before.x) + .0625f) < .00001f,
        "generated midpoint must combine the matched world and camera at the same weight");
    const auto group = b.worldTransformGroups[1];
    b.transformGroups[group].matrixId = G_EX_ID_AUTO;
    frame = matchPair();
    require(!frame.rr64InterpolationCompatible && (frame.rr64GeometryRejectionReasons & 4),
        "removing stock identity must reproduce membership rejection");
    b.transformGroups[group].matrixId = 0x52510000u;
    std::swap(b.faceIndices[0], b.faceIndices[1]);
    handoff->queue->workloads[1].rr64GeometryCache[0].reset();
    handoff->queue->workloads[1].rr64GeometryCache[1].reset();
    frame = matchPair();
    require(!frame.rr64InterpolationCompatible && (frame.rr64GeometryRejectionReasons & 16),
        "same tag with changed triangle order must remain rejected");
    std::puts("Terrain RSP handoff PASS: real list traversal, matched perspective scene, strict ID/topology negatives");
    return 0;
}
