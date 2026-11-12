#include "rr64_world_terrain_assets.hpp"
#include "recomp.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

extern "C" void func_8007CFFC(std::uint8_t*, recomp_context*);
extern "C" void func_8000CD34(std::uint8_t*, recomp_context*) {
    std::fputs("FAIL: original loader must never execute in asset compilation\n", stderr); std::abort();
}

namespace {
int failures = 0;
void check(bool pass, const char* message) {
    if (!pass) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void put16(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t value) {
    b[p] = std::uint8_t(value >> 8); b[p + 1] = std::uint8_t(value);
}
void put32(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) { b[p + i] = std::uint8_t(value >> (24u - i * 8u)); }
}
std::uint32_t word(std::span<const std::uint8_t> b, std::size_t p) {
    return (std::uint32_t(b[p]) << 24u) | (std::uint32_t(b[p + 1]) << 16u) |
        (std::uint32_t(b[p + 2]) << 8u) | b[p + 3];
}
void memory_word(std::vector<std::uint8_t>& memory, std::uint32_t p, std::uint32_t v) {
    std::memcpy(memory.data() + (p & 0x7fffffu), &v, 4);
}
std::uint32_t memory_word(const std::vector<std::uint8_t>& memory, std::uint32_t p) {
    std::uint32_t v; std::memcpy(&v, memory.data() + (p & 0x7fffffu), 4); return v;
}
void memory_bytes(std::vector<std::uint8_t>& memory, std::uint32_t p, std::span<const std::uint8_t> b) {
    for (std::size_t i = 0; i < b.size(); ++i) { memory[((p & 0x7fffffu) + i) ^ 3u] = b[i]; }
}
gpr guest(std::uint32_t p) { return static_cast<gpr>(static_cast<std::int32_t>(p)); }
constexpr std::size_t map = 0x18d380u, refs = map + 0x18u;
constexpr std::size_t texture = 0x200000u, partition = 0x300000u, submesh = partition + 0x100u;
constexpr std::size_t submesh_size = 0x18u + 64u + 64u;
constexpr std::uint32_t original_submesh = 0x80200000u;

std::vector<std::uint8_t> fixture(std::uint16_t bits, std::uint16_t flags,
    std::uint16_t render_packets = 2u, std::uint16_t material = 0u, bool animated = false) {
    std::vector<std::uint8_t> rom(32u * 1024u * 1024u);
    put32(rom, 0, 0x80371240u); put32(rom, map, 0x3eu);
    put16(rom, map + 0x10u, 1000u); put16(rom, map + 0x12u, 4900u);
    put16(rom, map + 0x14u, 926u);
    const auto pixel_bytes = 32u * 32u * bits / 8u;
    const auto palette_bytes = (flags & 0x8000u) ? 0u : (bits == 4u ? 32u : 512u);
    const auto stride = pixel_bytes + palette_bytes;
    const auto texture_size = 0x40u + stride * (animated ? 3u : 1u);
    for (std::size_t i = 0; i < 926u; ++i) {
        const auto entry = refs + i * 12u;
        put32(rom, entry, static_cast<std::uint32_t>(texture - entry)); put32(rom, entry + 4u, texture_size);
    }
    put32(rom, texture, animated ? 0x17u : 0x16u); put32(rom, texture + 4u, texture_size);
    put32(rom, texture + 8u, 0xffffffffu);
    put16(rom, texture + 0x20u, animated ? 3u : 1u); put16(rom, texture + 0x22u, palette_bytes);
    put16(rom, texture + 0x24u, flags); put16(rom, texture + 0x26u, pixel_bytes);
    put32(rom, texture + 0x2cu, 32u); put32(rom, texture + 0x30u, 32u);
    put32(rom, texture + 0x34u, bits); put32(rom, texture + 0x3cu, stride);
    for (std::size_t i = 0x40u; i < texture_size; ++i) { rom[texture + i] = std::uint8_t(i * 37u); }
    const auto cell_ref = refs + 926u * 12u;
    put32(rom, cell_ref, static_cast<std::uint32_t>(partition - cell_ref)); put32(rom, cell_ref + 4u, 0x200u);
    put32(rom, partition, 0x3fu); put32(rom, partition + 4u, 0x200u); put32(rom, partition + 8u, 0x1234u);
    put16(rom, partition + 0xcu, 1u);
    put32(rom, partition + 0x1cu, std::bit_cast<std::uint32_t>(1200.0f));
    put32(rom, partition + 0x20u, std::bit_cast<std::uint32_t>(-3400.0f));
    put32(rom, partition + 0x30u, std::bit_cast<std::uint32_t>(1.0f));
    put32(rom, partition + 0xf0u, 0x10u); put32(rom, partition + 0xf4u, submesh_size);
    put32(rom, submesh, 0x3du); put32(rom, submesh + 4u, submesh_size);
    put16(rom, submesh + 0xcu, 2u); put16(rom, submesh + 0xeu, render_packets);
    put16(rom, submesh + 0x12u, material);
    for (std::size_t packet = 0; packet < 2u; ++packet) {
        const auto p = submesh + 0x18u + packet * 64u;
        put16(rom, p, packet == 0 ? 1u : 2u); put16(rom, p + 2u, 3u);
        put16(rom, p + 4u, 64u); put16(rom, p + 6u, 48u);
        for (std::size_t vertex = 0; vertex < 3u; ++vertex) {
            for (std::size_t i = 0; i < 16u; ++i) { rom[p + 8u + vertex * 16u + i] = std::uint8_t(i + vertex * 31u + packet * 17u); }
            put16(rom, p + 8u + vertex * 16u, static_cast<std::uint16_t>(static_cast<std::int16_t>(int(vertex) * 11 - 5)));
            put16(rom, p + 10u + vertex * 16u, static_cast<std::uint16_t>(static_cast<std::int16_t>(int(vertex) * 7 - 23)));
            put16(rom, p + 12u + vertex * 16u, static_cast<std::uint16_t>(vertex * 13u));
        }
        put16(rom, p + 56u, (0u << 10u) | (1u << 5u) | 2u);
        if (packet) { put16(rom, p + 58u, (2u << 10u) | (0u << 5u) | 1u); }
    }
    return rom;
}

void original_parity(const std::vector<std::uint8_t>& rom, const rr64::world::TerrainAssets& assets,
    bool textured) {
    std::vector<std::uint8_t> memory(8u * 1024u * 1024u);
    memory_bytes(memory, 0x80100000u, std::span(rom).subspan(texture, word(rom, texture + 4u)));
    memory_bytes(memory, original_submesh, std::span(rom).subspan(submesh, submesh_size));
    memory_word(memory, 0x800dea84u, 0x80300000u); memory_word(memory, 0x80300000u, 0x80100000u);
    memory_word(memory, 0x800b1a20u, 0xffffffffu); memory_word(memory, 0x803f0000u, 0x80400000u);
    recomp_context context{}; context.f_odd = &context.f0.u32h;
    context.r4 = guest(original_submesh); context.r6 = guest(0x803f0000u);
    context.r7 = guest(0x803f0004u); context.r29 = guest(0x807ff000u);
    const auto original_rom = rom;
    func_8007CFFC(memory.data(), &context);
    const auto& cell = assets.cells[0];
    const auto cache_begin = cell.display_list_offset + (textured ? 8u : 0u);
    const auto cache_end = cell.display_list_offset + cell.display_list_size - 8u;
    const auto original_end = memory_word(memory, 0x803f0000u);
    check(cache_end - cache_begin == original_end - 0x80400000u, "cached packet/material command length equals original7CFFC");
    if (cache_end - cache_begin != original_end - 0x80400000u) { return; }
    for (auto cache = cache_begin, original = 0x80400000u; cache < cache_end; cache += 8u, original += 8u) {
        const auto w0 = word(assets.bytes, cache), w1 = word(assets.bytes, cache + 4u);
        check(w0 == memory_word(memory, original), "cached command opcode and fields match original7CFFC");
        if ((w0 >> 24u) == 1u) {
            const auto relocation = std::find_if(assets.relocations.begin(), assets.relocations.end(),
                [cache](const auto& item) { return item.word_offset == cache + 4u; });
            check(relocation != assets.relocations.end(), "every cached vertex load has a relocation");
            if (relocation == assets.relocations.end()) { continue; }
            const auto source = memory_word(memory, original + 4u) - original_submesh + submesh;
            const auto count = (w0 >> 12u) & 0xffu;
            check(std::equal(assets.bytes.begin() + relocation->target_offset,
                assets.bytes.begin() + relocation->target_offset + count * 16u, rom.begin() + source),
                "cached vertices retain original XYZ, ST, flags and RGBA bytes");
        }
        else if ((w0 >> 24u) != 5u) {
            check(w1 == memory_word(memory, original + 4u), "cached command second word matches original7CFFC");
        }
    }
    check(rom == original_rom, "asset construction does not modify input ROM bytes");
    if (textured) {
        check(word(assets.bytes, cell.display_list_offset) == 0xdb06001cu,
            "cached terrain explicitly binds material segment seven");
        const auto& relocation = assets.relocations.front();
        check(relocation.texture_index == 0u && relocation.target_offset == assets.textures[0].raw_offset + 0x40u,
            "segment seven points to raw texture pixel data and retains palette-relative offsets");
    }
    check(word(assets.bytes, cache_end) == 0xdf000000u, "cell display list is explicitly terminated");
}

void synthetic() {
    for (const auto bits : {4u, 8u}) {
        for (const auto flags : {0u, 0x4000u, 0x8000u}) {
            const auto rom = fixture(bits, flags);
            rr64::world::TerrainAssets assets; std::string error;
            const bool built = rr64::world::build_terrain_assets(rom, assets, error);
            check(built, error.c_str()); if (!built) { continue; }
            check(assets.cells.size() == 1u && assets.packets == 2u && assets.vertices == 6u && assets.triangles == 3u,
                "synthetic map has expected rendered packet inventory");
            original_parity(rom, assets, true);
        }
    }
    for (const auto material : {0u, 0xffffu}) {
        const auto rom = fixture(4u, 0u, 1u, material);
        rr64::world::TerrainAssets assets; std::string error;
        check(rr64::world::build_terrain_assets(rom, assets, error), error.c_str());
        if (!assets.cells.empty()) {
            check(assets.packets == 1u && assets.non_render_packets == 1u && assets.triangles == 1u,
                "authored non-render packets are validated and omitted");
            original_parity(rom, assets, material != 0xffffu);
        }
    }
    const auto animated = fixture(4u, 0x14u, 2u, 0u, true);
    rr64::world::TerrainAssets animation_assets; std::string error;
    check(rr64::world::build_terrain_assets(animated, animation_assets, error), error.c_str());
    check(animation_assets.textures.size() == 926u && animation_assets.textures[0].frame_count == 3u,
        "animated texture keeps all frames and their stride metadata");
    for (int invalid = 0; invalid < 11; ++invalid) {
        auto rom = fixture(4u, 0u);
        switch (invalid) {
        case 0: rom.resize(map); break;
        case 1: put32(rom, refs, 0xffffffffu); break;
        case 2: put16(rom, texture + 0x20u, 64u); break;
        case 3: put32(rom, texture + 0x2cu, 0u); break;
        case 4: put16(rom, texture + 0x26u, 0u); break;
        case 5: put16(rom, submesh + 0xeu, 3u); break;
        case 6: put16(rom, submesh + 0x12u, 926u); break;
        case 7: put16(rom, submesh + 0x18u + 2u, 33u); break;
        case 8: put16(rom, submesh + 0x18u + 56u, 31u); break;
        case 9: put32(rom, partition + 0x24u, 0x7fc00000u); break;
        case 10: put32(rom, partition + 0xf4u, 0xffffffffu); break;
        }
        rr64::world::TerrainAssets sentinel; sentinel.bytes = {0xa5u};
        check(!rr64::world::build_terrain_assets(rom, sentinel, error) && !error.empty() &&
            sentinel.bytes == std::vector<std::uint8_t>{0xa5u}, "invalid asset input refuses transactionally without changing prior cache");
    }
}
} // namespace

int main(int argc, char** argv) {
    synthetic();
    if (argc == 3 && std::string(argv[1]) == "--rom") {
        std::ifstream input(argv[2], std::ios::binary);
        std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(input)), {});
        rr64::world::TerrainAssets assets; std::string error;
        const bool built = rr64::world::build_terrain_assets(rom, assets, error);
        check(built, error.c_str());
        if (built) {
            std::printf("ROM assets: cells=%zu textures=%zu render-packets=%u vertices=%u triangles=%u non-render-packets=%u bytes=%zu relocations=%zu\n",
                assets.cells.size(), assets.textures.size(), assets.packets, assets.vertices, assets.triangles,
                assets.non_render_packets, assets.bytes.size(), assets.relocations.size());
            check(assets.cells.size() == 3289u && assets.textures.size() == 926u,
                "actual supported ROM cache includes every occupied cell and texture reference");
        }
    }
    std::printf("Terrain assets smoke: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
