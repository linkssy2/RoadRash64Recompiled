// Reuse the renderer-free RSP fixture, including actual GBI list traversal.
#define SDL_MAIN_HANDLED
#define main rr64_shadow_unused_triangle_main
#include "rr64_rsp_triangle_batch_smoke.cpp"
#undef main
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include "rr64_shadow_tags.hpp"
#include "rr64_engine_layout.hpp"
#include "recomp.h"
#include <fstream>

extern "C" void func_8000D518(unsigned char *, recomp_context *);
extern "C" void func_8001AD24(unsigned char *, recomp_context *);
extern "C" void func_80010420(unsigned char *, recomp_context *);
extern "C" void fixture_bike_owner(unsigned char *, recomp_context *);
extern "C" void fixture_rider_owner(unsigned char *, recomp_context *);
extern "C" void fixture_route_reset(unsigned char *, recomp_context *);
extern "C" void fixture_actor_slot(unsigned char *, recomp_context *);

namespace {
using namespace rr64::engine;
constexpr unsigned base = 0x80100000u, block = 0x80500000u, graph = 0x80510000u;
constexpr unsigned node = 0x80511000u, bike_entity = 0x80200000u, rider_entity = 0x80201000u;
constexpr unsigned racer_slot = 3, actor = 0x800d8570u + racer_slot * 0x118u;
constexpr unsigned route = 0x800d7810u + racer_slot * 0x64u;
constexpr unsigned resource = block + 0xcc0u, vertex_command = base + 0x150u;
unsigned word(unsigned char *m, unsigned p) { unsigned v = 0; read_u32(m, p, v); return v; }
void seed(std::vector<uint8_t> &ram) {
    ram.assign(kRdramSize, 0); auto *m = ram.data();
    write_u32(m, 0x8009cba4u, 0); write_u32(m, 0x800bc9a0u, 0x4650u);
    write_u32(m, 0x800ac658u, base); write_u32(m, 0x8009cb90u, base);
    write_u32(m, 0x800ac650u, base + 0x140u); write_u32(m, 0x800a1830u, 19);
    recomp_context ctx{}; ctx.r29 = int32_t(0x807ff000u);
    ctx.r16 = racer_slot; ctx.r20 = int32_t(actor); fixture_actor_slot(m, &ctx);
    ctx.r4 = int32_t(bike_entity); ctx.r5 = racer_slot; ctx.r6 = 1; fixture_bike_owner(m, &ctx);
    ctx.r4 = int32_t(rider_entity); ctx.r5 = racer_slot; ctx.r6 = 2; fixture_rider_owner(m, &ctx);
    ctx.r4 = int32_t(route); fixture_route_reset(m, &ctx);
    require(word(m, bike_entity + 4) == actor && word(m, rider_entity + 4) == actor &&
        word(m, actor) == racer_slot && word(m, route) == 0,
        "original constructors distinguish racer owner/slot from zero-initialized route state");
    write_u16(m, actor + 0x24u, 1);
    write_u32(m, actor + 0xe0u, bike_entity);
    write_u32(m, actor + 0xe4u, rider_entity);
    write_u32(m, actor + 0xe8u, route);
    write_u32(m, bike_entity + 8, node);
    write_u32(m, bike_entity + 0x800u, rider_entity); write_u32(m, rider_entity + 0x584u, bike_entity);
    write_u32(m, node, 1); write_u32(m, node + 4, bike_entity); write_u32(m, node + 0x40u, racer_slot);
    write_u32(m, 0x800dac50u + racer_slot * 4, graph); write_u32(m, 0x800d42d8u, block);
    write_u32(m, block, 0x40); write_u32(m, block + 4, 0x2100); write_u32(m, block + 12, 5);
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned p = graph + i * 24;
        write_u16(m, p + 4, i == 0 ? 0x13 : i == 1 ? 0x12 : 0x10);
        write_u16(m, p + 8, i == 2 ? 0 : 3);
        write_u16(m, p + 10, i == 2 ? 0x4000 : i == 1 ? 0x2000 : 0x1000);
        write_u32(m, p + 20, resource + (i == 0 ? 0 : i == 1 ? 0x50 : 0x100));
    }
    write_u32(m, resource, 0x13); write_u16(m, resource + 18, 1);
    write_u32(m, resource + 0x50, 0x12); write_u32(m, resource + 0x100, 0x10);
    write_u16(m, resource + 0x10c, 1); write_u16(m, resource + 0x128, 2);
    write_u16(m, resource + 0x12a, 4); write_u16(m, resource + 0x12e, 64);
    write_u16(m, resource + 0x170, 0x22); write_u16(m, resource + 0x172, 0x860);
    for (unsigned v = 0; v < 4; ++v) {
        const unsigned p = resource + 0x130 + v * 16;
        write_u16(m, p, v == 0 || v == 3 ? -1 : 1);
        write_u16(m, p + 2, v < 2 ? -1 : 1); write_u16(m, p + 4, 0);
        write_u32(m, p + 12, 0xffffffffu);
    }
    for (unsigned matrix : {0x80600000u, 0x80600040u})
        for (unsigned i = 0; i < 16; ++i) write_u16(m, matrix + i * 2, i % 5 == 0 ? 1 : 0);
}
void emit(unsigned char *m, bool extra_vertex = false, unsigned display_base = base) {
    unsigned p = display_base + 0x140;
    auto cmd = [&](unsigned a, unsigned b) { write_u32(m, p, a); write_u32(m, p + 4, b); p += 8; };
    cmd(0xda380003u, 0x80600000u); cmd(0xda380000u, 0x80600040u);
    cmd(0x01004008u, resource + 0x130u);
    if (extra_vertex) cmd(0x01004008u, resource + 0x130u);
    cmd(0x06000204u, 0x00040600u); cmd(0xd8380002u, 64);
    write_u32(m, 0x800ac650u, p);
}
unsigned finish(unsigned char *m, unsigned reserved_tail = 0, unsigned display_base = base) {
    unsigned p = word(m, 0x800ac650u);
    write_u32(m, p, 0xe9000000u); write_u32(m, p + 4, 0);
    write_u32(m, p + 8, 0xdf000000u); write_u32(m, p + 12, 0);
    p += 16; write_u32(m, 0x800ac650u, p + reserved_tail);
    return (p - display_base - 0x140u) / 4;
}
void begin(unsigned char *m, unsigned generation = 1) {
    std::array<uint32_t, 14> generations{}; generations[racer_slot] = generation;
    rr64::shadow_tags::begin(m, 1, 1, generations);
}
void record(unsigned char *m, bool extra_vertex = false, unsigned display_base = base) {
    rr64_shadow_tags_before(m, node, graph, 0); emit(m, extra_vertex, display_base); rr64_shadow_tags_after(m);
}
struct Decoded { std::vector<float> positions; std::vector<uint32_t> faces; unsigned tag = 0; };
Decoded decode(const std::vector<uint8_t> &ram, unsigned display_base = base, unsigned quads = 1) {
    auto f = std::make_unique<Fixture>(); f->ram = ram; f->state->RDRAM = f->ram.data();
    auto &workload = f->workload(); workload.reset(); f->state->reset(); f->state->clearExtended();
    f->state->rdramCheckPending = false; f->interpreter->setup(f->state.get());
    f->interpreter->extendedOpCode = 0; f->gbi = {}; f->gbi.ucode = GBIUCode::F3DEX2;
    GBI_RDP::setup(&f->gbi, true); GBI_F3DEX2::setup(&f->gbi); auto &rsp = f->rsp(); rsp.setGBI(&f->gbi);
    rsp.viewportStack[0].scale = {160,120,512}; rsp.viewportStack[0].translate = {160,120,512};
    f->rdp().colorImage = {}; f->rdp().colorImage.width = 320; f->rdp().colorImage.siz = G_IM_SIZ_16b;
    f->rdp().depthImage = {}; f->rdp().scissorRectStack[0] = {0,0,1280,960};
    // Stop immediately before FullSync's GPU submission. All model, vertex,
    // triangle and wrapper commands still traverse the production interpreter.
    unsigned stop = display_base + 0x140;
    while (word(f->ram.data(), stop) != 0xe9000000u && stop < display_base + 0x1000u) stop += 8;
    require(word(f->ram.data(), stop) == 0xe9000000u, "fixture has a bounded full-sync boundary");
    write_u32(f->ram.data(), stop, 0xdf000000u);
    f->interpreter->processDisplayLists(display_base + 0x140u - kRdramBegin,
        reinterpret_cast<DisplayList *>(f->ram.data() + display_base + 0x140u - kRdramBegin));
    f->state->flush();
    const auto &d = workload.drawData;
    require(d.vertexCount() == quads * 4 && d.faceIndices.size() == quads * 6 && d.worldTransforms.size() == quads + 1,
        "each authenticated shadow remains one quad and one captured world transform");
    require(rsp.extended.modelMatrixIdStackSize == 1 && rsp.modelMatrixStackSize == 1 &&
        f->interpreter->extendedOpCode == 0 && !f->state->extended.extendRDRAM,
        "vertex wrapper restores model ID, matrix depth, GBI and address mode");
    const auto &group = d.transformGroups[d.worldTransformGroups[1]];
    require(group.decompose && group.vertexInterpolation == G_EX_COMPONENT_SKIP,
        "explicit shadow identity preserves default transform/vertex interpolation policies");
    return {d.posFloats, d.faceIndices, group.matrixId};
}
void native_producer(std::vector<uint8_t> &ram, const char *rom_path, bool distant) {
    seed(ram); auto *m = ram.data();
    std::ifstream rom(rom_path, std::ios::binary);
    std::array<unsigned char, 0x2100> asset{};
    rom.seekg(0x18b280); rom.read(reinterpret_cast<char *>(asset.data()), asset.size());
    require(bool(rom), "read the original supported ROM shadow asset locally");
    for (unsigned i = 0; i < asset.size(); i += 4)
        write_u32(m, block + i, unsigned(asset[i]) << 24 | unsigned(asset[i + 1]) << 16 |
            unsigned(asset[i + 2]) << 8 | unsigned(asset[i + 3]));
    recomp_context ctx{}; ctx.r29 = int32_t(0x807ff000u);
    ctx.r4 = int32_t(block); ctx.r5 = 0; ctx.r6 = 0; ctx.r7 = int32_t(0x800d4318u);
    write_u32(m, unsigned(ctx.r29) + 0x10u, 0x800a1738u);
    func_8000D518(m, &ctx);
    require(word(m, 0x800a1738u) == 5, "original loader resolves five shadow-block assets");
    // Native allocator consumes this free list; 1AD24 creates the runtime
    // records and their links/flags from ROM bytes, without test graph constants.
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned p = graph + i * 24;
        for (unsigned n = 0; n < 24; n += 4) write_u32(m, p + n, 0);
        write_u16(m, p + 8, i == 5 ? 0 : 3);
    }
    write_u32(m, 0x800bacc0u + 0x10u, graph);
    for (unsigned part = 0; part < 2; ++part) {
        ctx.r4 = int32_t(word(m, 0x800d431cu + part * 4)); ctx.r5 = 0;
        func_8001AD24(m, &ctx);
        const unsigned root = word(m, 0x800bacd8u);
        write_u32(m, (part ? 0x800dac88u : 0x800dac50u) + racer_slot * 4, root);
        // Pose math is a fixture boundary. The real matrix producers below
        // still emit the model-load/push operations used by the guard.
        write_u32(m, root + 12, 0x80610000u);
        write_u32(m, root + 24 + 12, 0x80610020u);
    }
    write_u32(m, node + 0x28, graph); write_u32(m, 0x800a1450u, node);
    write_float(m, node + 8, distant ? 100.0f : 1.0f);
    write_float(m, 0x8009dc60u, 50.0f); write_float(m, 0x8009dc68u, 200.0f);
    write_float(m, bike_entity + 0x4d4u, 1.0f); write_u16(m, bike_entity + 0x7f8u, 1);
    write_u32(m, 0x8009dbecu, 1); write_u32(m, 0x800b6550u, 0x80600000u);
    const auto count_before = rr64_shadow_tags_count();
    begin(m); func_80010420(m, &ctx);
    const auto words = finish(m); const auto original = ram;
    rr64_shadow_tags_finalize(m, words);
    require(rr64_shadow_tags_count() == count_before + 2,
        "original near/far producer activates both bike and rider shadows for nonzero racer slot");
    const auto vanilla = decode(original, base, 2), tagged = decode(ram, base, 2);
    require(vanilla.positions == tagged.positions && vanilla.faces == tagged.faces &&
        (tagged.tag & 0xffff0000u) == 0x52530000u,
        "ROM-driven original graph and quad emission retain geometry with explicit shadow identities");
}
}
// Omit material/texture state and unrelated presentation hooks; the fixture
// exercises original graph and geometry commands, not visual shading.
extern "C" void func_8000F594(unsigned char *, recomp_context *) {}
extern "C" void func_8001F76C(unsigned char *, recomp_context *) {}
extern "C" void rr64_highlight_render_projection(unsigned char *) {}
extern "C" void rr64_rider_skin_preview_begin(unsigned char *, unsigned) {}
extern "C" void rr64_rider_skin_preview_end(unsigned char *) {}
extern "C" void rr64_weapon_source(unsigned char *, void *, unsigned) {}
extern "C" void rr64_weapon_matrix(unsigned char *, unsigned, unsigned) {}
extern "C" void rr64_weapon_packed(unsigned char *, unsigned, unsigned) {}
extern "C" void rr64_highlights_weapon_matrix(unsigned char *, unsigned, unsigned) {}
extern "C" void func_80015A90(unsigned char *m, recomp_context *c) {
    for (unsigned i = 0; i < 16; ++i) rr64::engine::write_float(m, unsigned(c->r6) + i * 4, i % 5 == 0 ? 1.0f : 0.0f);
}
extern "C" void guMtxF2L(unsigned char *m, recomp_context *c) {
    for (unsigned i = 0; i < 32; ++i) rr64::engine::write_u16(m, unsigned(c->r5) + i * 2, i < 16 && i % 5 == 0 ? 1 : 0);
}
extern "C" void n_alSeqpDelete(unsigned char *m, recomp_context *c) { guMtxF2L(m, c); }
extern "C" void _bcopy(unsigned char *m, recomp_context *c) {
    for (unsigned i = 0; i < unsigned(c->r6); i += 4)
        rr64::engine::write_u32(m, unsigned(c->r5) + i, word(m, unsigned(c->r4) + i));
}
int main(int argc, char **argv) {
    require(argc == 3 && std::string(argv[1]) == "--rom", "usage: RR64ShadowTagsSmoke --rom <supported roadrash64.us.z64>");
    GBI_EXTENDED::initialize();
    std::vector<uint8_t> ram; seed(ram); auto *m = ram.data(); begin(m); record(m);
    require(rr64_shadow_tags_count() == 0, "recording a candidate does not count unsubmitted work");
    const unsigned words = finish(m), original_end = word(m, 0x800ac650u);
    const auto original = ram; rr64_shadow_tags_finalize(m, words);
    require(word(m, vertex_command) == 0xde000000u && word(m, vertex_command + 4) == original_end &&
        word(m, 0x800ac650u) == original_end + 72, "one vertex load becomes a bounded same-slot tail call");
    const auto vanilla = decode(original), tagged = decode(ram);
    require(vanilla.positions == tagged.positions && vanilla.faces == tagged.faces &&
        vanilla.tag == G_EX_ID_AUTO && (tagged.tag & 0xffff0000u) == 0x52530000u,
        "actual RSP captures explicit identity with identical geometry after tag pop");
    const unsigned first_id = tagged.tag;
    seed(ram);
    constexpr unsigned other_base = base + 0x30000u;
    write_u32(m, 0x8009cba4u, 1); write_u32(m, 0x800ac65cu, other_base);
    write_u32(m, 0x8009cb90u, other_base); write_u32(m, 0x800ac650u, other_base + 0x140u);
    begin(m); record(m, false, other_base);
    auto count = finish(m, 0, other_base); rr64_shadow_tags_finalize(m, count);
    require(word(m, other_base + 0x154u) == other_base + 0x178u &&
        decode(ram, other_base).tag == first_id,
        "alternating graphics allocations retain actor identity and own their wrapper storage");
    seed(ram); begin(m); record(m); count = finish(m, 24); const auto before_tail = word(m, 0x800ac650u);
    rr64_shadow_tags_finalize(m, count);
    require(word(m, vertex_command + 4) == before_tail && decode(ram).tag == first_id,
        "same owner generation keeps identity and respects terrain's earlier tail reservation");
    seed(ram); begin(m, 2); record(m); count = finish(m); rr64_shadow_tags_finalize(m, count);
    require(decode(ram).tag != first_id, "committed actor recovery cannot inherit an old shadow identity");
    for (unsigned mode = 0; mode < 10; ++mode) {
        seed(ram); begin(m);
        if (mode == 0) write_u32(m, bike_entity + 0x800u, 0);
        if (mode == 1) write_u16(m, resource + 18, 2);
        if (mode == 7) write_u32(m, bike_entity + 4, route);
        if (mode == 8) write_u32(m, rider_entity + 4, route);
        if (mode == 9) write_u32(m, actor, racer_slot + 1);
        record(m, mode == 2); count = finish(m);
        if (mode == 3) write_u32(m, 0x800a1830u, 20);
        if (mode == 4) write_u32(m, 0x800ac650u, base + 0x140u + 0x4650u * 8u - 64u);
        if (mode == 5) write_u32(m, vertex_command + 4, 0x805f0000u);
        if (mode == 6) rr64_shadow_tags_reset();
        const auto unchanged = ram; rr64_shadow_tags_finalize(m, count);
        require(ram == unchanged, "invalid owner/graph/span/epoch/capacity/command/scope must write nothing");
    }
    require(rr64_shadow_tags_count() == 4, "cumulative activation count includes only finalized quads");
    native_producer(ram, argv[2], false); native_producer(ram, argv[2], true);
    std::puts("Shadow tags PASS: original ROM graph and near/far draw producers, nonzero racer ownership, real RSP geometry, generation cut and refusal cases");
}
